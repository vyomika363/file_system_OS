#include <string.h>
#include <stdlib.h>
#include "simplefs-ops.h"

extern struct filehandle_t file_handle_array[MAX_OPEN_FILES];

int simplefs_create(char *filename) {
    if (filename == NULL || strlen(filename) == 0) {
        printf("Error: Invalid filename\n");
        return -1;
    }

    struct inode_t inode;

    // --- Check for duplicates ---
    for (int i = 0; i < NUM_INODES; i++) {
        simplefs_readInode(i, &inode);

        if (inode.status == INODE_IN_USE && strlen(inode.name) > 0) {
            if (strcmp(inode.name, filename) == 0) {
                printf("Error: File '%s' already exists (inode %d)\n", filename, i);
                return -1;
            }
        }
    }

    // --- Allocate free inode ---
    int inum = simplefs_allocInode();
    if (inum < 0) {
        printf("Error: No free inode available\n");
        return -1;
    }

    // --- Initialize new inode ---
    memset(&inode, 0, sizeof(struct inode_t));
    inode.status = INODE_IN_USE;
    strncpy(inode.name, filename, MAX_NAME_STRLEN - 1);
    inode.file_size = 0;
    for (int k = 0; k < MAX_FILE_SIZE; k++)
        inode.direct_blocks[k] = -1;

    simplefs_writeInode(inum, &inode);
    printf("File '%s' created at inode %d\n", filename, inum);
    return inum;
}



void simplefs_delete(char *filename) {
    struct inode_t inode;
    // find the inode by name
    for (int i = 0; i < NUM_INODES; i++) {
        simplefs_readInode(i, &inode);
        if (inode.status == INODE_IN_USE && strcmp(inode.name, filename) == 0) {
            // free all data blocks used by this file
            for (int b = 0; b < MAX_FILE_SIZE; b++) {
                if (inode.direct_blocks[b] != -1) {
                    simplefs_freeDataBlock(inode.direct_blocks[b]);
                    inode.direct_blocks[b] = -1;
                }
            }
            // Free the inode itself (also writes inode/free list updates)
            simplefs_freeInode(i);
            return;
        }
    }
    // if not found, do nothing
}

int simplefs_open(char *filename) {
    struct inode_t inode;
    int inum = -1;
    for (int i = 0; i < NUM_INODES; i++) {
        simplefs_readInode(i, &inode);
        if (inode.status == INODE_IN_USE && strcmp(inode.name, filename) == 0) {
            inum = i;
            break;
        }
    }
    if (inum < 0) return -1;

    // find a free file handle slot
    for (int fh = 0; fh < MAX_OPEN_FILES; fh++) {
        if (file_handle_array[fh].inode_number == -1) {
            file_handle_array[fh].inode_number = inum;
            file_handle_array[fh].offset = 0;
            return fh;
        }
    }
    return -1; // no free handle
}

void simplefs_close(int filehandle) {
    if (filehandle < 0 || filehandle >= MAX_OPEN_FILES) return;
    file_handle_array[filehandle].inode_number = -1;
    file_handle_array[filehandle].offset = 0;
}

int simplefs_read(int filehandle, char *buf, int nbytes) {
    if (filehandle < 0 || filehandle >= MAX_OPEN_FILES) return -1;
    struct filehandle_t *fh = &file_handle_array[filehandle];
    if (fh->inode_number == -1) return -1;

    struct inode_t inode;
    simplefs_readInode(fh->inode_number, &inode);
    int offset = fh->offset;

    // Check bounds: cannot read past EOF
    if (offset + nbytes > inode.file_size) return -1;

    int bytesToRead = nbytes;
    int currOffset = offset;
    int copied = 0;
    char blockBuf[BLOCKSIZE];

    while (bytesToRead > 0) {
        int blockIndex = currOffset / BLOCKSIZE;
        int blockOffset = currOffset % BLOCKSIZE;

        // Safety checks
        if (blockIndex < 0 || blockIndex >= MAX_FILE_SIZE) return -1;
        int dataBlockNum = inode.direct_blocks[blockIndex];
        if (dataBlockNum == -1) return -1; // trying to read an unallocated block -> fail

        simplefs_readDataBlock(dataBlockNum, blockBuf);

        int chunk = BLOCKSIZE - blockOffset;
        if (chunk > bytesToRead) chunk = bytesToRead;
        memcpy(buf + copied, blockBuf + blockOffset, chunk);

        bytesToRead -= chunk;
        copied += chunk;
        currOffset += chunk;
    }
    return 0;
}

int simplefs_write(int filehandle, char *buf, int nbytes) {
    if (filehandle < 0 || filehandle >= MAX_OPEN_FILES) return -1;
    struct filehandle_t *fh = &file_handle_array[filehandle];
    if (fh->inode_number == -1) return -1;

    struct inode_t inode;
    simplefs_readInode(fh->inode_number, &inode);
    int offset = fh->offset;

    // Check size limit
    if (offset + nbytes > MAX_FILE_SIZE * BLOCKSIZE) return -1;

    int startBlock = offset / BLOCKSIZE;
    int endBlock = (offset + nbytes - 1) / BLOCKSIZE;

    // Range checks
    if (startBlock < 0 || endBlock >= MAX_FILE_SIZE) return -1;

    // Allocate any needed blocks and track which inode entries we set
    int allocatedBlocks[MAX_FILE_SIZE];
    int allocatedCount = 0;
    int modifiedIndices[MAX_FILE_SIZE];
    int modifiedCount = 0;

    for (int b = startBlock; b <= endBlock; b++) {
        if (inode.direct_blocks[b] == -1) {
            int newBlock = simplefs_allocDataBlock();
            if (newBlock < 0) {
                // Allocation failed, rollback: free allocated blocks and restore inode entries
                for (int k = 0; k < allocatedCount; k++) {
                    simplefs_freeDataBlock(allocatedBlocks[k]);
                }
                for (int k = 0; k < modifiedCount; k++) {
                    int idx = modifiedIndices[k];
                    inode.direct_blocks[idx] = -1;
                }
                return -1;
            }
            inode.direct_blocks[b] = newBlock;
            allocatedBlocks[allocatedCount++] = newBlock;
            modifiedIndices[modifiedCount++] = b;
        }
    }

    // perform the actual write
    int bytesToWrite = nbytes;
    int currOffset = offset;
    int bufPos = 0;
    char blockBuf[BLOCKSIZE];

    while (bytesToWrite > 0) {
        int blockIndex = currOffset / BLOCKSIZE;
        int blockOffset = currOffset % BLOCKSIZE;
        int writeBlock = inode.direct_blocks[blockIndex];

        // If writeBlock should always be valid here (we allocated above if needed)
        if (writeBlock == -1) {
            // Unexpected: treat as failure and rollback (free blocks we allocated in this write)
            for (int k = 0; k < allocatedCount; k++) simplefs_freeDataBlock(allocatedBlocks[k]);
            for (int k = 0; k < modifiedCount; k++) {
                int idx = modifiedIndices[k];
                inode.direct_blocks[idx] = -1;
            }
            return -1;
        }

        // If this block was newly allocated above, clear it; otherwise read existing content
        int isNewBlock = 0;
        for (int k = 0; k < allocatedCount; k++) {
            if (allocatedBlocks[k] == writeBlock) { isNewBlock = 1; break; }
        }
        if (isNewBlock) {
            memset(blockBuf, 0, BLOCKSIZE);
        } else {
            simplefs_readDataBlock(writeBlock, blockBuf);
        }

        int chunk = BLOCKSIZE - blockOffset;
        if (chunk > bytesToWrite) chunk = bytesToWrite;
        memcpy(blockBuf + blockOffset, buf + bufPos, chunk);
        simplefs_writeDataBlock(writeBlock, blockBuf);

        bytesToWrite -= chunk;
        currOffset += chunk;
        bufPos += chunk;
    }

    // Update file size if extended
    if (offset + nbytes > inode.file_size) inode.file_size = offset + nbytes;
    simplefs_writeInode(fh->inode_number, &inode);
    return 0;
}

int simplefs_seek(int filehandle, int nseek) {
    if (filehandle < 0 || filehandle >= MAX_OPEN_FILES) return -1;
    struct filehandle_t *fh = &file_handle_array[filehandle];
    if (fh->inode_number == -1) return -1;

    struct inode_t inode;
    simplefs_readInode(fh->inode_number, &inode);

    int newOffset = fh->offset + nseek;
    if (newOffset < 0 || newOffset > inode.file_size) return -1;

    fh->offset = newOffset;
    return 0;
}
