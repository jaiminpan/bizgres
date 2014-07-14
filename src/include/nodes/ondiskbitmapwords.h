/*-------------------------------------------------------------------------
 *
 * ondiskbitmapwords.h
 * 	PostgreSQL
 *
 * Copyright (c) 2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	$PostgreSQL$
 *-------------------------------------------------------------------------
 */

#ifndef ONDISKBITMAPWORDS_H
#define ONDISKBITMAPWORDS_H

#include "nodes/plannodes.h"
#include "access/bitmap.h"

struct PlanState;

#define ODBM_BATCH_TIDS  BATCH_TIDS

typedef struct ODBMIterateResult
{
	uint64	nextTid;
	uint32	lastScanPos;
	uint32	lastScanWordNo;
	uint32	numOfTids;
	uint64	nextTids[ODBM_BATCH_TIDS];
	uint32	nextTidLoc;
} ODBMIterateResult;

/*
 * The structure of OnDiskBitmapWords.
 */
typedef struct OnDiskBitmapWords
{
	NodeTag	type; 				/* to make it a valid Node */
	MemoryContext mcxt;			/* memory context containing me */
	uint32	maxNumOfWords;		/* maximum number of words in this list */

	/* The following two variables are for performing AND/OR operations */

	/* number of words that have been read in this list */
	uint32	numOfWordsRead;
	/* the position of the next word to be read */
	uint32	nextReadNo;

	/* the starting tid number of this list of bitmap words */
	uint64	firstTid;
	/* the starting position of meaningful bitmap words in the list */
	uint32	startNo;
	uint32	numOfWords;			/* the number of bitmap words in this list */
	BM_HRL_WORD* bitmapHeaderWords; /* the header words */
	BM_HRL_WORD* bitmapContentWords;	/* the list of bitmap words */
} OnDiskBitmapWords;

#define ODBM_MAX_WORDS BM_NUM_OF_HRL_WORDS_PER_PAGE*2

/* function prototypes in nodes/ondiskbitmapwords.c */
extern OnDiskBitmapWords* odbm_create(uint32 maxNumOfWords);
extern void odbm_free(OnDiskBitmapWords *odbm);
extern ODBMIterateResult* odbm_res_create(OnDiskBitmapWords *odbm);
extern void odbm_res_free(ODBMIterateResult* odbmres);

extern void odbm_reset(OnDiskBitmapWords *odbm);
extern uint64 odbm_findnexttid(OnDiskBitmapWords *odbm,
							   ODBMIterateResult *odbmres);
extern void odbm_findnexttids(OnDiskBitmapWords *odbm,
							  ODBMIterateResult *odbmres, uint32 maxTids);
extern void odbm_intersect(OnDiskBitmapWords **odbms, uint32 numOdbms,
						   OnDiskBitmapWords *result);
extern void odbm_union(OnDiskBitmapWords **odbms, uint32 numOdbms,
					   OnDiskBitmapWords *result);
extern void odbm_begin_iterate(OnDiskBitmapWords *odbm,
							   ODBMIterateResult *odbmres);
extern void odbm_set_bitmaptype(Plan* plan, bool inmem);
extern void odbm_set_child_resultnode(struct PlanState* ps, 
									  OnDiskBitmapWords* odbm);

#endif /* ONDISKBITMAPWORDS_H */
