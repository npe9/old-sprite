/*
 * fsMigrate.c --
 *
 * Procedures to handle migrating open files between machines.  The basic
 * strategy is to first do some local book-keeping on the client we are
 * leaving, then ship state to the new client, then finally tell the
 * I/O server about it, and finish up with local book-keeping on the
 * new client.  There are three stream-type procedures used: 'migStart'
 * does the initial book-keeping on the original client, 'migEnd' does
 * the final book-keeping on the new client, and 'migrate' is called
 * on the I/O server to shift around state associated with the client.
 *
 * Copyright (C) 1985, 1988, 1989 Regents of the University of California
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
#endif not lint


#include "sprite.h"
#include "fs.h"
#include "fsInt.h"
#include "fsMigrate.h"
#include "fsStream.h"
#include "fsClient.h"
#include "fsPdev.h"
#include "fsFile.h"
#include "fsDevice.h"
#include "fsPrefix.h"
#include "fsOpTable.h"
#include "fsDebug.h"
#include "fsNameOpsInt.h"
#include "byte.h"
#include "rpc.h"
#include "procMigrate.h"

Boolean fsMigDebug = FALSE;
#define DEBUG( format ) \
	if (fsMigDebug) { printf format ; }


/*
 * ----------------------------------------------------------------------------
 *
 * Fs_EncapStream --
 *
 *	Package up a stream's state for migration to another host.  This
 *	copies the stream's offset, streamID, ioFileID, nameFileID, and flags.
 *	This routine is side-effect free with respect to both
 *	the stream and the I/O handles.  The bookkeeping is done later
 *	during Fs_DeencapStream so proper syncronization with Fs_Close
 *	bookkeeping can be done.
 *	It is reasonable to call Fs_DeencapStream again on this host,
 *	for example, to back out an aborted migration.
 *
 * Results:
 *	This always returns SUCCESS.
 *	
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 *
 */

ReturnStatus
Fs_EncapStream(streamPtr, bufPtr)
    Fs_Stream	*streamPtr;	/* Stream to be migrated */
    Address	bufPtr;		/* Buffer to hold encapsulated stream */
{
    register	FsMigInfo	*migInfoPtr;
    register FsHandleHeader	*ioHandlePtr;

    /*
     * Synchronize with stream duplication and closes
     */
    FsHandleLock(streamPtr);

    /*
     * The encapsulated stream state includes the read/write offset,
     * the I/O server, the useFlags of the stream, and our SpriteID so
     * the target of migration and the server can do the right thing later. 
     */
    migInfoPtr = (FsMigInfo *) bufPtr;
    ioHandlePtr = streamPtr->ioHandlePtr;
    migInfoPtr->streamID = streamPtr->hdr.fileID;
    migInfoPtr->ioFileID = ioHandlePtr->fileID;
    if (streamPtr->nameInfoPtr == (FsNameInfo *)NIL) {
	/*
	 * Anonymous pipes have no name information.
	 */
	migInfoPtr->nameID.type = -1;
    } else {
	migInfoPtr->nameID = streamPtr->nameInfoPtr->fileID;
	migInfoPtr->rootID = streamPtr->nameInfoPtr->rootID;
    }
    migInfoPtr->offset = streamPtr->offset;
    migInfoPtr->srcClientID = rpc_SpriteID;
    migInfoPtr->flags = streamPtr->flags;

    FsHandleUnlock(streamPtr);
    return(SUCCESS);
}

/*
 * ----------------------------------------------------------------------------
 *
 * Fs_DeencapStream --
 *
 *	Deencapsulate the stream that was packaged up on another machine
 *	and recreate the stream on this machine.  This uses two stream-type
 *	routines to complete the setup of the stream.  First, the
 *	migrate routine is called to shift client references on the
 *	server.  Then the migEnd routine is called to do local book-keeping.
 *
 * Results:
 *	A return status, plus *streamPtrPtr is set to the new stream.
 *
 * Side effects:
 *	Ensures that the stream exists on this host, along with the
 *	associated I/O handle.  This calls a stream-type specific routine
 *	to shuffle reference counts and detect cross-machine stream
 *	sharing.  If a stream is shared by proceses on different machines
 *	its flags field is marked with FS_RMT_SHARED.  This also calls
 *	a stream-type specific routine to create the I/O handle when the
 *	first reference to a stream migrates to this host.
 *
 * ----------------------------------------------------------------------------
 *
 */

ReturnStatus
Fs_DeencapStream(bufPtr, streamPtrPtr)
    Address	bufPtr;		/* Encapsulated stream information. */
    Fs_Stream	**streamPtrPtr;	/* Where to return pointer to the new stream */
{
    register	Fs_Stream	*streamPtr;
    register	FsMigInfo	*migInfoPtr;
    register	FsNameInfo	*nameInfoPtr;
    ReturnStatus		status = SUCCESS;
    Boolean			foundClient;
    Boolean			foundStream;
    int				size;
    ClientData			data;

    migInfoPtr = (FsMigInfo *) bufPtr;

    if (migInfoPtr->srcClientID == rpc_SpriteID) {
	/*
	 * Migrating to ourselves.  Just fetch the stream.
	 */
	*streamPtrPtr = FsHandleFetchType(Fs_Stream, &migInfoPtr->streamID);
	if (*streamPtrPtr == (Fs_Stream *)NIL) {
	    return(FS_FILE_NOT_FOUND);
	} else {
	    return(SUCCESS);
	}
    }
    /*
     * Create a top-level stream and note if this is a new stream.  This is
     * important because extra things happen when the first reference to
     * a stream migrates to this host.  FS_NEW_STREAM is used to indicate this.
     * Note that the stream has (at least) one reference count and a client
     * list entry that will be cleaned up by a future call to Fs_Close.
     */
    streamPtr = FsStreamAddClient(&migInfoPtr->streamID, rpc_SpriteID,
			     (FsHandleHeader *)NIL,
			     migInfoPtr->flags & ~FS_NEW_STREAM, (char *)NIL,
			     &foundClient, &foundStream);
    if (!foundClient) {
	migInfoPtr->flags |= FS_NEW_STREAM;
	streamPtr->offset = migInfoPtr->offset;
	DEBUG( ("Deencap NEW stream %d, migOff %d, ",
		streamPtr->hdr.fileID.minor, migInfoPtr->offset) );
    } else {
	migInfoPtr->flags &= ~FS_NEW_STREAM;
	DEBUG( ("Deencap OLD stream %d, migOff %d, ",
		streamPtr->hdr.fileID.minor,
		migInfoPtr->offset, streamPtr->offset) );
    }
    if (streamPtr->nameInfoPtr == (FsNameInfo *)NIL) {
	if (migInfoPtr->nameID.type == -1) {
	    /*
	     * No name info to re-create.  This happens when anonymous
	     * pipes get migrated.
	     */
	    streamPtr->nameInfoPtr = (FsNameInfo *)NIL;
	} else {
	    /*
	     * Set up the nameInfo.  We sacrifice the name string as it is only
	     * used in error messages.  The fileID is used with get/set attr.
	     * If this file is the current directory then rootID is passed
	     * to the server to trap "..", domainType is used to index the
	     * name lookup operation switch, and prefixPtr is used for
	     * efficient handling of lookup redirections.
	     * Convert from remote to local file types, and vice-versa,
	     * as needed.
	     */
	    streamPtr->nameInfoPtr = nameInfoPtr = mnew(FsNameInfo);
	    nameInfoPtr->fileID = migInfoPtr->nameID;
	    nameInfoPtr->rootID = migInfoPtr->rootID;
	    if (nameInfoPtr->fileID.serverID != rpc_SpriteID) {
		nameInfoPtr->domainType = FS_REMOTE_SPRITE_DOMAIN;
		nameInfoPtr->fileID.type =
		    fsLclToRmtType[nameInfoPtr->fileID.type];
		nameInfoPtr->rootID.type =
		    fsLclToRmtType[nameInfoPtr->rootID.type];
	    } else {
		/*
		 * FIX HERE PROBABLY TO HANDLE PSEUDO_FILE_SYSTEMS.
		 */
		nameInfoPtr->domainType = FS_LOCAL_DOMAIN;
		nameInfoPtr->fileID.type =
		    fsRmtToLclType[nameInfoPtr->fileID.type];
		nameInfoPtr->rootID.type =
		    fsRmtToLclType[nameInfoPtr->rootID.type];
	    }
	    nameInfoPtr->prefixPtr = FsPrefixFromFileID(&migInfoPtr->rootID);
	    if (nameInfoPtr->prefixPtr == (struct FsPrefix *)NIL) {
		printf("Fs_DeencapStream: No prefix entry for <%d,%d,%d>\n",
		    migInfoPtr->rootID.serverID,
		    migInfoPtr->rootID.major, migInfoPtr->rootID.minor);
	    }
	}
    }
    /*
     * Contact the I/O server to tell it that the client moved.  The I/O
     * server checks for cross-network stream sharing and sets the
     * FS_RMT_SHARED flag if it is shared.  Note that we set FS_NEW_STREAM
     * in the migInfoPtr->flags, and this flag often gets rammed into
     * the streamPtr->flags, which we don't want because it would confuse
     * FsMigrateUseCounts on subsequent migrations.
     */
    FsHandleUnlock(streamPtr);
    status = (*fsStreamOpTable[migInfoPtr->ioFileID.type].migrate)
		(migInfoPtr, rpc_SpriteID, &streamPtr->flags,
		 &streamPtr->offset, &size, &data);
    streamPtr->flags &= ~FS_NEW_STREAM;

    DEBUG( (" Type %d <%d,%d> offset %d, ", migInfoPtr->ioFileID.type,
		migInfoPtr->ioFileID.major, migInfoPtr->ioFileID.minor,
		streamPtr->offset) );

    if (status == SUCCESS && !foundClient) {
	/*
	 * The stream is newly created on this host so we call down to
	 * the I/O handle level to ensure that the I/O handle exists and
	 * so the local object manager gets told about the new stream.
	 */
	migInfoPtr->flags = streamPtr->flags;
	status = (*fsStreamOpTable[migInfoPtr->ioFileID.type].migEnd)
		(migInfoPtr, size, data, &streamPtr->ioHandlePtr);
	DEBUG( ("migEnd status %x\n", status) );
    } else {
	DEBUG( ("migrate status %x\n", status) );
    }

    if (status == SUCCESS) {
	*streamPtrPtr = streamPtr;
    } else {
	FsHandleLock(streamPtr);
	if (!foundStream &&
	    FsStreamClientClose(&streamPtr->clientList, rpc_SpriteID)) {
	    FsStreamDispose(streamPtr);
	} else {
	    FsHandleRelease(streamPtr, TRUE);
	}
    }

    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsMigrateUseCounts --
 *
 *	This updates use counts to reflect any network sharing that
 *	is a result of migration.  The rule adhered to is that there
 *	are use counts kept on the I/O handle for each stream on each client
 *	that uses the I/O handle.  A stream with only one reference
 *	does not change use counts when it migrates, for example, because
 *	the reference just moves.  A stream with two references will
 *	cause a new client host to have a stream after migration, so the
 *	use counts are updated in case both clients do closes.  Finally,
 *	use counts get decremented when a stream completely leaves a
 *	client after being shared.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Adjusts the use counts to reflect sharing of the I/O handle
 *	due to migration.
 *
 * ----------------------------------------------------------------------------
 *
 */
ReturnStatus
FsMigrateUseCounts(flags, closeSrcClient, usePtr)
    register int	 flags;		/* Flags from the stream */
    Boolean		closeSrcClient;	/* TRUE if I/O close was done at src */
    register FsUseCounts *usePtr;	/* Use counts from the I/O handle */
{
    if ((flags & FS_NEW_STREAM) && !closeSrcClient) {
	/*
	 * The stream is becoming shared across the network because
	 * it is new at the destination and wasn't closed at the source.
	 * Increment the use counts on the I/O handle
	 * to reflect the additional client stream.
	 */
	usePtr->ref++;
	if (flags & FS_WRITE) {
	    usePtr->write++;
	}
	if (flags & FS_EXECUTE) {
	    usePtr->exec++;
	}
    } else if ((flags & FS_NEW_STREAM) == 0 && closeSrcClient) {
	/*
	 * The stream is becoming un-shared.  The last reference from the
	 * source was closed and there is already a reference at the dest.
	 * Decrement the use counts to reflect the fact that the stream on
	 * the original client is not referencing the I/O handle.
	 */
	usePtr->ref--;
	if (flags & FS_WRITE) {
	    usePtr->write--;
	}
	if (flags & FS_EXECUTE) {
	    usePtr->exec--;
	}
    } else {
	/*
	 * The stream moved completly, or a reference just moved between
	 * two existing streams, so there is no change visible to
	 * the I/O handle use counts.
	 */
     }
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsIOClientMigrate --
 *
 *	Move a client of an I/O handle from one host to another.  Flags
 *	indicate if the migration results in a newly shared stream, or
 *	in a stream that is no longer shared, or in a stream with
 *	no change visible at the I/O handle level.  We are careful to only
 *	open the dstClient if it getting the stream for the first time.
 *	Also, if the srcClient is switching from a writer to a reader, we
 *	remove its write reference.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Adds the destination client to the clientList, if needed.
 *	Removes the source client from the list, if needed.
 *
 * ----------------------------------------------------------------------------
 */

ENTRY void
FsIOClientMigrate(clientList, srcClientID, dstClientID, flags, closeSrcClient)
    List_Links	*clientList;	/* List of clients for the I/O handle. */
    int		srcClientID;	/* The original client. */
    int		dstClientID;	/* The destination client. */
    int		flags;		/* FS_RMT_SHARED if a copy of the stream
				 * still exists on the srcClient.
				 * FS_NEW_STREAM if stream is new on dst.
				 * FS_READ | FS_WRITE | FS_EXECUTE */
    Boolean	closeSrcClient;	/* TRUE if we should close src client.  This
				 * is set by FsStreamMigClient */
{
    register Boolean found;
    Boolean cache = FALSE;

    if (closeSrcClient) {
	/*
	 * The stream is not shared so we nuke the original client's use.
	 */
	found = FsIOClientClose(clientList, srcClientID, flags, &cache);
	if (!found) {
	    printf("FsIOClientMigrate, srcClient %d not found\n", srcClientID);
	}
    }
    if (flags & FS_NEW_STREAM) {
	/*
	 * The stream is new on the destination host.
	 */
	(void)FsIOClientOpen(clientList, dstClientID, flags, FALSE);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * FsNotifyOfMigration --
 *
 *	This invokes the stream-specific migration routine on the I/O server.
 *	This is used by various RMT (remote) stream types.
 *
 * Results:
 *	A return status, plus new flags containing FS_RMT_SHARED bit,
 *	a new stream offset, plus some stream-type-specific data used
 *	when creating the I/O handle in the migEnd procedure.
 *
 * Side effects:
 *      None here, but bookkeeping is done at the I/O server.
 *	
 *----------------------------------------------------------------------
 */
ReturnStatus
FsNotifyOfMigration(migInfoPtr, flagsPtr, offsetPtr, outSize, outData)
    FsMigInfo	*migInfoPtr;	/* Encapsulated information */
    int		*flagsPtr;	/* New flags, may have FS_RMT_SHARED bit set */
    int		*offsetPtr;	/* New stream offset */
    int		outSize;	/* Size of returned data, outData */
    Address	outData;	/* Returned data from server */
{
    register ReturnStatus	status;
    Rpc_Storage 	storage;
    FsMigParam		migParam;

    storage.requestParamPtr = (Address) migInfoPtr;
    storage.requestParamSize = sizeof(FsMigInfo);
    storage.requestDataPtr = (Address)NIL;
    storage.requestDataSize = 0;

    storage.replyParamPtr = (Address)&migParam;
    storage.replyParamSize = sizeof(FsMigParam);
    storage.replyDataPtr = (Address) NIL;
    storage.replyDataSize = 0;

    status = Rpc_Call(migInfoPtr->ioFileID.serverID, RPC_FS_MIGRATE, &storage);

    if (status == SUCCESS) {
	FsMigrateReply	*migReplyPtr;

	migReplyPtr = &(migParam.migReply);
	*flagsPtr = migReplyPtr->flags;
	*offsetPtr = migReplyPtr->offset;
	if (migParam.dataSize > 0) {
	    if (outSize < migParam.dataSize) {
		panic("FsNotifyOfMigration: too much data returned %d not %d\n",
			  migParam.dataSize, outSize);
		status = FAILURE;
	    } else {
		bcopy((Address)&migParam.data, outData, migParam.dataSize);
	    }
	}
    } else if (fsMigDebug) {
	printf("FsNotifyOfMigration: status %x from remote migrate routine.\n",
		  status);
    }
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_RpcMigrateStream --
 *
 *	The RPC service stub for FsNotifyOfMigration.
 *	This invokes the Migrate routine for the I/O handle given in
 *	the encapsulated stream state.
 *
 * Results:
 *	FS_STALE_HANDLE if handle that if client that is migrating the file
 *	doesn't have the file opened on this machine.  Otherwise return
 *	SUCCESS.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
Fs_RpcMigrateStream(srvToken, clientID, command, storagePtr)
    ClientData srvToken;	/* Handle on server process passed to
				 * Rpc_Reply */
    int clientID;		/* Sprite ID of client host */
    int command;		/* Command identifier */
    Rpc_Storage *storagePtr;    /* The request fields refer to the request
				 * buffers and also indicate the exact amount
				 * of data in the request buffers.  The reply
				 * fields are initialized to NIL for the
				 * pointers and 0 for the lengths.  This can
				 * be passed to Rpc_Reply */
{
    register FsMigInfo		*migInfoPtr;
    register FsHandleHeader	*hdrPtr;
    register ReturnStatus	status;
    register FsMigrateReply	*migReplyPtr;
    register FsMigParam		*migParamPtr;
    register Rpc_ReplyMem	*replyMemPtr;
    Address    			dataPtr;
    int				dataSize;

    migInfoPtr = (FsMigInfo *) storagePtr->requestParamPtr;

    hdrPtr = (*fsStreamOpTable[migInfoPtr->ioFileID.type].clientVerify)
	    (&migInfoPtr->ioFileID, migInfoPtr->srcClientID, (int *)NIL);
    if (hdrPtr == (FsHandleHeader *) NIL) {
	printf("Fs_RpcMigrateStream, unknown %s handle <%d,%d>\n",
	    FsFileTypeToString(migInfoPtr->ioFileID.type),
	    migInfoPtr->ioFileID.major, migInfoPtr->ioFileID.minor);
	return(FS_STALE_HANDLE);
    }
    FsHandleUnlock(hdrPtr);
    migParamPtr = mnew(FsMigParam);
    migReplyPtr = &(migParamPtr->migReply);
    migReplyPtr->flags = migInfoPtr->flags;
    storagePtr->replyParamPtr = (Address)migParamPtr;
    storagePtr->replyParamSize = sizeof(FsMigParam);
    storagePtr->replyDataPtr = (Address)NIL;
    storagePtr->replyDataSize = 0;
    status = (*fsStreamOpTable[hdrPtr->fileID.type].migrate) (migInfoPtr,
		clientID, &migReplyPtr->flags, &migReplyPtr->offset,
		&dataSize, &dataPtr);
    migParamPtr->dataSize = dataSize;
    if ((status == SUCCESS) && (dataSize > 0)) {
	if (dataSize <= sizeof(migParamPtr->data)) {
	    bcopy(dataPtr, (Address) &migParamPtr->data, dataSize);
	    free(dataPtr);
	} else {
	    panic("Fs_RpcMigrateStream: migrate returned oversized buffer.\n");
	    return(FAILURE);
	}
    } 
	
    FsHandleRelease(hdrPtr, FALSE);

    replyMemPtr = (Rpc_ReplyMem *) malloc(sizeof(Rpc_ReplyMem));
    replyMemPtr->paramPtr = storagePtr->replyParamPtr;
    replyMemPtr->dataPtr = (Address) NIL;
    Rpc_Reply(srvToken, status, storagePtr, Rpc_FreeMem,
		(ClientData)replyMemPtr);
    return(SUCCESS);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsPassStream --
 *
 *	This is called from Fs_Open as a cltOpen routine.  It's job is
 *	to take an encapsulated stream from a pseudo-device server and
 *	unencapsulate it so the Fs_Open returns the stream that the
 *	pseudo-device server had.
 *
 * Results:
 *	A return status.
 *
 * Side effects:
 *	Deencapsulates a stream.
 *
 * ----------------------------------------------------------------------------
 *
 */

/* ARGSUSED */
ReturnStatus
FsPassStream(ioFileIDPtr, flagsPtr, clientID, streamData, name, ioHandlePtrPtr)
    Fs_FileID		*ioFileIDPtr;	/* I/O fileID from the name server */
    int			*flagsPtr;	/* Return only.  The server returns
					 * a modified useFlags in FsFileState */
    int			clientID;	/* IGNORED */
    ClientData		streamData;	/* Pointer to encapsulated stream. */
    char		*name;		/* File name for error msgs */
    FsHandleHeader	**ioHandlePtrPtr;/* Return - a handle set up for
					 * I/O to a file, NIL if failure. */
{
    return(FAILURE);
}

/*
 * ----------------------------------------------------------------------------
 *
 * Fs_InitiateMigration --
 *
 *	Return the size of the encapsulated file system state.
 *	(Note: for now, we'll let the encapsulation procedure do the same
 *	work (in part); later things can be simplified to use a structure
 *	and to keep around some info off the ClientData hook.)
 *
 * Results:
 *	SUCCESS is returned directly; the size of the encapsulated state
 *	is returned in infoPtr->size.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 *
 */

/* ARGSUSED */
ReturnStatus
Fs_InitiateMigration(procPtr, hostID, infoPtr)
    Proc_ControlBlock *procPtr;			/* process being migrated */
    int hostID;					/* host to which it migrates */
    Proc_EncapInfo *infoPtr;			/* area w/ information about
						 * encapsulated state */
{
    Fs_ProcessState *fsPtr;
    int numStreams;
    int streamFlagsLen;
    FsPrefix *prefixPtr;
    int cwdLength;


    fsPtr = procPtr->fsPtr;
    numStreams = fsPtr->numStreams;
    /*
     * Get the prefix for the current working directory, and its size.
     * We pass the name over so it can be opened to make sure the prefix
     * is available.
     */
    if (fsPtr->cwdPtr->nameInfoPtr == (FsNameInfo *)NIL) {
	panic("Fs_GetEncapSize: no name information for cwd.\n");
	return(FAILURE);
    }
    prefixPtr = fsPtr->cwdPtr->nameInfoPtr->prefixPtr;
    if (prefixPtr == (FsPrefix *)NIL) {
	panic("Fs_GetEncapSize: no prefix for cwd.\n");
	return(FAILURE);
    }
    cwdLength = Byte_AlignAddr(prefixPtr->prefixLength + 1);
    
    /*
     * When sending an array of characters, it has to be even-aligned.
     */
    streamFlagsLen = Byte_AlignAddr(numStreams * sizeof(char));
    
    /*
     * Send the groups, file permissions, number of streams, and encapsulated
     * current working directory.  For each open file, send the
     * streamID and encapsulated stream contents.
     *
     *	        data			size
     *	 	----			----
     * 		# groups		int
     *	        groups			(# groups) * int
     *		permissions		int
     *		# files			int
     *		per-file flags		(# files) * char
     *		encapsulated files	(# files) * (FsMigInfo + int)
     *		cwd			FsMigInfo + int + strlen(cwdPrefix) + 1
     */
    infoPtr->size = (4 + fsPtr->numGroupIDs) * sizeof(int) +
	    streamFlagsLen + numStreams * (sizeof(FsMigInfo) + sizeof(int)) +
	    sizeof(FsMigInfo) + cwdLength;
    return(SUCCESS);	
}


/*
 * ----------------------------------------------------------------------------
 *
 * Fs_GetEncapSize --
 *
 *	Return the size of the encapsulated stream.
 *
 * Results:
 *	The size of the migration information structure.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 *
 */

int
Fs_GetEncapSize()
{
    return(sizeof(FsMigInfo));
}


/*
 *----------------------------------------------------------------------
 *
 * Fs_EncapFileState --
 *
 *	Encapsulate the file system state of a process for migration.  
 *
 * Results:
 *	Any error during stream encapsulation
 *	is returned; otherwise, SUCCESS.  The encapsulated state is placed
 *	in the area referenced by ptr.
 *
 * Side effects:
 *	None.  
 *
 *----------------------------------------------------------------------
 */

/* ARGSUSED */
ReturnStatus
Fs_EncapFileState(procPtr, hostID, infoPtr, ptr)
    register Proc_ControlBlock 	*procPtr;  /* The process being migrated */
    int hostID;				   /* host to which it migrates */
    Proc_EncapInfo *infoPtr;		   /* area w/ information about
					    * encapsulated state */
    Address ptr;			   /* Pointer to allocated buffer */
{
    Fs_ProcessState *fsPtr;
    int numStreams;
    int numGroups;
    int streamFlagsLen;
    Fs_Stream *streamPtr;
    int i;
    ReturnStatus status;
    FsPrefix *prefixPtr;
    int cwdLength;
    int size;


    fsPtr = procPtr->fsPtr;
    numStreams = fsPtr->numStreams;
    /*
     * Get the prefix for the current working directory, and its size.
     * We pass the name over so it can be opened to make sure the prefix
     * is available.
     */
    if (fsPtr->cwdPtr->nameInfoPtr == (FsNameInfo *)NIL) {
	panic("Fs_EncapFileState: no name information for cwd.\n");
	return(FAILURE);
    }
    prefixPtr = fsPtr->cwdPtr->nameInfoPtr->prefixPtr;
    if (prefixPtr == (FsPrefix *)NIL) {
	panic("Fs_EncapFileState: no prefix for cwd.\n");
	return(FAILURE);
    }
    cwdLength = Byte_AlignAddr(prefixPtr->prefixLength + 1);
    
    /*
     * When sending an array of characters, it has to be even-aligned.
     */
    streamFlagsLen = Byte_AlignAddr(numStreams * sizeof(char));
    
    /*
     * Send the groups, file permissions, number of streams, and encapsulated
     * current working directory.  For each open file, send the
     * streamID and encapsulated stream contents.
     *
     *	        data			size
     *	 	----			----
     * 		# groups		int
     *	        groups			(# groups) * int
     *		permissions		int
     *		# files			int
     *		per-file flags		(# files) * char
     *		encapsulated files	(# files) * (FsMigInfo + int)
     *		cwd			FsMigInfo + int + strlen(cwdPrefix) + 1
     */
    size = (4 + fsPtr->numGroupIDs) * sizeof(int) +
	    streamFlagsLen + numStreams * (sizeof(FsMigInfo) + sizeof(int)) +
	    sizeof(FsMigInfo) + cwdLength;
    if (size != infoPtr->size) {
	panic("Fs_EncapState: size of encapsulated state changed.\n");
	return(FAILURE);
    }

    /*
     * Send groups, filePermissions, numStreams, the cwd, and each file.
     */
    
    numGroups = fsPtr->numGroupIDs;
    Byte_FillBuffer(ptr, unsigned int, numGroups);
    if (numGroups > 0) {
	bcopy((Address) fsPtr->groupIDs, ptr, numGroups * sizeof(int));
	ptr += numGroups * sizeof(int);
    }
    Byte_FillBuffer(ptr, unsigned int, fsPtr->filePermissions);
    Byte_FillBuffer(ptr, int, numStreams);
    if (numStreams > 0) {
	bcopy((Address) fsPtr->streamFlags, ptr, numStreams * sizeof(char));
	ptr += streamFlagsLen;
    }
    
    Byte_FillBuffer(ptr, int, prefixPtr->prefixLength);
    strncpy(ptr, prefixPtr->prefix, prefixPtr->prefixLength);
    ptr[prefixPtr->prefixLength] = '\0';
    ptr += cwdLength;

    status = Fs_EncapStream(fsPtr->cwdPtr, ptr);
    if (status != SUCCESS) {
	printf(
		  "Fs_EncapFileState: Error %x from Fs_EncapStream on cwd.\n",
		  status);
	return(status);
    }
    fsPtr->cwdPtr = (Fs_Stream *) NIL;
    ptr += sizeof(FsMigInfo);

    for (i = 0; i < fsPtr->numStreams; i++) {
	streamPtr = fsPtr->streamList[i];
	if (streamPtr != (Fs_Stream *) NIL) {
	    Byte_FillBuffer(ptr, int, i);
	    status = Fs_EncapStream(streamPtr, ptr);
	    if (status != SUCCESS) {
		printf(
			  "Fs_EncapFileState: Error %x from Fs_EncapStream.\n",
			  status);
		return(status);
	    }
	    fsPtr->streamList[i] = (Fs_Stream *) NIL;
	} else {
	    Byte_FillBuffer(ptr, int, NIL);
	    bzero(ptr, sizeof(FsMigInfo));
	}	
	ptr += sizeof(FsMigInfo);
    }

    return(SUCCESS);
}


/*
 *----------------------------------------------------------------------
 *
 * Fs_DeencapFileState --
 *
 *	Get the file system state of a process from another node.  The
 *	buffer contains group information, permissions, the encapsulated
 *	current working directory, and encapsulated streams.
 *
 * Results:
 *	If Fs_DeencapStream returns an error, that error is returned.
 *	Otherwise, SUCCESS is returned.  
 *
 * Side effects:
 *	"Local" Fs_Streams are created and allocated to the foreign process.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Fs_DeencapFileState(procPtr, infoPtr, buffer)
    register Proc_ControlBlock 	*procPtr; /* The process being migrated */
    Proc_EncapInfo *infoPtr;		  /* information about the buffer */
    Address buffer;			  /* buffer containing data */
{
    register Fs_ProcessState *fsPtr;
    int i;
    int index;
    int numGroups;
    int numStreams;
    ReturnStatus status;
    char *cwdName;
    int cwdLength;
    Fs_Stream *prefixStreamPtr;

    /*
     * Set up an fsPtr for the process.  Initialize some fields so that
     * at any point we can bail out on error by calling Fs_CloseState.  Some
     * fields are initialized from the information from the other host.
     */
    procPtr->fsPtr = fsPtr = mnew(Fs_ProcessState);
    fsPtr->cwdPtr = (Fs_Stream *) NIL;

    /*
     * Get group and permissions information.
     */
    Byte_EmptyBuffer(buffer, unsigned int, numGroups);
    fsPtr->numGroupIDs = numGroups;
    if (numGroups > 0) {
	fsPtr->groupIDs = (int *)malloc(numGroups * sizeof(int));
	bcopy(buffer, (Address) fsPtr->groupIDs, numGroups * sizeof(int));
	buffer += numGroups * sizeof(int);
    } else {
	fsPtr->groupIDs = (int *)NIL;
    }
    Byte_EmptyBuffer(buffer, unsigned int, fsPtr->filePermissions);

    /*
     * Get numStreams, flags, and the encapsulated cwd.  Allocate memory
     * for the streams and flags arrays if non-empty.  The array of
     * streamFlags may be an odd number of bytes, so we skip past the
     * byte of padding if it exists (using the Byte_AlignAddr macro).
     */

    Byte_EmptyBuffer(buffer, int, numStreams);
    fsPtr->numStreams = numStreams;
    if (numStreams > 0) {
	fsPtr->streamList = (Fs_Stream **)
		malloc(numStreams * sizeof(Fs_Stream *));
	fsPtr->streamFlags = (char *)malloc(numStreams * sizeof(char));
	bcopy(buffer, (Address) fsPtr->streamFlags, numStreams * sizeof(char));
	buffer += Byte_AlignAddr(numStreams * sizeof(char));
	for (i = 0; i < fsPtr->numStreams; i++) {
	    fsPtr->streamList[i] = (Fs_Stream *) NIL;
	}
    } else {
	fsPtr->streamList = (Fs_Stream **)NIL;
	fsPtr->streamFlags = (char *)NIL;
    }
    /*
     * Get the name of the current working directory and make sure it's
     * an installed prefix.
     */
    Byte_EmptyBuffer(buffer, int, cwdLength);
    cwdName = buffer;
    buffer += Byte_AlignAddr(cwdLength + 1);
    status = Fs_Open(cwdName, FS_READ | FS_FOLLOW, FS_FILE, 0,
		     &prefixStreamPtr);
    if (status != SUCCESS) {
	if (fsMigDebug) {
	    panic("Unable to open prefix '%s' for migrated process.\n",
		   cwdName);
	} else if (proc_MigDebugLevel > 1) {
	    printf("%s unable to open prefix '%s' for migrated process.\n",
		   "Warning: Fs_DeencapFileState:", cwdName);
	}
	goto failure;
    } else {
	(void) Fs_Close(prefixStreamPtr);
    }

    status = Fs_DeencapStream(buffer, &fsPtr->cwdPtr);
    if (status != SUCCESS) {
	if (fsMigDebug) {
	    panic("GetFileState: Fs_DeencapStream returned %x for cwd.\n",
		  status);
	} else if (proc_MigDebugLevel > 1) {
	    printf("%s Fs_DeencapStream returned %x for cwd.\n",
		  "Warning: Fs_DeencapFileState:", status);
	}
	fsPtr->cwdPtr = (Fs_Stream *) NIL;
        goto failure;
    }
    buffer += sizeof(FsMigInfo);

    

    /*
     * Get the other streams.
     */
    for (i = 0; i < fsPtr->numStreams; i++) {
	Byte_EmptyBuffer(buffer, int, index);
	if ((status == SUCCESS) && (index != NIL)) {
	    status = Fs_DeencapStream(buffer, &fsPtr->streamList[index]);
	    if (status != SUCCESS) {
		    printf(
      "Fs_DeencapFileState: Fs_DeencapStream for file id %d returned %x.\n",
			      index, status);
		    fsPtr->streamList[index] = (Fs_Stream *) NIL;
		    goto failure;
	    }
	}
	buffer += sizeof(FsMigInfo);
    }
    return(SUCCESS);
    
failure:
    Fs_CloseState(procPtr);
    return(status);
    
}
