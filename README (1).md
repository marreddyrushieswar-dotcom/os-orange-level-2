# PES-VCS Lab Report

**Name:** G Sai Hemanth  
**SRN:** PES1UG24AM101  
**GitHub Repository:** https://github.com/GSAIHEMANTH279/PES1UG24AM101-pes-vcs  

---

## Phase 1: Object Storage Foundation

**Screenshot 1A:** `./test_objects` output showing all tests passing.
<img width="996" height="204" alt="image" src="https://github.com/user-attachments/assets/328a56f7-1a92-4dab-b9c7-fb53c18065f0" />


**Screenshot 1B:** `find .pes/objects -type f` showing sharded directory structure.
<img width="857" height="120" alt="image" src="https://github.com/user-attachments/assets/0fb6ecd3-727d-4dc6-a151-8d6a1f8ac18d" />


---

## Phase 2: Tree Objects

**Screenshot 2A:** `./test_tree` output showing all tests passing.
<img width="763" height="168" alt="image" src="https://github.com/user-attachments/assets/c391b94f-ff38-4d0e-84d6-333d17fd1ff8" />


**Screenshot 2B:** `xxd` of a raw tree object (first 20 lines).
<img width="1220" height="123" alt="image" src="https://github.com/user-attachments/assets/2242e7dd-6445-47de-9b75-7e9b63fee334" />


---

## Phase 3: The Index (Staging Area)

**Screenshot 3A:** `pes init` → `pes add` → `pes status` sequence.
<img width="913" height="609" alt="image" src="https://github.com/user-attachments/assets/32944afa-e8a7-45cf-9841-55a965030db6" />

**Screenshot 3B:** `cat .pes/index` showing the text-format index.
<img width="1051" height="122" alt="image" src="https://github.com/user-attachments/assets/7b1f0354-216f-4526-a4e9-f37306cb243d" />

---

## Phase 4: Commits and History

**Screenshot 4A:** `pes log` output with three commits.
<img width="835" height="587" alt="image" src="https://github.com/user-attachments/assets/820c9e56-a64b-4a60-9044-02af132e301b" />


**Screenshot 4B:** `find .pes -type f | sort` showing object growth.
<img width="902" height="338" alt="image" src="https://github.com/user-attachments/assets/a9ef1014-d3f0-4304-af1c-9b4e05f16828" />


**Screenshot 4C:** `cat .pes/refs/heads/main` and `cat .pes/HEAD`.
<img width="802" height="89" alt="image" src="https://github.com/user-attachments/assets/b0794b51-a72f-4f65-836f-0d22a58a613b" />

**Final Integration Test:** Full integration test (`make test-integration`).
<img width="300" height="659" alt="image" src="https://github.com/user-attachments/assets/86dedf59-5338-40f0-996a-691fcc8f5e4b" />


---

## Phase 5: Branching and Checkout Analysis

**Q5.1: A branch in Git is just a file in `.git/refs/heads/` containing a commit hash. Creating a branch is creating a file. Given this, how would you implement `pes checkout <branch>` — what files need to change in `.pes/`, and what must happen to the working directory? What makes this operation complex?**

To implement `checkout`, the system must first update the `.pes/HEAD` file to point to the new branch reference (e.g., `ref: refs/heads/<branch>`). Then, it reads the commit hash from that branch file, locates the commit object, and retrieves its root tree hash. 

The system must then recursively traverse this tree and update the working directory to match the exact snapshot represented by the tree. This involves creating new files, modifying existing ones, and deleting files that exist in the working directory but not in the target commit. The operation becomes highly complex because the system must ensure it does not accidentally overwrite or delete uncommitted local changes the user has made, requiring careful comparison between the working directory, the index, and the target tree before proceeding.

**Q5.2: When switching branches, the working directory must be updated to match the target branch's tree. If the user has uncommitted changes to a tracked file, and that file differs between branches, checkout must refuse. Describe how you would detect this "dirty working directory" conflict using only the index and the object store.**

To detect a dirty working directory conflict, the system would iterate through the entries in the `.pes/index`. For each file, it would use the `stat()` system call to check the file's current modification time and size against the metadata stored in the index. If they differ, the system knows the working directory file has been modified.

Next, the system would compute the SHA-256 hash of the modified file in the working directory. It would then traverse the object store to find the blob hash for that exact same file path in the *target* branch's tree. If the newly computed hash of the uncommitted file differs from the hash stored in the target branch's tree, a conflict exists. The system must abort the checkout to prevent the user's uncommitted changes from being overwritten by the target branch's data.

**Q5.3: "Detached HEAD" means HEAD contains a commit hash directly instead of a branch reference. What happens if you make commits in this state? How could a user recover those commits?**

When making commits in a "Detached HEAD" state, new commit objects are successfully created in the object store, and `.pes/HEAD` is updated with the new commit hash. However, because `HEAD` is not pointing to a branch file (like `refs/heads/main`), no branch pointer moves forward to track this new history. If the user subsequently checks out a different branch, the commits made in the detached state become "orphaned" or unreachable, as no named reference points to them.

To recover these commits, the user needs to find the hash of the last orphaned commit. Once the hash is identified, the user can recover the work by creating a new branch that points directly to it (e.g., creating a file `.pes/refs/heads/recovered-branch` containing the orphaned hash). 

---

## Phase 6: Garbage Collection and Space Reclamation

**Q6.1: Over time, the object store accumulates unreachable objects — blobs, trees, or commits that no branch points to (directly or transitively). Describe an algorithm to find and delete these objects. What data structure would you use to track "reachable" hashes efficiently? For a repository with 100,000 commits and 50 branches, estimate how many objects you'd need to visit.**

To clean up unreachable objects, a **Mark-and-Sweep** algorithm is utilized:
1. **Mark Phase:** The system starts at the "roots"—all branch files in `.pes/refs/heads/`, the `.pes/HEAD` file, and the staging `.pes/index`. It reads the commit hashes and adds them to a tracking structure. It recursively traverses every parent commit, and for every commit, it traverses its root tree, marking all sub-trees and blob hashes it encounters as "reachable."
2. **Sweep Phase:** The system scans the entire `.pes/objects/` directory. If an object's filename (its hash) is not found in the tracking structure, it is deemed unreachable garbage and is deleted from the disk.

The most efficient data structure to track "reachable" hashes is a **Hash Set** (or a Bloom Filter combined with a Hash Set for extremely large repositories), as it provides $O(1)$ average time complexity for lookups.

For a repository with 100,000 commits and 50 branches, the system must visit the objects referenced across the entire history of those commits. Since trees and blobs are deduplicated, the exact number depends on file churn. However, assuming an average of 10 changed files (blobs + trees) per commit, the algorithm would need to visit and mark well over **1,000,000 unique objects** during the traversal.

**Q6.2: Why is it dangerous to run garbage collection concurrently with a commit operation? Describe a race condition where GC could delete an object that a concurrent commit is about to reference. How does Git's real GC avoid this?**

Running garbage collection concurrently with repository modifications introduces a severe race condition. For example, if a developer runs `pes add new_file.c`, the system hashes the file and writes the new blob object to `.pes/objects/`. However, if the GC's "Mark" phase has already finished scanning the index just milliseconds *before* this file was staged, the GC will not mark the newly created blob as reachable. When the GC proceeds to the "Sweep" phase, it will find the new, unmarked blob and permanently delete it, destroying the user's work before they even get a chance to run `pes commit`.

Real Git avoids this race condition by implementing a **grace period**. During the sweep phase, Git checks the filesystem modification time (`mtime`) of the object file. It will only delete an unmarked object if it is older than a specific timeframe (e.g., 2 weeks by default). This ensures that newly created, but not yet linked, objects are completely safe from accidental deletion while concurrent operations finish.
