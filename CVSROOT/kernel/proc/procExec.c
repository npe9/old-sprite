/* procExec.c --
 *
 *	Routines to exec a program.  There is no monitor required in this
 *	file.  Access to the proc table is synchronized by locking the PCB
 *	when modifying the genFlags field.
 *
 * Copyright (C) 1985, 1988 Regents of the University of California
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
#include "mach.h"
#ifdef sun4
#include "machMon.h"
#endif sun4
#include "proc.h"
#include "procInt.h"
#include "sync.h"
#include "sched.h"
#include "fs.h"
#include "stdlib.h"
#include "sig.h"
#include "spriteTime.h"
#include "list.h"
#include "vm.h"
#include "sys.h"
#include "procAOUT.h"
#include "status.h"
#include "string.h"

static ReturnStatus	DoExec();
extern char *malloc();

#define NEWEXEC
/*
 * This will go away when libc is changed.
 */
#ifndef PROC_MAX_ENVIRON_LENGTH
#define PROC_MAX_ENVIRON_LENGTH (PROC_MAX_ENVIRON_NAME_LENGTH + \
				 PROC_MAX_ENVIRON_VALUE_LENGTH)
#endif /*  PROC_MAX_ENVIRON_LENGTH */

typedef struct {
    List_Links	links;
    Address	stringPtr;
    int		stringLen;
} ArgListElement;



/*
 *----------------------------------------------------------------------
 *
 * Proc_Exec --
 *
 *	Process the Exec system call.  This just calls Proc_ExecEnv
 *	with a NULL environment.  This should simulate past behavior,
 *	in which all processes were set up by unixCrt0.s with a NULL
 *	environment on their stack.
 *
 * Results:
 *	This process will not return unless an error occurs in which case it
 *	returns the error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Proc_Exec(fileName, argPtrArray, debugMe)
    char	*fileName;	/* The name of the file to exec. */
    char	**argPtrArray;	/* The array of arguments to the exec'd 
				 * program. */
    Boolean	debugMe;	/* TRUE means that the process is 
				 * to be sent a SIG_DEBUG signal before
    				 * executing its first instruction. */
{
    int status;			/* status in case Proc_ExecEnv returns */
    status = Proc_ExecEnv(fileName, argPtrArray, (char **) USER_NIL, debugMe);
    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_ExecEnv --
 *
 *	Process the Exec system call.
 *
 * Results:
 *	This process will not return unless an error occurs in which case it
 *	returns the error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Proc_ExecEnv(fileName, argPtrArray, envPtrArray, debugMe)
    char	*fileName;	/* The name of the file to exec. */
    char	**argPtrArray;	/* The array of arguments to the exec'd 
				 * program. */
    char	**envPtrArray;	/* The array of environment variables
				 * of the form NAME=VALUE. */ 
    Boolean	debugMe;	/* TRUE means that the process is 
				 * to be sent a SIG_DEBUG signal before
    				 * executing its first instruction. */
{
    char		**newArgPtrArray;
    int			newArgPtrArrayLength;
    char		**newEnvPtrArray;
    int			newEnvPtrArrayLength;
    int			strLength;
    int			accessLength;
    ReturnStatus	status;
    char 		execFileName[FS_MAX_PATH_NAME_LENGTH];


    /*
     * Make the file name accessible. 
     */

    status = Proc_MakeStringAccessible(FS_MAX_PATH_NAME_LENGTH, &fileName,
				       &accessLength, &strLength);
    if (status != SUCCESS) {
	return(status);
    }

    strcpy(execFileName, fileName);
    Proc_MakeUnaccessible((Address) fileName, accessLength);

    /*
     * Make the arguments array accessible.
     */

    if (argPtrArray != (char **) USER_NIL) {
	Vm_MakeAccessible(VM_READONLY_ACCESS,
			  (PROC_MAX_EXEC_ARGS + 1) * sizeof(Address),
			  (Address) argPtrArray, 
		          &newArgPtrArrayLength, (Address *) &newArgPtrArray);
	if (newArgPtrArrayLength == 0) {
	    return(SYS_ARG_NOACCESS);
	}
    } else {
	newArgPtrArray = (char **) NIL;
	newArgPtrArrayLength = 0;
    }

    /*
     * Make the environments array accessible.
     */

    if (envPtrArray != (char **) USER_NIL) {
	Vm_MakeAccessible(VM_READONLY_ACCESS,
			  (PROC_MAX_ENVIRON_SIZE + 1) * sizeof(Address),
			  (Address) envPtrArray, 
		          &newEnvPtrArrayLength, (Address *) &newEnvPtrArray);
	if (newEnvPtrArrayLength == 0) {
	    return(SYS_ARG_NOACCESS);
	}
    } else {
	newEnvPtrArray = (char **) NIL;
	newEnvPtrArrayLength = 0;
    }
    status = DoExec(execFileName, strLength, newArgPtrArray, 
			   newArgPtrArrayLength / 4, newEnvPtrArray, 
			   newEnvPtrArrayLength / 4, TRUE, debugMe);
    if (status == SUCCESS) {
	panic("Proc_ExecEnv: DoExec returned SUCCESS!!!\n");
    }

    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_KernExec --
 *
 *	Do an exec from a kernel process.  This will exec the program and
 *	change the type of process to a user process.
 *
 * Results:
 *	This routine does not return unless an error occurs from DoExec.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

/* 
 * Use the old Proc_KernExec until this gets installed as the sole procExec.c.
 */
int
Proc_KernExec(fileName, argPtrArray)
    char *fileName;
    char **argPtrArray;
{
    register	Proc_ControlBlock	*procPtr;
    ReturnStatus			status;

    procPtr = Proc_GetCurrentProc();

    /*
     * Set up dummy segments so that DoExec can work properly.
     */

    procPtr->vmPtr->segPtrArray[VM_CODE] = 
				Vm_SegmentNew(VM_CODE, (Fs_Stream *) NIL, 0,
					          1, 0, procPtr);
    if (procPtr->vmPtr->segPtrArray[VM_CODE] == (Vm_Segment *) NIL) {
	return(PROC_NO_SEGMENTS);
    }

    procPtr->vmPtr->segPtrArray[VM_HEAP] =
		    Vm_SegmentNew(VM_HEAP, (Fs_Stream *) NIL, 0, 1, 1, procPtr);
    if (procPtr->vmPtr->segPtrArray[VM_HEAP] == (Vm_Segment *) NIL) {
	Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_CODE], procPtr);
	return(PROC_NO_SEGMENTS);
    }

    procPtr->vmPtr->segPtrArray[VM_STACK] =
		    Vm_SegmentNew(VM_STACK, (Fs_Stream *) NIL, 
				   0 , 1, mach_LastUserStackPage, procPtr);
    if (procPtr->vmPtr->segPtrArray[VM_STACK] == (Vm_Segment *) NIL) {
	Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_CODE], procPtr);
	Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_HEAP], procPtr);
	return(PROC_NO_SEGMENTS);
    }

    /*
     * Change this process to a user process.
     */

    Proc_Lock(procPtr);
    procPtr->genFlags &= ~PROC_KERNEL;
    procPtr->genFlags |= PROC_USER;
    Proc_Unlock(procPtr);

    VmMach_ReinitContext(procPtr);

    status = DoExec(fileName, strlen(fileName), argPtrArray,
		    PROC_MAX_EXEC_ARGS, (char **) NIL, 0, FALSE, FALSE);
    /*
     * If the exec failed, then delete the extra segments and fix up the
     * proc table entry to put us back into kernel mode.
     */

    Proc_Lock(procPtr);
    procPtr->genFlags &= ~PROC_USER;
    procPtr->genFlags |= PROC_KERNEL;
    Proc_Unlock(procPtr);

    VmMach_ReinitContext(procPtr);

    Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_CODE], procPtr);
    Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_HEAP], procPtr);
    Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_STACK], procPtr);

    return(status);
}

static ReturnStatus	SetupInterpret();
static Boolean		SetupVM();


/*
 *----------------------------------------------------------------------
 *
 * DoExec --
 *
 *	Exec a new program.  The current process image is overlayed by the 
 *	newly exec'd program.
 *
 *	Note: environments are now being added; after they work, I can
 *	think about coalescing some of the code rather than duplicating
 *	it.
 *
 * Results:
 *	This routine does not return unless an error occurs in which case the
 *	error code is returned.
 *
 * Side effects:
 *	The state of the calling process is modified for the new image and
 *	the argPtrArray and envPtrArray are made unaccessible if they this
 *	is a user process.
 *
 *----------------------------------------------------------------------
 */

static ReturnStatus
DoExec(fileName, fileNameLength, argPtrArray, numArgs, envPtrArray, numEnvs,
       userProc, debugMe)
    char	fileName[];	/* The name of the file that is to be exec'd */
    int		fileNameLength;	/* The length of the file name. */
    char	**argPtrArray;	/* The array of argument pointers. */
    int		numArgs;	/* The number of arguments in the argArray. */
    char	**envPtrArray;	/* The array of environment pointers. */
    int		numEnvs;	/* The number of arguments in the envArray. */
    Boolean	userProc;	/* Set if the calling process is a user 
				 * process. */
    Boolean	debugMe;	/* TRUE means that the process is to be 
				 * sent a SIG_DEBUG signal before
    				 * executing its first instruction. */
{
    register	Proc_ControlBlock	*procPtr;
    register	ArgListElement	*argListPtr;
    register	Proc_AOUT		*aoutPtr;
    register	char			**argPtr;
    register	int			argNumber;
    register	char			**envPtr;
    ArgListElement			*envListPtr;
    int					envNumber;
    int					origNumArgs;
    int					origNumEnvs;
    Vm_ExecInfo				*execInfoPtr;
    char				**newArgPtrArray;
    char				**newEnvPtrArray;
    int					tArgNumber;
    Proc_AOUT				aout;
    List_Links				argList;
    List_Links				envList;
    Vm_Segment				*codeSegPtr = (Vm_Segment *) NIL;
    char				*copyAddr;
    int					copyLength;
    register	char			*argString;
    int					argStringLength;
    Fs_Stream				*filePtr;
    ReturnStatus			status;
    char				buffer[PROC_MAX_INTERPRET_SIZE];
    int					extraArgs = 0;
    char				*shellArgPtr;
    int					argBytes;
    Address				userStackPointer;
    int					usp;
    int					entry;
    Boolean				usedFile;
    int					uid = -1;
    int					gid = -1;
    char				*stringPtr;
    int					stringLength;
    int					realLength;
    Boolean 				accessible;

    origNumArgs = numArgs;
    origNumEnvs = numEnvs;

    procPtr = Proc_GetActualProc();
    List_Init(&argList);
    List_Init(&envList);

    /* Turn off profiling */
    if (procPtr->Prof_Scale != 0) {
	Prof_Disable(procPtr);
    }

    /*
     * Open the file that is to be exec'd.
     */
    filePtr = (Fs_Stream *) NIL;
    status =  Fs_Open(fileName, FS_EXECUTE | FS_FOLLOW, FS_FILE, 0, &filePtr);
    if (status != SUCCESS) {
	filePtr = (Fs_Stream *) NIL;
	goto execError;
    }

    /*
     * Determine if this file has the set uid or set gid bits set.
     */
    Fs_CheckSetID(filePtr, &uid, &gid);

    /*
     * See if this file is already cached by the virtual memory system.
     */
    codeSegPtr = Vm_FindCode(filePtr, procPtr, &execInfoPtr, &usedFile);
    if (codeSegPtr == (Vm_Segment *) NIL) {
	int	sizeRead;

	/*
	 * Read the file header.
	 */
	sizeRead = PROC_MAX_INTERPRET_SIZE;
	status = Fs_Read(filePtr, buffer, 0, &sizeRead);
	if (status != SUCCESS) {
	    goto execError;
	}
	if (sizeRead >= 2 && buffer[0] == '#' && buffer[1] == '!') {
	    Vm_InitCode(filePtr, (Vm_Segment *) NIL, (Vm_ExecInfo *) NIL);
	    /*
	     * See if this is an interpreter file.
	     */
	    status = SetupInterpret(buffer, sizeRead, &filePtr, 
				    &shellArgPtr, &extraArgs, &aout); 
	    if (status != SUCCESS) {
		filePtr = (Fs_Stream *)NIL;
		goto execError;
	    }
	    sizeRead = sizeof(Proc_AOUT);
	    aoutPtr = &aout;
	    codeSegPtr = Vm_FindCode(filePtr, procPtr, &execInfoPtr, &usedFile);
	} else {
	    aoutPtr = (Proc_AOUT *) buffer;
	}
	if (codeSegPtr == (Vm_Segment *) NIL && 
	    (sizeRead < sizeof(Proc_AOUT) || PROC_BAD_MAGIC_NUMBER(*aoutPtr))) {
	    status = PROC_BAD_AOUT_FORMAT;
	    goto execError;
	}
    }

    /*
     * The total number of arguments is the number passed in plus the
     * number of extra arguments because is an interpreter file.
     */
    if (argPtrArray == (char **) NIL) {
	numArgs = extraArgs;
    } else {
	numArgs += extraArgs;
    }
    argStringLength = 0;

    /*
     * Copy in all of the arguments.  If we are executing a normal
     * program then we just copy in the arguments as is.  If this is
     * an interpreter file then we do the following:
     *
     *	1) If the user passed in arguments, then arg[0] is left alone.
     *	2) If arguments were passed to the interpreter program in the
     *	   interpreter file then they are placed in arg[1].
     *  3) The name of the interpreter file is placed in arg[1] or arg[2]
     *	   depending if there were interpreter arguments from step 2.
     *  3) Original arguments arg[1] through arg[n] are shifted over.
     */
    userStackPointer = mach_MaxUserStackAddr;
    for (argNumber = 0, argPtr = argPtrArray; 
	 argNumber < numArgs;
	 argNumber++) {

	accessible = FALSE;

	if ((argNumber > 0 || argPtrArray == (char **) NIL) && extraArgs > 0) {
	    if (extraArgs == 2) {
		stringPtr = shellArgPtr;
		realLength = strlen(shellArgPtr) + 1;
	    } else {
		stringPtr = fileName;
		realLength = fileNameLength + 1;
	    }
	    extraArgs--;
	} else {
	    if (!userProc) {
		if (*argPtr == (char *) NIL) {
		    break;
		}
		stringPtr = *argPtr;
		stringLength = PROC_MAX_EXEC_ARG_LENGTH;
	    } else {
		if (*argPtr == (char *) USER_NIL) {
		    break;
		}
		Vm_MakeAccessible(VM_READONLY_ACCESS,
				  PROC_MAX_EXEC_ARG_LENGTH + 1, 
				  (Address) *argPtr, 
				  &stringLength,
				  (Address *) &stringPtr);
		if (stringLength == 0) {
		    status = SYS_ARG_NOACCESS;
		    goto execError;
		}
		accessible = TRUE;
	    }
	    /*
	     * Find out the length of the argument.  Because of accessibility
	     * limitations the whole string may not be available.
	     */
	    {
		register char *charPtr;
		for (charPtr = stringPtr, realLength = 0;
		     (realLength < stringLength) && (*charPtr != '\0');
		     charPtr++, realLength++) {
		}
		realLength++;
	    }
	    /*
	     * Move to the next argument.
	     */
	    argPtr++;
	}

	/*
	 * Put this string onto the argument list.
	 */
	argListPtr = (ArgListElement *)
		malloc(sizeof(ArgListElement));
	argListPtr->stringPtr =  malloc(realLength);
	argListPtr->stringLen = realLength;
	List_InitElement((List_Links *) argListPtr);
	List_Insert((List_Links *) argListPtr, LIST_ATREAR(&argList));
	/*
	 * Make room on the stack for this string.  Make it 4 byte aligned.
	 * Also up the amount needed to save the argument list.
	 */
#ifndef sun4
	userStackPointer -= ((realLength) + 3) & ~3;
#else
	/*
	 * For initial sun4 port, I'm requiring the user stack pointer to
	 * be double-word aligned.
	 */
	userStackPointer -= ((realLength) + 7) & ~7;
#endif /* sun4 */
	argStringLength += realLength;
	/*
	 * Copy over the argument and ensure null termination.
	 */
	bcopy((Address) stringPtr, (Address) argListPtr->stringPtr, realLength);
	argListPtr->stringPtr[realLength-1] = '\0';
	/*
	 * Clean up 
	 */
	if (accessible) {
	    Vm_MakeUnaccessible((Address) stringPtr, stringLength);
	}
    }

    /*
     * Copy in the environment.
     */
    for (envNumber = 0, envPtr = envPtrArray; 
	 envNumber < numEnvs;
	 envNumber++) {

	accessible = FALSE;

	if (!userProc) {
	    if (*envPtr == (char *) NIL) {
		break;
	    }
	    stringPtr = *envPtr;
	    stringLength = PROC_MAX_ENVIRON_LENGTH;
	} else {
	    if (*envPtr == (char *) USER_NIL) {
		break;
	    }
	    Vm_MakeAccessible(VM_READONLY_ACCESS,
			      PROC_MAX_ENVIRON_LENGTH + 1, 
			      (Address) *envPtr, 
			      &stringLength,
			      (Address *) &stringPtr);
	    if (stringLength == 0) {
		status = SYS_ARG_NOACCESS;
		goto execError;
	    }
	    accessible = TRUE;
	}
	/*
	 * Find out the length of the environment variable.  Because of
	 * accessibility limits the whole string may not be available.
	 */
	{
	    register char *charPtr;
	    for (charPtr = stringPtr, realLength = 0;
		 (realLength < stringLength) && (*charPtr != '\0');
		 charPtr++, realLength++) {
	    }
	    realLength++;
	}
	/*
	 * Move to the next environment variable.
	 */
	envPtr++;

	/*
	 * Put this string onto the environment variable list.
	 */
	envListPtr = (ArgListElement *) 
		malloc(sizeof(ArgListElement));
	envListPtr->stringPtr =  malloc(realLength);
	envListPtr->stringLen = realLength;
	List_InitElement((List_Links *) envListPtr);
	List_Insert((List_Links *) envListPtr, LIST_ATREAR(&envList));
	/*
	 * Make room on the stack for this string.  Make it 4 byte aligned.
	 */
#ifndef sun4
	userStackPointer -= ((realLength) + 3) & ~3;
#else
	/*
	 * For the initial sun4 port, I'm requiring the user stack pointer
	 * to be double-word aligned.
	 */
	userStackPointer -= ((realLength) + 7) & ~7;
#endif /* sun4 */
	/*
	 * Copy over the environment variable and ensure null termination.
	 */
	bcopy((Address) stringPtr, (Address) envListPtr->stringPtr, realLength);
	envListPtr->stringPtr[realLength-1] = '\0';
	/*
	 * Clean up 
	 */
	if (accessible) {
	    Vm_MakeUnaccessible((Address) stringPtr, stringLength);
	}
    }

    /*
     * We no longer need access to the old arguments or the environment. 
     */
    if (userProc) {
	if (argPtrArray != (char **) NIL) {
	    Vm_MakeUnaccessible((Address) argPtrArray, origNumArgs * 4);
	    argPtrArray = (char **)NIL;
	}
	if (envPtrArray != (char **) NIL) {
	    Vm_MakeUnaccessible((Address) envPtrArray, origNumEnvs * 4);
	    envPtrArray = (char **)NIL;
	}
    }

    /*
     * Set up virtual memory for the new image.
     */
    if (!SetupVM(procPtr, aoutPtr, filePtr, usedFile, &codeSegPtr, 
		 execInfoPtr, &entry)) {
	/*
	 * Setup VM will make sure that the file is closed and that
	 * all new segments are freed up.
	 */
	filePtr = (Fs_Stream *) NIL;
	status = VM_NO_SEGMENTS;
	goto execError;
    }

    /*
     * Now copy all of the arguments and environment variables onto the stack.
     */
    argBytes = mach_MaxUserStackAddr - userStackPointer;
    (void) Vm_MakeAccessible(VM_READWRITE_ACCESS,
			  argBytes, userStackPointer,
			  &copyLength, (Address *) &copyAddr);
    newArgPtrArray = (char **) malloc((argNumber + 1) * sizeof(Address));
    argNumber = 0;
    usp = (int)userStackPointer;
    argString = malloc(argStringLength + 1);
    if (procPtr->argString != (char *) NIL) {
	free(procPtr->argString);
    }
    procPtr->argString = argString;

    while (!List_IsEmpty(&argList)) {
	argListPtr = (ArgListElement *) List_First(&argList);
	/*
	 * Copy over the argument.
	 */
	bcopy((Address) argListPtr->stringPtr, (Address) copyAddr,
		    argListPtr->stringLen);
	newArgPtrArray[argNumber] = (char *) usp;
	copyAddr += ((argListPtr->stringLen) + 3) & ~3;
	usp += ((argListPtr->stringLen) + 3) & ~3;
	bcopy((Address) argListPtr->stringPtr, argString,
		    argListPtr->stringLen - 1);
	argString[argListPtr->stringLen - 1] = ' ';
	argString += argListPtr->stringLen;
	/*
	 * Clean up
	 */
	List_Remove((List_Links *) argListPtr);
	free((Address) argListPtr->stringPtr);
	free((Address) argListPtr);
	argNumber++;
    }
    argString[0] = '\0';
    
    newEnvPtrArray = (char **) malloc((envNumber + 1) * sizeof(Address));
    envNumber = 0;
    while (!List_IsEmpty(&envList)) {
	envListPtr = (ArgListElement *) List_First(&envList);
	/*
	 * Copy over the environment variable.
	 */
	bcopy((Address) envListPtr->stringPtr, (Address) copyAddr,
		    envListPtr->stringLen);
	newEnvPtrArray[envNumber] = (char *) usp;
	copyAddr += ((envListPtr->stringLen) + 3) & ~3;
	usp += ((envListPtr->stringLen) + 3) & ~3;
	/*
	 * Clean up 
	 */
	List_Remove((List_Links *) envListPtr);
	free((Address) envListPtr->stringPtr);
	free((Address) envListPtr);
	envNumber++;
    }
    Vm_MakeUnaccessible(copyAddr - argBytes, argBytes);
    /*
     * The stack ends up in the following state:  the top word is argc.
     * Right below this is the array of pointers to arguments (argv).  Right
     * below this is the array of pointers to environment stuff (envp).  So,
     * to figure out the address of argv, one simply adds a word to the address
     * of the top of the stack.  To figure out the address of envp, one
     * looks at argc and skips over the appropriate amount of space to jump
     * over argc and argv = (1 + (argc + 1)) * 4 bytes.  The extra "1" is for
     * the null argument at the end of argv.  Below all that stuff on the
     * stack, but not necessarily immediately below, are the environment and
     * argument stuff copied there earlier.
     */
    copyLength = (argNumber + envNumber + 2) * sizeof(Address) + sizeof(int);
    newArgPtrArray[argNumber] = (char *) USER_NIL;
    newEnvPtrArray[envNumber] = (char *) USER_NIL;
    userStackPointer -= copyLength;
#ifdef sun4
    userStackPointer -= ((realLength) + 7) & ~7;
#endif /* sun4 */
    tArgNumber = argNumber;
    (void) Vm_CopyOut(sizeof(int), (Address) &tArgNumber,
		      userStackPointer);
    (void) Vm_CopyOut((argNumber + 1) * sizeof(Address),
		      (Address) newArgPtrArray,
		      userStackPointer + sizeof(int));
    (void) Vm_CopyOut((envNumber + 1) * sizeof(Address),
		      (Address) newEnvPtrArray,
		      userStackPointer + sizeof(int) +
		      (argNumber + 1) * sizeof(Address));
    /*
     * Free the arrays of arguments and environment variables.
     */
    free((Address) newArgPtrArray);
    free((Address) newEnvPtrArray);

    /*
     * Close any streams that have been marked close-on-exec.
     */
    Fs_CloseOnExec(procPtr);

    /*
     * Do set uid and set gid here.
     */
    if (uid != -1) {
	procPtr->effectiveUserID = uid;
    }
    if (gid != -1) {
	ProcAddToGroupList(procPtr, gid);
    }

    /*
     * Take signal actions for execed process.
     */
    Sig_Exec(procPtr);
    if (debugMe) {
	/*
	 * Debugged processes get a SIG_DEBUG at start up.
	 */
	Sig_SendProc(procPtr, SIG_DEBUG, SIG_NO_CODE);
    }
    if (procPtr->genFlags & PROC_FOREIGN) {
	ProcRemoteExec(procPtr, uid);
    }
    Proc_Unlock(procPtr);

    /*
     * Disable interrupts.  Note that we don't use the macro DISABLE_INTR 
     * because there is an implicit enable interrupts when we return to user 
     * mode.
     */
#ifdef sun4
    Mach_MonPrintf("Calling Mach_ExecUserProc with userStackPtr 0x%x, argc %d\n", userStackPointer, tArgNumber);
#endif sun4
    Mach_ExecUserProc(procPtr, userStackPointer, (Address) entry);
    panic("DoExec: Proc_RunUserProc returned.\n");

execError:
    /*
     * The exec failed after or while copying in the arguments.  Free any
     * virtual memory allocated and free any arguments or environment
     * variables that were copied in.
     */
    if (filePtr != (Fs_Stream *) NIL) {
	if (codeSegPtr != (Vm_Segment *) NIL) {
	    Vm_SegmentDelete(codeSegPtr, procPtr);
	    if (!usedFile) {
		/*
		 * If usedFile is TRUE then the file has already been closed
		 * by Vm_SegmentDelete.
		 */
		(void) Fs_Close(filePtr);
	    }
	} else {
	    Vm_InitCode(filePtr, (Vm_Segment *) NIL, (Vm_ExecInfo *) NIL);
	    (void) Fs_Close(filePtr);
	}
    }
    if (userProc) {
	if (argPtrArray != (char **) NIL) {
	    Vm_MakeUnaccessible((Address) argPtrArray, origNumArgs * 4);
	}
	if (envPtrArray != (char **) NIL) {
	    Vm_MakeUnaccessible((Address) envPtrArray, origNumEnvs * 4);
	}
    }
    while (!List_IsEmpty(&argList)) {
	argListPtr = (ArgListElement *) List_First(&argList);
	List_Remove((List_Links *) argListPtr);
	free((Address) argListPtr->stringPtr);
	free((Address) argListPtr);
    }
    while (!List_IsEmpty(&envList)) {
	envListPtr = (ArgListElement *) List_First(&envList);
	List_Remove((List_Links *) envListPtr);
	free((Address) envListPtr->stringPtr);
	free((Address) envListPtr);
    }
    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * SetupInterpret --
 *
 *	Read in the interpreter name, arguments and object file header.
 *
 * Results:
 *	Error if for some reason could not parse the interpreter name
 *	and arguments are could not open the interpreter object file.
 *
 * Side effects:
 *	*filePtrPtr contains pointer to the interpreter object file, 
 *	*argPtrPtr points to interpreter argument string, *extraArgsPtr
 *	contains number of arguments that have to be prepended to the 
 *	argument vector passed to the interpreter and *aoutPtr contains the
 *	interpreter files a.out header.
 *
 *----------------------------------------------------------------------
 */ 

static ReturnStatus
SetupInterpret(buffer, sizeRead, filePtrPtr, argPtrPtr, 
	       extraArgsPtr, aoutPtr)
    register	char	*buffer;	/* Bytes read in from file.*/
    int			sizeRead;	/* Number of bytes in buffer. */	
    register	Fs_Stream **filePtrPtr;	/* IN/OUT parameter: Exec'd file as 
					 * input, interpreter file as output. */
    char		**argPtrPtr;	/* Pointer to shell argument string. */
    int			*extraArgsPtr;	/* Number of arguments that have to be
					 * added for the intepreter. */
    Proc_AOUT		*aoutPtr;	/* Place to read a.out header into. */
{
    register	char	*strPtr;
    char		*shellNamePtr;
    int			i;
    ReturnStatus	status;

    (void) Fs_Close(*filePtrPtr);

    /*
     * Make sure the interpreter name and arguments are terminated by a 
     * carriage return.
     */
    for (i = 2, strPtr = &(buffer[2]);
	 i < sizeRead && *strPtr != '\n';
	 i++, strPtr++) {
    }
    if (i == sizeRead) {
	return(PROC_BAD_FILE_NAME);
    }
    *strPtr = '\0';

    /*
     * Get a pointer to the name of the file to exec.
     */

    for (strPtr = &(buffer[2]); isspace(*strPtr); strPtr++) {
    }
    if (*strPtr == '\0') {
	return(PROC_BAD_FILE_NAME);
    }
    shellNamePtr = strPtr;
    while (!isspace(*strPtr) && *strPtr != '\0') {
	strPtr++;
    }
    *extraArgsPtr = 1;

    /*
     * Get a pointer to the arguments if there are any.
     */

    if (*strPtr != '\0') {
	*strPtr = '\0';
	strPtr++;
	while (isspace(*strPtr)) {
	    strPtr++;
	}
	if (*strPtr != '\0') {
	    *argPtrPtr = strPtr;
	    *extraArgsPtr = 2;
	}
    }

    /*
     * Open the interpreter to exec and read the a.out header.
     */

    status = Fs_Open(shellNamePtr, FS_EXECUTE | FS_FOLLOW, FS_FILE, 0,
		     filePtrPtr);
    if (status != SUCCESS) {
	return(status);
    }

    sizeRead = sizeof(Proc_AOUT);
    status = Fs_Read(*filePtrPtr, (Address) aoutPtr, 0, &sizeRead);
    if (status == SUCCESS && sizeRead != sizeof(Proc_AOUT)) {
	status = PROC_BAD_AOUT_FORMAT;
    }
    if (status != SUCCESS) {
	(void) Fs_Close(*filePtrPtr);
    }
    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * SetupVM --
 *
 *	Setup virtual memory for this process.
 *
 * Results:
 *	TRUE if could set up VM, false otherwise.
 *
 * Side effects:
 *	*filePtrPtr contains pointer to the interpreter object file, 
 *	*argPtrPtr points to interpreter argument string, *extraArgsPtr
 *	contains number of arguments that have to be prepended to the 
 *	argument vector passed to the interpreter and *aoutPtr contains the
 *	interpreter files a.out header.
 *
 *----------------------------------------------------------------------
 */ 
static Boolean
SetupVM(procPtr, aoutPtr, codeFilePtr, usedFile, codeSegPtrPtr, execInfoPtr, 
	entryPtr)
    register	Proc_ControlBlock	*procPtr;
    register	Proc_AOUT		*aoutPtr;
    Fs_Stream				*codeFilePtr;
    Boolean				usedFile;
    Vm_Segment				**codeSegPtrPtr;
    register	Vm_ExecInfo		*execInfoPtr;
    int					*entryPtr;
{
    register	Vm_Segment	*segPtr;
    int				numPages;
    int				fileOffset;
    int				pageOffset;
    Boolean			notFound;
    Vm_Segment			*heapSegPtr;
    Vm_ExecInfo			execInfo;
    Fs_Stream			*heapFilePtr;

    if (*codeSegPtrPtr == (Vm_Segment *) NIL) {
	execInfoPtr = &execInfo;
	execInfoPtr->entry = aoutPtr->entry;
	if (aoutPtr->data != 0) {
	    execInfoPtr->heapPages = (aoutPtr->data - 1) / vm_PageSize + 1;
	} else { 
	    execInfoPtr->heapPages = 0;
	}
	if (aoutPtr->bss != 0) {
	    execInfoPtr->heapPages += (aoutPtr->bss - 1) / vm_PageSize + 1;
	}
	execInfoPtr->heapPageOffset = 
			PROC_DATA_LOAD_ADDR(*aoutPtr) / vm_PageSize;
	execInfoPtr->heapFileOffset = 
			(int) PROC_DATA_FILE_OFFSET(*aoutPtr);
	execInfoPtr->bssFirstPage = 
			PROC_BSS_LOAD_ADDR(*aoutPtr) / vm_PageSize;
	if (aoutPtr->bss > 0) {
	    execInfoPtr->bssLastPage = (int) (execInfoPtr->bssFirstPage + 
				       (aoutPtr->bss - 1) / vm_PageSize);
	} else {
	    execInfoPtr->bssLastPage = 0;
	}
	/* 
	 * Set up the code image.
	 */
	numPages = (aoutPtr->code - 1) / vm_PageSize + 1;
	fileOffset = PROC_CODE_FILE_OFFSET(*aoutPtr);
	pageOffset = PROC_CODE_LOAD_ADDR(*aoutPtr) / vm_PageSize;
	segPtr = Vm_SegmentNew(VM_CODE, codeFilePtr, fileOffset,
			       numPages, pageOffset, procPtr);
	if (segPtr == (Vm_Segment *) NIL) {
	    Vm_InitCode(codeFilePtr, (Vm_Segment *) NIL, (Vm_ExecInfo *) NIL);
	    (void) Fs_Close(codeFilePtr);
	    return(FALSE);
	}
	Vm_ValidatePages(segPtr, pageOffset, pageOffset + numPages - 1,
			 FALSE, TRUE);
	Vm_InitCode(codeFilePtr, segPtr, execInfoPtr);
	*codeSegPtrPtr = segPtr;
	notFound = TRUE;
    } else {
	notFound = FALSE;
    }

    if (usedFile || notFound) {
	(void) Fs_StreamCopy(codeFilePtr, &heapFilePtr);
    } else {
	heapFilePtr = codeFilePtr;
    }
    /* 
     * Set up the heap image.
     */
    segPtr = Vm_SegmentNew(VM_HEAP, heapFilePtr, execInfoPtr->heapFileOffset,
			       execInfoPtr->heapPages, 
			       execInfoPtr->heapPageOffset, procPtr);
    if (segPtr == (Vm_Segment *) NIL) {
	Vm_SegmentDelete(*codeSegPtrPtr, procPtr);
	(void) Fs_Close(heapFilePtr);
	return(FALSE);
    }
    Vm_ValidatePages(segPtr, execInfoPtr->heapPageOffset,
		     execInfoPtr->bssFirstPage - 1, FALSE, TRUE);
    if (execInfoPtr->bssLastPage > 0) {
	Vm_ValidatePages(segPtr, execInfoPtr->bssFirstPage,
			 execInfoPtr->bssLastPage, TRUE, TRUE);
    }
    heapSegPtr = segPtr;
    /*
     * Set up a new stack.
     */
    segPtr = Vm_SegmentNew(VM_STACK, (Fs_Stream *) NIL, 0, 
			   1, mach_LastUserStackPage, procPtr);
    if (segPtr == (Vm_Segment *) NIL) {
	Vm_SegmentDelete(*codeSegPtrPtr, procPtr);
	Vm_SegmentDelete(heapSegPtr, procPtr);
	return(FALSE);
    }
    Vm_ValidatePages(segPtr, mach_LastUserStackPage, 
		    mach_LastUserStackPage, TRUE, TRUE);

    Proc_Lock(procPtr);
    procPtr->genFlags |= PROC_NO_VM;
    Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_CODE], procPtr);
    Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_HEAP], procPtr);
    Vm_SegmentDelete(procPtr->vmPtr->segPtrArray[VM_STACK], procPtr);
    procPtr->vmPtr->segPtrArray[VM_CODE] = *codeSegPtrPtr;
    procPtr->vmPtr->segPtrArray[VM_HEAP] = heapSegPtr;
    procPtr->vmPtr->segPtrArray[VM_STACK] = segPtr;
    procPtr->genFlags &= ~PROC_NO_VM;
    VmMach_ReinitContext(procPtr);

    *entryPtr = execInfoPtr->entry;

    return(TRUE);
}
