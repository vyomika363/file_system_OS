#include <string.h>
#include <stdlib.h>
#include "simplefs-ops.h"
#include "simplefs-disk.h"

extern struct filehandle_t file_handle_array[MAX_OPEN_FILES];

int simplefs_create(char *filename) {
    //Check if filename already exists
    struct inode_t inode;
    for(int i = 0; i < NUM_INODES; i++) {
        simplefs_readInode(i, &inode);
        if (inode.status == INODE_IN_USE && strcmp(inode.name, filename) == 0) {
            //if file already exists, then fail
            return -1;
        }
    }
    //Allocate a free inode
    int inum = simplefs_allocInode();
    if (inum < 0) {
        return -1;  //if no free inode
    }
    //Initialize inode fields
    inode.status = INODE_IN_USE;
    strncpy(inode.name, filename, MAX_NAME_STRLEN);
    inode.name[MAX_NAME_STRLEN-1] = '\0';
    inode.file_size = 0;
    for(int k = 0; k < MAX_FILE_SIZE; k++) {
        inode.direct_blocks[k] = -1;
    }
    //Write the inode to disk
    simplefs_writeInode(inum, &inode);
    return inum;
}

void simplefs_delete(char *filename) {
    struct inode_t inode;
    //find the inode by name
    for(int i = 0; i < NUM_INODES; i++) {
        simplefs_readInode(i, &inode);
        if (inode.status == INODE_IN_USE && strcmp(inode.name, filename) == 0) {
            //free all data blocks used by this file
            for(int b = 0; b < MAX_FILE_SIZE; b++) {
                if (inode.direct_blocks[b] != -1) {
                    simplefs_freeDataBlock(inode.direct_blocks[b]);
                }
            }
            // Free the inode itself
            simplefs_freeInode(i);
            return;
        }
    }
    //If not found then nothing is done
}

int simplefs_open(char *filename) {
    struct inode_t inode;
    // Find the inode number for this filename
    int inum = -1;
    for(int i = 0; i < NUM_INODES; i++) {
        simplefs_readInode(i, &inode);
        if (inode.status == INODE_IN_USE && strcmp(inode.name, filename) == 0) {
            inum = i;
            break;
        }
    }
    if (inum < 0) {
        return -1;  // file not found
    }
    // Find a free file handle slot
    for(int fh = 0; fh < MAX_OPEN_FILES; fh++) {
        if (file_handle_array[fh].inode_number == -1) {
            // Initialize this handle
            file_handle_array[fh].inode_number = inum;
            file_handle_array[fh].offset = 0;
            return fh;
        }
    }
    return -1;  // no free handle
}

void simplefs_close(int filehandle) {
    if (filehandle < 0 || filehandle >= MAX_OPEN_FILES) return;
    if (file_handle_array[filehandle].inode_number != -1) {
        file_handle_array[filehandle].inode_number = -1;
        file_handle_array[filehandle].offset = 0;
    }
}

int simplefs_read(int filehandle, char *buf, int nbytes) {
    if (filehandle < 0 || filehandle >= MAX_OPEN_FILES) return -1;
    struct filehandle_t *fh = &file_handle_array[filehandle];
    if (fh->inode_number == -1) return -1;  // not open

    struct inode_t inode;
    simplefs_readInode(fh->inode_number, &inode);
    int offset = fh->offset;
    // Check bounds: cannot read past EOF
    if (offset + nbytes > inode.file_size) {
        return -1;
    }

    int bytesToRead = nbytes;
    int currOffset = offset;
    int copied = 0;
    char blockBuf[BLOCKSIZE];

    while (bytesToRead > 0) {
        int blockIndex = currOffset / BLOCKSIZE;
        int blockOffset = currOffset % BLOCKSIZE;
        simplefs_readDataBlock(inode.direct_blocks[blockIndex], blockBuf);

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

    // Check size limit (max 4*64=256 bytes)
    if (offset + nbytes > MAX_FILE_SIZE * BLOCKSIZE) {
        return -1;
    }
    int startBlock = offset / BLOCKSIZE;
    int endBlock = (offset + nbytes - 1) / BLOCKSIZE;
    int blocksNeeded = endBlock - startBlock + 1;

    // Allocate any needed blocks
    int allocatedBlocks[MAX_FILE_SIZE];
    int allocCount = 0;
    for (int b = startBlock; b <= endBlock; b++) {
        if (inode.direct_blocks[b] == -1) {
            int newBlock = simplefs_allocDataBlock();
            if (newBlock < 0) {
                // Allocation failed, rollback any done
                for (int k = 0; k < allocCount; k++) {
                    simplefs_freeDataBlock(allocatedBlocks[k]);
                }
                return -1;
            }
            inode.direct_blocks[b] = newBlock;
            allocatedBlocks[allocCount++] = newBlock;
        }
    }

    //performing the actual write
    int bytesToWrite = nbytes;
    int currOffset = offset;
    int bufPos = 0;
    char blockBuf[BLOCKSIZE];

    while (bytesToWrite > 0) {
        int blockIndex = currOffset / BLOCKSIZE;
        int blockOffset = currOffset % BLOCKSIZE;
        int writeBlock = inode.direct_blocks[blockIndex];

        //reading existing data if not newly allocated (we'll check if allocation happened)
        int isNewBlock = 0;
        for (int k = 0; k < allocCount; k++) {
            if (writeBlock == allocatedBlocks[k]) { isNewBlock = 1; break; }
        }
        if (!isNewBlock) {
            simplefs_readDataBlock(writeBlock, blockBuf);
        } else {
            // Newly allocated block: clear to zeros
            memset(blockBuf, 0, BLOCKSIZE);
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
    if (offset + nbytes > inode.file_size) {
        inode.file_size = offset + nbytes;
    }
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
    if (newOffset < 0 || newOffset > inode.file_size) {
        return -1;
    }
    fh->offset = newOffset;
    return 0;
}