/*
 * rpcCall.c --
 *
 *      These are the top-level routines for the client side of Remote
 *      Procedure Call - the routines do overhead tasks like setting up a
 *      message, and managing client channels.  The network protocol for
 *      the client side is in rpcClient.c.
 *
 * Copyright 1985 Regents of the University of California
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif /* not lint */


#include "sprite.h"

#include "rpc.h"
#include "rpcClient.h"
#include "rpcTrace.h"
#include "rpcHistogram.h"
#include "sys.h"
#ifdef NEG_ACK
#include "timerTick.h"
#include "timer.h"
#endif NEG_ACK

/*
 * So we can print out rpc names we include the rpcServer definitions.
 */
#include "rpcServer.h"

/*
 * The client channel table is kept as an array of pointers to channels.
 * rpcClient.h describes the contents of a ClientChannel.  The number
 * of channels limits the parallelism available on the client.  During
 * system shutdown, for example, there may be many processes doing
 * remote operations (removing swap files) at the same time.
 */

RpcClientChannel **rpcChannelPtrPtr = (RpcClientChannel **)NIL;
int		   rpcNumChannels = 8;
int		   numFreeChannels = 8;

/*
 * The allocation and freeing of channels is monitored.
 * A process might have to wait for a free RPC channel.
 */
Sync_Condition freeChannels;
Sync_Semaphore rpcMutex = Sync_SemInitStatic("Rpc:rpcMutex");

/*
 * There is sequence of rpc transaction ids that increases over time.
 */

unsigned int rpcID = 1;

/*
 * A boottime Id is used to help servers realize that a client has
 * rebooted since it talked to it last.  It is zero for the first
 * RPC, one that gets the time.  Then it is set to that time and does
 * not change until we reboot.
 */

unsigned int rpcBootID = 0;
/*
 * A count is kept of the number of RPCs made.
 * The zero'th element is used to count attempts at
 * RPCs with unknown RPC numbers - RPC number 0 is unused.
 */
int rpcClientCalls[RPC_LAST_COMMAND+1];

#ifdef NEG_ACK
typedef struct	UnhappyServer {
    int		serverID;
    Timer_Ticks	time;
} UnhappyServer;

UnhappyServer	serverAllocState[8];

unsigned int	channelStateInterval;
#define RPC_NACK_ERROR	0x3000b		/* move to status.h after testing */
    
#endif NEG_ACK


/*
 *----------------------------------------------------------------------
 *
 * Rpc_Call --
 *
 *      Top-level interface for a client that makes a remote procedure
 *      call.  Our caller has to pre-allocate all storage for the data
 *      going to the server in the request message, and for the data
 *      returning in the reply message.  The storage argument contains
 *      pointers and sizes for this preallocated space.  Upon return
 *	the data from the reply message will be in the specified storage
 *	areas.
 *
 * Results:
 *      An error code that either reflects an error in delivery/transport
 *      or is an error code from the remote procedure.  Also, the storage
 *      input specification is modified - the return parameter and data
 *      size fields are updated to reflect the true size of the return
 *      parameter and data blocks.  Finally, the return parameter and data
 *      areas contain the results of the remote procedure call.
 *
 * Side effects:
 *	There are no side effects on this machine except that some addressing
 *	information is kept as a hint for future RPCs.  The semantics of
 *	the remote procedure are unlimited on the server machine.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Rpc_Call(serverID, command, storagePtr)
    int serverID;		/* Indicates the server for the RPC.  The
				 * special value SERVER_BROADCAST is used
				 * to specify a broadcast RPC.  The first
				 * reply received is returned and subsequent
				 * replies get discarded.  Broadcast RPCs
				 * are NOT retried if there is no response. */
    int command;		/* Rpc command.  Values defined in rpcCall.h */
    Rpc_Storage *storagePtr;	/* Specifies buffer areas for request and
				 * reply messages. */
{
    register RpcClientChannel *chanPtr;	/* Handle for communication channel */
    register ReturnStatus error;	/* General error return status */
    Time histTime;			/* Time for histogram taking */
    unsigned	int srvBootID;		/* Boot time stamp from server, used to
					 * track server reboots */
    Boolean notActive = 0;		/* Not active flag from server */

    if (serverID < 0 || serverID >= NET_NUM_SPRITE_HOSTS) {
	printf("Rpc_Call, bad serverID <%d>\n", serverID);
	return(GEN_INVALID_ARG);
    } else if (serverID != RPC_BROADCAST_SERVER_ID &&
	       serverID == rpc_SpriteID) {
	printf("Rpc_Call: Trying RPC #%d to myself\n", command);
	return(GEN_INVALID_ARG);
    } else if ((serverID == RPC_BROADCAST_SERVER_ID) &&
	       ! (command == RPC_FS_PREFIX ||
		  command == RPC_GETTIME)) {
	panic("Trying to broadcast a non-prefix RPC");
	return(GEN_INVALID_ARG);
    }
#ifdef TIMESTAMP
    RPC_NIL_TRACE(RPC_CLIENT_A, "Rpc_Call");
#endif /* TIMESTAMP */

#ifdef NEG_ACK
allocAgain:
#endif
    RPC_CALL_TIMING_START(command, &histTime);
    chanPtr = RpcChanAlloc(serverID);

#ifdef TIMESTAMP
    RPC_NIL_TRACE(RPC_CLIENT_B, "alloc");
#endif /* TIMESTAMP */
    /*
     * Initialize the RPC request message header and put buffer
     * specifications of our caller into the state of the channel.  This
     * is once-per-rpc initialization that does not need to be repeated if
     * we have to re-send.
     */
    RpcSetup(serverID, command, storagePtr, chanPtr);

    /*
     * Update a histogram of RPCs made.  The zeroth element is used to
     * count unknown rpcs
     */
    if (command > 0 &&
	command <= RPC_LAST_COMMAND) {
	rpcClientCalls[command]++;
    } else {
	/*
	 */
	printf("Rpc_Call: unknown rpc command (%d)\n", command);
	rpcClientCalls[0]++;	/* 0 == RPC_BAD_COMMAND */
    }
#ifdef TIMESTAMP
    RPC_NIL_TRACE(RPC_CLIENT_C, "setup");
#endif /* TIMESTAMP */

    /*
     * Call RpcDoCall, which synchronizes with RpcClientDispatch,
     * to do the send-receive-timeout loop for the RPC.
     */
    error = RpcDoCall(serverID, chanPtr, storagePtr, command,
		      &srvBootID, &notActive);
    RpcChanFree(chanPtr);
    
#ifdef TIMESTAMP
    RPC_NIL_TRACE(RPC_CLIENT_OUT, "return");
#endif /* TIMESTAMP */

    RPC_CALL_TIMING_END(command, &histTime);
#ifdef NEG_ACK
    if (error == RPC_NACK_ERROR) {
	/* reroute to usable channel */
	/* Check how many channels for this server */
	/* reduce to one. */
	/* do something to be able to unreduce! */
	RpcSetChannelAllocState(serverID, TRUE);
	goto allocAgain;
    }
#endif NEG_ACK
#ifndef NO_RECOVERY
    if (error == RPC_TIMEOUT || error == NET_UNREACHABLE_NET) {
	if (command != RPC_ECHO_2) {
	    printf("<%s> ", rpcService[command].name);
	    Sys_HostPrint(serverID, "RPC timed-out\n");
	}
	Recov_HostDead(serverID);
    } else {
	Recov_HostAlive(serverID, srvBootID, TRUE, notActive);
    }
#endif /* NO_RECOVERY */
    return(error);
}

/*
 *----------------------------------------------------------------------
 *
 * RpcSetup --
 *
 *      Initialize the RPC header for the request message and set up the
 *      buffer specifications for the request and reply messages.  The
 *      operations done here are done once before the request message is
 *      sent out the first time, and they do not have to be re-done if the
 *      request message needs to be re-sent.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      The Rpc header is initialized.  The buffer specifications for the
 *      request and reply messages are set up.
 *
 *----------------------------------------------------------------------
 */
void
RpcSetup(serverID, command, storagePtr, chanPtr)
    int serverID;			/* The server for the RPC */
    int command;			/* The RPC to perform */
    register Rpc_Storage *storagePtr;	/* Specifies storage for the RPC
					 * parameters */
    register RpcClientChannel *chanPtr;	/* The channel for the RPC */
{
    register Net_ScatterGather *bufferPtr;	/* This specifies a part of the
						 * packet to the network driver
						 */
    register RpcHdr *rpcHdrPtr;			/* The RPC header */

    /*
     * Initialize the RPC header for the request message.  A couple fields
     * are set up elsewhere.  The server hint is left over from previous
     * RPCs.  The channel ID and the version number are set up at
     * boot time by Rpc_Init.
     */
    rpcHdrPtr = &chanPtr->requestRpcHdr;
    if(command == RPC_ECHO_1) {
	rpcHdrPtr->flags = RPC_ECHO;
    } else {
	rpcHdrPtr->flags = RPC_REQUEST;
    }
    rpcHdrPtr->flags |= RPC_SERVER;

    rpcHdrPtr->clientID = rpc_SpriteID;
    rpcHdrPtr->serverID = serverID;
    rpcHdrPtr->bootID = rpcBootID;
    rpcHdrPtr->ID = rpcID++;
    rpcHdrPtr->command = command;

    /*
     * Setup the timeout parameters depending on the route to the server.
     * This is rather simple minded.
     */
    if (chanPtr->constPtr == (RpcConst *)NIL) {
	Net_Route *routePtr = Net_IDToRoute(serverID);
	if (routePtr != (Net_Route *)NIL &&
	    routePtr->type == NET_ROUTE_INET) {
	    chanPtr->constPtr = &rpcInetConst;
	} else {
	    chanPtr->constPtr = &rpcEtherConst;
	}
    }
    /*
     * Copy buffer pointers into the state of the channel.
     */
    bufferPtr		= &chanPtr->request.paramBuffer;
    bufferPtr->bufAddr	= storagePtr->requestParamPtr;
    bufferPtr->length	= storagePtr->requestParamSize;

    rpcHdrPtr->paramSize = storagePtr->requestParamSize;

    bufferPtr		= &chanPtr->request.dataBuffer;
    bufferPtr->bufAddr	= storagePtr->requestDataPtr;
    bufferPtr->length	= storagePtr->requestDataSize;

    rpcHdrPtr->dataSize = storagePtr->requestDataSize;

    bufferPtr		= &chanPtr->reply.paramBuffer;
    bufferPtr->bufAddr	= storagePtr->replyParamPtr;
    bufferPtr->length	= storagePtr->replyParamSize;

    bufferPtr		= &chanPtr->reply.dataBuffer;
    bufferPtr->bufAddr	= storagePtr->replyDataPtr;
    bufferPtr->length	= storagePtr->replyDataSize;

    /*
     * Reset state about the reception of replies.
     */
    chanPtr->actualDataSize = 0;
    chanPtr->actualParamSize = 0;
    chanPtr->fragsReceived = 0;
    chanPtr->fragsDelivered = 0;
}
#ifdef DEBUG
#define CHAN_TRACESIZE 1000
#define INC(ctr) { (ctr) = ((ctr) == CHAN_TRACESIZE-1) ? 0 : (ctr)+1; }
typedef struct {
    RpcClientChannel	*chanPtr;
    char		*action;
    int			pNum;
    int			serverID;
    int			chanNum;
} debugElem;

static debugElem	debugArray[CHAN_TRACESIZE];
static int 		debugCtr;
#define CHAN_TRACE(channel, string) \
{	\
    debugElem *ptr = &debugArray[debugCtr];	\
    INC(debugCtr);	\
    ptr->chanPtr = (channel);	\
    ptr->action = string;	\
    ptr->chanNum = (channel)->index;	\
    ptr->serverID = (channel)->serverID;	\
    ptr->pNum = Mach_GetProcessorNumber();	\
}	
#else
#define CHAN_TRACE(channel, string)
#endif

#ifdef NEG_ACK

/*
 *----------------------------------------------------------------------
 *
 * RpcGetChannelAllocState --
 *
 *	Get the state of channel allocation in regards to a certain server.
 *
 * Results:
 *	True if the server is marked as being congested.  False otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
Boolean
RpcGetChannelAllocState(serverID, time)
    int		serverID;
    Timer_Ticks	*time;
{
    int		i;
    Boolean	found = FALSE;

    for (i = 0; i < (sizeof (serverAllocState) / sizeof (UnhappyServer)); i++) {
	if (serverAllocState[i].serverID == serverID) {
	    found = TRUE;
	    break;
	}
    }
    if (!found) {
	return FALSE;
    }
    *time = serverAllocState[i].time;
    return TRUE;
}
    

/*
 *----------------------------------------------------------------------
 *
 * RpcSetChannelAllocState --
 *
 *	Set the state of channel allocation in regards to a certain server.
 *	If we've been getting "noAllocs" back from a server, we want to
 *	ramp down our use of it by using fewer client channels with it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
RpcSetChannelAllocState(serverID, trouble)
    int		serverID;
    Boolean	trouble;
{
    int		i;
    Boolean	found = FALSE;

    for (i = 0; i < (sizeof (serverAllocState) / sizeof (UnhappyServer)); i++) {
	if (serverAllocState[i].serverID == serverID) {
	    found = TRUE;
	    break;
	}
    }
    if (!found && !trouble) {
	/* server isn't already marked as being in trouble. */
	return;
    }
    if (!found) {
	found = FALSE;
	for (i = 0; i < (sizeof (serverAllocState) / sizeof (UnhappyServer));
		i++) {
	    if (serverAllocState[i].serverID == -1) {
		found = TRUE;
		break;
	    }
	}
	if (!found) {
	    /* No more spaces to mark unhappy server! */
	    printf("RpcSetChannelAllocState: %s\n",
		    "No more room to keep track of congested servers.");
	    return;
	}
    }
    if (trouble) {
	Timer_GetCurrentTicks(&(serverAllocState[i].time));
	serverAllocState[i].serverID = serverID;
    } else {
	serverAllocState[i].serverID = -1;
    }

    return;
}
#endif NEG_ACK

/*
 *----------------------------------------------------------------------
 *
 * RpcChanAlloc --
 *
 *      Allocate a channel for an RPC.  A pointer to the channel is
 *      returned.  The allocation is done on the basis of the server
 *      machine involved.  The goal is to send a long series of RPC
 *      requests to the same server over the same channel.  To that end a
 *      channel is chosen first if its cached address matches the input
 *      server address.  Second choice is a previously unused channel,
 *      lastly we re-use a channel that has been used with a different server.
 *
 * Results:
 *	A pointer to the channel.  This returns a good pointer, or it panics.
 *
 * Side effects:
 *	The channel is dedicated to the RPC until the caller frees
 *	the channel with RpcChanFree.
 *
 *----------------------------------------------------------------------
 */
ENTRY RpcClientChannel *
RpcChanAlloc(serverID)
    int serverID;	/* Server ID to base our allocation on. */
{
    register RpcClientChannel *chanPtr;	/* The channel we allocate */
    register int i;			/* Index into channel table */
    register int firstUnused = -1;	/* The first unused channel */
#ifdef NEG_ACK
    int firstBusy = -1;			/* The first busy channel for server */
    int firstFreeMatch = -1;		/* The first chan free for server */
    Timer_Ticks	time;			/* When server channel state set. */
    Timer_Ticks	currentTime;		/* Current ticks. */
    Boolean	result;			/* Result of function call. */
#endif NEG_ACK
    register int firstFree = -1;	/* The first chan used but now free */

    MASTER_LOCK(&rpcMutex);
#ifdef NEG_ACK
waitForBusyChannel:
#endif NEG_ACK
    while (numFreeChannels < 1) {
	rpcCltStat.chanWaits++;
	Sync_MasterWait(&freeChannels, &rpcMutex, FALSE);
    }

#ifdef NEG_ACK
    result = RpcGetChannelAllocState(serverID, &time);
    if (result) {
	Timer_AddIntervalToTicks(time, channelStateInterval, &time);
	Timer_GetCurrentTicks(&currentTime);
    }
    if (result && (Timer_TickGE(time, currentTime))) {
	/*
	 * Server is congested, so ramp down use of channels.
	 * If there's a channel, free or busy, with the server we want,
	 * wait for it.  Make sure it's not stuck busy or something,
	 * or put a retry limit on this.  Set something up so we know
	 * we've done this with this server so that we know to undo it.
	 * If no channel for this server, take free one.
	 */
	for (i=0 ; i<rpcNumChannels ; i++) {
	    chanPtr = rpcChannelPtrPtr[i];
	    if (serverID == chanPtr->serverID) {
		if (chanPtr->state == CHAN_FREE) {
		    if (firstFreeMatch < 0) {
			firstFreeMatch = i;
		    }
		} else {
		    if (firstBusy < 0) {
			firstBusy = i;
		    }
		}
	    } else if (chanPtr->serverID == -1) {
		if (firstUnused < 0) {
		    firstUnused = i;
		}
	    } else {
		if (chanPtr->state == CHAN_FREE) {
		    if (firstFree < 0)  {
			firstFree = i;
		    }
		}
	    }
	}
	if (firstFreeMatch < 0 && firstBusy < 0) {
	    /* No channel for our serverID - so create one. */
	    /* Fall through in case of Free or Unused */
	} else if (firstFreeMatch >= 0) {
	    if (firstBusy == -1 || firstBusy > firstFreeMatch) {
		/* got it. */
		goto found;
	    } else {
		/* We need to wait for busy one to free up. */
		goto waitForBusyChannel;
	    }
	} else {
	    /*
	     * Busy one, wait for it to free up.  What will we do if it
	     * never frees up?
	     */
	    goto waitForBusyChannel;
	}
	/* Fall through in case of Free or Unused */
    } else {
	if (result) {
	    /* Mark server as okay now. */
	    RpcSetChannelAllocState(serverID, FALSE);
	}
	/* use regular alloc */
	
#endif NEG_ACK

	for (i=0 ; i<rpcNumChannels ; i++) {
	    chanPtr = rpcChannelPtrPtr[i];
	    if (chanPtr->state == CHAN_FREE) {
		if (chanPtr->serverID == -1) {
		    /*
		     * Remember the first unused channel.
		     */
		    if (firstUnused < 0) {
			firstUnused = i;
		    }
		} else if (serverID == chanPtr->serverID) {
		    /*
		     * Agreement between the channels old server and the
		     * server ID.  By reusing this channel we hope to give
		     * the server an implicit acknowledgment for the
		     * previous transaction.
		     */
		    rpcCltStat.chanHits++;
		    CHAN_TRACE(chanPtr, "alloc channel w/ same server");
		    goto found;
		} else if (firstFree < 0) {
		    /*
		     * The first free channel (with some server) is less of
		     * a good candidate for allocation.
		     */
		    firstFree = i;
		}
	    }
	}
#ifdef NEG_ACK
    }
#endif NEG_ACK
    /*
     * We didn't find an address match on a free channel, so we use
     * the first previously unused channel or the first used but free channel.
     */
    if (firstUnused >= 0) {
	rpcCltStat.chanNew++;
	chanPtr = rpcChannelPtrPtr[firstUnused];
	CHAN_TRACE(chanPtr, "alloc first unused");
    } else if (firstFree >= 0) {
	rpcCltStat.chanReuse++;
	chanPtr = rpcChannelPtrPtr[firstFree];
	CHAN_TRACE(chanPtr, "alloc first free");
    } else {
	panic("Rpc_ChanAlloc can't find the free channel.\n");
    }
    chanPtr->serverID = serverID;
    chanPtr->constPtr = (RpcConst *)NIL;	/* Set in RpcSetup */

found:
    chanPtr->state = CHAN_BUSY;
    numFreeChannels--;

    MASTER_UNLOCK(&rpcMutex);
    return(chanPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * RpcChanFree --
 *
 *	Free an RPC channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The channel is available for other RPC calls.
 *
 *----------------------------------------------------------------------
 */
ENTRY void
RpcChanFree(chanPtr)
    RpcClientChannel *chanPtr;		/* The channel to free */
{

    MASTER_LOCK(&rpcMutex);
    CHAN_TRACE(chanPtr, "free channel");
    if (chanPtr->state == CHAN_FREE) {
	panic("Rpc_ChanFree: freeing free channel\n");
    }
    chanPtr->state = CHAN_FREE;

    numFreeChannels++;
    if (numFreeChannels == 1) {
	rpcCltStat.chanBroads++;
	Sync_MasterBroadcast(&freeChannels);
    }
    
    MASTER_UNLOCK(&rpcMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * RpcChanClose --
 *
 *	Process a close request from a server. This is called from
 *	RpcClientDispatch at interrupt level.  If the channel is not
 *	busy then the interrupt handler
 *	needs to mark the channel as busy momentarily and send an
 *	ack to the server. See RpcClientDispatch for more details.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The channel might be used to send an ack to the server.
 *
 *----------------------------------------------------------------------
 */

void
RpcChanClose(chanPtr,rpcHdrPtr)
    register RpcClientChannel *chanPtr;	/* The channel to use.*/
    register RpcHdr *rpcHdrPtr;		/* The Rpc header as it sits in the
					 * network module's buffer.  The data
					 * in the message follows this. */
{
    register RpcHdr *ackHdrPtr;
    if ((chanPtr->state & CHAN_BUSY) == 0) {
	MASTER_LOCK(&rpcMutex);
	/*
	 * Check again to make sure the channel isn't busy,
	 * then temporarily allocate it while we issue the explicit ack.
	 * If a process has slipped in and allocated the channel then its
	 * request will serve as the acknowledgement and we can bail out.
	 */
	if ((chanPtr->state & CHAN_BUSY) != 0) {
	    MASTER_UNLOCK(&rpcMutex);
	    return;
	}
	chanPtr->state |= CHAN_BUSY;
	numFreeChannels--;
	MASTER_UNLOCK(&rpcMutex);

	rpcCltStat.close++;
	/*
	 * Set up and transmit the explicit acknowledgement packet.
	 * Note that fields that never change have already been
	 * set in RpcBufferInit.
	 */
	ackHdrPtr = &chanPtr->ackHdr;
	ackHdrPtr->flags = RPC_ACK | RPC_CLOSE | RPC_SERVER;
	ackHdrPtr->clientID = rpc_SpriteID;
	ackHdrPtr->serverID = rpcHdrPtr->serverID;
	ackHdrPtr->serverHint = rpcHdrPtr->serverHint;
	ackHdrPtr->command = rpcHdrPtr->command;
	ackHdrPtr->bootID = rpcBootID;
	ackHdrPtr->ID = rpcHdrPtr->ID;
	(void)RpcOutput(rpcHdrPtr->serverID, &chanPtr->ackHdr, &chanPtr->ack,
			     (RpcBufferSet *)NIL, 0, (Sync_Semaphore *)NIL);
	/*
	 * Note that the packet can linger in the network output queue,
	 * so we are relying on the fact that it would only be reused
	 * for another ack in the worst case.
	 */

	MASTER_LOCK(&rpcMutex);
	chanPtr->state &= ~CHAN_BUSY;
	numFreeChannels++;
	MASTER_UNLOCK(&rpcMutex);
    }
}

#ifdef NEG_ACK

/*
 *----------------------------------------------------------------------
 *
 * RpcInitServerChannelState --
 *
 *	description.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
RpcInitServerChannelState()
{
    int		i;

    for (i = 0; i < (sizeof (serverAllocState) / sizeof (UnhappyServer)); i++) {
	serverAllocState[i].serverID = -1;
	serverAllocState[i].time = timer_TicksZeroSeconds;
    }
    channelStateInterval = timer_IntOneSecond * 10;
}
#endif NEG_ACK
