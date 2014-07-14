/*-------------------------------------------------------------------------
 *
 * bitmapsearch.c
 *	  Search routines for Postgres bitmap index access method.
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
#include "access/bitmap.h"
#include "storage/lmgr.h"
#include "parser/parse_oper.h"

static void _bitmap_findbitmaps(IndexScanDesc scan, ScanDirection dir);
static void _bitmap_comp_nonzero_and_words(IndexScanDesc scan);
static void _bitmap_find_next_n_setbits
	(IndexScanDesc scan, uint32 maxTids, uint64* tidLocs);
static void
_bitmap_readWords(Relation rel, Buffer lovBuffer, OffsetNumber lovOffset,
				  BlockNumber* nextBlockNoP, 
				  BM_HRL_WORD* headerWords, BM_HRL_WORD* words,
				  uint32* numOfWordsP, bool* readLastWords);

/*
 * _bitmap_find_bitset() -- find the leftmost set bit (bit=1) in the 
 * 		given word since 'lastPos', not including 'lastPos'.
 *
 * The leftmost bit in the given word is considered the position 1, and
 * the rightmost bit is considered the position BM_HRL_WORD_SIZE.
 *
 * If such set bit does not exist in this word, 0 is returned.
 */
uint8
_bitmap_find_bitset(BM_HRL_WORD word, uint8 lastPos)
{
	uint8 pos = lastPos+1;
	BM_HRL_WORD	leftmostBitWord;

	if (pos > BM_HRL_WORD_SIZE)
	  return 0;

	leftmostBitWord = (((BM_HRL_WORD)1) << (BM_HRL_WORD_SIZE - pos));

	while ((pos<=BM_HRL_WORD_SIZE) &&
		   ((word & leftmostBitWord) == 0))
	{
		leftmostBitWord >>= 1;
		pos ++;
	}

	if (pos > BM_HRL_WORD_SIZE)
		pos = 0;

	return pos;
}

/*
 * _bitmap_first() -- find the first tuple that satisfies the given scan.
 */
bool
_bitmap_first(IndexScanDesc scan,
			  ScanDirection dir)
{
	_bitmap_findbitmaps(scan, dir);
	return _bitmap_next(scan, dir);
}

/*
 * _bitmap_next() -- return the next tuple that satisfies the given scan.
 */
bool
_bitmap_next(IndexScanDesc scan,
			 ScanDirection dir)
{
	BMScanOpaque			so ;

	so = (BMScanOpaque) scan->opaque;

	/* check if this scan is over. */
	if (so->bm_currPos->bm_done && 
		(so->bm_currPos->bm_curr_tid_pos >= so->bm_currPos->bm_num_of_tids))
		return false;

	/* if there is any tid that has been retrieved from the previous operation,
	   then we simply return it. */
	if (so->bm_currPos->bm_curr_tid_pos < so->bm_currPos->bm_num_of_tids)
	{
		uint64	tidOffset = 
			so->bm_currPos->bm_tidOffsets[so->bm_currPos->bm_curr_tid_pos]-1;

#ifdef BM_DEBUG
		/* if ((tidOffset%(1<<(8*sizeof(OffsetNumber)))) >= 16000)*/
		if (tidOffset == 44043235 && (tidOffset%MaxNumHeapTuples) >= 285)
			ereport(LOG, (errcode(LOG), 
					errmsg("pos=%d, tidOffset=%lld, blkno=%lld, offset=%lld\n",
							so->bm_currPos->bm_curr_tid_pos, tidOffset, 
							tidOffset/MaxNumHeapTuples, 
							tidOffset%MaxNumHeapTuples)));
							/* tidOffset/(1<<(8*sizeof(OffsetNumber))),
							(tidOffset%(1<<(8*sizeof(OffsetNumber))))))); */
#endif

		/* Assert((tidOffset%(1<<(8*sizeof(OffsetNumber))))+1 > 0);*/
		Assert((tidOffset%MaxNumHeapTuples)+1 > 0);

		ItemPointerSet(&(scan->xs_ctup.t_self),
					   tidOffset/MaxNumHeapTuples,
					   (tidOffset%MaxNumHeapTuples)+1);
					   /*tidOffset/(1<<(8*sizeof(OffsetNumber))),
					   (tidOffset%(1<<(8*sizeof(OffsetNumber))))+1);*/

		so->bm_currPos->bm_curr_tid_pos++;

		scan->currentItemData = scan->xs_ctup.t_self;

		return true;
	}

	so->bm_currPos->bm_num_of_tids = so->bm_currPos->bm_curr_tid_pos = 0;

	/* Find a batch of 1 bits from the corresponding bitmaps. In this way,
	   we can reduce the times of intertwining access to the bitmap pages and
	   the tid pages. This leads to lower seek time, essentially
	   good performance.*/
	_bitmap_find_next_n_setbits(scan, BATCH_TIDS,
								so->bm_currPos->bm_tidOffsets);

	if (so->bm_currPos->bm_num_of_tids > 0)
	{
		uint64		tidOffset =
			so->bm_currPos->bm_tidOffsets[0]-1;
		ItemPointerSet(&(scan->xs_ctup.t_self),
					   tidOffset/MaxNumHeapTuples,
					   (tidOffset%MaxNumHeapTuples)+1);
					   /*tidOffset/(1<<(8*sizeof(OffsetNumber))),
					   (tidOffset%(1<<(8*sizeof(OffsetNumber))))+1);*/
		so->bm_currPos->bm_curr_tid_pos = 1;

#ifdef BM_DEBUG
		if (tidOffset == 44043235 && (tidOffset%MaxNumHeapTuples) >= 285)
		/*if ((tidOffset%(1<<(8*sizeof(OffsetNumber)))) >= 16000)*/
			ereport(LOG, (errcode(LOG), 
					errmsg("pos=0, tidOffset=%lld, blkno=%lld, offset=%lld\n",
							tidOffset,
							tidOffset/MaxNumHeapTuples,
							(tidOffset%MaxNumHeapTuples)+1)));
							/*tidOffset/(1<<(8*sizeof(OffsetNumber))),
							(tidOffset%(1<<(8*sizeof(OffsetNumber)))))));*/
#endif

		scan->currentItemData = scan->xs_ctup.t_self;

		return true;
	}

	return false;
}

bool
_bitmap_firstblockwords(IndexScanDesc scan,
						ScanDirection dir)
{
	_bitmap_findbitmaps(scan, dir);

	return _bitmap_nextblockwords(scan, dir);
}

bool
_bitmap_nextblockwords(IndexScanDesc scan,
					   ScanDirection dir)
{
	BMScanOpaque	so;

	so = (BMScanOpaque) scan->opaque;

	/* check if this scan if over */
	if (so->bm_currPos->bm_done)
		return false;

	_bitmap_comp_nonzero_and_words(scan);

	return true;
}

/*
 * _bitmap_find_next_n_setbits() -- find the next set bits for the given scan.
 *	The tid positions are stored in tidLocs.
 */
static void
_bitmap_find_next_n_setbits(IndexScanDesc scan, uint32 maxTids,
							uint64* tidLocs)
{
	BMScanOpaque			so ;
	BMScanPosition			scanPos;
	/* indicate if this round is done */
	bool 					done = false;

	so = (BMScanOpaque) scan->opaque;
	scanPos = so->bm_currPos;

	while ((!scanPos->bm_done) && (scanPos->bm_num_of_tids < maxTids) && !done)
	{
		/* if there are not ANDed words, then compute words that are
		   at most filling one block */
		if (scanPos->bm_num_of_words == 0 && 
			scanPos->bm_num_of_tids < maxTids)
			_bitmap_comp_nonzero_and_words(scan);

		/* if there are some previous ANDed words available, then scan
		   through these words and store final tids. */
		while (scanPos->bm_num_of_words > 0 &&
				scanPos->bm_num_of_tids < maxTids && !done)
		{
			uint8 oldScanPos = scanPos->bm_last_scanpos;
			BM_HRL_WORD word = 
				scanPos->bm_last_scanwords[scanPos->bm_lastScanWordNo];

			/* if this is a new word, and a zero fill word */
			if ((oldScanPos == 0) &&
				((IS_FILL_WORD(scanPos->bm_last_scanHeaderWords, 
								scanPos->bm_lastScanWordNo) && 
				  (GET_FILL_BIT(word) == 0)) ||
				 (word == 0))) {
				uint32	fillLength;
				if (word == 0)
					fillLength = 1;
				else
					fillLength = FILL_LENGTH(word);
				scanPos->bm_tid_pos +=
					fillLength * BM_HRL_WORD_SIZE;

				scanPos->bm_lastScanWordNo++;
				scanPos->bm_num_of_words--;
				scanPos->bm_last_scanpos = 0;
				continue;
			}

			/* if this is a set fill word */
			else if (IS_FILL_WORD(scanPos->bm_last_scanHeaderWords, 
								  scanPos->bm_lastScanWordNo) && 
					 (GET_FILL_BIT(word) == 1)) {
				uint32	numOfFillWords = FILL_LENGTH(word);
				uint8 	bitNo;

				while ((numOfFillWords > 0) && 
					   (scanPos->bm_num_of_tids+BM_HRL_WORD_SIZE < maxTids)) {
					for (bitNo=0; bitNo<BM_HRL_WORD_SIZE; bitNo++) {
						tidLocs[scanPos->bm_num_of_tids++] =
							(++scanPos->bm_tid_pos);
					}
					numOfFillWords--;
					scanPos->bm_last_scanwords[scanPos->bm_lastScanWordNo]--;
				}

				if (numOfFillWords == 0) {
					scanPos->bm_lastScanWordNo++;
					scanPos->bm_num_of_words--;
					scanPos->bm_last_scanpos = 0;
					continue;
				} else {
					done = true;
					break;
				}
			}

			if(oldScanPos == 0)
				oldScanPos = BM_HRL_WORD_SIZE+1;

			while (oldScanPos != 0 && scanPos->bm_num_of_tids < maxTids) {
				if (oldScanPos == BM_HRL_WORD_SIZE+1)
					oldScanPos = 0;
				scanPos->bm_last_scanpos =
					_bitmap_find_bitset(
						scanPos->bm_last_scanwords[scanPos->bm_lastScanWordNo],
						oldScanPos);

				/* if we found a set bit in this word. */
				if (scanPos->bm_last_scanpos != 0)
				{
					scanPos->bm_tid_pos += 
						(scanPos->bm_last_scanpos - oldScanPos);
					tidLocs[scanPos->bm_num_of_tids++] = scanPos->bm_tid_pos;
				}
				else
				{
					scanPos->bm_tid_pos +=
						BM_HRL_WORD_SIZE - oldScanPos;

					/* start scanning a new word */
					scanPos->bm_num_of_words --;
					scanPos->bm_lastScanWordNo ++;
					scanPos->bm_last_scanpos = 0;
				}

				oldScanPos = scanPos->bm_last_scanpos;
			}
		}

	}
}

/*
 * _bitmap_comp_nonzero_and_words() -- ANDED up to a block-size words
 *	from all related bitmaps.
 */
static void
_bitmap_comp_nonzero_and_words(IndexScanDesc scan)
{
	BMScanOpaque			so ;
	BMScanPosition			scanPos;
	uint32					keyNo;
	BMBitmapScanPosition	bmScanPos ;
	uint32*					prevStartWordNos;
	bool					done;

	so = (BMScanOpaque) scan->opaque;
	scanPos = so->bm_currPos;
	bmScanPos = (BMBitmapScanPosition) 
		(((char*)scanPos) + sizeof(BMScanPositionData));

	/* find a block-size of words from the bitmaps for all attributes.
	   If there are some remaining words from previous operations for
	   some attributes, the do nothing for these attributes, otherwise,
	   read a block-size words from the bitmap page. */
	for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
	{
		/* if these is no leftover words from previous operations,
		   then read one-block of words from the next bitmap page. */
		if ((bmScanPos[keyNo]).bm_num_of_words == 0){
			if (!((bmScanPos[keyNo]).bm_readLastWords)) {
				_bitmap_readWords(scan->indexRelation,
								  (bmScanPos[keyNo]).bm_lovBuffer,
								  (bmScanPos[keyNo]).bm_lovOffset,
								  &((bmScanPos[keyNo]).bm_nextBlockNo),
								  (bmScanPos[keyNo]).bm_headerWordsABlock,
								  (bmScanPos[keyNo]).bm_wordsABlock,
							  	  &((bmScanPos[keyNo]).bm_num_of_words),
								  &((bmScanPos[keyNo]).bm_readLastWords));
				(bmScanPos[keyNo]).bm_startWordNo = 0;
			}

			/* if there are no more words, then done */
			if ((bmScanPos[keyNo]).bm_num_of_words == 0) {
				scanPos->bm_num_of_words = 0;
				scanPos->bm_done = true;
				return;
			}
		}
	}

	/* if there is only one attribute, simply copy words in a page
	   to scanPos->bm_last_scanwords.  */
	if (scan->numberOfKeys == 1)
	{
		memcpy(scanPos->bm_last_scanHeaderWords,
				(bmScanPos[0]).bm_headerWordsABlock,
				BM_MAX_NUM_OF_HEADER_WORDS*sizeof(BM_HRL_WORD));
		memcpy(scanPos->bm_last_scanwords, (bmScanPos[0]).bm_wordsABlock,
			   (bmScanPos[0]).bm_num_of_words * sizeof(BM_HRL_WORD));
		scanPos->bm_num_of_words = (bmScanPos[0]).bm_num_of_words;
		scanPos->bm_lastScanWordNo = 0;

		(bmScanPos[0]).bm_num_of_words = 0;
		return;
	}

	/* AND these words. */
	/* Reset the next batch of words to be computed. */
	scanPos->bm_num_of_words = 0;
	scanPos->bm_lastScanWordNo = 0;

	/* the previous startWordNo for all attributes */
	prevStartWordNos = 
		(uint32*)palloc0(scan->numberOfKeys*sizeof(uint32));

	memset(scanPos->bm_last_scanHeaderWords, 0, 
			BM_MAX_NUM_OF_HEADER_WORDS*sizeof(BM_HRL_WORD));

	done = false;
	while(scanPos->bm_num_of_words < BM_NUM_OF_HRL_WORDS_PER_PAGE && !done)
	{
		BM_HRL_WORD andWord = LITERAL_ALL_ONE;
		BM_HRL_WORD	word;

		bool		andWordIsLiteral = true;

		for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
		{
			/* skip nextReadNo-numOfWordsRead[keyNo]-1 words */
			while ( (bmScanPos[keyNo].bm_num_of_words > 0) &&
					((bmScanPos[keyNo]).bm_numOfWordsRead < 
					  scanPos->bm_nextReadNo-1))
			{
				word = (bmScanPos[keyNo]).bm_wordsABlock
						[(bmScanPos[keyNo]).bm_startWordNo];

				if (IS_FILL_WORD((bmScanPos[keyNo]).bm_headerWordsABlock,
								 (bmScanPos[keyNo]).bm_startWordNo)) {
					if(FILL_LENGTH(word) <= 
						(scanPos->bm_nextReadNo-
						(bmScanPos[keyNo]).bm_numOfWordsRead-1))
					{
						(bmScanPos[keyNo]).bm_numOfWordsRead +=
							FILL_LENGTH(word);
						(bmScanPos[keyNo]).bm_startWordNo ++;
						(bmScanPos[keyNo]).bm_num_of_words --;
					}

					else {
						(bmScanPos[keyNo]).bm_wordsABlock
						[(bmScanPos[keyNo]).bm_startWordNo] -=
							(scanPos->bm_nextReadNo-
							 (bmScanPos[keyNo]).bm_numOfWordsRead-1);
						(bmScanPos[keyNo]).bm_numOfWordsRead = 
							scanPos->bm_nextReadNo-1;
					}
				}

				else {
					(bmScanPos[keyNo]).bm_numOfWordsRead ++;
					(bmScanPos[keyNo]).bm_startWordNo ++;
					(bmScanPos[keyNo]).bm_num_of_words --;
				}
			}

			if (bmScanPos[keyNo].bm_num_of_words == 0)
			{
				done = true;
				break;
			}

			Assert((bmScanPos[keyNo]).bm_numOfWordsRead == 
					scanPos->bm_nextReadNo-1);

			/* Here, bm_startWordNo should point to the word to be read. */
			word = (bmScanPos[keyNo]).bm_wordsABlock
					[(bmScanPos[keyNo]).bm_startWordNo];

			if (IS_FILL_WORD((bmScanPos[keyNo]).bm_headerWordsABlock,
							 (bmScanPos[keyNo]).bm_startWordNo) &&
				(GET_FILL_BIT(word) == 0))
			{
				(bmScanPos[keyNo]).bm_numOfWordsRead +=
					FILL_LENGTH(word);

				andWord = BM_MAKE_FILL_WORD
					(0, (bmScanPos[keyNo]).bm_numOfWordsRead-
						scanPos->bm_nextReadNo+1);
				andWordIsLiteral = false;

				scanPos->bm_nextReadNo = 
					(bmScanPos[keyNo]).bm_numOfWordsRead+1;
				(bmScanPos[keyNo]).bm_startWordNo ++;
				(bmScanPos[keyNo]).bm_num_of_words --;
				break;
			}

			else if (IS_FILL_WORD((bmScanPos[keyNo]).bm_headerWordsABlock,
								  (bmScanPos[keyNo]).bm_startWordNo) &&
					 (GET_FILL_BIT(word) == 1)) {
				(bmScanPos[keyNo]).bm_numOfWordsRead ++;

				prevStartWordNos[keyNo] = (bmScanPos[keyNo]).bm_startWordNo;

				if (FILL_LENGTH(word) == 1)
				{
					(bmScanPos[keyNo]).bm_startWordNo ++;
					(bmScanPos[keyNo]).bm_num_of_words --;
				}

				else {
					(bmScanPos[keyNo]).bm_wordsABlock
					[(bmScanPos[keyNo]).bm_startWordNo] -= 1;
				}

				andWordIsLiteral = true;
			}

			else if (!IS_FILL_WORD((bmScanPos[keyNo]).bm_headerWordsABlock,
									(bmScanPos[keyNo]).bm_startWordNo))
			{
				prevStartWordNos[keyNo] = (bmScanPos[keyNo]).bm_startWordNo;

				andWord &= word;
				(bmScanPos[keyNo]).bm_numOfWordsRead ++;
				(bmScanPos[keyNo]).bm_startWordNo ++;
				(bmScanPos[keyNo]).bm_num_of_words --;

				andWordIsLiteral = true;
			}
		}

		/* Since there are not enough words in this attribute,
			break this loop. */
		if (done) {
			uint32 preKeyNo;

			/* reset the attributes before keyNo */
			for (preKeyNo=0; preKeyNo<keyNo; preKeyNo++) {
				if ((bmScanPos[preKeyNo]).bm_startWordNo > 
					prevStartWordNos[preKeyNo]) {

					Assert ((bmScanPos[preKeyNo]).bm_startWordNo ==
							prevStartWordNos[preKeyNo] + 1);

					(bmScanPos[preKeyNo]).bm_startWordNo =
						prevStartWordNos[preKeyNo];
					(bmScanPos[preKeyNo]).bm_num_of_words++;
				}

				else {
					BM_HRL_WORD prevWord = 
						(bmScanPos[preKeyNo]).bm_wordsABlock[
							(bmScanPos[preKeyNo]).bm_startWordNo];
					Assert (((bmScanPos[preKeyNo]).bm_startWordNo ==
							prevStartWordNos[preKeyNo]) && 
							IS_FILL_WORD(
							  	(bmScanPos[preKeyNo]).bm_headerWordsABlock,
								(bmScanPos[preKeyNo]).bm_startWordNo) &&
							(GET_FILL_BIT(prevWord) == 1));
					(bmScanPos[preKeyNo]).bm_wordsABlock[
						(bmScanPos[preKeyNo]).bm_startWordNo] ++;
				}

				(bmScanPos[preKeyNo]).bm_numOfWordsRead--;
			}

			break;
		}

		if (!done) {
			if (!andWordIsLiteral)
				scanPos->bm_last_scanHeaderWords[
					scanPos->bm_num_of_words/BM_HRL_WORD_SIZE] |=
					(((BM_HRL_WORD)1)<<(BM_HRL_WORD_SIZE-1-
					 (scanPos->bm_num_of_words%BM_HRL_WORD_SIZE)));
			scanPos->bm_last_scanwords[scanPos->bm_num_of_words] =
				andWord;
			scanPos->bm_num_of_words++;
		}

		if (andWordIsLiteral)
			(scanPos->bm_nextReadNo)++;

		if (keyNo == scan->numberOfKeys-1 && 
			(bmScanPos[keyNo]).bm_num_of_words == 0)
			done = true;
	}

	pfree(prevStartWordNos);
}

/*
 * _bitmap_readWords() -- read one-block of bitmap words from
 *	the bitmap page.
 *
 * If nextBlockNo is an invalid block number, then the last words
 * are stored in lovItem. Otherwise, read words from nextBlockNo.
 */
static void
_bitmap_readWords(Relation rel, Buffer lovBuffer, OffsetNumber lovOffset,
				  BlockNumber* nextBlockNoP, 
				  BM_HRL_WORD* headerWords, BM_HRL_WORD* words,
				  uint32* numOfWordsP, bool* readLastWords)
{
	if (BlockNumberIsValid(*nextBlockNoP))
	{
		Buffer bitmapBuffer =
			_bitmap_getbuf(rel, *nextBlockNoP, BM_READ);

		Page			bitmapPage;
		BMBitmap		bitmap;
		BMBitmapOpaque	bitmapOpaque;

		bitmapPage = BufferGetPage(bitmapBuffer);

		bitmap = (BMBitmap) PageGetContents(bitmapPage);
		bitmapOpaque = (BMBitmapOpaque)
			PageGetSpecialPointer(bitmapPage);

		*numOfWordsP = bitmapOpaque->bm_hrl_words_used;
		memcpy(headerWords, bitmap->bm_headerWords,
				BM_MAX_NUM_OF_HEADER_WORDS*sizeof(BM_HRL_WORD));
		memcpy(words, bitmap->bm_contentWords, 
				sizeof(BM_HRL_WORD)*(*numOfWordsP));

		*nextBlockNoP = bitmapOpaque->bm_bitmap_next;

		_bitmap_relbuf(bitmapBuffer);

		*readLastWords = false;
	}

	else {
		BMLOVItem	lovItem;
		Page		lovPage;

		LockBuffer(lovBuffer, BM_READ);

		lovPage = BufferGetPage(lovBuffer);
		lovItem = (BMLOVItem) 
			PageGetItem(lovPage, PageGetItemId (lovPage, lovOffset));

		if (lovItem->bm_last_compword != LITERAL_ALL_ONE) {
			*numOfWordsP = 2;
			headerWords[0] = (((BM_HRL_WORD)lovItem->bm_last_two_headerbits) <<
							  (BM_HRL_WORD_SIZE-2));
			words[0] = lovItem->bm_last_compword;
			words[1] = lovItem->bm_last_word;
		}

		else {
			*numOfWordsP = 1;
			headerWords[0] = (((BM_HRL_WORD)lovItem->bm_last_two_headerbits) <<
							  (BM_HRL_WORD_SIZE-1));
			words[0] = lovItem->bm_last_word;
		}

		LockBuffer(lovBuffer, BUFFER_LOCK_UNLOCK);

		*readLastWords = true;
	}
}

/*
 * _bitmap_findbitmaps() -- find the bitmaps that satisfy the index predicate.
 */
void
_bitmap_findbitmaps(IndexScanDesc scan,
					ScanDirection dir)
{
	BMScanOpaque	so ;
	uint32			keyNo;
	Buffer			lovMetaBuffer;
	Page			lovMetapage;

	BMBitmapScanPosition	bmScanPos;

	so = (BMScanOpaque) scan->opaque;

	Assert(so->bm_currPos == NULL);

	/* initialize so->bm_currPos */
	so->bm_currPos = (BMScanPosition) palloc0
		(MAXALIGN(sizeof(BMScanPositionData)) + 
	 	scan->numberOfKeys * sizeof(BMBitmapScanPositionData));

	so->bm_currPos->bm_tid_pos = 0;
	so->bm_currPos->bm_num_of_words = 0;
	so->bm_currPos->bm_last_scanpos = 0 ;
	so->bm_currPos->bm_nextReadNo = 1;
	so->bm_currPos->bm_done = false;

	so->bm_currPos->bm_num_of_tids = 0;
	so->bm_currPos->bm_curr_tid_pos = 0;

	bmScanPos = (BMBitmapScanPosition) 
		(((char*)so->bm_currPos) + MAXALIGN(sizeof(BMScanPositionData)));

	lovMetaBuffer = 
		_bitmap_getbuf(scan->indexRelation, BM_LOV_STARTPAGE-1, BM_READ);
	lovMetapage = BufferGetPage(lovMetaBuffer);

	/* For each attribute in the index predicates, find their
	   corresponding LOV items. */
	for (keyNo=0; keyNo<scan->numberOfKeys; keyNo++)
	{
		Relation		lovHeap, lovIndex;
		bool			valueExists = false;
		BlockNumber		lovBlock;
		OffsetNumber	lovOffset;
		bool			blockNull, offsetNull;
		bool			isnull = false;

		TupleDesc		indexTupDesc;
		RegProcedure	opfuncid;
		ScanKeyData		scanKeyData;
		IndexScanDesc	scanDesc;

		if ((scan->keyData[keyNo]).sk_flags & SK_ISNULL)
			isnull = true;

		/* if the value for this key is null */
		if (isnull)
		{
			Page lovPage;
			BMLOVItem	lovItem;

			lovBlock = BM_LOV_STARTPAGE+((scan->keyData[keyNo]).sk_attno-1);
			lovOffset = 1;

			valueExists = true;

			(bmScanPos[keyNo]).bm_lovOffset = lovOffset;
			(bmScanPos[keyNo]).bm_lovBuffer =
				_bitmap_getbuf(scan->indexRelation, lovBlock, BM_READ);

			lovPage	= BufferGetPage((bmScanPos[keyNo]).bm_lovBuffer);
			lovItem = (BMLOVItem) 
				PageGetItem(lovPage, 
					PageGetItemId (lovPage, 
						(bmScanPos[keyNo]).bm_lovOffset));
			
			(bmScanPos[keyNo]).bm_nextBlockNo = lovItem->bm_lov_head;
			(bmScanPos[keyNo]).bm_numOfWordsRead = 0;
			(bmScanPos[keyNo]).bm_num_of_words = 0;
			(bmScanPos[keyNo]).bm_readLastWords = false;

			LockBuffer((bmScanPos[keyNo]).bm_lovBuffer, BUFFER_LOCK_UNLOCK);

			continue;
		}

		_bitmap_open_lov_heapandindex
			(scan->indexRelation, lovMetapage, 
			(scan->keyData[keyNo]).sk_attno-1,
			 &lovHeap, &lovIndex, AccessShareLock);

		indexTupDesc = RelationGetDescr(lovIndex);
		opfuncid = equality_oper_funcid(indexTupDesc->attrs[0]->atttypid);
		ScanKeyEntryInitialize
			(&scanKeyData, SK_ISNULL, 1, 
			BTEqualStrategyNumber, InvalidOid, opfuncid, 0);
		scanDesc =
			index_beginscan (lovHeap, lovIndex, SnapshotAny, 1, &scanKeyData);

		/* if such value is find, then we obtain the page number and 
			offset number of LOV item in the LOV. */
		if (_bitmap_findvalue(lovHeap, lovIndex,
				(scan->keyData[keyNo]).sk_argument, isnull, 
				&scanKeyData, scanDesc,
				 &lovBlock, &blockNull, &lovOffset, &offsetNull))
		{
			Page lovPage;
			BMLOVItem	lovItem;

			valueExists = true;

			(bmScanPos[keyNo]).bm_lovOffset = lovOffset;
			(bmScanPos[keyNo]).bm_lovBuffer =
				_bitmap_getbuf(scan->indexRelation, lovBlock, BM_READ);

			lovPage	= BufferGetPage((bmScanPos[keyNo]).bm_lovBuffer);
			lovItem = (BMLOVItem) 
				PageGetItem(lovPage, 
					PageGetItemId (lovPage, 
						(bmScanPos[keyNo]).bm_lovOffset));
			
			(bmScanPos[keyNo]).bm_nextBlockNo = lovItem->bm_lov_head;
			(bmScanPos[keyNo]).bm_numOfWordsRead = 0;
			(bmScanPos[keyNo]).bm_num_of_words = 0;
			(bmScanPos[keyNo]).bm_readLastWords = false;

			LockBuffer((bmScanPos[keyNo]).bm_lovBuffer, BUFFER_LOCK_UNLOCK);
		}

		index_endscan(scanDesc);
		_bitmap_close_lov_heapandindex(lovHeap, lovIndex, AccessShareLock);

		/* if we did not find a match for this key, then the scan is over. */
		if (!valueExists)
			so->bm_currPos->bm_done = true;
	}

	_bitmap_relbuf(lovMetaBuffer);
}
