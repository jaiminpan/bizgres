/*-------------------------------------------------------------------------
 *
 * bitmap.c
 *	Implementation of Word-Aligned Hybrid (WAH) bitmap index.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	$PostgreSQL$
 *
 * NOTES
 *	This file contains only the public interface routines.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/bitmap.h"
#include "catalog/index.h"
#include "storage/lmgr.h"
#include "parser/parse_oper.h"

static void bmbuildCallback(Relation index,
							HeapTuple htup,
							Datum *attdata,
							bool *nulls,
							bool tupleIsAlive,
							void *state);


/*
 * bmbuild() -- Build a new bitmap index.
 */
Datum
bmbuild(PG_FUNCTION_ARGS)
{
	Relation    heap = (Relation) PG_GETARG_POINTER(0);
	Relation    index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	double      reltuples;
	BMBuildState bstate;

	TupleDesc	tupDesc;
	uint32 		attno;
	Page		lovMetapage;

	/* We expect this to be called exactly once. */
	if (RelationGetNumberOfBlocks(index) != 0)
	{
		ereport (ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				errmsg("index \"%s\" already contains data",
				RelationGetRelationName(index))));
	}

	/* initialize the bitmap */
	_bitmap_init(index);

	/* initialize the build state */
	bstate.bm_metabuf = _bitmap_getbuf(index, BM_METAPAGE, BM_NOLOCK);
	bstate.bm_lov_metabuf = _bitmap_getbuf(index, BM_LOV_STARTPAGE-1, BM_NOLOCK);
	tupDesc = RelationGetDescr(index);
	if (tupDesc->natts <= 0)
		PG_RETURN_VOID () ;

	bstate.bm_tupDescs = palloc(tupDesc->natts * sizeof(TupleDesc));
	bstate.bm_lov_heaps = palloc(tupDesc->natts * sizeof(Relation));
	bstate.bm_lov_indexes = palloc(tupDesc->natts * sizeof(Relation));
	bstate.bm_lov_scanKeys = 
		palloc(tupDesc->natts * sizeof(ScanKeyData));
	bstate.bm_lov_scanDescs = 
		palloc(tupDesc->natts * sizeof(IndexScanDesc));

	LockBuffer(bstate.bm_lov_metabuf, BM_WRITE);
	lovMetapage = BufferGetPage(bstate.bm_lov_metabuf);
	for (attno=0; attno<tupDesc->natts; attno++)
	{
		RegProcedure	opfuncid;

		bstate.bm_tupDescs[attno] = 
			_bitmap_copyOneInTupleDesc(NULL, tupDesc, attno);

		_bitmap_open_lov_heapandindex
			(index, lovMetapage, attno, 
			 &(bstate.bm_lov_heaps[attno]), &(bstate.bm_lov_indexes[attno]), 
			 RowExclusiveLock);
		opfuncid = 
			equality_oper_funcid
				((bstate.bm_tupDescs[attno])->attrs[0]->atttypid);

		ScanKeyEntryInitialize
			(&(bstate.bm_lov_scanKeys[attno]), SK_ISNULL, 1, 
			BTEqualStrategyNumber, InvalidOid, opfuncid, 0);
		bstate.bm_lov_scanDescs[attno] = 
			index_beginscan (bstate.bm_lov_heaps[attno],
							 bstate.bm_lov_indexes[attno],
							 SnapshotAny, 1, &(bstate.bm_lov_scanKeys[attno]));
	}

	LockBuffer(bstate.bm_lov_metabuf, BUFFER_LOCK_UNLOCK);
  ereport(DEBUG1, (errmsg_internal("build initialize complete")));
	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   bmbuildCallback, (void*)&bstate);

  ereport(DEBUG1, (errmsg_internal("IndexBuildHeapScan complete")));
	ReleaseBuffer(bstate.bm_lov_metabuf);

	ReleaseBuffer(bstate.bm_metabuf);

  ereport(DEBUG1, (errmsg_internal("ReleaseBuffer complete")));
	for (attno=0; attno<tupDesc->natts; attno++)
	{
		index_endscan(bstate.bm_lov_scanDescs[attno]);

		FreeTupleDesc(bstate.bm_tupDescs[attno]);	
		_bitmap_close_lov_heapandindex
			(bstate.bm_lov_heaps[attno],bstate.bm_lov_indexes[attno],
			 RowExclusiveLock);
	}

  ereport(DEBUG1, (errmsg_internal("heap scan complete")));

	pfree(bstate.bm_tupDescs);
	pfree(bstate.bm_lov_heaps);
	pfree(bstate.bm_lov_indexes);
	pfree(bstate.bm_lov_scanKeys);
	pfree(bstate.bm_lov_scanDescs);

	IndexCloseAndUpdateStats(heap, reltuples, index, bstate.ituples);

  ereport(DEBUG1, (errmsg_internal("IndexCloseAndUpdateStats complete")));

	PG_RETURN_VOID () ;
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
bmbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				bool *nulls,
				bool tupleIsAlive,
				void *state)
{
	BMBuildState *bstate = (BMBuildState *) state;

	_bitmap_buildinsert(index, htup->t_self, attdata, nulls, bstate);

	bstate->ituples += 1;
}

/*
 * bminsert() -- insert an index tuple into a bitmap index.
 */
Datum
bminsert(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	Datum		*datum = (Datum *) PG_GETARG_POINTER(1);
	bool		*nulls = (bool *) PG_GETARG_POINTER(2);
	ItemPointer	ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
#endif

	_bitmap_doinsert(rel, *ht_ctid, datum, nulls);

	PG_RETURN_BOOL(true);
}

/*
 * bmgettuple() -- return the next tuple in a scan.
 */
Datum
bmgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);

	bool res;

	/* If we have already started this scan, then we just continue this
	   scan in the appropriate direction. */
	if (ItemPointerIsValid(&(scan->currentItemData)))
		res = _bitmap_next(scan, dir);
	else
		res = _bitmap_first(scan, dir);

	PG_RETURN_BOOL(res);
}

/*
 * bmgetmulti() -- return multiple tuples at once in a scan.
 */
Datum
bmgetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer	tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32		*returned_tids = (int32 *) PG_GETARG_POINTER(3);

	bool		res = false;
	int32		ntids = 0;

	while (ntids < max_tids)
	{
		if (ItemPointerIsValid(&(scan->currentItemData)))
			res = _bitmap_next(scan, ForwardScanDirection);
		else
			res = _bitmap_first(scan, ForwardScanDirection);

		if (!res)
			break ;

		tids[ntids] = scan->xs_ctup.t_self;
		ntids ++;
	}

	*returned_tids = ntids;
	PG_RETURN_BOOL(res);
}

/*
 * bmbeginscan() -- start a scan on the bitmap index.
 */
Datum
bmbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, scankey);

	PG_RETURN_POINTER(scan);
}

/*
 * bmrescan() -- restart a scan on the bitmap index.
 */
Datum
bmrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey			scankey = (ScanKey) PG_GETARG_POINTER(1);
	ItemPointer	iptr;
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;
	BMBitmapScanPosition	bmScanPos;
	uint32			keyNo;

	if (so == NULL) 		/* if called from bmbeginscan */
	{
		so = (BMScanOpaque) palloc(sizeof(BMScanOpaqueData));
		so->bm_currPos = NULL;
		so->bm_markPos = NULL;

		scan->opaque = so;
	}

	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		Assert(so->bm_currPos != NULL);

		/* release the buffers that have been stored for each related bitmap.*/
		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_currPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
				ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);
		}

		pfree(so->bm_currPos);
		so->bm_currPos = NULL;
		
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		Assert(so->bm_markPos != NULL);

		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_markPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
				ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);

		}

		pfree(so->bm_markPos);
		so->bm_markPos = NULL;

		ItemPointerSetInvalid(iptr);
	}

	/* reset the scan key */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));

	PG_RETURN_VOID();
}

/*
 * bmendscan() -- close a scan.
 */
Datum
bmendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;

	BMBitmapScanPosition	bmScanPos;
	uint32 keyNo;

	/* free the space */
	if (so->bm_currPos != NULL)
	{

		/* release the buffers that have been stored for each related bitmap.*/
		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_currPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
				ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);
		}

		pfree(so->bm_currPos);
		so->bm_currPos = NULL;
	}

	if (so->bm_markPos != NULL)
	{
		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_markPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
				ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);
		}

		pfree(so->bm_markPos);
		so->bm_markPos = NULL;
	}

	pfree(so);

	scan->opaque = NULL;

	if (ItemPointerIsValid(&(scan->currentItemData)))
		ItemPointerSetInvalid(&(scan->currentItemData));
	if (ItemPointerIsValid(&(scan->currentMarkData)))
		ItemPointerSetInvalid(&(scan->currentMarkData));


	PG_RETURN_VOID();
}

/*
 * bmmarkpos() -- save the current scan position.
 */
Datum
bmmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;

	BMBitmapScanPosition	bmScanPos;
	uint32 keyNo;

	/* free the space */
	if (ItemPointerIsValid(&(scan->currentMarkData)))
	{
		/* release the buffers that have been stored for each related bitmap.*/
		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_markPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
			{
				ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);
				(bmScanPos[keyNo]).bm_lovBuffer = InvalidBuffer;
			}
		}
		
		ItemPointerSetInvalid(&(scan->currentMarkData));
	}

	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		uint32	size = MAXALIGN(sizeof(BMScanPositionData)) +
					   scan->numberOfKeys * sizeof(BMBitmapScanPositionData);
		/* set the mark position */
		if (so->bm_markPos == NULL)
		{
			so->bm_markPos = (BMScanPosition) 
				palloc(size);
		}

		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_currPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
				IncrBufferRefCount((bmScanPos[keyNo]).bm_lovBuffer);
		}		

		memcpy(so->bm_markPos, so->bm_currPos, size);

		scan->currentMarkData = scan->currentItemData;
	}

	PG_RETURN_VOID();
}

/*
 * bmrestrpos() -- restore a scan to the last saved position.
 */
Datum
bmrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;

	BMBitmapScanPosition	bmScanPos;
	uint32 keyNo;

	/* free space */
	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		/* release the buffers that have been stored for each related bitmap.*/
		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_currPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
			{
				ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);
				(bmScanPos[keyNo]).bm_lovBuffer = InvalidBuffer;
			}
		}

		ItemPointerSetInvalid(&(scan->currentItemData));
	}

	if (ItemPointerIsValid(&(scan->currentMarkData)))
	{
		uint32	size = MAXALIGN(sizeof(BMScanPositionData)) +
					   scan->numberOfKeys * sizeof(BMBitmapScanPositionData);

		/* set the current position */
		if (so->bm_currPos == NULL)
		{
			so->bm_currPos = (BMScanPosition) palloc(size);
		}

		bmScanPos = (BMBitmapScanPosition) 
			(((char*)so->bm_markPos) + MAXALIGN(sizeof(BMScanPositionData)));

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
				IncrBufferRefCount((bmScanPos[keyNo]).bm_lovBuffer);
		}		

		memcpy(so->bm_currPos, so->bm_markPos, size);
		scan->currentItemData = scan->currentMarkData;
	}

	PG_RETURN_VOID();
}

/*
 * bmbulkdelete() -- bulk delete index entries
 *
 * This function marks the tuples that have been deleted in the heap.
 */
Datum
bmbulkdelete(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);

	IndexBulkDeleteResult* result;

	result = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	result->num_pages = RelationGetNumberOfBlocks(rel);
/*
	result->num_index_tuples = numOfIndexTuples;
	result->tuples_removed = numOfTuplesRemoved;
*/
	result->tuples_removed = 0;
	
	PG_RETURN_POINTER(result);

}

/*
 * bmvacuumcleanup() -- post-vacuum cleanup.
 *
 * We do not really delete any pages and return them to the freelist here.
 */
Datum
bmvacuumcleanup(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	/* IndexVacuumCleanupInfo *info = 
		(IndexVacuumCleanupInfo *) PG_GETARG_POINTER(1); */
	IndexBulkDeleteResult *stats = 
		(IndexBulkDeleteResult *) PG_GETARG_POINTER(2);

	/* update statistics */
	stats->num_pages = RelationGetNumberOfBlocks(rel);
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	PG_RETURN_POINTER(stats);
}

/*
 * bmgetbitmapwords() -- return a given number of bitmap words in a scan.
 */
Datum
bmgetbitmapwords(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	uint32			maxNumOfBitmapWords = PG_GETARG_UINT32(1);
	uint32*			returnedNumOfBitmapWords = (uint32*)PG_GETARG_POINTER(2);
	BM_HRL_WORD*	bitmapHeaderWords = (BM_HRL_WORD*)PG_GETARG_POINTER(3);
	BM_HRL_WORD*	bitmapContentWords = (BM_HRL_WORD*)PG_GETARG_POINTER(4);

	bool 	res = false;

	Assert(maxNumOfBitmapWords > 0 &&
		   maxNumOfBitmapWords%BM_HRL_WORD_SIZE == 0 &&
		   maxNumOfBitmapWords%BM_NUM_OF_HRL_WORDS_PER_PAGE == 0);

	*returnedNumOfBitmapWords = 0;
	while ((*returnedNumOfBitmapWords) < maxNumOfBitmapWords)
	{
		BMScanPosition	scanPos;
		int32			numOfWords = 0;

		if (((BMScanOpaque)(scan->opaque))->bm_currPos != NULL)
			res = _bitmap_nextblockwords(scan, ForwardScanDirection);
		else
			res = _bitmap_firstblockwords(scan, ForwardScanDirection);

		if (!res)
			break;

		scanPos = ((BMScanOpaque) scan->opaque)->bm_currPos;
		if (scanPos->bm_num_of_words >= 
			(maxNumOfBitmapWords - *returnedNumOfBitmapWords))
			numOfWords = maxNumOfBitmapWords - *returnedNumOfBitmapWords;
		else
			numOfWords = scanPos->bm_num_of_words;

		/* copy the content words */
		memcpy(bitmapContentWords + *returnedNumOfBitmapWords,
			   scanPos->bm_last_scanwords,
			   numOfWords*sizeof(BM_HRL_WORD));

		/* copy the header words */
		memcpy(bitmapHeaderWords +
				(*returnedNumOfBitmapWords)/BM_HRL_WORD_SIZE,
				scanPos->bm_last_scanHeaderWords,
				(numOfWords/BM_HRL_WORD_SIZE + 
				((numOfWords%BM_HRL_WORD_SIZE == 0) ? 0 : 1))*
				 sizeof(BM_HRL_WORD));

		*returnedNumOfBitmapWords += numOfWords;

		if (*returnedNumOfBitmapWords%BM_NUM_OF_HRL_WORDS_PER_PAGE != 0)
			break;
	}

	PG_RETURN_BOOL(res);
}
