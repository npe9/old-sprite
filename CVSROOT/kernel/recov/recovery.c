/* 
 * recovery.c --
 *
 *	The routines here maintain up/down state about other hosts.
 *	Recovery actions that are registered via Recov_HostNotify are
 *	called-back when a host crashes and when it reboots.
 *	Regular message	traffic plus explicit pings are used to determine
 *	the state of other hosts.  The external procedures are
 *	Recov_HostAlive and Recov_HostDead, used by RPC to tell us when
 *	a messages have arrived, or if transactions have timed out.
 *	Recov_IsHostDown, used to query the state of another host, and
 *	Recov_WaitForHost, used to block a process until a host reboots.
 *	(Recov_WaitForHost isn't used much.  Instead, modules rely on the
 *	recovery callbacks to indicate that a host is back to life, and
 *	they block processes in their own way.)
 *
 *	Note: A synchronization hook is provided by Recov_HostAlive;  its
 *	caller can be blocked if crash recovery actions are in progress.
 *
 * Copyright 1987 Regents of the University of California
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint


#include "sprite.h"
#include "recov.h"
#include "sync.h"
#include "net.h"
#include "rpc.h"
#include "hash.h"
#include "mem.h"
#include "trace.h"

/*
 * The state of other hosts is kept in a hash table keyed on SpriteID.
 * This state is maintained by Recov_HostAlive and Recov_HostDead, which are
 * called in turn after packet reception or RPC timeout, respectively.
 * Recov_HostDead is also called by the Rpc_Daemon if it can't get an
 * explicit acknowledgment from a client.
 */
static Hash_Table	recovHashTableStruct;
static Hash_Table	*recovHashTable = &recovHashTableStruct;

typedef struct RecovHostState {
    int			state;		/* flags defined below */
    int			clientState;	/* flags defined in recov.h */
    int			spriteID;	/* Sprite Host ID */
    int			bootID;		/* Timestamp from RPC header */
    Time		time;		/* Time of last message */
    Sync_Condition	alive;		/* Notified when host comes up */
    Sync_Condition	recovery;	/* Notified when recovery is complete */
} RecovHostState;

/*
 * Access to the hash table is monitored.
 */
static Sync_Lock recovLock;
#define LOCKPTR (&recovLock)

/*
 * Host state:
 *	RECOV_STATE_UNKNOWN	Initial state.
 *	RECOV_HOST_ALIVE	Set when we receive a message from the host
 *	RECOV_HOST_DEAD		Set when an RPC times out.
 *
 *	RECOV_CRASH_CALLBACKS	Set during the crash call-backs, this is used
 *				to block RPC server processes until the
 *				crash recovery actions have completed.
 *	RECOV_HOST_PINGING	Set while there are pinging call-backs scheduled
 *	RECOV_REBOOT_CALLBACKS	Set while reboot callbacks are pending.	
 *
 *	RECOV_WAITING		artificial state to trace Rpc_WaitForHost
 *	RECOV_CRASH		artificial state to trace RecovCrashCallBacks
 *	RECOV_REBOOT		artificial state to trace RecovRebootCallBacks
 */
#define RECOV_STATE_UNKNOWN	0x0
#define RECOV_HOST_ALIVE	0x1
#define RECOV_HOST_DEAD		0x2

#define RECOV_CRASH_CALLBACKS	0x0100
#define RECOV_HOST_PINGING	0x0200
#define RECOV_REBOOT_CALLBACKS	0x0400

#define RECOV_WAITING		0x4
#define RECOV_CRASH		0x8
#define RECOV_REBOOT		0x10

/*
 * A host is "pinged" (to see when it reboots) at an interval determined by
 * rpcPingSeconds.
 */
int recovPingSeconds = 30;

/*
 * After a host reboots we pause a bit before attempting recovery.  This
 * allows a host to complete boot-time start up.  If we don't pause the
 * ping done by the recovery call backs may fail and we may erroneously
 * think that the other guy crashed right away.
 */
int recovPause = 30;	/* Seconds */

/*
 * Other kernel modules can arrange call-backs when a host reboots.
 * The following list structure is used to keep these.  The calling
 * sequence of the callback is as follows:
 *	(*proc)(spriteID, clientData, when)
 * where 'when' is RECOV_WHEN_HOST_DOWN or RECOV_WHEN_HOST_REBOOTS (never both).
 */

typedef struct {
    List_Links	links;
    void	(*proc)();
    int		flags;	/* RECOV_WHEN_HOST_DOWN, RECOV_WHEN_HOST_REBOOTS */
    ClientData	clientData;
} NotifyElement;

List_Links	recovNotifyList;

/*
 * A trace is kept for debugging/understanding the host state transisions.
 */
typedef struct RecovTraceRecord {
    int		spriteID;		/* Host ID whose state changed */
    int		state;			/* Their new state */
} RecovTraceRecord;

/*
 * Tracing events, these describe the trace record.  Note that some
 *	trace types are defined in rpc.h for use with Rpc_HostTrace.
 *
 *	RECOV_CUZ_WAIT		Wait in Rpc_WaitForHost
 *	RECOV_CUZ_WAKEUP	Wakeup in Rpc_WaitForHost
 *	RECOV_CUZ_INIT		First time we were interested in the host
 *	RECOV_CUZ_REBOOT	We detected a reboot
 *	RECOV_CUZ_CRASH		We detected a crash
 *	RECOV_CUZ_DONE		Recovery actions completed
 *	RECOV_CUZ_PING_CHK	We are pinging the host to check it out
 *	RECOV_CUZ_PING_ASK	We are pinging the host because we were asked
 */
#define RECOV_CUZ_WAIT		0x1
#define RECOV_CUZ_WAKEUP	0x2
#define RECOV_CUZ_INIT		0x4
#define RECOV_CUZ_REBOOT	0x8
#define RECOV_CUZ_CRASH		0x10
#define RECOV_CUZ_DONE		0x20
#define RECOV_CUZ_PING_CHK	0x40
#define RECOV_CUZ_PING_ASK	0x80

Trace_Header recovTraceHdr;
Trace_Header *recovTraceHdrPtr = &recovTraceHdr;
int recovTraceLength = 50;
Boolean recovTracing = TRUE;

#ifndef CLEAN

#define RECOV_TRACE(zspriteID, zstate, event) \
    if (recovTracing) {\
	RecovTraceRecord rec;\
	rec.spriteID = zspriteID;\
	rec.state = zstate;\
	Trace_Insert(recovTraceHdrPtr, event, &rec);\
    }
#else

#define RECOV_TRACE(zspriteID, zstate, event)

#endif not CLEAN
/*
 * Forward declarations.
 */
void RecovRebootCallBacks();
void RecovCrashCallBacks();
void CallBacksDone();
void MarkRecoveryComplete();
int  GetHostState();
void StartPinging();
void CheckHost();
void StopPinging();


/*
 *----------------------------------------------------------------------
 *
 * Recov_Init --
 *
 *	Set up the data structures used by the recovery module.
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
Recov_Init()
{
    Hash_Init(recovHashTable, 8, HASH_ONE_WORD_KEYS);
    List_Init(&recovNotifyList);
    Trace_Init(recovTraceHdrPtr, recovTraceLength,
		sizeof(RecovTraceRecord), 0);
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_Notify --
 *
 *	Add a call-back for other modules to use when a host crashes/reboots.
 *	The 'when' parameter specifies when to callback the client procedure.
 *	If RECOV_WHEN_HOST_DOWN then the procedure is called when the RPC
 *	module has gotten a timeout trying to reach the host.  If it is
 *	RECOV_WHEN_HOST_REBOOTS then the call-back is made when the RPC
 *	module detects a reboot due to the bootID changing.  If both
 *	are specified then the call-back is made at both times.
 *	
 * Results:
 *	None.
 *
 * Side effects:
 *	Entry added to notify list.
 *
 *----------------------------------------------------------------------
 */
void
Recov_Notify(proc, clientData, when)
    void	(*proc)();
    ClientData	clientData;
    int		when;	/* RECOV_WHEN_HOST_DOWN, RECOV_WHEN_HOST_REBOOTS */
{
    register	NotifyElement	*notifyPtr;

    notifyPtr = (NotifyElement *) Mem_Alloc(sizeof(NotifyElement));
    notifyPtr->proc = proc;
    notifyPtr->clientData = clientData;
    notifyPtr->flags = when;
    List_InitElement((List_Links *) notifyPtr);
    List_Insert((List_Links *) notifyPtr, LIST_ATREAR(&recovNotifyList));
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_IsHostDown --
 *
 *	This decides if the specified host is down, and will make sure
 *	that the host is being "pinged" if the caller wants to find
 *	out (via the callbacks setup in Recov_HostNotify) when the host
 *	comes back to life.  If the host is known to be down this routine
 *	returns TRUE and makes sure pinging is initiated (if needed).
 *	Otherwise, if there hasn't been recent message traffic 
 *	(within the last 10 seconds) then this will ping the host to find
 *	out if it's still up.  There are two cases then, the host isn't
 *	up, or it is booting but it's RPC service is not ready yet.
 *	We return FALSE so that our caller doesn't think the host
 *	has crashed
 *
 * Results:
 *	SUCCESS if the host is up, FAILURE if it doesn't respond to
 *	pings or is known to be down, and RPC_SERVICE_DISABLED if
 *	the host says so.
 *
 * Side effects:
 *	May do a ping.  If the 'ping' parameter is TRUE this will make
 *	sure that pinging is in progress if the host is down.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Recov_IsHostDown(spriteID, ping)
    int spriteID;
    Boolean ping;	/* If TRUE, we make sure the host is being pinged
			 * if it is down now */
{
    register ReturnStatus status = SUCCESS;

    if (spriteID == NET_BROADCAST_HOSTID) {
	Sys_Panic(SYS_WARNING, "Recov_IsHostDown, got broadcast address\n");
	return(SUCCESS);
    }
    switch (GetHostState(spriteID)) {
	case RECOV_STATE_UNKNOWN:
	    RECOV_TRACE(spriteID, RECOV_STATE_UNKNOWN, RECOV_CUZ_PING_ASK);
	    status = Rpc_Ping(spriteID);
	    break;
	case RECOV_HOST_ALIVE:
	    status = SUCCESS;
	    break;
	case RECOV_HOST_DEAD:
	    status = FAILURE;
	    break;
    }
    if (status != SUCCESS && ping) {
	StartPinging(spriteID);
    }
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_Pending --
 *
 *	This returns TRUE if the reboot call-backs for this host are
 *	scheduled but haven't happened yet.  This is used by other modules
 *	to decide if they should try recovery actions now, or wait until
 *	regularly scheduled recovery call-backs are made.  If the other
 *	host is not up now this returns TRUE.
 *	
 *
 * Results:
 *	TRUE if recovery actions are scheduled for this host.
 *
 * Side effects:
 *	This will start pinging if the host is currently thought down.
 *
 *----------------------------------------------------------------------
 */

ENTRY Boolean
Recov_Pending(spriteID)
    int spriteID;
{
    Hash_Entry *hashPtr;
    RecovHostState *hostPtr;
    Boolean pending = FALSE;

    LOCK_MONITOR;

    if (spriteID <= 0 || spriteID == rpc_SpriteID) {
	Sys_Panic(SYS_FATAL, "Recov_Pending, bad hostID %d\n", spriteID);
    } else {
	hashPtr = Hash_LookOnly(recovHashTable, spriteID);
	if (hashPtr != (Hash_Entry *)NIL) {
	    hostPtr = (RecovHostState *)hashPtr->value;
	    if (hostPtr != (RecovHostState *)NIL) {
		pending = (hostPtr->state & RECOV_REBOOT_CALLBACKS);
		if (hostPtr->state & RECOV_HOST_DEAD) {
		    pending = TRUE;
		    if ((hostPtr->state & RECOV_HOST_PINGING) == 0) {
			hostPtr->state |= RECOV_HOST_PINGING;
			Proc_CallFunc(CheckHost, spriteID, 0);
		    }
		}
	    }
	}
    }
    UNLOCK_MONITOR;
    return(pending);
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_WaitForHost --
 *
 *	Block the current process (at an interruptable priority) until
 *	the given host comes back up.  This is used when retrying
 *	filesystem operations when a fileserver goes down, for example.
 *
 * Results:
 *	TRUE if the wait was interrupted.
 *
 * Side effects:
 *	The current process is blocked
 *	until messages from the host indicate it is up.
 *
 *----------------------------------------------------------------------
 */
Boolean
Recov_WaitForHost(spriteID)
    int spriteID;			/* Host to monitor */
{
    /*
     * Set up the hosts state (dead or alive) by pinging it.
     * If it's down we drop into a monitored routine to do 
     * the actual waiting.  It will check again to make sure
     * we don't sleep on an alive host.
     */
    if (Recov_IsHostDown(spriteID, TRUE) == FAILURE) {
	RECOV_TRACE(spriteID, RECOV_STATE_UNKNOWN, RECOV_CUZ_WAIT);
	return(WaitForHostInt(spriteID));
    } else {
	return(FALSE);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * WaitForHostInt --
 *
 *	Block the current process (at an interruptable priority) until
 *	the given host comes back up.  Our caller should have already
 *	probed to host with Recov_IsHostDown so that pinging is already
 *	initiated.
 *
 * Results:
 *	TRUE is the wait was interrupted by a signal.
 *
 * Side effects:
 *	If the host is thought down, the current process is blocked
 *	until messages from the host indicate it is up.
 *
 *----------------------------------------------------------------------
 */
static ENTRY Boolean
WaitForHostInt(spriteID)
    int spriteID;			/* Host to monitor */
{
    Hash_Entry *hashPtr;
    RecovHostState *hostPtr;
    Boolean sigPending = FALSE;

    LOCK_MONITOR;

    if (spriteID <= 0 || spriteID == rpc_SpriteID) {
	Sys_Panic(SYS_FATAL, "WaitForHostInt, bad hostID %d\n", spriteID);
	UNLOCK_MONITOR;
	return(FALSE);
    }

    hashPtr = Hash_Find(recovHashTable, spriteID);
    if (hashPtr->value == (Address)NIL) {
	Sys_Panic(SYS_FATAL, "WaitForHostInt, no host state\n");
	UNLOCK_MONITOR;
	return(FALSE);
    } else {
	hostPtr = (RecovHostState *)hashPtr->value;
    }
    while (!sigPending && (hostPtr->state & RECOV_HOST_DEAD)) {
	sigPending = Sync_Wait(&hostPtr->alive, TRUE);
    }
    RECOV_TRACE(hostPtr->spriteID, hostPtr->state, RECOV_CUZ_WAKEUP);

    UNLOCK_MONITOR;
    return(sigPending);
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_HostTrace --
 *
 *	Add an entry to the rpc recovery trace.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Recov_HostTrace(spriteID, event)
    int spriteID;
    int event;
{
    LOCK_MONITOR;

    RECOV_TRACE(spriteID, RECOV_STATE_UNKNOWN, event);

    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_GetClientState --
 *
 *	Return the client state associated with a host.  The recovery host
 *	table is a convenient object keyed on spriteID.  Other modules can
 *	set their own state in the table (beyond the simple up/down state
 *	mainted by the rest of this module), and retrieve it with this call.
 *
 * Results:
 *	A copy of the clientState field.  0 is returned if there is no
 *	host table entry.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ENTRY int
Recov_GetClientState(spriteID)
    int spriteID;
{
    Hash_Entry *hashPtr;
    RecovHostState *hostPtr;
    int stateBits = 0;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    if (hashPtr != (Hash_Entry *)NIL) {
	hostPtr = (RecovHostState *)hashPtr->value;
	if (hostPtr != (RecovHostState *)NIL) {
	    stateBits = hostPtr->clientState;
	}
    }
    UNLOCK_MONITOR;
    return(stateBits);
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_SetClientState --
 *
 *	Set a client state bit.  This or's the parameter into the
 *	client state word.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets bits in the clientState field of the host state.  This will add
 *	an entry to the host table if one doesn't alreay exist.  Its RPC
 *	up/down state is set to "unknown" in this case.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Recov_SetClientState(spriteID, stateBits)
    int spriteID;
    int stateBits;
{
    Hash_Entry *hashPtr;
    RecovHostState *hostPtr;

    LOCK_MONITOR;

    hashPtr = Hash_Find(recovHashTable, spriteID);
    hostPtr = (RecovHostState *)hashPtr->value;
    if (hostPtr == (RecovHostState *)NIL) {
	hostPtr = Mem_New(RecovHostState);
	hashPtr->value = (Address)hostPtr;

	Byte_Zero(sizeof(RecovHostState), (Address)hostPtr);
	hostPtr->state = RECOV_STATE_UNKNOWN;
	hostPtr->spriteID = spriteID;
    }
    hostPtr->clientState |= stateBits;
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_ClearClientState --
 *
 *	Clear a client state bit.  .
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Clears bits in the clientState field of the host state.  This does
 *	nothing if the state doesn't exist.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Recov_ClearClientState(spriteID, stateBits)
    int spriteID;
    int stateBits;
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    if (hashPtr != (Hash_Entry *)NIL) {
	hostPtr = (RecovHostState *)hashPtr->value;
	if (hostPtr != (RecovHostState *)NIL) {
	    hostPtr->clientState &= ~stateBits;
	}
    }
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_HostAlive --
 *
 *	Mark the host as being alive.  This is called when we've received
 *	a message from the host.  It uses state from the host table and
 *	the bootID parameter to detect reboots.  If a reboot is detected
 *	but we thought the host was up then the Crash call-backs are invoked.
 *	In any case, a reboot invokes the Reboot call-backs.  (Call-backs
 *	are installed with Recov_Notify.)  Finally, a time stamp is
 *	kept so we can check when we last got a message from a host.
 *
 *	This procedure is called from client RPC upon successful completion
 *	of an RPC, and by server RPC upon reciept of a client request.
 *	These two cases are identified by the 'asyncRecovery' parameter.
 *	Servers want synchronous recovery so they don't service anything
 *	until state associated with that client has been cleaned up via
 *	the Crash call-backs.  So Recov_HostAlive blocks (if !asyncRecovery)
 *	until the crash call-backs are complete.  Clients don't have the
 *	same worries so they let the crash call-backs complete in the
 *	background (asyncRecovery is TRUE).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Updates the boot timestamp of the other host.  Procedures installed
 *	with Rpc_HostNotify are called when the bootID changes.  A timestamp
 *	of when this message was received is obtained from the "cheap" clock
 *	so we can tell later if there has been recent message traffic.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Recov_HostAlive(spriteID, bootID, asyncRecovery)
    int spriteID;		/* Host ID of the message sender */
    int bootID;			/* Boot time stamp from message header */
    Boolean asyncRecovery;	/* TRUE means do recovery call-backs in
				 * the background. FALSE causes the process
				 * to wait until crash recovery is complete. */
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;
    Boolean reboot = FALSE;	/* Used to control print statements at reboot */

    LOCK_MONITOR;
    if (spriteID == NET_BROADCAST_HOSTID || bootID == 0) {
	/*
	 * Don't track the broadcast address.  Also ignore zero valued
	 * bootIDs.  These come from hosts at early boot time, or
	 * in certain error conditions like trying to send too much
	 * data in a single RPC.
	 */
	UNLOCK_MONITOR;
	return;
    }

    hashPtr = Hash_Find(recovHashTable, spriteID);
    if (hashPtr->value == (Address)NIL) {
	/*
	 * Initialize the host's state. This is the first time we've talked
	 * to it since we've been up, so take no action.
	 */
	hostPtr = Mem_New(RecovHostState);
	hashPtr->value = (Address)hostPtr;

	Byte_Zero(sizeof(RecovHostState), (Address)hostPtr);
	hostPtr->state = RECOV_HOST_ALIVE;
	hostPtr->spriteID = spriteID;
	hostPtr->bootID = bootID;

	Net_HostPrint(spriteID, "is up");
	RECOV_TRACE(spriteID, RECOV_HOST_ALIVE, RECOV_CUZ_INIT);
    } else {
	hostPtr = (RecovHostState *)hashPtr->value;
    }
    if (hostPtr != (RecovHostState *)NIL) {
	/*
	 * Have to read the clock in order to suppress repeated pings,
	 * see GetHostState and Rpc_HostIsDown.
	 */
	Timer_GetTimeOfDay(&hostPtr->time, (int *)NIL, (Boolean *)NIL);
	/*
	 * Check for a rebooted peer by comparing boot time stamps.
	 * The first process to detect this initiates recovery actions.
	 */
	if (hostPtr->bootID != bootID) {
	    Net_HostPrint(spriteID, "rebooted");
	    hostPtr->bootID = bootID;
	    reboot = TRUE;
	    RECOV_TRACE(spriteID, hostPtr->state, RECOV_CUZ_REBOOT);
	    if (hostPtr->state & RECOV_HOST_ALIVE) {
		/*
		 * A crash occured un-detected.  We do the crash call-backs
		 * first, and block server processes in the meantime.
		 * RECOV_CRASH_CALLBACKS flag is reset by RecovCrashCallBacks.
		 * The host is marked dead here so we fall into the
		 * switch below and call the reboot callbacks.
		 */
		RECOV_TRACE(spriteID, RECOV_CRASH, RECOV_CUZ_REBOOT);
		hostPtr->state &= ~RECOV_HOST_ALIVE;
		hostPtr->state |= (RECOV_HOST_DEAD | RECOV_CRASH_CALLBACKS);
		Proc_CallFunc(RecovCrashCallBacks, spriteID, 0);
	    }
	}
	/*
	 * Block servers until crash recovery actions complete.
	 * Servers are synchronous with respect to reboot recovery.
	 * This blocks requests from clients until after the
	 * recovery actions complete.
	 */
	if (! asyncRecovery) {
	    while (hostPtr->state & RECOV_CRASH_CALLBACKS) {
		Sync_Wait(&hostPtr->recovery, FALSE);
		if (sys_ShuttingDown) {
		    Sys_Printf("Warning, Server exiting Recov_HostAlive\n");
		    Proc_Exit(1);
		}
	    }
	}
	/*
	 * Now that we've taken care of crash recovery, we see if the host
	 * is newly up.  If so, invoke the reboot call-backs and then notify
	 * waiting processes. This means clientA (us) may start
	 * re-opening files from serverB (the other guy) at the same time
	 * as clientA (us) is closing files that serverB had had open.
	 * ie. both the crash and reboot call backs may proceed in parallel.
	 */
	switch(hostPtr->state & (RECOV_HOST_ALIVE|RECOV_HOST_DEAD)) {
	    case RECOV_HOST_ALIVE:
		/*
		 * Host already alive.
		 */
		break;
	    case RECOV_HOST_DEAD: {
		register int wait;
		/*
		 * Notify interested parties that the host is up.  If the host
		 * has done a full reboot we wait a bit before pounding on
		 * it with our re-open requests.  This gives it a chance
		 * to create RPC server processes, etc. so we don't think
		 * it crashed because we tried to talk to it too soon.
		 */
		if ( !reboot ) {
		    Net_HostPrint(spriteID, "is back again");
		    wait = 0;
		} else {
		    wait = timer_IntOneSecond * recovPause;
		}
		hostPtr->state &= ~RECOV_HOST_DEAD;
		hostPtr->state |= RECOV_HOST_ALIVE;
		if ((hostPtr->state & RECOV_REBOOT_CALLBACKS) == 0) {
		    hostPtr->state |= RECOV_REBOOT_CALLBACKS;
		    Proc_CallFunc(RecovRebootCallBacks, spriteID, wait);
		}
		break;
	    default:
		Sys_Panic(SYS_WARNING, "Unexpected state <%x> for ",
			hostPtr->state);
		Net_HostPrint(spriteID, "");
		break;
	    }
	}
    }
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_HostDead --
 *
 *	Change the host's state to "dead".  This is called from client RPC
 *	when an RPC timed out with no response.  It is also called by the
 *	Rpc_Daemon when it can't recontact a client to get an explicit
 *	acknowledgment.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the state in the host state table to dead.  Pings are not
 *	initiated here because we may or may not be interested in
 *	the other host.  See Rpc_HostIsDown.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Recov_HostDead(spriteID)
    int spriteID;
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;

    LOCK_MONITOR;
    if (spriteID == NET_BROADCAST_HOSTID || rpc_NoTimeouts) {
	/*
	 * If rpcNoTimeouts is set the Rpc_Daemon may still call us if
	 * it can't get an acknowledgment from a host to close down
	 * a connection.  We ignore this so that we don't take action
	 * against the offending host (who is probably in the debugger)
	 */
	UNLOCK_MONITOR;
	return;
    }

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    if (hashPtr != (Hash_Entry *)NIL) {
	hostPtr = (RecovHostState *)hashPtr->value;
	if (hostPtr != (RecovHostState *)NIL) {
	    switch(hostPtr->state & ~(RECOV_CRASH_CALLBACKS|
				      RECOV_HOST_PINGING)) {
		case RECOV_HOST_DEAD:
		    /*
		     * Host already dead.
		     */
		    break;
		case RECOV_STATE_UNKNOWN:
		case RECOV_HOST_ALIVE:
		    hostPtr->state &= ~(RECOV_HOST_ALIVE|RECOV_STATE_UNKNOWN);
		    hostPtr->state |= RECOV_HOST_DEAD|RECOV_CRASH_CALLBACKS;
		    Net_HostPrint(spriteID, "is down");
		    RECOV_TRACE(spriteID, hostPtr->state, RECOV_CUZ_CRASH);
		    Proc_CallFunc(RecovCrashCallBacks, spriteID, 0);
		    break;
	    }
	}
    }
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * RecovRebootCallBacks --
 *
 *	This calls the call-back procedures installed by other modules
 *	via Rpc_HostNotify.  It is invoked asynchronously from Recov_HostAlive
 *	when that procedure detects a reboot.  It does an explict ping
 *	of the other host to make sure it is ready for our recovery actions.
 *	This will reschedule itself for later if the host isn't ready.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Invoke the call-backs.
 *
 *----------------------------------------------------------------------
 */

void
RecovRebootCallBacks(data, callInfoPtr)
    ClientData data;
    Proc_CallInfo *callInfoPtr;
{
    ReturnStatus status;
    register NotifyElement *notifyPtr;
    register int spriteID = (int)data;

    status = Rpc_Ping(spriteID);
    switch(status) {
	case RPC_SERVICE_DISABLED:
	    Net_HostPrint(spriteID, "still booting");
	    callInfoPtr->interval = recovPause * timer_IntOneSecond;
	    break;
	case RPC_TIMEOUT:
	    Net_HostPrint(spriteID, "not responding");
	    callInfoPtr->interval = recovPause * timer_IntOneSecond;
	    break;
	case SUCCESS:
	    LIST_FORALL(&recovNotifyList, (List_Links *)notifyPtr) {
		if (notifyPtr->flags & RECOV_WHEN_HOST_REBOOTS) {
		    (*notifyPtr->proc)(spriteID, notifyPtr->clientData,
						 RECOV_WHEN_HOST_REBOOTS);
		 }
	    }
	    CallBacksDone(spriteID);
	    RECOV_TRACE(spriteID, RECOV_REBOOT, RECOV_CUZ_DONE);
	    callInfoPtr->interval = 0;	/* Don't call again */
	    break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * RecovCrashCallBacks --
 *
 *	Invoked asynchronously from Recov_HostDead so that other modules
 *	can clean up behind the crashed host.  When done the host
 *	is marked as having recovery complete.  This unblocks server
 *	processes stalled in Recov_HostAlive.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Invoke the call-backs with the RECOV_WHEN_HOST_DOWN flag.
 *	Clears the recovery in progress flag checked in Recov_HostAlive.
 *
 *----------------------------------------------------------------------
 */

void
RecovCrashCallBacks(data, callInfoPtr)
    ClientData data;
    Proc_CallInfo *callInfoPtr;
{
    register NotifyElement *notifyPtr;
    register int spriteID = (int)data;

    LIST_FORALL(&recovNotifyList, (List_Links *)notifyPtr) {
	if (notifyPtr->flags & RECOV_WHEN_HOST_DOWN) {
	    (*notifyPtr->proc)(spriteID, notifyPtr->clientData,
					 RECOV_WHEN_HOST_DOWN);
	 }
    }
    MarkRecoveryComplete(spriteID);
    RECOV_TRACE(spriteID, RECOV_CRASH, RECOV_CUZ_DONE);
    callInfoPtr->interval = 0;	/* Don't call again */
}

/*
 *----------------------------------------------------------------------
 *
 * MarkRecoveryComplete --
 *
 *	The recovery call-backs have completed, and this procedure's
 *	job is to mark that fact in the host hash table and to notify
 *	any processes that are blocked in Recov_HostAlive waiting for this.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the state, if any, in the host state table.
 *	Notifies the hostPtr->recovery condition
 *
 *----------------------------------------------------------------------
 */

ENTRY static void
MarkRecoveryComplete(spriteID)
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    if (hashPtr != (Hash_Entry *)NIL) {
	hostPtr = (RecovHostState *)hashPtr->value;
	if (hostPtr != (RecovHostState *)NIL) {
	    hostPtr->state &= ~RECOV_CRASH_CALLBACKS;
	    Sync_Broadcast(&hostPtr->recovery);
	}
    }
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * GetHostState --
 *
 *	This looks into	the host table to see and provides a guess
 *	as to the host's current state.  It uses a timestamp kept in
 *	the host state to see if there's been recent message traffic.
 *	If so, RECOV_HOST_ALIVE is returned.  If not, RECOV_STATE_UNKNOWN
 *	is returned and the caller should ping to make sure.  Finally,
 *	if it is known that the host is down already, then RECOV_HOST_DEAD
 *	is returned.
 *
 * Results:
 *	RECOV_STATE_UNKNOWN if the caller should ping to make sure.
 *	RECOV_HOST_ALIVE if the host is up (recent message traffic).
 *	RECOV_HOST_DEAD if the host is down (recent timeouts).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ENTRY int
GetHostState(spriteID)
    int spriteID;
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;
    register int state = RECOV_STATE_UNKNOWN;
    Time time;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    if (hashPtr != (Hash_Entry *)NIL) {
	hostPtr = (RecovHostState *)hashPtr->value;
	if (hostPtr != (RecovHostState *)NIL) {
	    state = hostPtr->state &
	    ~(RECOV_CRASH_CALLBACKS|RECOV_HOST_PINGING|RECOV_REBOOT_CALLBACKS);
	    if (state == RECOV_HOST_ALIVE) {
		/*
		 * Check for recent message traffic before admitting
		 * that the other machine is up.
		 */
		Timer_GetTimeOfDay(&time, (int *)NIL, (Boolean *)NIL);
		Time_Subtract(time, hostPtr->time, &time);
		if (Time_GT(time, time_TenSeconds)) {
		    state = RECOV_STATE_UNKNOWN;
		}
	    }
	}
    }
    UNLOCK_MONITOR;
    return(state);
}

/*
 *----------------------------------------------------------------------
 *
 * StartPinging --
 *
 *	Make sure there is a background pinging process for the host.
 *	The state bit used to indicate pinging is reset by RpcHostCheck
 *	after it finally gets in a good ping.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Starts the pinging callback if not already in progress.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
StartPinging(spriteID)
    int spriteID;
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    hostPtr = (RecovHostState *)hashPtr->value;
    if ((hostPtr->state & RECOV_HOST_PINGING) == 0) {
	hostPtr->state |= RECOV_HOST_PINGING;
	Proc_CallFunc(CheckHost, spriteID, 0);
    }
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * CheckHost --
 *
 *	This is the call back setup when a host is detected as crashed
 *	and we want to find out when it comes back up.  This pings
 *	the remote host if it's down or there hasn't been recent traffic.
 *	A side effect of a successful ping is a call to Recov_HostAlive which
 *	triggers the recovery actions.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	This will pings the host unless there has been recent message
 *	traffic.  It reschedules itself if the ping fails.
 *
 *----------------------------------------------------------------------
 */

static void
CheckHost(data, callInfoPtr)
    ClientData data;
    Proc_CallInfo *callInfoPtr;
{
    register int spriteID = (int)data;
    register int state;
    ReturnStatus status = SUCCESS;

    state = GetHostState(spriteID);
    switch (state) {
	case RECOV_HOST_DEAD:
	case RECOV_STATE_UNKNOWN:
	    RECOV_TRACE(spriteID, state, RECOV_CUZ_PING_CHK);
	    status = Rpc_Ping(spriteID);
	    break;
	case RECOV_HOST_ALIVE:
	    break;
    }
    if (status != SUCCESS) {
	/*
	 * Try again later if the host is still down.
	 */
	callInfoPtr->interval = recovPingSeconds * timer_IntOneSecond;
    } else {
	StopPinging(spriteID);
	callInfoPtr->interval = 0;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * StopPinging --
 *
 *	Clear the internal state bit that says we are pinging.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	As above.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
StopPinging(spriteID)
    int spriteID;
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    hostPtr = (RecovHostState *)hashPtr->value;
    if ((hostPtr->state & RECOV_HOST_PINGING) == 0) {
	Sys_Panic(SYS_WARNING, "StopPinging found bad state\n");
    }
    hostPtr->state &= ~RECOV_HOST_PINGING;
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * CallBacksDone --
 *
 *	Clear the internal state bit that says callbacks are in progress.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	As above.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
CallBacksDone(spriteID)
    int spriteID;
{
    register Hash_Entry *hashPtr;
    register RecovHostState *hostPtr;

    LOCK_MONITOR;

    hashPtr = Hash_LookOnly(recovHashTable, spriteID);
    hostPtr = (RecovHostState *)hashPtr->value;
    if ((hostPtr->state & RECOV_REBOOT_CALLBACKS) == 0) {
	Sys_Panic(SYS_WARNING, "StopPinging found bad state\n");
    }
    hostPtr->state &= ~RECOV_REBOOT_CALLBACKS;
    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_PrintTraceRecord --
 *
 *	Format and print the client data part of a recovery trace record.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sys_Printf to the display.
 *
 *----------------------------------------------------------------------
 */
int
Recov_PrintTraceRecord(clientData, event, printHeaderFlag)
    ClientData clientData;	/* Client data in the trace record */
    int event;			/* Type, or event, from the trace record */
    Boolean printHeaderFlag;	/* If TRUE, a header line is printed */
{
    RecovTraceRecord *recPtr = (RecovTraceRecord *)clientData;
    char *name;
    if (printHeaderFlag) {
	/*
	 * Print column headers and a newline.
	 */
	Sys_Printf("%10s %10s %17s\n", "Host", "State", "Event ");
    }
    if (clientData != (ClientData)NIL) {
	Net_SpriteIDToName(recPtr->spriteID, &name);
	if (name == (char *)NIL) {
	    Sys_Printf("%10d ", recPtr->spriteID);
	} else {
	    Sys_Printf("%10s ", name);
	}
	switch(recPtr->state & ~(RECOV_CRASH_CALLBACKS|RECOV_HOST_PINGING)) {
	    case RECOV_STATE_UNKNOWN:
		Sys_Printf("%-8s", "Unknown");
		break;
	    case RECOV_HOST_ALIVE:
		Sys_Printf("%-8s ", "Alive");
		break;
	    case RECOV_HOST_DEAD:
		Sys_Printf("%-8s ", "Dead");
		break;
	    case RECOV_WAITING:
		Sys_Printf("%-8s ", "Waiting");
		break;
	    case RECOV_CRASH:
		Sys_Printf("%-8s ", "Crash callbacks");
		break;
	    case RECOV_REBOOT:
		Sys_Printf("%-8s ", "Reboot callbacks");
		break;
	}
	Sys_Printf("%3s", (recPtr->state & RECOV_CRASH_CALLBACKS) ?
			    " C " : "   ");
	Sys_Printf("%3s", (recPtr->state & RECOV_HOST_PINGING) ?
			    " P " : "   ");
	switch(event) {
	    case RECOV_CUZ_WAIT:
		Sys_Printf("waiting");
		break;
	    case RECOV_CUZ_WAKEUP:
		Sys_Printf("wakeup");
		break;
	    case RECOV_CUZ_INIT:
		Sys_Printf("init");
		break;
	    case RECOV_CUZ_REBOOT:
		Sys_Printf("reboot");
		break;
	    case RECOV_CUZ_CRASH:
		Sys_Printf("crash");
		break;
	    case RECOV_CUZ_DONE:
		Sys_Printf("done");
		break;
	    case RECOV_CUZ_PING_ASK:
		Sys_Printf("ping (ask)");
		break;
	    case RECOV_CUZ_PING_CHK:
		Sys_Printf("ping (check)");
		break;
	    case RPC_RECOV_TRACE_STALE:
		Sys_Printf("stale FS handle");
		break;
	    default:
		Sys_Printf("(%x)", event);
		break;
	}
	/* Our caller prints a newline */
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Recov_PrintTrace --
 *
 *	Dump out the recovery trace.  Called via a console L1 keystroke.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Prints to the console.
 *
 *----------------------------------------------------------------------
 */

void
Recov_PrintTrace(numRecs)
    int numRecs;
{
    if (numRecs <= 0 || numRecs > recovTraceLength) {
	numRecs = recovTraceLength;
    }
    Sys_Printf("RECOVERY TRACE\n");
    Trace_Print(recovTraceHdrPtr, numRecs, Recov_PrintTraceRecord);
}
