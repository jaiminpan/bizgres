/*-------------------------------------------------------------------------
 *
 * ondiskbitmapwords.c
 * 	PostgreSQL
 *
 * Copyright (c) 2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	$PostgreSQL$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/ondiskbitmapwords.h"
#include "access/bitmap.h"
#include "nodes/execnodes.h"

static void odbm_findnextword(OnDiskBitmapWords* odbm, uint32 nextReadNo);
static void odbm_resetWord(OnDiskBitmapWords *odbm, uint32 preStartNo);

OnDiskBitmapWords*
odbm_create(uint32 maxNumOfWords)
{
	OnDiskBitmapWords* odbm;
	uint32	numOfHeaderWords;

	/* we want to read the words in a page at once. */
	Assert(maxNumOfWords%BM_NUM_OF_HRL_WORDS_PER_PAGE == 0);

	odbm = (OnDiskBitmapWords*) palloc0(sizeof(OnDiskBitmapWords));

	odbm->type = T_OnDiskBitmapWords; /*set NodeTag */
	odbm->mcxt = CurrentMemoryContext;
	odbm->numOfWordsRead = 0;
	odbm->nextReadNo = 1;
	odbm->startNo = 0;
	odbm->numOfWords = 0;

	numOfHeaderWords = 
		maxNumOfWords/BM_HRL_WORD_SIZE +
		((maxNumOfWords%BM_HRL_WORD_SIZE == 0) ? 0 : 1);

	odbm->maxNumOfWords = maxNumOfWords;

	/* Make sure that we have at least one page of words */
	Assert(odbm->maxNumOfWords >= BM_NUM_OF_HRL_WORDS_PER_PAGE);

	odbm->bitmapHeaderWords =
		MemoryContextAllocZero(odbm->mcxt, 
								sizeof(BM_HRL_WORD)*numOfHeaderWords);
	odbm->bitmapContentWords = 
		MemoryContextAllocZero(odbm->mcxt, 
								sizeof(BM_HRL_WORD)*odbm->maxNumOfWords);

	return odbm;
}

/*
 * odbm_free() -- release the space for a OnDiskBitmapWords.
 */
void
odbm_free(OnDiskBitmapWords* odbm)
{
	if (odbm->bitmapHeaderWords)
		pfree(odbm->bitmapHeaderWords);
	if (odbm->bitmapContentWords)
		pfree(odbm->bitmapContentWords);

	pfree(odbm);
}

/*
 * odbm_reset() -- reset the OnDiskBitmapWords.
 */
void
odbm_reset(OnDiskBitmapWords* odbm)
{
	odbm->startNo = 0;
	odbm->numOfWords = 0;
	memset (odbm->bitmapHeaderWords, 0,
			(ODBM_MAX_WORDS/BM_HRL_WORD_SIZE +
			 ((ODBM_MAX_WORDS%BM_HRL_WORD_SIZE == 0) ? 0 : 1)));
}

/*
 * odbm_findnexttid() -- find the next tid offset from this list of words
 */
uint64
odbm_findnexttid(OnDiskBitmapWords *odbm, ODBMIterateResult *odbmres)
{

	/* if there is not tids from previous computation, then we
	   try to find next set of tids. */
	if (odbmres->nextTidLoc >= odbmres->numOfTids)
		odbm_findnexttids(odbm, odbmres, ODBM_BATCH_TIDS);

	/* if find more tids, then return the first one */
	if (odbmres->nextTidLoc < odbmres->numOfTids)
	{
		odbmres->nextTidLoc++;
		return (odbmres->nextTids[odbmres->nextTidLoc-1]);
	}

	/* no more tids */
	return 0;
}

/*
 * odbm_findnexttids() -- find the next set of tids in these words.
 */
void
odbm_findnexttids(OnDiskBitmapWords* odbm, 
				  ODBMIterateResult *odbmres,
				  uint32 maxTids)
{
	bool done = false;

	odbmres->nextTidLoc = odbmres->numOfTids = 0;
	while (odbm->numOfWords > 0 &&
		   odbmres->numOfTids < maxTids && !done) {

		uint8 oldScanPos = odbmres->lastScanPos;
		BM_HRL_WORD word = 
			odbm->bitmapContentWords[odbmres->lastScanWordNo];

		/* if this is a new word, and a zero fill word */
		if ((oldScanPos == 0) &&
			((IS_FILL_WORD(odbm->bitmapHeaderWords, 
						   odbmres->lastScanWordNo) && 
			(GET_FILL_BIT(word) == 0)) ||
			(word == 0))) {
			uint32	fillLength;
			if (word == 0)
				fillLength = 1;
			else
				fillLength = FILL_LENGTH(word);
			odbmres->nextTid +=
				fillLength * BM_HRL_WORD_SIZE;

			odbmres->lastScanWordNo ++;
			odbm->numOfWords --;
			odbmres->lastScanPos = 0;
			continue;
		}

		/* if this is a set fill word */
		else if (IS_FILL_WORD(odbm->bitmapHeaderWords, 
							  odbmres->lastScanWordNo) && 
				 (GET_FILL_BIT(word) == 1)) {
			uint32	numOfFillWords = FILL_LENGTH(word);
			uint8 	bitNo;

			while ((numOfFillWords > 0) && 
				   (odbmres->numOfTids+BM_HRL_WORD_SIZE <= maxTids)) {
				for (bitNo=0; bitNo<BM_HRL_WORD_SIZE; bitNo++) {
					odbmres->nextTids[odbmres->numOfTids++] =
						(++odbmres->nextTid);
				}
				numOfFillWords--;
				odbm->bitmapContentWords[odbmres->lastScanWordNo]--;
			}

			if (numOfFillWords == 0) {
				odbmres->lastScanWordNo++;
				odbm->numOfWords--;
				odbmres->lastScanPos = 0;
				continue;
			} else {
				done = true;
				break;
			}
		}

		if(oldScanPos == 0)
			oldScanPos = BM_HRL_WORD_SIZE+1;

		while (oldScanPos != 0 && odbmres->numOfTids < maxTids) {
			if (oldScanPos == BM_HRL_WORD_SIZE+1)
				oldScanPos = 0;
			odbmres->lastScanPos =
				_bitmap_find_bitset(
					odbm->bitmapContentWords[odbmres->lastScanWordNo],
					oldScanPos);

			/* if we found a set bit in this word. */
			if (odbmres->lastScanPos != 0)
			{
				odbmres->nextTid += 
					(odbmres->lastScanPos - oldScanPos);
				odbmres->nextTids[odbmres->numOfTids++] = odbmres->nextTid;
			}
			else
			{
				odbmres->nextTid +=
					BM_HRL_WORD_SIZE - oldScanPos;

				/* start scanning a new word */
				odbm->numOfWords --;
				odbmres->lastScanWordNo ++;
				odbmres->lastScanPos = 0;
			}

			oldScanPos = odbmres->lastScanPos;
		}
	}
}

/*
 * odbm_intersect() -- intersect two list of bitmap words.
 */
void
odbm_intersect(OnDiskBitmapWords **odbms, uint32 numOdbms,
			   OnDiskBitmapWords *result)
{
	bool done = false;
	uint32	*prevStartNos;
	uint32	nextReadNo;
	uint32	odbmNo;

	Assert(numOdbms > 0);

	prevStartNos = (uint32*)palloc0(numOdbms*sizeof(uint32));

	nextReadNo = odbms[0]->nextReadNo;

	while (!done &&
			result->numOfWords < result->maxNumOfWords)
	{
		BM_HRL_WORD andWord = LITERAL_ALL_ONE;
		BM_HRL_WORD	word;

		bool		andWordIsLiteral = true;

		for (odbmNo=0; odbmNo<numOdbms; odbmNo++)
		{
			/* skip nextReadNo-numOfWordsRead-1 words */
			odbm_findnextword(odbms[odbmNo], nextReadNo);

			if ((odbms[odbmNo])->numOfWords == 0)
			{
				done = true;
				break;
			}

			Assert((odbms[odbmNo])->numOfWordsRead == nextReadNo-1);

			/* Here, startNo should point to the word to be read. */
			word = (odbms[odbmNo])->bitmapContentWords
					[(odbms[odbmNo])->startNo];

			if (IS_FILL_WORD((odbms[odbmNo])->bitmapHeaderWords,
							 (odbms[odbmNo])->startNo) &&
				(GET_FILL_BIT(word) == 0))
			{
				(odbms[odbmNo])->numOfWordsRead +=
					FILL_LENGTH(word);

				andWord = BM_MAKE_FILL_WORD
					(0, (odbms[odbmNo])->numOfWordsRead-nextReadNo+1);
				andWordIsLiteral = false;

				nextReadNo = (odbms[odbmNo])->numOfWordsRead+1;
				(odbms[odbmNo])->startNo ++;
				(odbms[odbmNo])->numOfWords --;
				break;
			}

			else if (IS_FILL_WORD((odbms[odbmNo])->bitmapHeaderWords,
								  (odbms[odbmNo])->startNo) &&
					 (GET_FILL_BIT(word) == 1)) {
				(odbms[odbmNo])->numOfWordsRead ++;

				prevStartNos[odbmNo] = (odbms[odbmNo])->startNo;

				if (FILL_LENGTH(word) == 1)
				{
					(odbms[odbmNo])->startNo ++;
					(odbms[odbmNo])->numOfWords --;
				}

				else {
					(odbms[odbmNo])->bitmapContentWords
					[(odbms[odbmNo])->startNo] -= 1;
				}

				andWordIsLiteral = true;
			}

			else if (!IS_FILL_WORD((odbms[odbmNo])->bitmapHeaderWords,
									(odbms[odbmNo])->startNo))
			{
				prevStartNos[odbmNo] = (odbms[odbmNo])->startNo;

				andWord &= word;
				(odbms[odbmNo])->numOfWordsRead ++;
				(odbms[odbmNo])->startNo ++;
				(odbms[odbmNo])->numOfWords --;

				andWordIsLiteral = true;
			}
		}

		/* Since there are not enough words in this attribute,
			break this loop. */
		if (done) {
			uint32 preOdbmNo;

			/* reset the attributes before odbmNo */
			for (preOdbmNo=0; preOdbmNo<odbmNo; preOdbmNo++) {
				odbm_resetWord(odbms[preOdbmNo], prevStartNos[preOdbmNo]);
			}

			break;
		}

		if (!done) {
			if (!andWordIsLiteral)
				result->bitmapHeaderWords[
					result->numOfWords/BM_HRL_WORD_SIZE] |=
					(((BM_HRL_WORD)1)<<(BM_HRL_WORD_SIZE-1-
					 (result->numOfWords%BM_HRL_WORD_SIZE)));
			result->bitmapContentWords[result->numOfWords] =
				andWord;
			result->numOfWords++;
		}

		if (andWordIsLiteral)
			nextReadNo++;

		if (odbmNo == 1 && 
			(odbms[odbmNo])->numOfWords == 0)
			done = true;
	}

	/* set the nextReadNo */
	for (odbmNo=0; odbmNo<numOdbms; odbmNo++)
		odbms[odbmNo]->nextReadNo = nextReadNo;

	pfree(prevStartNos);
}

void
odbm_union(OnDiskBitmapWords **odbms, uint32 numOdbms,
			OnDiskBitmapWords *result)
{
	bool done = false;
	uint32	*prevStartNos;
	uint32	nextReadNo;
	uint32		odbmNo;

	Assert (numOdbms >= 0);

	if (numOdbms == 0)
		return;

	prevStartNos = (uint32*)palloc0(numOdbms*sizeof(uint32));

	nextReadNo = odbms[0]->nextReadNo;

	while (!done &&
			result->numOfWords < result->maxNumOfWords)
	{
		BM_HRL_WORD orWord = LITERAL_ALL_ZERO;
		BM_HRL_WORD	word;

		bool		orWordIsLiteral = true;

		for (odbmNo=0; odbmNo<numOdbms; odbmNo++)
		{
			/* skip nextReadNo-numOfWordsRead-1 words */
			odbm_findnextword(odbms[odbmNo], nextReadNo);

			if ((odbms[odbmNo])->numOfWords == 0)
			{
				done = true;
				break;
			}

			Assert((odbms[odbmNo])->numOfWordsRead == nextReadNo-1);

			/* Here, startNo should point to the word to be read. */
			word = (odbms[odbmNo])->bitmapContentWords
					[(odbms[odbmNo])->startNo];

			if (IS_FILL_WORD((odbms[odbmNo])->bitmapHeaderWords,
							 (odbms[odbmNo])->startNo) &&
				(GET_FILL_BIT(word) == 1))
			{
				(odbms[odbmNo])->numOfWordsRead +=
					FILL_LENGTH(word);

				orWord = BM_MAKE_FILL_WORD
					(1, (odbms[odbmNo])->numOfWordsRead-nextReadNo+1);
				orWordIsLiteral = false;

				nextReadNo = (odbms[odbmNo])->numOfWordsRead+1;
				(odbms[odbmNo])->startNo ++;
				(odbms[odbmNo])->numOfWords --;
				break;
			}

			else if (IS_FILL_WORD((odbms[odbmNo])->bitmapHeaderWords,
								  (odbms[odbmNo])->startNo) &&
					 (GET_FILL_BIT(word) == 0)) {
				(odbms[odbmNo])->numOfWordsRead ++;

				prevStartNos[odbmNo] = (odbms[odbmNo])->startNo;

				if (FILL_LENGTH(word) == 1)
				{
					(odbms[odbmNo])->startNo ++;
					(odbms[odbmNo])->numOfWords --;
				}

				else {
					(odbms[odbmNo])->bitmapContentWords
					[(odbms[odbmNo])->startNo] -= 1;
				}

				orWordIsLiteral = true;
			}

			else if (!IS_FILL_WORD((odbms[odbmNo])->bitmapHeaderWords,
									(odbms[odbmNo])->startNo))
			{
				prevStartNos[odbmNo] = (odbms[odbmNo])->startNo;

				orWord |= word;
				(odbms[odbmNo])->numOfWordsRead ++;
				(odbms[odbmNo])->startNo ++;
				(odbms[odbmNo])->numOfWords --;

				orWordIsLiteral = true;
			}
		}

		/* Since there are not enough words in this attribute,
			break this loop. */
		if (done) {
			uint32 preOdbmNo;

			/* reset the attributes before odbmNo */
			for (preOdbmNo=0; preOdbmNo<odbmNo; preOdbmNo++) {
				odbm_resetWord(odbms[preOdbmNo], prevStartNos[preOdbmNo]);
			}

			break;
		}

		if (!done) {
			if (!orWordIsLiteral)
				result->bitmapHeaderWords[
					result->numOfWords/BM_HRL_WORD_SIZE] |=
					(((BM_HRL_WORD)1)<<(BM_HRL_WORD_SIZE-1-
					 (result->numOfWords%BM_HRL_WORD_SIZE)));
			result->bitmapContentWords[result->numOfWords] = orWord;
			result->numOfWords++;
		}

		if (orWordIsLiteral)
			nextReadNo++;

		if (odbmNo == numOdbms-1 && 
			(odbms[odbmNo])->numOfWords == 0)
			done = true;
	}

	/* set the nextReadNo */
	for (odbmNo=0; odbmNo<numOdbms; odbmNo++)
		odbms[odbmNo]->nextReadNo = nextReadNo;

	pfree(prevStartNos);
}

static void
odbm_findnextword(OnDiskBitmapWords* odbm, uint32 nextReadNo)
{
	while ( (odbm->numOfWords > 0) &&
			(odbm->numOfWordsRead < 
			  nextReadNo-1))
	{
		BM_HRL_WORD word = odbm->bitmapContentWords
				[odbm->startNo];

		if (IS_FILL_WORD(odbm->bitmapHeaderWords,
						 odbm->startNo)) {
			if(FILL_LENGTH(word) <= 
				(nextReadNo-
				odbm->numOfWordsRead-1))
			{
				odbm->numOfWordsRead +=
					FILL_LENGTH(word);
				odbm->startNo ++;
				odbm->numOfWords --;
			}

			else {
				odbm->bitmapContentWords
				[odbm->startNo] -=
					(nextReadNo-
					 odbm->numOfWordsRead-1);
				odbm->numOfWordsRead = 
					nextReadNo-1;
			}
		}
		else {
			odbm->numOfWordsRead ++;
			odbm->startNo ++;
			odbm->numOfWords --;
		}
	}
}

static void
odbm_resetWord(OnDiskBitmapWords *odbm, uint32 prevStartNo)
{
	if (odbm->startNo > prevStartNo) {

		Assert (odbm->startNo == prevStartNo + 1);

		odbm->startNo = prevStartNo;
		odbm->numOfWords++;
	}

	else {
		Assert ((odbm->startNo == prevStartNo) && 
				IS_FILL_WORD(odbm->bitmapHeaderWords, odbm->startNo));
		odbm->bitmapContentWords[odbm->startNo] ++;
	}

	odbm->numOfWordsRead--;
}

void
odbm_begin_iterate(OnDiskBitmapWords *odbm, ODBMIterateResult* odbmres)
{
	odbmres->nextTid = odbm->firstTid;
	odbmres->lastScanPos = 0;
	odbmres->lastScanWordNo = odbm->startNo;
	odbmres->numOfTids = 0;
	odbmres->nextTidLoc = 0;
}

void
odbm_set_bitmaptype(Plan* plan, bool inmem)
{
	if(IsA(plan, BitmapAnd))
		((BitmapAnd*)plan)->inmem = inmem;
	else if (IsA(plan, BitmapOr))
		((BitmapOr*)plan)->inmem = inmem;
	else if (IsA(plan, BitmapIndexScan))
		((BitmapIndexScan*)plan)->inmem = inmem;
}

void 
odbm_set_child_resultnode(struct PlanState* ps, OnDiskBitmapWords* odbm)
{
	if (IsA(ps, BitmapAndState))
		((BitmapAndState*)ps)->resultOdbm = odbm;
	else if (IsA(ps, BitmapOrState))
		((BitmapOrState*)ps)->resultOdbm = odbm;
	else if (IsA(ps, BitmapIndexScanState))
		((BitmapIndexScanState*)ps)->odbiss_result = odbm;
	else
		elog(ERROR, "wrong type in the subplan");
}

ODBMIterateResult*
odbm_res_create(OnDiskBitmapWords* odbm)
{
	ODBMIterateResult* odbmres =
		MemoryContextAllocZero(odbm->mcxt, sizeof(ODBMIterateResult));

	return odbmres;
}

void
odbm_res_free(ODBMIterateResult* odbmres)
{
	if (odbmres != NULL)
		pfree(odbmres);
}

