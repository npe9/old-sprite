/* 
 * lfsDesc.c --
 *
 *	File descriptor management routines for LFS. The routine in 
 *	this module provide the interface for file descriptors I/O 
 *      and allocation for a LFS file system.   The implementation uses
 *	the file system block cache to cache groups of descriptors and
 *	provide write buffering. 
 *
 * Copyright 1989 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif /* not lint */

#include "sprite.h"
#include "lfs.h"
#include "lfsInt.h"
#include "lfsDesc.h"
#include "lfsDescMap.h"
#include "fs.h"
#include "fsdm.h"


/*
 * @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
 * Start of routines exported to higher levels of the file system.
 * @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
 */


/*
 *----------------------------------------------------------------------
 *
 * Lfs_GetNewFileNumber --
 *
 *	Allocate an used file number for a newly created file or directory.
 *
 * Results:
 *	An error if could not find a free file descriptor.
 *
 * Side effects:
 *	fileNumberPtr is set to the number of the file descriptor allocated
 *	and the descriptor map entry *fileNumberPtr is mark as allcoated.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Lfs_GetNewFileNumber(domainPtr, dirFileNum, fileNumberPtr)
    Fsdm_Domain 	*domainPtr;	/* Domain to allocate the file 
					 * descriptor out of. */
    int			dirFileNum;	/* File number of the directory that
					   the file is in.  -1 means that
					   this file descriptor is being
					   allocated for a directory. */
    int			*fileNumberPtr; /* Place to return the number of
					   the file descriptor allocated. */

{
    Lfs	*lfsPtr = LfsFromDomainPtr(domainPtr);
    return LfsDescMapAllocFileNum(lfsPtr, dirFileNum, fileNumberPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Lfs_FreeFileNumber() --
 *
 *	Mark a file number as unused and make it available for re-allocation.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Descriptor map entry is modified for the file.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Lfs_FreeFileNumber(domainPtr, fileNumber)
     Fsdm_Domain 	*domainPtr;	/* Domain of the file descriptor. */
     int		fileNumber; 	/* Number of file descriptor to free.*/
{
     Lfs	*lfsPtr = LfsFromDomainPtr(domainPtr);
    return LfsDescMapFreeFileNum(lfsPtr, fileNumber);

}

/*
 *----------------------------------------------------------------------
 *
 * Lfs_FileDescFetch() --
 *
 *	Fetch the specified file descriptor from the file system and 
 *	store it in *fileDescPtr.
 *
 * Results:
 *	An error if could not read the file descriptor from disk or is not
 *	allocated.
 *
 * Side effects:
 *	A block of descriptors may be read into the cache.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Lfs_FileDescFetch(domainPtr, fileNumber, fileDescPtr)
     Fsdm_Domain 	*domainPtr;	/* Domain to fetch file descriptor. */
     int		fileNumber; 	/* Number of file descriptor to fetch.*/
     Fsdm_FileDescriptor *fileDescPtr;	/* File descriptor structure to fetch.*/
{
    Lfs	*lfsPtr = LfsFromDomainPtr(domainPtr);
    LfsFileDescriptor	*descPtr;
    Fscache_Block	    *blockPtr;
    ReturnStatus	status;
    unsigned int diskAddr;
    int		i;
    Boolean	found;

    status = LfsDescMapGetDiskAddr(lfsPtr, fileNumber, &diskAddr);
    if (status != SUCCESS) {
	return status;
    }
    /*
     * See if the value is in the descriptor block cache. If it is not 
     * then read it in.
     */
    Fscache_FetchBlock(&lfsPtr->descCacheHandle.cacheInfo, diskAddr, 
		      FSCACHE_DESC_BLOCK, &blockPtr, &found);
    if (!found) {
	status = LfsReadBytes(lfsPtr, diskAddr, 
		lfsPtr->fileLayout.params.descPerBlock * sizeof(*descPtr),
		blockPtr->blockAddr);
	if (status != SUCCESS) {
	    printf( "Could not read in file descriptor\n");
	    Fscache_UnlockBlock(blockPtr, 0, -1, 0, FSCACHE_DELETE_BLOCK);
	    return status;
	}
    }
    descPtr = (LfsFileDescriptor *) (blockPtr->blockAddr);
    for (i = 0; i < lfsPtr->fileLayout.params.descPerBlock; i++) {
	if (!(descPtr->common.flags & FSDM_FD_ALLOC)) {
	    break;
	}
	if (descPtr->fileNumber == fileNumber) {
	    bcopy((char *) &(descPtr->common), (char *)fileDescPtr,
			sizeof(descPtr->common));
	     status = LfsDescMapGetAccessTime(lfsPtr, fileNumber,
			    &(fileDescPtr->accessTime));
	     if (status != SUCCESS) {
		  LfsError(lfsPtr, status, "Can't get access time.\n");
	     }
	    Fscache_UnlockBlock(blockPtr, 0, -1, FS_BLOCK_SIZE, 0);
	    return status;
	}
	descPtr++;
    }
    Fscache_UnlockBlock(blockPtr, 0, -1, 0, FSCACHE_DELETE_BLOCK);
    panic("Descriptor map foulup, can't find file %d at %d\n", fileNumber,
			diskAddr);
    return FAILURE;

}


/*
 *----------------------------------------------------------------------
 *
 * Lfs_FileDescStore() --
 *
 *
 * Results:
 *	An error if could not read the file descriptor from disk or is not
 *	allocated.
 *
 * Side effects:
 *	A block of descriptors may be read into the cache.
 *
 *----------------------------------------------------------------------
 */

Boolean
Lfs_FileDescStore(handlePtr)
    Fsio_FileIOHandle	*handlePtr;
{
     Fsdm_Domain 	*domainPtr;	/* Domain to store file descriptor. */
     int		fileNumber; 	/* Number of file descriptor to store.*/
     Fsdm_FileDescriptor *fileDescPtr;	/* File descriptor structure to store.*/
     ReturnStatus	status;
     Lfs		*lfsPtr;

    domainPtr = Fsdm_DomainFetch(handlePtr->hdr.fileID.major, FALSE);
    if (domainPtr == (Fsdm_Domain *)NIL) {
        return TRUE;
    }
    lfsPtr = LfsFromDomainPtr(domainPtr);
    fileDescPtr = handlePtr->descPtr;
    if (fileDescPtr->flags & FSDM_FD_ACCESSTIME_DIRTY) {
	 status = LfsDescMapSetAccessTime(lfsPtr, handlePtr->hdr.fileID.minor, 
			    fileDescPtr->accessTime);
	  if (status != SUCCESS) {
		  LfsError(lfsPtr, status, "Can't update descriptor map.\n");
          }
	  fileDescPtr->flags &= ~FSDM_FD_ACCESSTIME_DIRTY;
    }
    Fsdm_DomainRelease(handlePtr->hdr.fileID.major);
    return ((fileDescPtr->flags & FSDM_FD_DIRTY) != 0);
}

/*
 *----------------------------------------------------------------------
 *
 * Lfs_FileDescTrunc --
 *
 *      Shorten a file to length bytes.  This updates the descriptor
 *      and may free blocks and indirect blocks from the end of the file.
 *
 * Results:
 *      Error if had problem with indirect blocks, otherwise SUCCESS.
 *
 * Side effects:
 *      May modify the truncateVersion number.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Lfs_FileDescTrunc(handlePtr, size, delete)
    Fsio_FileIOHandle   *handlePtr;     /* File to truncate. */
    int                 size;           /* Size to truncate the file to. */
    Boolean		delete;
{
    Fsdm_FileDescriptor	*descPtr;
    Fsdm_Domain		*domainPtr;
    ReturnStatus	status = SUCCESS;
    Lfs		    	*lfsPtr;
    int			newLastByte;
    int		blocks;

    if (size < 0) {
	return(GEN_INVALID_ARG);
    }

    domainPtr = Fsdm_DomainFetch(handlePtr->hdr.fileID.major, TRUE);
    if (domainPtr == (Fsdm_Domain *)NIL) {
	return(FS_DOMAIN_UNAVAILABLE);
    }
    descPtr = handlePtr->descPtr;
    lfsPtr = LfsFromDomainPtr(domainPtr);

    newLastByte = size - 1;
    if (descPtr->lastByte <= newLastByte) {
	status = SUCCESS;
	goto exit;
    }

    blocks = (size + (FS_BLOCK_SIZE-1))/FS_BLOCK_SIZE;

    status = LfsFile_TruncIndex(lfsPtr, handlePtr, blocks);
    if (status == SUCCESS) {
	if (size == 0) {
	    int	newVersion;
	    status = LfsDescMapIncVersion(lfsPtr, 
			handlePtr->hdr.fileID.minor, &newVersion);
	}
	descPtr->lastByte = newLastByte;
	descPtr->descModifyTime = fsutil_TimeInSeconds;
	descPtr->flags |= FSDM_FD_SIZE_DIRTY;
    }
exit:
    if (delete) { 
	descPtr->flags &= ~FSDM_FD_DIRTY;
	status = Fscache_InvalidateDesc(handlePtr);
    } else {
	if (descPtr->flags & FSDM_FD_DIRTY) {
	    status = Fscache_FileDescStore(handlePtr);
	}
    }
    Fsdm_DomainRelease(handlePtr->hdr.fileID.major);
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Lfs_FileDescInit() --
 *
 *	Initialize a new file descriptor.
 *
 * Results:
 *	SUCCESS
 *
 * Side effects:
 *	The file decriptor is initialized.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Lfs_FileDescInit(domainPtr, fileNumber, type, permissions, uid, gid, fileDescPtr)
    Fsdm_Domain 	*domainPtr;	/* Domain of the file */
    int			fileNumber; 	/* Number of file descriptor */
    int			type;		/* Type of the file */
    int			permissions;	/* Permission bits for the file */
    int			uid;		/* Owner ID for the file */
    int			gid;		/* Group ID for the file */
    Fsdm_FileDescriptor	*fileDescPtr;	/* File descriptor structure to
					   initialize. */
{
    ReturnStatus status;
    int index;

    fileDescPtr->magic = FSDM_FD_MAGIC;
    fileDescPtr->flags = FSDM_FD_ALLOC|FSDM_FD_DIRTY;
    fileDescPtr->fileType = type;
    fileDescPtr->permissions = permissions;
    fileDescPtr->uid = uid;
    fileDescPtr->gid = gid;
    fileDescPtr->lastByte = -1;
    fileDescPtr->firstByte = -1;
    fileDescPtr->userType = FS_USER_TYPE_UNDEFINED;
    fileDescPtr->numLinks = 1;
    fileDescPtr->numKbytes = 0;
    /*
     * Give this new file a new version number.  The increment is by 2 to
     * ensure that a client invalidates any cache blocks associated with
     * the previous incarnation of the file.  Remember that when a client
     * opens for writing a version number 1 greater means that its old
     * cache blocks are still ok, and also remember that clients with
     * clean blocks are not told when a file is deleted.
     */
    fileDescPtr->version = LfsGetCurrentTimestamp(LfsFromDomainPtr(domainPtr));

    /*
     * Clear out device info.  It is set up properly by the make-device routine.
     */
    fileDescPtr->devServerID = -1;
    fileDescPtr->devType = -1;
    fileDescPtr->devUnit = -1;

    /*
     * Set the time stamps.  These times should come from the client.
     */
    fileDescPtr->createTime = fsutil_TimeInSeconds;
    fileDescPtr->accessTime = fsutil_TimeInSeconds;
    fileDescPtr->descModifyTime = fsutil_TimeInSeconds;
    fileDescPtr->dataModifyTime = fsutil_TimeInSeconds;

    for (index = 0; index < FSDM_NUM_DIRECT_BLOCKS ; index++) {
	fileDescPtr->direct[index] = FSDM_NIL_INDEX;
    }
    for (index = 0; index < FSDM_NUM_INDIRECT_BLOCKS ; index++) {
	fileDescPtr->indirect[index] = FSDM_NIL_INDEX;
    }
    return(SUCCESS);
}

/*
 * @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
 * End of routines exported to higher levels of the file system.
 * @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
 *
 * Start of LFS private routines. 
 */
void
LfsDescCacheInit(lfsPtr)
    Lfs		*lfsPtr;
{
    Fscache_Attributes		attr;
    /*
     * Initialize the file handle used to cache descriptor blocks.
     */

    bzero((char *)(&lfsPtr->descCacheHandle), sizeof(lfsPtr->descCacheHandle));
    lfsPtr->descCacheHandle.hdr.fileID.major = lfsPtr->domainPtr->domainNumber;
    lfsPtr->descCacheHandle.hdr.fileID.minor = 0;
    lfsPtr->descCacheHandle.hdr.fileID.type = FSIO_LCL_FILE_STREAM;
    lfsPtr->descCacheHandle.descPtr = (Fsdm_FileDescriptor *)NIL;


    bzero((Address)&attr, sizeof(attr));
    attr.lastByte = 0x7fffffff;
    Fscache_InfoInit(&lfsPtr->descCacheHandle.cacheInfo,
		    (Fs_HandleHeader *) &lfsPtr->descCacheHandle,
		    0, TRUE, &attr, (Fscache_IOProcs *) NIL);

}
