// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    if (type == OBJ_BLOB)       type_str = "blob";
    else if (type == OBJ_TREE)  type_str = "tree";
    else                         type_str = "commit";

    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t full_size = hlen + 1 + len;

    uint8_t *full = malloc(full_size);
    if (!full) return -1;
    memcpy(full, header, hlen + 1);
    memcpy(full + hlen + 1, data, len);

    compute_hash(full, full_size, id_out);

    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[256], path[512], tmp_path[520];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    snprintf(path, sizeof(path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    mkdir(OBJECTS_DIR, 0755);
    mkdir(shard_dir, 0755);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full); return -1; }

    write(fd, full, full_size);
    fsync(fd);
    close(fd);
    free(full);

    if (rename(tmp_path, path) != 0) return -1;

    int dfd = open(shard_dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fsize, f);
    fclose(f);

    ObjectID computed;
    compute_hash(buf, fsize, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    char *null_pos = memchr(buf, '\0', fsize);
    if (!null_pos) { free(buf); return -1; }

    char type_str[16];
    size_t data_size;
    sscanf((char *)buf, "%15s %zu", type_str, &data_size);

    if (strcmp(type_str, "blob") == 0)       *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0)  *type_out = OBJ_TREE;
    else                                      *type_out = OBJ_COMMIT;

    *len_out = data_size;

    uint8_t *out = malloc(data_size);
    if (!out) { free(buf); return -1; }
    memcpy(out, null_pos + 1, data_size);
    free(buf);

    *data_out = out;
    return 0;
}
