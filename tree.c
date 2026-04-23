// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Forward declarations — defined in object.c, no shared header exists
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int hex_to_hash(const char *hex, ObjectID *id_out);

// ─── Mode Constants ──────────────────────────────────────────────────────────

#define MODE_FILE  0100644
#define MODE_EXEC  0100755
#define MODE_DIR   0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    if (max_size == 0) max_size = 1; // avoid malloc(0)
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted = *tree;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", e->mode, e->name);
        offset += (size_t)written + 1; // +1 for null terminator
        memcpy(buffer + offset, e->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Internal: minimal index reader (no dependency on index.o) ───────────────
// test_tree links only object.o + tree.o, so we cannot call index_load().
// We read .pes/index directly here.
// Format per line: <mode-octal> <64-hex-hash> <mtime> <size> <path>

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} RawEntry;

static int load_raw_entries(RawEntry **out, int *count_out) {
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        *out = NULL;
        *count_out = 0;
        return 0; // no index = empty, not an error
    }

    int capacity = 64;
    RawEntry *entries = malloc((size_t)capacity * sizeof(RawEntry));
    if (!entries) { fclose(f); return -1; }

    int count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (count >= capacity) {
            capacity *= 2;
            RawEntry *tmp = realloc(entries, (size_t)capacity * sizeof(RawEntry));
            if (!tmp) { free(entries); fclose(f); return -1; }
            entries = tmp;
        }
        char hex[HASH_HEX_SIZE + 2];
        uint64_t mtime;
        uint32_t size;
        int rc = sscanf(line, "%o %65s %lu %u %511s",
                        &entries[count].mode, hex,
                        &mtime, &size, entries[count].path);
        if (rc == 5 && hex_to_hash(hex, &entries[count].hash) == 0)
            count++;
    }
    fclose(f);

    *out = entries;
    *count_out = count;
    return 0;
}

// ─── Recursive tree builder ───────────────────────────────────────────────────

static int compare_raw_paths(const void *a, const void *b) {
    return strcmp(((const RawEntry *)a)->path, ((const RawEntry *)b)->path);
}

static int write_tree_recursive(RawEntry *entries, int count,
                                int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Navigate to the path component at this depth level
        const char *p = entries[i].path;
        for (int d = 0; d < depth; d++) {
            const char *sl = strchr(p, '/');
            if (sl) p = sl + 1;
        }

        const char *slash = strchr(p, '/');

        if (!slash) {
            // Direct file at this level
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            strncpy(e->name, p, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = entries[i].hash;
            i++;
        } else {
            // Subdirectory — extract name and group all entries under it
            size_t dir_len = (size_t)(slash - p);
            char dir_name[256];
            if (dir_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, p, dir_len);
            dir_name[dir_len] = '\0';

            // Find end of this subdir's entries
            int j = i;
            while (j < count) {
                const char *q = entries[j].path;
                for (int d = 0; d < depth; d++) {
                    const char *sl = strchr(q, '/');
                    if (sl) q = sl + 1;
                }
                if (strncmp(q, dir_name, dir_len) == 0 && q[dir_len] == '/')
                    j++;
                else
                    break;
            }

            // Recurse to build subtree for entries[i..j)
            ObjectID subtree_id;
            if (write_tree_recursive(entries + i, j - i, depth + 1, &subtree_id) != 0)
                return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = MODE_DIR;
            strncpy(e->name, dir_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = subtree_id;
            i = j;
        }
    }

    void *tdata;
    size_t tlen;
    if (tree_serialize(&tree, &tdata, &tlen) != 0) return -1;
    int rc = object_write(OBJ_TREE, tdata, tlen, id_out);
    free(tdata);
    return rc;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int tree_from_index(ObjectID *id_out) {
    RawEntry *entries;
    int count;
    if (load_raw_entries(&entries, &count) != 0) return -1;

    if (count == 0) {
        Tree empty;
        empty.count = 0;
        void *tdata; size_t tlen;
        if (tree_serialize(&empty, &tdata, &tlen) != 0) { free(entries); return -1; }
        int rc = object_write(OBJ_TREE, tdata, tlen, id_out);
        free(tdata);
        free(entries);
        return rc;
    }

    qsort(entries, (size_t)count, sizeof(RawEntry), compare_raw_paths);

    int rc = write_tree_recursive(entries, count, 0, id_out);
    free(entries);
    return rc;
}
