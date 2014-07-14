/*-------------------------------------------------------------------------
 *
 * bitmapinsert.c
 *	  Tuple insertion in bitmaps for Postgres.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/tupdesc.h"
#include "access/heapam.h"
#include "access/bitmap.h"
#include "parser/parse_oper.h"
#include "miscadmin.h"

OffsetNumber _bitmap_movetobitmappage(Relation rel, BMLOVItem lovItem);
void _bitmap_mergewords(Relation rel, Buffer lovBuffer, BMLOVItem lovItem,
						uint64 tidNumber);
void _bitmap_insertsetbit(Relation rel, Buffer lovBuffer, BMLOVItem lovItem,
						  uint64 currTidNumber);
void _bitmap_inserttuple(Relation rel, uint64 currTidNumber,
						 Buffer lovMetaBuffer, ItemPointerData ht_ctid, 
						 TupleDesc tupDesc, Datum* attdata,
						 bool* nulls, uint32 attno,
						 Relation lovHeap, Relation lovIndex,
						 ScanKey scanKey, IndexScanDesc scanDesc);

/*
 * _bitmap_movetobitmappage() -- write bm_last_compword in an LOV item
 *								 to its bitmap page.
 *
 * The position of the last word in this page is returned.
 */
OffsetNumber
_bitmap_movetobitmappage(Relation rel, BMLOVItem lovItem)
{
	Buffer			lastBuffer;
	Page			lastPage;
	BMBitmapOpaque	bitmapPageOpaque;
	Buffer			newBuffer;
	BMBitmap		bitmap;
	OffsetNumber	lastWordPos;

	if (lovItem->bm_lov_head == InvalidBlockNumber)
	{
		lastBuffer = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
		_bitmap_bitmappageinit(rel, lastBuffer);
		_bitmap_log_newpage(rel, XLOG_BITMAP_INSERT_NEWBITMAP, 
							lastBuffer, NULL);

		lovItem->bm_lov_head = BufferGetBlockNumber(lastBuffer);
		lovItem->bm_lov_tail = lovItem->bm_lov_head;
	} else
		lastBuffer = 
			_bitmap_getbuf(rel, lovItem->bm_lov_tail, BM_WRITE);

	lastPage = BufferGetPage(lastBuffer);

	bitmapPageOpaque = (BMBitmapOpaque)
		PageGetSpecialPointer(lastPage);

	/* if there is not space in this page */
	if (bitmapPageOpaque->bm_hrl_words_used == BM_NUM_OF_HRL_WORDS_PER_PAGE)
	{
		newBuffer = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
		_bitmap_bitmappageinit(rel, newBuffer);

		_bitmap_log_newpage(rel, XLOG_BITMAP_INSERT_NEWBITMAP, 
							newBuffer, NULL);

		lovItem->bm_lov_tail = BufferGetBlockNumber(newBuffer);
		bitmapPageOpaque->bm_bitmap_next = lovItem->bm_lov_tail;

		_bitmap_log_bitmappage(rel, lastBuffer, true);

		_bitmap_wrtbuf(lastBuffer);

		lastBuffer = newBuffer;
		lastPage = BufferGetPage(lastBuffer);
		bitmapPageOpaque = (BMBitmapOpaque)
			PageGetSpecialPointer(lastPage);

	}

	bitmap = (BMBitmap) PageGetContents(lastPage);

	if (lovItem->bm_last_two_headerbits == 2 ||
		lovItem->bm_last_two_headerbits == 3)
		bitmap->bm_headerWords
			[(bitmapPageOpaque->bm_hrl_words_used/BM_HRL_WORD_SIZE)] |=
			(1<<(BM_HRL_WORD_SIZE-1-
				 (bitmapPageOpaque->bm_hrl_words_used%BM_HRL_WORD_SIZE)));
	bitmap->bm_contentWords[bitmapPageOpaque->bm_hrl_words_used] =
		lovItem->bm_last_compword;

	bitmapPageOpaque->bm_hrl_words_used++;
	lastWordPos = bitmapPageOpaque->bm_hrl_words_used;

	_bitmap_log_bitmappage(rel, lastBuffer, false);

	_bitmap_wrtbuf(lastBuffer);

	return lastWordPos;
}

/*
 * _bitmap_mergewords() -- merge two last words in a bitmap.
 *
 * If bm_last_word can not be merged into bm_last_compword, we write
 * bm_last_compword into the disk, and reset bm_last_word and
 * bm_last_compword.
 */
void
_bitmap_mergewords(Relation rel, Buffer lovBuffer, BMLOVItem lovItem,
				   uint64 tidNumber)
{
	OffsetNumber	lastWordPosInBlock = 0;
	BM_HRL_WORD		lastWordInBlock = ALL_ZERO;
	bool			lastWordIsFill = 
						(lovItem->bm_last_two_headerbits == 1 ||
						 lovItem->bm_last_two_headerbits == 3);

	/* If two words are both fill word, then try to increase the
	   fill length in bm_last_compword. If this fill length exceeds
	   the maximum fill length, then write it out to disk, and create
	   a new word for bm_last_compword. */
	if ((lovItem->bm_last_two_headerbits == 3) &&
		(GET_FILL_BIT(lovItem->bm_last_compword) ==
		 GET_FILL_BIT(lovItem->bm_last_word)))
	{
		BM_HRL_WORD	lastCompWordFillLength = 
			FILL_LENGTH(lovItem->bm_last_compword);
		BM_HRL_WORD lastWordFillLength =
			FILL_LENGTH(lovItem->bm_last_word);

		if (lastCompWordFillLength+lastWordFillLength >= MAX_FILL_LENGTH) {
			lovItem->bm_last_compword += 
				(MAX_FILL_LENGTH-lastCompWordFillLength);
			lovItem->bm_last_word -=
				(MAX_FILL_LENGTH-lastCompWordFillLength);

			lastWordPosInBlock = 
				_bitmap_movetobitmappage(rel, lovItem);
			lovItem->bm_last_compword  = lovItem->bm_last_word;
			lastWordInBlock = lovItem->bm_last_compword;
		}

		else
			lovItem->bm_last_compword += lastWordFillLength;

		lovItem->bm_last_two_headerbits = 2;
	}
	else
	{
		if (tidNumber != BM_HRL_WORD_SIZE)
		{
			lastWordPosInBlock =
				_bitmap_movetobitmappage(rel, lovItem);
			lastWordInBlock = lovItem->bm_last_compword;
		}

		/* move the last word to the last complete word. */
		lovItem->bm_last_compword = lovItem->bm_last_word;
		if (lastWordIsFill)
			lovItem->bm_last_two_headerbits = 2;
		else 
			lovItem->bm_last_two_headerbits = 0;
	}

	lovItem->bm_last_word = ALL_ZERO;

}

/*
 * _bitmap_insertsetbit() -- insert a set bit into a bitmap.
 */
void
_bitmap_insertsetbit(Relation rel, Buffer lovBuffer, BMLOVItem lovItem,
					 uint64 currTidNumber)
{
	uint32		numOfZeros;
	uint16		zerosNeeded, insertingPos;

	/* If this is the first time to insert a set bit, then
	   we have already inserted the first currTidNumber/BM_HRL_WORD_SIZE
	   zeros. */
	if (lovItem->bm_last_setbit == 0)
		numOfZeros = currTidNumber%BM_HRL_WORD_SIZE;
	else
		numOfZeros = currTidNumber - lovItem->bm_last_setbit - 1;

	/* If there are some zeros between these two set bits, then
	   we need to fill these zero bits into the bitmap. */
	if (numOfZeros > 0){

		/* try to fill bm_last_word */
		if (lovItem->bm_last_setbit == 0)
			zerosNeeded = BM_HRL_WORD_SIZE;
		else
			zerosNeeded = BM_HRL_WORD_SIZE - 
				((lovItem->bm_last_setbit-1)%BM_HRL_WORD_SIZE) - 1 ;
		if ((zerosNeeded != 0) && (numOfZeros >= zerosNeeded))
		{
			/* merge bm_last_word into bm_last_compword */
			_bitmap_mergewords (rel, lovBuffer, lovItem,
								(lovItem->bm_last_setbit+zerosNeeded));

			numOfZeros -= zerosNeeded;
		}

		/* if the remaining zeros are more than BM_HRL_WORD_SIZE,
		   we construct the last word to be a fill word, and merge it
		   with bm_last_compword */
		if (numOfZeros >= BM_HRL_WORD_SIZE)
		{
			uint32	numOfTotalFillWords = numOfZeros/BM_HRL_WORD_SIZE;
			uint32	loopNo=0;

			while (numOfTotalFillWords > 0) {
				BM_HRL_WORD numOfFillWords ;
				if (numOfTotalFillWords >= MAX_FILL_LENGTH)
					numOfFillWords = MAX_FILL_LENGTH;
				else
					numOfFillWords = numOfTotalFillWords;

				lovItem->bm_last_word = 
					BM_MAKE_FILL_WORD(0, numOfFillWords);
				lovItem->bm_last_two_headerbits |= 1;
				_bitmap_mergewords (rel, lovBuffer, lovItem,
					(lovItem->bm_last_setbit+zerosNeeded+
					loopNo*MAX_FILL_LENGTH*BM_HRL_WORD_SIZE+
					numOfFillWords*BM_HRL_WORD_SIZE));
				loopNo++;

				numOfTotalFillWords -= numOfFillWords;
				numOfZeros -= numOfFillWords*BM_HRL_WORD_SIZE;
			}
		}
	}

	Assert((numOfZeros >= 0) && (numOfZeros<BM_HRL_WORD_SIZE));

	insertingPos = BM_HRL_WORD_SIZE - 
			((currTidNumber-1)%BM_HRL_WORD_SIZE) - 1 ;
	lovItem->bm_last_word |= (1<<insertingPos);
	lovItem->bm_last_setbit = currTidNumber;
	
	if (currTidNumber%BM_HRL_WORD_SIZE == 0)
	{
		if (lovItem->bm_last_word == LITERAL_ALL_ZERO)
		{
			lovItem->bm_last_word =
				BM_MAKE_FILL_WORD(0, 1);
			lovItem->bm_last_two_headerbits |= 1;
		}

		else if (lovItem->bm_last_word == LITERAL_ALL_ONE)
		{
			lovItem->bm_last_word =
				BM_MAKE_FILL_WORD(1, 1);
			lovItem->bm_last_two_headerbits |= 1;
		}

		_bitmap_mergewords(rel, lovBuffer, lovItem, currTidNumber);
	}
}

/*
 * _bitmap_inserttuple() -- insert a new tuple into all bitmaps for the given
 *		attribute.
 *
 * This function searches through all values that have been stored for this
 * attribute. If this value exists, then a bit 1 is appended to the 
 * corresponding bitmap. If this value does not exist, a new item is added
 * to this list of values.
 *
 * For better search performance, we also maintain a btree structure for
 * all these values. So if this inserting tuple is new, then we also need
 * to insert a new entry into the corresponding btree.
 */
void
_bitmap_inserttuple(Relation rel, uint64 currTidNumber,
					Buffer lovMetaBuffer, ItemPointerData ht_ctid, 
					TupleDesc tupDesc, Datum* attdata,
					bool* nulls, uint32 attno,
					Relation lovHeap, Relation lovIndex,
					ScanKey scanKey, IndexScanDesc scanDesc)
{
	Page			lovMetapage;
	BlockNumber		lovBlock;
	OffsetNumber	lovOffset;
	bool			blockNull, offsetNull;

	/* if the inserting tuple has the value NULL, then the LOV item is
	   the first item in the lovBuffer */
	if (*nulls)
	{
		Buffer			currLovBuffer;
		Page			currLovPage;
		BMLOVItem		lovItem;

		/* load the page that contains the bitmap to be appended 
			by one set bit. */
		currLovBuffer = 
			_bitmap_getbuf(rel, BM_LOV_STARTPAGE+(attno-1), BM_WRITE);
		currLovPage = BufferGetPage(currLovBuffer);

		lovItem = (BMLOVItem)
			PageGetItem(currLovPage, PageGetItemId(currLovPage, 1));

		_bitmap_insertsetbit(rel, currLovBuffer, lovItem, currTidNumber);

		_bitmap_log_lovitem(rel, currLovBuffer, false, 1, lovItem);

		_bitmap_wrtbuf(currLovBuffer);

		return;
	}

	/* search through the lov heap and index to find the LOV item which has 
	   the same value as the inserting tuple. If such an item is found, 
	   we append a bit 1 into its bitmap.
	 */
	if (_bitmap_findvalue(lovHeap, lovIndex, *attdata, *nulls, 
						  scanKey, scanDesc,
						  &lovBlock, &blockNull, &lovOffset, &offsetNull))
	{
		Buffer			currLovBuffer;
		Page			currLovPage;
		BMLOVItem		lovItem;

		/* load the page that contains the bitmap to be appended 
			by one set bit. */
		currLovBuffer = _bitmap_getbuf(rel, lovBlock, BM_WRITE);
		currLovPage = BufferGetPage(currLovBuffer);

		lovItem = (BMLOVItem)
				PageGetItem(currLovPage, PageGetItemId(currLovPage, lovOffset));

		_bitmap_insertsetbit(rel, currLovBuffer, lovItem, currTidNumber);

		_bitmap_log_lovitem(rel, currLovBuffer, false, lovOffset, lovItem);

		_bitmap_wrtbuf(currLovBuffer);
	}

	/* if the inserting tuple has a new value, then we create a new LOV item,
	   and insert it into the lov heap and index.*/
	else
	{
		BMLOVItem		lovItem;
		BMLOVMetaItem	lovMetaItems;
		Buffer			currLovBuffer;
		Page			currLovPage;
		Datum			lovDatum[3];
		char			lovNulls[3];
		OffsetNumber	itemSize, newOffset;

		LockBuffer(lovMetaBuffer, BM_WRITE);
		lovMetapage = BufferGetPage(lovMetaBuffer);
		lovMetaItems = (BMLOVMetaItem)PageGetContents(lovMetapage);

		currLovBuffer = 
			_bitmap_getbuf(rel, lovMetaItems[attno-1].bm_lov_lastpage, 
							BM_WRITE);
		currLovPage = BufferGetPage(currLovBuffer);

		lovItem = _bitmap_formitem(currTidNumber);

		newOffset = OffsetNumberNext(PageGetMaxOffsetNumber(currLovPage));
		itemSize = sizeof(BMLOVItemData);

		if (itemSize > PageGetFreeSpace(currLovPage))
		{
			Buffer		newLovBuffer;

			/* create a new LOV page. */
			newLovBuffer = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
			_bitmap_lovpageinit(rel, newLovBuffer);

			_bitmap_log_newpage(rel, XLOG_BITMAP_INSERT_NEWLOV, 
								newLovBuffer, NULL);
	
			_bitmap_relbuf(currLovBuffer);

			currLovBuffer = newLovBuffer;
			currLovPage = BufferGetPage (currLovBuffer);
			newOffset = OffsetNumberNext(PageGetMaxOffsetNumber(currLovPage));

			lovMetaItems[attno-1].bm_lov_lastpage = 
				BufferGetBlockNumber(currLovBuffer);

			_bitmap_log_lovmetapage(rel, lovMetaBuffer, tupDesc->natts);
			_bitmap_wrtnorelbuf(lovMetaBuffer);
		}

		LockBuffer(lovMetaBuffer, BUFFER_LOCK_UNLOCK);

		lovDatum[0] = *attdata;
		lovNulls[0] = ((*nulls) ? 'n' : ' ');
		lovDatum[1] = Int32GetDatum(BufferGetBlockNumber(currLovBuffer));
		lovNulls[1] = ' ';
		lovDatum[2] = Int16GetDatum(newOffset);
		lovNulls[2] = ' ';

		_bitmap_insertsetbit(rel, currLovBuffer, lovItem, currTidNumber);

		if (PageAddItem(currLovPage, (Item)lovItem, itemSize, newOffset,
						LP_USED) == InvalidOffsetNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("failed to add LOV item to \"%s\"",
					RelationGetRelationName(rel))));


		_bitmap_log_lovitem(rel, currLovBuffer, true, newOffset, lovItem);

		pfree(lovItem);

		_bitmap_wrtbuf(currLovBuffer);
		
		_bitmap_insert_lov(lovHeap, lovIndex, lovDatum, lovNulls);
	}
}

/*
 * _bitmap_buildinsert() -- insert an index tuple during index creation.
 */
void
_bitmap_buildinsert(Relation rel, ItemPointerData ht_ctid,
					Datum* attdata, bool* nulls,
					BMBuildState*	state)
{
	TupleDesc	tupDesc;
	TupleDesc	tempTupDesc = NULL;
	uint32		attno;
	uint64		tidOffset;

	attno = 0;
	tupDesc = RelationGetDescr(rel);

	Assert(ItemPointerGetOffsetNumber(&ht_ctid) <= MaxNumHeapTuples);

	tidOffset = 
		((uint64)ItemPointerGetBlockNumber(&ht_ctid) * 
		 MaxNumHeapTuples)
		+ ((uint64)ItemPointerGetOffsetNumber(&ht_ctid));
			/*(1<<(8*sizeof(OffsetNumber))))*/

	/* insert a new bit into each bitmap using the a HRL scheme */
	do {
		/* create a temporary tuple descriptor for the current attribute */
		tempTupDesc = state->bm_tupDescs[attno];

		/* insert this new tuple into each bitmap for this attribute by appending
		   a bit in each bitmap of all attribute values. */
		_bitmap_inserttuple
			(rel, tidOffset, 
			 state->bm_lov_metabuf, ht_ctid,
			 tempTupDesc, &(attdata[attno]), &(nulls[attno]), 
			 attno+1,
			 state->bm_lov_heaps[attno], state->bm_lov_indexes[attno],
			 &(state->bm_lov_scanKeys[attno]), state->bm_lov_scanDescs[attno]);

		attno++;

	} while (attno < tupDesc->natts);
}

/*
 * _bitmap_doinsert() -- insert an index tuple.
 */
void
_bitmap_doinsert(Relation rel, ItemPointerData ht_ctid,
				 Datum* attdata, bool* nulls)
{
	uint64			tidOffset;

	TupleDesc		tupDesc;
	BlockNumber		lovMetaPageNumber;
	Buffer			lovMetaBuffer;

	TupleDesc		tempTupDesc = NULL;
	uint32			attno;

	tidOffset = 
		((uint64)ItemPointerGetBlockNumber(&ht_ctid) * 
		 MaxNumHeapTuples)
		+ ((uint64)ItemPointerGetOffsetNumber(&ht_ctid));

	tupDesc = RelationGetDescr(rel);
	if (tupDesc->natts <= 0)
	{
		return ;
	}

	lovMetaPageNumber = BM_LOV_STARTPAGE-1;
	lovMetaBuffer = _bitmap_getbuf(rel, lovMetaPageNumber, BM_NOLOCK);

	attno = 0;

	/* insert a new bit into each bitmap using the HRL scheme */
	do {
		Relation	lovHeap, lovIndex;
		Page			lovMetapage;
		RegProcedure	opfuncid;
		ScanKeyData		scanKeyData;
		IndexScanDesc	scanDesc;

		/* create a temporary tuple descriptor for the current attribute */
		tempTupDesc = _bitmap_copyOneInTupleDesc(tempTupDesc, tupDesc, attno);

		lovMetapage = BufferGetPage(lovMetaBuffer);
		_bitmap_open_lov_heapandindex(rel, lovMetapage, attno,
									  &lovHeap, &lovIndex, RowExclusiveLock);

		opfuncid = equality_oper_funcid(tempTupDesc->attrs[0]->atttypid);

		ScanKeyEntryInitialize
			(&scanKeyData, SK_ISNULL, 1, 
			BTEqualStrategyNumber, InvalidOid, opfuncid, 0);
		scanDesc =
			index_beginscan (lovHeap, lovIndex, SnapshotAny, 1, &scanKeyData);

		/* insert this new tuple into each bitmap for this attribute by 
		   appending a bit in each bitmap of all attribute values. */
		_bitmap_inserttuple
			(rel, tidOffset, lovMetaBuffer, ht_ctid,
			 tempTupDesc, &(attdata[attno]), &(nulls[attno]), 
			 attno+1, lovHeap, lovIndex, &scanKeyData, scanDesc);

		index_endscan(scanDesc);
		_bitmap_close_lov_heapandindex(lovHeap, lovIndex, RowExclusiveLock);

		attno++;

	} while (attno < tupDesc->natts);

	FreeTupleDesc(tempTupDesc);

	ReleaseBuffer(lovMetaBuffer);
}
