/*
 * fsdmInt.h --
 *
 *	Definitions related to the storage of a filesystem on a disk.
 *
 * Copyright 1989 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 *
 *
 * $Header$ SPRITE (Berkeley)
 */

#ifndef _FSDMINT
#define _FSDMINT

/*
 * The lost and found directory is preallocated and is of a fixed size. Define
 * its size in 4K blocks here.
 */
#define	FSDM_NUM_LOST_FOUND_BLOCKS	2

/*
 * Structure to keep information about each fragment.
 */

typedef struct FsdmFragment {
    List_Links	links;		/* Links to put in list of free fragments of 
				   this size. */
    int		blockNum;	/* Block that this fragment comes from. */
} FsdmFragment;


/*
 * A table of domains.  This is used to go from domain number
 * to the state for the domain.
 *
 * FSDM_MAX_LOCAL_DOMAINS defines how many local domains a server can keep
 *      track of.
 */
#define FSDM_MAX_LOCAL_DOMAINS    10
extern Fsdm_Domain *fsdmDomainTable[];


extern ReturnStatus	FsdmFileDescAllocInit();
extern ReturnStatus	FsdmWriteBackFileDescBitmap();
extern	ReturnStatus	FsdmBlockAllocInit();
extern	ReturnStatus	FsdmWriteBackDataBlockBitmap();

extern	void		FsdmBlockFind();
extern	void		FsdmBlockFree();
extern	void		FsdmFragFind();
extern	void		FsdmFragFree();

extern  int		FsdmBlockRealloc();
extern  void	 	FsdmRecordDeletionStats();
extern  ReturnStatus	FsdmWriteBackSummaryInfo();


#endif /* _FSDMINT */
