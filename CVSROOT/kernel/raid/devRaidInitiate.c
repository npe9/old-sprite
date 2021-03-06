/* 
 * devRaidInitiate.c --
 *
 *	This file implements the BlockDevice interface for homogeneous disk
 *	arrays.
 *
 * Copyright 1989 Regents of the University of California
 * All rights reserved.
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#include <stdio.h>
#include <string.h>
#include "sync.h"
#include "sprite.h"
#include "fs.h"
#include "dev.h"
#include "devBlockDevice.h"
#include "devRaid.h"
#include "semaphore.h"
#include "stdlib.h"
#include "devRaidUtil.h"
#include "schedule.h"
#include "devRaidProto.h"


/*
 *----------------------------------------------------------------------
 *
 * Raid_InitiateIORequests --
 *
 *	Initiates IO requests specified by reqControlPtr.
 *	Calls doneProc with clientData, the number of requests that have
 *	failed and a pointer to the last failed request when the IO is complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operations.
 *
 *----------------------------------------------------------------------
 */

void blockIODoneProc();
void nonInterruptLevelCallBackProc();

void 
Raid_InitiateIORequests(reqControlPtr, doneProc, clientData)
    RaidRequestControl	*reqControlPtr;
    void	       (*doneProc)();
    ClientData		 clientData;
{
    RaidIOControl	*IOControlPtr;
    RaidBlockRequest	*reqPtr;
    int			 i;

    /*
     * Initiate IO's.
     */
#ifdef TESTING
    PrintRequests(reqControlPtr);
#endif TESTING
    IOControlPtr = Raid_MakeIOControl(doneProc, clientData);
    IOControlPtr->numIO++;
    for ( i = 0; i < reqControlPtr->numReq; i++ ) {
	reqPtr = &reqControlPtr->reqPtr[i];
	if (reqPtr->state == REQ_READY) {
	    reqPtr->state = REQ_PENDING;
	    MASTER_LOCK(&IOControlPtr->mutex);
	    IOControlPtr->numIO++;
	    MASTER_UNLOCK(&IOControlPtr->mutex);
	    reqPtr->devReq.doneProc   = blockIODoneProc;
	    reqPtr->devReq.clientData = (ClientData) IOControlPtr;
	    (void) Dev_BlockDeviceIO(
		    reqPtr->raidPtr->disk[reqPtr->col][reqPtr->row]->handlePtr,
		    (DevBlockDeviceRequest *) reqPtr);
	}
    }

    MASTER_LOCK(&IOControlPtr->mutex);
    IOControlPtr->numIO--;
    if (IOControlPtr->numIO == 0) {
        MASTER_UNLOCK(&IOControlPtr->mutex);
        IOControlPtr->doneProc(IOControlPtr->clientData,
		IOControlPtr->numFailed, IOControlPtr->failedReqPtr);
	Raid_FreeIOControl(IOControlPtr);
    } else {
        MASTER_UNLOCK(&IOControlPtr->mutex);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * blockIODoneProc --
 *
 *	Callback procedure for Raid_InitiateIORequests.
 *	This procedure is called once each time an individual IO reqeust
 *	completes.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Reports errors.
 *
 *----------------------------------------------------------------------
 */

void
blockIODoneProc(reqPtr, status, amountTransferred)
    RaidBlockRequest	*reqPtr;
    ReturnStatus	 status;
    int			 amountTransferred;
{
    RaidIOControl	*IOControlPtr;
    IOControlPtr = (RaidIOControl *) reqPtr->devReq.clientData;

    /*
     * Check to see if disk has failed since request was initiated.
     */
/*
    if (!IsValid(reqPtr->diskPtr,
	    ByteToSector(reqPtr->raidPtr, reqPtr->devReq.startAddress),
	    ByteToSector(reqPtr->raidPtr, reqPtr->devReq.bufferLen))) {
	status = FAILURE;
    }
*/

    reqPtr->status = status;
    if (status != SUCCESS) {
        reqPtr->state = REQ_FAILED;
	Raid_ReportRequestError(reqPtr);
	if (reqPtr->devReq.operation == FS_WRITE) {
	    Raid_FailDisk(reqPtr->raidPtr,
		    reqPtr->col, reqPtr->row, reqPtr->version);
	}
    } else {
        reqPtr->state = REQ_COMPLETED;
    }

    MASTER_LOCK(&IOControlPtr->mutex);
    IOControlPtr->amountTransferred += amountTransferred;

    /*
     * A Raid IO operation fails if any of the component operations fail.
     * Therefore, don't overwrite status if a previous operation has failed.
     */
    if (status != SUCCESS) {
        IOControlPtr->numFailed++;
        IOControlPtr->failedReqPtr = reqPtr;
    }

    /*
     * Check if all component IO's done.
     */
    IOControlPtr->numIO--;
    if (IOControlPtr->numIO == 0) {
        MASTER_UNLOCK(&IOControlPtr->mutex);
	/*
	 * this forces the call-back to happen at non-interrupt level
	 */
	Proc_CallFunc(nonInterruptLevelCallBackProc,(ClientData)IOControlPtr,0);
    } else {
        MASTER_UNLOCK(&IOControlPtr->mutex);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * nonInterruptLevelCallBackProc --
 *
 *	None-interrupt level callback procedure for Raid_InitiateIORequests.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

void
nonInterruptLevelCallBackProc(IOControlPtr)
    RaidIOControl	*IOControlPtr;
{
    IOControlPtr->doneProc(IOControlPtr->clientData,
	    IOControlPtr->numFailed, IOControlPtr->failedReqPtr);
    Raid_FreeIOControl(IOControlPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateStripeIOFailure --
 *
 *	Causes the IO operation to fail, presumably because it can not
 *	be completely (i.e. more than one disk in a group has failed.)
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void stripeIODoneProc();

static void
InitiateStripeIOFailure(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    stripeIODoneProc(stripeIOControlPtr, 2);
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateStripeWrite --
 *
 *	Initiates a stripe write (i.e. an IO that does not span stripe
 *	boundaries) via either Raid_InitiateIORequests, InitiateReconstructWrite
 *	or InitiateReadModifyWrite.
 *	Sets up the recovery procedure if recovery is possible.  Note that
 *	the recovery procedure for InitiateReconstructWrite is
 *	InitiateReadModifyWrite and visa versa.
 *	Calls callback procedure specified by stripeIOControlPtr with
 *	stripeIOControlPtr, number of requests that have failed and a
 *	pointer to the last failed request, when the IO is complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void InitiateSplitStripeWrite();
static void InitiateReadModifyWrite();
static void InitiateReconstructWrite();
static void oldInfoReadDoneProc();
static void stripeWriteDoneProc();

static void
InitiateStripeWrite(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    Raid		*raidPtr       = stripeIOControlPtr->raidPtr;
    unsigned		 firstSector   = stripeIOControlPtr->firstSector;
    unsigned		 nthSector     = stripeIOControlPtr->nthSector;
    Address		 buffer        = stripeIOControlPtr->buffer;
    int			 ctrlData      = stripeIOControlPtr->ctrlData;
    RaidRequestControl	*reqControlPtr = stripeIOControlPtr->reqControlPtr;
    char		*parityBuf     = stripeIOControlPtr->parityBuf;

    /*
     * Check to see if parity disk has failed.
     */
    reqControlPtr->numReq = reqControlPtr->numFailed = 0;
    AddRaidParityRequest(reqControlPtr, raidPtr, FS_READ,
	    firstSector, parityBuf, ctrlData);
    if (reqControlPtr->numFailed > 0) {
	/*
	 * If parity disk has failed, just write the data.
	 */
	reqControlPtr->numReq = reqControlPtr->numFailed = 0;
	AddRaidDataRequests(reqControlPtr, raidPtr, FS_WRITE,
		firstSector, nthSector, buffer, ctrlData);
	if (reqControlPtr->numFailed == 0) {
	    Raid_InitiateIORequests(stripeIOControlPtr->reqControlPtr,
		    stripeIODoneProc, (ClientData) stripeIOControlPtr);
	} else {
	    InitiateStripeIOFailure(stripeIOControlPtr);
	}
    } else if (nthSector-firstSector < raidPtr->dataSectorsPerStripe/2) {
	/*
	 * If less than half of the stripe is being written, do a
	 * read modify write.
	 */
	stripeIOControlPtr->recoverProc = InitiateReconstructWrite;
	InitiateReadModifyWrite(stripeIOControlPtr);
    } else {
	/*
	 * If half or more of the stripe is being written, do a
	 * reconstruct write.
	 */
	stripeIOControlPtr->recoverProc = InitiateReadModifyWrite;
	InitiateReconstructWrite(stripeIOControlPtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateSplitStripeWrite --
 *
 *	If a write request which covers multiple stripe units fails durring
 *	the read phase and the failed component is not a full stripe unit,
 *	It is necessary to do both a read-modify-write and a reconstruct write
 *	in order to complete the entire request.
 *	Note: StripeIOControlPtr->failedReqPtr is assumed to point to the data
 *	part of the failed request.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void
InitiateSplitStripeWrite(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    Raid		*raidPtr       = stripeIOControlPtr->raidPtr;
    unsigned		 firstSector   = stripeIOControlPtr->firstSector;
    unsigned		 nthSector     = stripeIOControlPtr->nthSector;
/*    Address		 buffer        = stripeIOControlPtr->buffer; */
    int			 ctrlData      = stripeIOControlPtr->ctrlData;
    RaidRequestControl	*reqControlPtr = stripeIOControlPtr->reqControlPtr;
    char		*parityBuf     = stripeIOControlPtr->parityBuf;
    char		*readBuf       = stripeIOControlPtr->readBuf;
    int			 failedOff     = (int) StripeUnitOffset(raidPtr,
    	  stripeIOControlPtr->reqControlPtr->failedReqPtr->devReq.startAddress);
    int			 failedLen     =
    	   stripeIOControlPtr->reqControlPtr->failedReqPtr->devReq.bufferLen;
    int			 rangeOff;
    int			 rangeLen;

#ifdef TESTING
    printf("InitiateSplitStripeWrite\n");
#endif TESTING
    /*
     * 'Deduce' data part of failed request.
     */
    if (stripeIOControlPtr->recoverProc == (void(*)())InitiateReadModifyWrite) {
	failedOff = StripeUnitOffset(raidPtr, failedOff + failedLen);
	failedLen = raidPtr->bytesPerStripeUnit - failedLen;
    }
    rangeOff = StripeUnitOffset(raidPtr, failedOff + failedLen);
    rangeLen = raidPtr->bytesPerStripeUnit - failedLen;

    stripeIOControlPtr->recoverProc = InitiateStripeIOFailure;
    stripeIOControlPtr->rangeOff = 0;
    stripeIOControlPtr->rangeLen = raidPtr->bytesPerStripeUnit;
    reqControlPtr->numReq = reqControlPtr->numFailed = 0;
    /*
     * reconstructWrite strip
     */
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    FirstSectorOfStripe(raidPtr, firstSector), firstSector,
	    readBuf, ctrlData,
	    failedOff, failedLen);
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    nthSector, NthSectorOfStripe(raidPtr, firstSector),
	    readBuf + SectorToByte(raidPtr,
	    	    firstSector - FirstSectorOfStripe(raidPtr, firstSector)),
	    ctrlData, failedOff, failedLen);
    /*
     * RMW strip
     */
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    firstSector, nthSector, readBuf + SectorToByte(raidPtr,
		    raidPtr->dataSectorsPerStripe - (nthSector-firstSector)),
	    ctrlData, rangeOff, rangeLen);
    Raid_AddParityRangeRequest(reqControlPtr, raidPtr, FS_READ,
	    firstSector, parityBuf, ctrlData,
	    rangeOff, rangeLen);
    if (reqControlPtr->numFailed == 0) {
	Raid_InitiateIORequests(reqControlPtr,
		oldInfoReadDoneProc, (ClientData) stripeIOControlPtr);
    } else {
	stripeIOControlPtr->recoverProc(stripeIOControlPtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateReadModifyWrite --
 *
 *	Initiates a read modify write.  (i.e. read old data and old parity,
 *	computes the new parity and then writes the new data and new parity)
 *	Calls the recovery procedure if a read modify wirte can not complete.
 *	Calls callback procedure specified by stripeIOControlPtr with
 *	stripeIOControlPtr, number of requests that have failed and a
 *	pointer to the last failed request, when the IO is complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void
InitiateReadModifyWrite(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    Raid		*raidPtr       = stripeIOControlPtr->raidPtr;
    unsigned		 firstSector   = stripeIOControlPtr->firstSector;
    unsigned		 nthSector     = stripeIOControlPtr->nthSector;
/*    Address		 buffer        = stripeIOControlPtr->buffer; */
    int			 ctrlData      = stripeIOControlPtr->ctrlData;
    RaidRequestControl	*reqControlPtr = stripeIOControlPtr->reqControlPtr;
    char		*parityBuf     = stripeIOControlPtr->parityBuf;
    char		*readBuf       = stripeIOControlPtr->readBuf;
    void	       (*recoverProc)()= stripeIOControlPtr->recoverProc;

#ifdef TESTING
    printf("InitiateReadModifyWrite\n");
#endif TESTING
    reqControlPtr->numReq = reqControlPtr->numFailed = 0;
    AddRaidDataRequests(reqControlPtr, raidPtr, FS_READ,
	    firstSector, nthSector, readBuf, ctrlData);
    if (reqControlPtr->numReq == 1) {
	stripeIOControlPtr->rangeOff =
		reqControlPtr->reqPtr[0].devReq.startAddress;
	stripeIOControlPtr->rangeLen =
		reqControlPtr->reqPtr[0].devReq.bufferLen;
    } else {
	stripeIOControlPtr->rangeOff = 0;
	stripeIOControlPtr->rangeLen = raidPtr->bytesPerStripeUnit;
    }
    Raid_AddParityRangeRequest(reqControlPtr, raidPtr, FS_READ,
	    firstSector, parityBuf, ctrlData,
	    stripeIOControlPtr->rangeOff, stripeIOControlPtr->rangeLen);
    if (reqControlPtr->numFailed == 0) {
	Raid_InitiateIORequests(reqControlPtr,
		oldInfoReadDoneProc, (ClientData) stripeIOControlPtr);
    } else {
	/*
	 * If the request covers multiple stripe units and is not stripe unit
	 * aligned, check to see if the failed request is a partial
	 * stripe unit.  If it is, then both a read-modify-write and
	 * a reconstruct write is necessary to complete the request.
	 */
	DevBlockDeviceRequest *devReqPtr = &reqControlPtr->failedReqPtr->devReq;
	if (stripeIOControlPtr->rangeLen == raidPtr->bytesPerStripeUnit &&
		devReqPtr->bufferLen != raidPtr->bytesPerStripeUnit &&
		recoverProc != (void (*)()) InitiateStripeIOFailure) {
	    stripeIOControlPtr->recoverProc = InitiateStripeIOFailure;
	    InitiateSplitStripeWrite(stripeIOControlPtr);
	} else {
	    stripeIOControlPtr->recoverProc = InitiateStripeIOFailure;
	    recoverProc(stripeIOControlPtr);
	}
    }
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateReconstructWrite --
 *
 *	Initiates a reconstruct write.  (i.e. read rest of stripe if any
 *	computes the new parity and then writes the new data and new parity)
 *	Calls the recovery procedure if a read modify wirte can not complete.
 *	Calls callback procedure specified by stripeIOControlPtr with
 *	stripeIOControlPtr, number of requests that have failed and a
 *	pointer to the last failed request, when the IO is complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void
InitiateReconstructWrite(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    Raid		*raidPtr       = stripeIOControlPtr->raidPtr;
    unsigned		 firstSector   = stripeIOControlPtr->firstSector;
    unsigned		 nthSector     = stripeIOControlPtr->nthSector;
/*    Address		 buffer        = stripeIOControlPtr->buffer; */
    int			 ctrlData      = stripeIOControlPtr->ctrlData;
    RaidRequestControl	*reqControlPtr = stripeIOControlPtr->reqControlPtr;
/*    char		*parityBuf     = stripeIOControlPtr->parityBuf; */
    char		*readBuf       = stripeIOControlPtr->readBuf;
    void	       (*recoverProc)()= stripeIOControlPtr->recoverProc;

    /*
     * If writing only one stripe unit, range restrict the write.
     */
#ifdef TESTING
    printf("InitiateReconstructWrite\n");
#endif TESTING
    if (SectorToStripeUnitID(raidPtr, firstSector) ==
	    SectorToStripeUnitID(raidPtr, nthSector-1)) {
	stripeIOControlPtr->rangeOff = SectorToByte(raidPtr, firstSector);
        stripeIOControlPtr->rangeLen =
		SectorToByte(raidPtr, nthSector-firstSector);
    } else {
	stripeIOControlPtr->rangeOff = 0;
	stripeIOControlPtr->rangeLen = raidPtr->bytesPerStripeUnit;
    }
    reqControlPtr->numReq = reqControlPtr->numFailed = 0;
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    FirstSectorOfStripe(raidPtr, firstSector), firstSector,
	    readBuf, ctrlData,
	    stripeIOControlPtr->rangeOff, stripeIOControlPtr->rangeLen);
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    nthSector, NthSectorOfStripe(raidPtr, firstSector),
	    readBuf + SectorToByte(raidPtr,
	    	    firstSector - FirstSectorOfStripe(raidPtr, firstSector)),
	    ctrlData,stripeIOControlPtr->rangeOff,stripeIOControlPtr->rangeLen);
    if (reqControlPtr->numFailed == 0) {
	Raid_InitiateIORequests(stripeIOControlPtr->reqControlPtr,
		oldInfoReadDoneProc, (ClientData) stripeIOControlPtr);
    } else {
	/*
	 * If the request covers multiple stripe units and is not stripe unit
	 * aligned, check to see if the failed request is a partial
	 * stripe unit.  If it is, then both a read-modify-write and
	 * a reconstruct write is necessary to complete the request.
	 */
	DevBlockDeviceRequest *devReqPtr = &reqControlPtr->failedReqPtr->devReq;
	if (stripeIOControlPtr->rangeLen == raidPtr->bytesPerStripeUnit &&
		devReqPtr->bufferLen != raidPtr->bytesPerStripeUnit &&
		recoverProc != (void(*)()) InitiateStripeIOFailure) {
	    InitiateSplitStripeWrite(stripeIOControlPtr);
	} else {
	    stripeIOControlPtr->recoverProc = InitiateStripeIOFailure;
	    recoverProc(stripeIOControlPtr);
	}
    }
}


/*
 *----------------------------------------------------------------------
 *
 * oldInfoReadDoneProc --
 *
 *	Callback procedure for InitiateReadModifyWrite and
 *	InitiateReconstructWrite.
 *	This procedure is called after the old data and parity have been read
 *	in the process of writing new data.
 *	If an error has occured, the recovery procedure is called.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	IO operations.
 *
 *----------------------------------------------------------------------
 */

static void
oldInfoReadDoneProc(stripeIOControlPtr, numFailed, failedReqPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
    int		 	 numFailed;
    RaidBlockRequest	*failedReqPtr;
{
    Raid		*raidPtr	= stripeIOControlPtr->raidPtr;
    RaidRequestControl	*reqControlPtr	= stripeIOControlPtr->reqControlPtr;

    if (numFailed == 0) {
	char			*parityBuf;

#ifndef NODATA
        parityBuf = Malloc((unsigned) raidPtr->bytesPerStripeUnit);
	bzero(parityBuf, raidPtr->bytesPerStripeUnit);
#endif

	Raid_XorRangeRequests(reqControlPtr,
		raidPtr, parityBuf,
		stripeIOControlPtr->rangeOff, stripeIOControlPtr->rangeLen);
        reqControlPtr->numReq = 0;
        reqControlPtr->numFailed = 0;
        Raid_AddDataRangeRequests(reqControlPtr,
		raidPtr, FS_WRITE,
		stripeIOControlPtr->firstSector, stripeIOControlPtr->nthSector,
                stripeIOControlPtr->buffer, stripeIOControlPtr->ctrlData,
		stripeIOControlPtr->rangeOff, stripeIOControlPtr->rangeLen);
	/*
	 * Raid_XorRangeRequests will not xor failed requests so we have
	 * to change its state to REQ_COMPLETED to make it xor the new data.
	 */
	if (reqControlPtr->numFailed > 0) {
	    reqControlPtr->failedReqPtr->state = REQ_COMPLETED;
	}
	Raid_XorRangeRequests(reqControlPtr,
		raidPtr, parityBuf,
		stripeIOControlPtr->rangeOff, stripeIOControlPtr->rangeLen);
#ifndef NODATA
	Free(stripeIOControlPtr->parityBuf);
#endif
	stripeIOControlPtr->parityBuf = parityBuf;
        Raid_AddParityRangeRequest(reqControlPtr,
		raidPtr, FS_WRITE,
	        stripeIOControlPtr->firstSector, stripeIOControlPtr->parityBuf,
		stripeIOControlPtr->ctrlData,
		stripeIOControlPtr->rangeOff, stripeIOControlPtr->rangeLen);
	switch (reqControlPtr->numFailed) {
	case 0:
            Raid_InitiateIORequests(reqControlPtr,
		    stripeWriteDoneProc, (ClientData) stripeIOControlPtr);
	    break;
	case 1:
            Raid_InitiateIORequests(reqControlPtr,
		    stripeIODoneProc, (ClientData) stripeIOControlPtr);
	    break;
	default:
	    InitiateStripeIOFailure(stripeIOControlPtr);
	    break;
	}
    } else {
        void       (*recoverProc)() = stripeIOControlPtr->recoverProc;
	reqControlPtr->failedReqPtr = failedReqPtr;
	/*
	 * If the request covers multiple stripe units and is not stripe unit
	 * aligned, check to see if the failed request is a partial
	 * stripe unit.  If it is, then both a read-modify-write and
	 * a reconstruct write is necessary to complete the request.
	 */
	if (stripeIOControlPtr->rangeLen == raidPtr->bytesPerStripeUnit &&
		failedReqPtr->devReq.bufferLen != raidPtr->bytesPerStripeUnit &&
		recoverProc != (void (*)()) InitiateStripeIOFailure) {
	    InitiateSplitStripeWrite(stripeIOControlPtr);
	} else {
	    stripeIOControlPtr->recoverProc = InitiateStripeIOFailure;
	    recoverProc(stripeIOControlPtr);
	}
    }
}


/*
 *----------------------------------------------------------------------
 *
 * stripeWriteDoneProc --
 *
 *	Callback procedure for oldInfoReadDoneProc.
 *	This procedure is called after the new data and parity have
 *	been written.
 *	Since one of the writes is redundant, the IO is considered to have
 *	succeeded as long as the number of failures is less than or equal
 *	to one.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
stripeWriteDoneProc(stripeIOControlPtr, numFailed)
    RaidStripeIOControl	*stripeIOControlPtr;
    int			 numFailed;
{
    if (numFailed <= 1) {
	stripeIODoneProc(stripeIOControlPtr, 0);
    } else {
	stripeIODoneProc(stripeIOControlPtr, numFailed);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateStripeRead --
 *
 *	Initiates a stripe read (i.e. an IO that does not span stripe
 *	boundaries).
 *	Calls callback procedure specified by stripeIOControlPtr with
 *	stripeIOControlPtr, number of requests that have failed and a
 *	pointer to the last failed request, when the IO is complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void stripeReadDoneProc();
static void reconstructStripeReadDoneProc();
static void InitiateReconstructRead();

static void
InitiateStripeRead(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    Raid		*raidPtr       = stripeIOControlPtr->raidPtr;
    unsigned		 firstSector   = stripeIOControlPtr->firstSector;
    unsigned		 nthSector     = stripeIOControlPtr->nthSector;
    Address		 buffer        = stripeIOControlPtr->buffer;
    int			 ctrlData      = stripeIOControlPtr->ctrlData;
    RaidRequestControl	*reqControlPtr = stripeIOControlPtr->reqControlPtr;
/*    char		*parityBuf     = stripeIOControlPtr->parityBuf; */
/*    char		*readBuf       = stripeIOControlPtr->readBuf; */

    reqControlPtr->numReq = reqControlPtr->numFailed = 0;
    AddRaidDataRequests(reqControlPtr, raidPtr, FS_READ,
	    firstSector, nthSector, buffer, ctrlData);
    switch (reqControlPtr->numFailed) {
    case 0:
        Raid_InitiateIORequests(reqControlPtr,
		stripeReadDoneProc, (ClientData) stripeIOControlPtr);
	break;
    case 1:
	InitiateReconstructRead(stripeIOControlPtr);
	break;
    default:
	InitiateStripeIOFailure(stripeIOControlPtr);
	break;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * stripeReadDoneProc --
 *
 *	Callback procedure for InitiateStripeRead.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
stripeReadDoneProc(stripeIOControlPtr, numFailed, failedReqPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
    int			 numFailed;
    RaidBlockRequest	*failedReqPtr;
{
    switch (numFailed) {
    case 0:
	stripeIODoneProc(stripeIOControlPtr, numFailed);
	break;
    case 1:
	stripeIOControlPtr->reqControlPtr->failedReqPtr = failedReqPtr;
	InitiateReconstructRead(stripeIOControlPtr);
	break;
    default:
	stripeIODoneProc(stripeIOControlPtr, numFailed);
	break;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateReconstructRead --
 *
 *	Initiates a reconstruct read (i.e. computes requested data by reading
 *	the rest of the stripe and parity).
 *	Calls callback procedure specified by stripeIOControlPtr with
 *	stripeIOControlPtr, number of requests that have failed and a
 *	pointer to the last failed request, when the IO is complete.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void
InitiateReconstructRead(stripeIOControlPtr)
    RaidStripeIOControl	*stripeIOControlPtr;
{
    Raid		*raidPtr       = stripeIOControlPtr->raidPtr;
    unsigned		 firstSector   = stripeIOControlPtr->firstSector;
    unsigned		 nthSector     = stripeIOControlPtr->nthSector;
/*    Address		 buffer        = stripeIOControlPtr->buffer; */
    int			 ctrlData      = stripeIOControlPtr->ctrlData;
    RaidRequestControl	*reqControlPtr = stripeIOControlPtr->reqControlPtr;
    char		*parityBuf     = stripeIOControlPtr->parityBuf;
    char		*readBuf       = stripeIOControlPtr->readBuf;

    reqControlPtr->numFailed = 0;
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    FirstSectorOfStripe(raidPtr, firstSector), firstSector,
	    readBuf, ctrlData,
	    (int) reqControlPtr->failedReqPtr->devReq.startAddress,
	    reqControlPtr->failedReqPtr->devReq.bufferLen);
    Raid_AddDataRangeRequests(reqControlPtr, raidPtr, FS_READ,
	    nthSector, NthSectorOfStripe(raidPtr, firstSector),
	    readBuf + SectorToByte(raidPtr,
		    firstSector - FirstSectorOfStripe(raidPtr, firstSector)),
	    ctrlData,
	    (int) reqControlPtr->failedReqPtr->devReq.startAddress,
	    reqControlPtr->failedReqPtr->devReq.bufferLen);
    Raid_AddParityRangeRequest(reqControlPtr, raidPtr, FS_READ,
	    firstSector, parityBuf, ctrlData,
	    (int) reqControlPtr->failedReqPtr->devReq.startAddress,
	    reqControlPtr->failedReqPtr->devReq.bufferLen);
    switch (reqControlPtr->numFailed) {
    case 0:
	Raid_InitiateIORequests(stripeIOControlPtr->reqControlPtr,
		reconstructStripeReadDoneProc, (ClientData) stripeIOControlPtr);
	break;
    default:
	InitiateStripeIOFailure(stripeIOControlPtr);
	break;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * reconstructStripeReadDoneProc --
 *
 *	Callback procedure for InitiateReconstructRead.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
reconstructStripeReadDoneProc(stripeIOControlPtr, numFailed)
    RaidStripeIOControl	*stripeIOControlPtr;
    int			 numFailed;
{
    RaidBlockRequest	*failedReqPtr;
    failedReqPtr = stripeIOControlPtr->reqControlPtr->failedReqPtr;

    switch (numFailed) {
    case 0:
#ifndef NODATA
	bzero(failedReqPtr->devReq.buffer, failedReqPtr->devReq.bufferLen);
#endif
	Raid_XorRangeRequests(stripeIOControlPtr->reqControlPtr,
		stripeIOControlPtr->raidPtr, failedReqPtr->devReq.buffer,
		(int) failedReqPtr->devReq.startAddress,
		failedReqPtr->devReq.bufferLen);
	stripeIODoneProc(stripeIOControlPtr, numFailed);
	break;
    default:
	stripeIODoneProc(stripeIOControlPtr, numFailed);
	break;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * InitiateSingleStripeIO --
 *
 *	Initiates a single stripe IO request.
 *	Locks stripe, does IO and then unlocks the stripe in order to
 *	guarantee the consistency of parity.  (The unlocking is done in the
 *	associated callback procedure.)
 *	Calls doneProc with clientData, status and the amount transferreed
 *	as arguments when the IO is completed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *	Locks stripe.
 *
 *----------------------------------------------------------------------
 */

static void stripeIODoneProc();

static void 
InitiateSingleStripeIO(raidPtr, operation, firstSector, nthSector,
				buffer, doneProc, clientData, ctrlData)
    Raid       *raidPtr;
    int		operation;
    unsigned 	firstSector, nthSector;
    Address  	buffer;
    void      (*doneProc)();
    ClientData	clientData;
    int         ctrlData;
{
    RaidStripeIOControl	*stripeIOControlPtr;
    stripeIOControlPtr = Raid_MakeStripeIOControl(raidPtr, operation,
	    firstSector, nthSector, buffer, doneProc, clientData, ctrlData);

    switch (stripeIOControlPtr->operation) {
    case FS_READ:
	Raid_SLockStripe(raidPtr,
		SectorToStripeID(raidPtr, stripeIOControlPtr->firstSector));
	InitiateStripeRead(stripeIOControlPtr);
	break;
    case FS_WRITE:
	Raid_XLockStripe(raidPtr,
		SectorToStripeID(raidPtr, stripeIOControlPtr->firstSector));
	InitiateStripeWrite(stripeIOControlPtr);
	break;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * stripeIODoneProc --
 *
 *	Callback procedure for InitiateSingleStripeIO.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Unlocks stripe.
 *
 *----------------------------------------------------------------------
 */

static void
stripeIODoneProc(stripeIOControlPtr, numFailed)
    RaidStripeIOControl	*stripeIOControlPtr;
    int			 numFailed;
{
    if (stripeIOControlPtr->operation == FS_WRITE) {
	Raid_XUnlockStripe(stripeIOControlPtr->raidPtr,
		SectorToStripeID(stripeIOControlPtr->raidPtr,
		stripeIOControlPtr->firstSector));
    } else {
	Raid_SUnlockStripe(stripeIOControlPtr->raidPtr,
		SectorToStripeID(stripeIOControlPtr->raidPtr,
		stripeIOControlPtr->firstSector));
    }
    if (numFailed == 0) {
    	stripeIOControlPtr->doneProc(stripeIOControlPtr->clientData, SUCCESS, 
		SectorToByte(stripeIOControlPtr->raidPtr,
			stripeIOControlPtr->nthSector -
			stripeIOControlPtr->firstSector));
    } else {
    	stripeIOControlPtr->doneProc(stripeIOControlPtr->clientData, FAILURE,0);
    }
    Raid_FreeStripeIOControl(stripeIOControlPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Raid_InitiateStripeIOs --
 *
 *	Breaks IO requests into single stripe requests.
 *	Calls doneProc with clientData, status and the amount transferreed
 *	as arguments when the IO is completed.
 *
 * Results:
 *	The return code from queuing the I/O operation.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void singleStripeIODoneProc();

void 
Raid_InitiateStripeIOs(raidPtr, operation, firstSector, nthSector,
				buffer, doneProc, clientData, ctrlData)
    Raid       *raidPtr;
    int		operation;
    unsigned 	firstSector, nthSector;
    Address  	buffer;
    void      (*doneProc)();
    ClientData	clientData;
    int         ctrlData;
{
    RaidIOControl 	  *IOControlPtr;
    int 		   numSectorsToTransfer;
    unsigned 		   currentSector;

    /*
     * Break up IO request into stripe requests.
     */
    Raid_BeginUse(raidPtr);
    IOControlPtr = Raid_MakeIOControl(doneProc, clientData);
    IOControlPtr->raidPtr = raidPtr;
    IOControlPtr->numIO++;
    currentSector = firstSector;
    while ( currentSector < nthSector ) {
        numSectorsToTransfer = MIN( raidPtr->dataSectorsPerStripe -
                currentSector%raidPtr->dataSectorsPerStripe,
                nthSector - currentSector );

        MASTER_LOCK(&IOControlPtr->mutex);
	IOControlPtr->numIO++;
        MASTER_UNLOCK(&IOControlPtr->mutex);

        InitiateSingleStripeIO(raidPtr, operation,
                 currentSector, currentSector+numSectorsToTransfer, buffer,
                 singleStripeIODoneProc, (ClientData) IOControlPtr,
                 ctrlData);

        currentSector += numSectorsToTransfer;
	buffer += SectorToByte(raidPtr, numSectorsToTransfer);
    }

    MASTER_LOCK(&IOControlPtr->mutex);
    IOControlPtr->numIO--;
    if (IOControlPtr->numIO == 0) {
        MASTER_UNLOCK(&IOControlPtr->mutex);
        IOControlPtr->doneProc(IOControlPtr->clientData,
		IOControlPtr->status, IOControlPtr->amountTransferred);
	Raid_FreeIOControl(IOControlPtr);
	Raid_EndUse(IOControlPtr->raidPtr);
    } else {
        MASTER_UNLOCK(&IOControlPtr->mutex);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * singleStripeIODoneProc --
 *
 *	Callback procedure for Raid_InitiateStripeIOs.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
singleStripeIODoneProc(IOControlPtr, status, amountTransferred)
    RaidIOControl	*IOControlPtr;
    ReturnStatus  	 status;
    int		  	 amountTransferred;
{
    MASTER_LOCK(&IOControlPtr->mutex);
    IOControlPtr->amountTransferred += amountTransferred;

    /*
     * A Raid IO operation fails if any of the component operations fail.
     * Therefore, don't overwrite status if a previous operation has failed.
     */
    if (IOControlPtr->status == SUCCESS) {
        IOControlPtr->status = status;
    }

    /*
     * Check if all component IO's done.
     */
    IOControlPtr->numIO--;
    if (IOControlPtr->numIO == 0) {
        MASTER_UNLOCK(&IOControlPtr->mutex);
        IOControlPtr->doneProc(IOControlPtr->clientData,
		IOControlPtr->status, IOControlPtr->amountTransferred);
	Raid_FreeIOControl(IOControlPtr);
	Raid_EndUse(IOControlPtr->raidPtr);
    } else {
        MASTER_UNLOCK(&IOControlPtr->mutex);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Raid_InitiateSimpleStripeIOs --
 *
 *	Breaks up IO requests in stripes and then initiates them.
 *	This procedure is used when the RAID device is configured without
 *	parity.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The IO operation.
 *
 *----------------------------------------------------------------------
 */

static void simpleStripeIODoneProc();

void 
Raid_InitiateSimpleStripeIOs(raidPtr, operation, firstSector, nthSector,
				buffer, doneProc, clientData, ctrlData)
    Raid       *raidPtr;
    int		operation;
    unsigned 	firstSector, nthSector;
    Address  	buffer;
    void      (*doneProc)();
    ClientData	clientData;
    int         ctrlData;
{
    RaidIOControl 	  *IOControlPtr;
    DevBlockDeviceRequest *devReqPtr;
    int 		   numSectorsToTransfer;
    unsigned 		   currentSector;
    unsigned 		   diskSector;
    int			   col, row;

    /*
     * Break up entire IO request into stripe units.
     */
    IOControlPtr = Raid_MakeIOControl(doneProc, clientData);
    IOControlPtr->numIO++;
    currentSector = firstSector;
    while ( currentSector < nthSector ) {
        numSectorsToTransfer = MIN( raidPtr->sectorsPerStripeUnit -
                currentSector%raidPtr->sectorsPerStripeUnit,
                nthSector - currentSector );
	Raid_MapSector(raidPtr, currentSector, &col, &row, &diskSector);
	devReqPtr = Raid_MakeBlockDeviceRequest(raidPtr, operation,
		diskSector, numSectorsToTransfer, buffer,
		simpleStripeIODoneProc, (ClientData) IOControlPtr, ctrlData);

        MASTER_LOCK(&IOControlPtr->mutex);
	IOControlPtr->numIO++;
        MASTER_UNLOCK(&IOControlPtr->mutex);

        (void) Dev_BlockDeviceIO(raidPtr->disk[col][row]->handlePtr, devReqPtr);

        currentSector += numSectorsToTransfer;
	buffer += SectorToByte(raidPtr, numSectorsToTransfer);
    }

    MASTER_LOCK(&IOControlPtr->mutex);
    IOControlPtr->numIO--;
    if (IOControlPtr->numIO == 0) {
        MASTER_UNLOCK(&IOControlPtr->mutex);
        IOControlPtr->doneProc(IOControlPtr->clientData,
		IOControlPtr->status, IOControlPtr->amountTransferred);
	Raid_FreeIOControl(IOControlPtr);
    } else {
        MASTER_UNLOCK(&IOControlPtr->mutex);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * simpleStripeIODoneProc --
 *
 *	Callback procedure for Raid_InitiateSimpleStripeIOs.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Reports errors.
 *	Calls callback procedure.
 *
 *----------------------------------------------------------------------
 */

static void
simpleStripeIODoneProc(devReqPtr, status, amountTransferred)
    DevBlockDeviceRequest *devReqPtr;
    ReturnStatus  	 status;
    int		  	 amountTransferred;
{
    RaidIOControl	*IOControlPtr = (RaidIOControl *) devReqPtr->clientData;

    Raid_FreeBlockDeviceRequest(devReqPtr);

    MASTER_LOCK(&IOControlPtr->mutex);
    IOControlPtr->amountTransferred += amountTransferred;

    /*
     * A Raid IO operation fails if any of the component operations fail.
     * Therefore, don't overwrite status if a previous operation has failed.
     */
    if (IOControlPtr->status == SUCCESS) {
        IOControlPtr->status = status;
    }

    /*
     * Check if all component IO's done.
     */
    IOControlPtr->numIO--;
    if (IOControlPtr->numIO == 0) {
        MASTER_UNLOCK(&IOControlPtr->mutex);
        IOControlPtr->doneProc(IOControlPtr->clientData,
		IOControlPtr->status, IOControlPtr->amountTransferred);
	Raid_FreeIOControl(IOControlPtr);
    } else {
        MASTER_UNLOCK(&IOControlPtr->mutex);
    }
}
