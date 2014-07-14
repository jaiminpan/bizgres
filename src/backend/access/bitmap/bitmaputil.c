/*-------------------------------------------------------------------------
 *
 * bitmaputil.c
 *	  Utility routines for Postgres bitmap index access method.
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
#include "access/bitmap.h"
#include "miscadmin.h"

/*
 * _bitmap_formitem() -- construct a LOV entry.
 *
 * We look at an attribute value and its associated bitmap as a bitmap
 * index entry.
 */
BMLOVItem
_bitmap_formitem(uint64 currTidNumber)
{
	int			nbytes_bmitem;
	BMLOVItem	bmitem;

	nbytes_bmitem = sizeof(BMLOVItemData);

	bmitem = (BMLOVItem)palloc(nbytes_bmitem);

	bmitem->bm_lov_head = bmitem->bm_lov_tail = InvalidBlockNumber;
	bmitem->bm_last_setbit = 0;
	bmitem->bm_last_two_headerbits = (1<<7);

	/* fill up all existing bits with 0. */
	if (currTidNumber < BM_HRL_WORD_SIZE)
	{
		bmitem->bm_last_compword = LITERAL_ALL_ONE;
		bmitem->bm_last_word = LITERAL_ALL_ZERO;
		bmitem->bm_last_two_headerbits = 0;
	}

	else
	{
		uint32		numOfTotalFillWords;
		BM_HRL_WORD	numOfFillWords;

		numOfTotalFillWords = (currTidNumber-1)/BM_HRL_WORD_SIZE;

		numOfFillWords =
				(numOfTotalFillWords >= MAX_FILL_LENGTH) ? MAX_FILL_LENGTH :
				numOfTotalFillWords;

		bmitem->bm_last_compword = 
			BM_MAKE_FILL_WORD (0, numOfFillWords);
		bmitem->bm_last_word = LITERAL_ALL_ZERO;
		bmitem->bm_last_two_headerbits = 2;

		/* If number of zeros is too much for one word, then
			we set bm_last_setbit so that the remaining zeros can
			be handled outside. */
		if (numOfTotalFillWords > numOfFillWords)
			bmitem->bm_last_setbit = numOfFillWords*BM_HRL_WORD_SIZE;
	}

	return bmitem;
}

/*
 * _bitmap_copyOneInTupleDesc() -- copy one attribute inside oldTupDesc
 *		into newTupDesc, and return newTupDesc.
 *
 * If newTupDesc is not NULL, this function creates a new TupleDesc instance.
 * Otherwise, this function replaces its attribute to the appropriate one.
 */
TupleDesc
_bitmap_copyOneInTupleDesc(TupleDesc newTupDesc,
						   TupleDesc oldTupDesc, uint32 attno)
{
	Assert (attno < oldTupDesc->natts);

	if (newTupDesc == NULL)
	{
		newTupDesc = CreateTemplateTupleDesc(1, false);

		newTupDesc->attrs[0] =
			(Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
	}

	memcpy(newTupDesc->attrs[0], oldTupDesc->attrs[attno], 
		   ATTRIBUTE_TUPLE_SIZE);
	(newTupDesc->attrs[0])->attnum = 1;

	return newTupDesc;
}

/*
 * _bitmap_log_newpage() -- log a new page.
 *
 * This function is called before writing a new buffer. If metapage is not NULL,
 * this function also logs the changes to the metapage.
 */
void
_bitmap_log_newpage(Relation rel, uint8 info, Buffer buf, BMMetaPage metapage)
{
	Page page;

	page = BufferGetPage(buf);

	/* XLOG stuff */
	START_CRIT_SECTION();

	if (!(rel->rd_istemp))
	{
		xl_bm_newpage		xlNewPage;
		XLogRecPtr			recptr;

#ifdef BM_DEBUG
		elog(LOG, "call _bitmap_log_newpage: blkno=%d", 
			 BufferGetBlockNumber(buf));
#endif

		xlNewPage.bm_node = rel->rd_node;
		xlNewPage.bm_new_blkno = BufferGetBlockNumber(buf);

		if (metapage != NULL)
		{
			XLogRecData			rdata[2];
			xl_bm_metapage*		xlMeta = (xl_bm_metapage*)
				palloc(MAXALIGN(sizeof(xl_bm_metapage)));

			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char*)&xlNewPage;
			rdata[0].len = sizeof(xl_bm_newpage);
			rdata[0].next = &(rdata[1]);

			xlMeta->bm_node = rel->rd_node;

			rdata[1].buffer = InvalidBuffer;
			rdata[1].data = (char*)xlMeta;
			rdata[1].len = MAXALIGN(sizeof(xl_bm_metapage));
			rdata[1].next = NULL;

			recptr = XLogInsert(RM_BITMAP_ID, info, rdata);

			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);
			PageSetLSN(metapage, recptr);
			PageSetTLI(metapage, ThisTimeLineID);
			pfree(xlMeta);
		}

		else 
		{
			XLogRecData			rdata[1];

			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char*)&xlNewPage;
			rdata[0].len = sizeof(xl_bm_newpage);
			rdata[0].next = NULL;
			
			recptr = XLogInsert(RM_BITMAP_ID, info, rdata);

			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);

			if (metapage != NULL)
			{
				PageSetLSN(metapage, recptr);
				PageSetTLI(metapage, ThisTimeLineID);
			}
		}

	}

	END_CRIT_SECTION();
}

/*
 * _bitmap_log_metapage() -- log the changes to the metapage
 */
void
_bitmap_log_metapage(Relation rel, BMMetaPage metapage)
{
	/* XLOG stuff */
	START_CRIT_SECTION();

	if (!(rel->rd_istemp))
	{
		xl_bm_metapage*		xlMeta;
		XLogRecPtr			recptr;
		XLogRecData			rdata[1];

#ifdef BM_DEBUG
		elog(LOG, "call _bitmap_log_metapage.");
#endif

		xlMeta = (xl_bm_metapage*)
			palloc(MAXALIGN(sizeof(xl_bm_metapage)));
		xlMeta->bm_node = rel->rd_node;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char*)xlMeta;
		rdata[0].len = MAXALIGN(sizeof(xl_bm_metapage));
		rdata[0].next = NULL;
			
		recptr = XLogInsert(RM_BITMAP_ID, XLOG_BITMAP_INSERT_META, rdata);

		PageSetLSN(metapage, recptr);
		PageSetTLI(metapage, ThisTimeLineID);
		pfree(xlMeta);
	}

	END_CRIT_SECTION();
}

/*
 * _bitmap_log_bitmappage() -- log the changes to a bitmap page.
 *
 * The changes may be related to either the opaque data or non-opaque data.
 */
void
_bitmap_log_bitmappage(Relation rel, Buffer bitmapBuffer, bool isOpaque)
{
	Page			bitmapPage;
	BMBitmapOpaque	bitmapPageOpaque;
	BMBitmap		bitmap;

	bitmapPage = BufferGetPage(bitmapBuffer);
	bitmapPageOpaque = (BMBitmapOpaque)PageGetSpecialPointer(bitmapPage);
	bitmap = (BMBitmap) PageGetContents(bitmapPage);

	/* XLOG stuff */
	START_CRIT_SECTION();

	if (!(rel->rd_istemp))
	{
		xl_bm_bitmappage	xlBitmap;
		XLogRecPtr			recptr;
		XLogRecData			rdata[1];

#ifdef BM_DEBUG
		elog(LOG, 
		"call _bitmap_log_bitmappage: isOpaque=%d, blkno=%d, lastword_pos=%d", 
		isOpaque, BufferGetBlockNumber(bitmapBuffer), 
		bitmapPageOpaque->bm_hrl_words_used);
#endif

		xlBitmap.bm_node = rel->rd_node;
		xlBitmap.bm_bitmap_blkno = BufferGetBlockNumber(bitmapBuffer);
		xlBitmap.bm_isOpaque = isOpaque;
		if (!isOpaque)
		{
			xlBitmap.bm_lastword_pos = bitmapPageOpaque->bm_hrl_words_used;
			xlBitmap.bm_lastword_in_block = 
				bitmap->bm_contentWords[bitmapPageOpaque->bm_hrl_words_used-1];
			xlBitmap.bm_isFillWord = 
				(((bitmap->bm_headerWords
					[bitmapPageOpaque->bm_hrl_words_used/BM_HRL_WORD_SIZE]) &
				  (1<<(BM_HRL_WORD_SIZE-1-
					  bitmapPageOpaque->bm_hrl_words_used%BM_HRL_WORD_SIZE))) !=
				 0);
			xlBitmap.bm_next_blkno = InvalidBlockNumber;
		} else {
			xlBitmap.bm_lastword_pos = 0;
			xlBitmap.bm_lastword_in_block = 0;
			xlBitmap.bm_next_blkno = bitmapPageOpaque->bm_bitmap_next;
		}

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char*)&xlBitmap;
		rdata[0].len = sizeof(xl_bm_bitmappage);
		rdata[0].next = NULL;

		recptr = XLogInsert(RM_BITMAP_ID, XLOG_BITMAP_INSERT_BITMAP, rdata);

		PageSetLSN(bitmapPage, recptr);
		PageSetTLI(bitmapPage, ThisTimeLineID);
	}	

	END_CRIT_SECTION();
}

/*
 * _bitmap_log_bitmap_lastwords() -- log the last two words in a bitmap.
 */
void
_bitmap_log_bitmap_lastwords(Relation rel, Buffer lovBuffer, 
							 OffsetNumber lovOffset, BMLOVItem lovItem)
{
	/* XLOG stuff */
	START_CRIT_SECTION();

	if (!(rel->rd_istemp))
	{
		xl_bm_bitmap_lastwords	xlLastwords;
		XLogRecPtr				recptr;
		XLogRecData				rdata[1];

#ifdef BM_DEBUG
		elog(LOG, 
		"call _bitmap_log_bitmap_lastwords: lov_blkno=%d, last_compword=%x, last_word=%x", 
		BufferGetBlockNumber(lovBuffer), lovItem->bm_last_compword, 
		lovItem->bm_last_word);
#endif

		xlLastwords.bm_node = rel->rd_node;
		xlLastwords.bm_last_compword = lovItem->bm_last_compword;
		xlLastwords.bm_last_word = lovItem->bm_last_word;
		xlLastwords.bm_last_two_headerbits = lovItem->bm_last_two_headerbits;
		xlLastwords.bm_lov_blkno = BufferGetBlockNumber(lovBuffer);
		xlLastwords.bm_lov_offset = lovOffset;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char*)&xlLastwords;
		rdata[0].len = sizeof(xl_bm_bitmap_lastwords);
		rdata[0].next = NULL;

		recptr = 
			XLogInsert(RM_BITMAP_ID, XLOG_BITMAP_INSERT_BITMAP_LASTWORDS, rdata);

		PageSetLSN(BufferGetPage(lovBuffer), recptr);
		PageSetTLI(BufferGetPage(lovBuffer), ThisTimeLineID);
	}

	END_CRIT_SECTION();
}

/*
 * _bitmap_log_lovitem() -- log adding a new lov item to a lov page.
 */
void
_bitmap_log_lovitem(Relation rel, Buffer lovBuffer, bool isNewItem,
					OffsetNumber offset, BMLOVItem lovItem)
{
	Page lovPage = BufferGetPage(lovBuffer);

	/* XLOG stuff */
	START_CRIT_SECTION();

	if (!(rel->rd_istemp))
	{
		xl_bm_lovitem	xlLovItem;
		XLogRecPtr		recptr;
		XLogRecData		rdata[1];

#ifdef BM_DEBUG
		elog(LOG, "call _bitmap_log_lovitem: blkno=%d, offset=%d, isNew=%d", 
			 BufferGetBlockNumber(lovBuffer), offset, isNewItem);
#endif

		xlLovItem.bm_node = rel->rd_node;
		xlLovItem.bm_lov_blkno = BufferGetBlockNumber(lovBuffer);
		xlLovItem.bm_isNewItem = isNewItem;
		xlLovItem.bm_lov_offset = offset;
		memcpy(&(xlLovItem.bm_lovItem), lovItem,
				sizeof(BMLOVItemData));

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char*)&xlLovItem;
		rdata[0].len = sizeof(xl_bm_lovitem);
		rdata[0].next = NULL;

		recptr = XLogInsert(RM_BITMAP_ID, 
							XLOG_BITMAP_INSERT_LOVITEM, rdata);

		PageSetLSN(lovPage, recptr);
		PageSetTLI(lovPage, ThisTimeLineID);
	}

	END_CRIT_SECTION();
}

/*
 * _bitmap_log_lovmetapage() -- log the lov meta page.
 */
void
_bitmap_log_lovmetapage(Relation rel, Buffer lovMetaBuffer, uint8 numOfAttrs)
{
	Page			lovMetapage;
	BMLOVMetaItem	metaItems;

	lovMetapage = BufferGetPage(lovMetaBuffer);
	metaItems = (BMLOVMetaItem)PageGetContents(lovMetapage);

	/* XLOG stuff */
	START_CRIT_SECTION();

	if (!(rel->rd_istemp))
	{
		BMLOVMetaItem	copyMetaItems;
		XLogRecPtr		recptr;
		XLogRecData		rdata[1];


		xl_bm_lovmetapage* xlLovMeta;

#ifdef BM_DEBUG
		elog(LOG, "call _bitmap_log_lovmetapage: numOfAttrs=%d", numOfAttrs);
#endif

		xlLovMeta = (xl_bm_lovmetapage*)
			palloc(sizeof(xl_bm_lovmetapage)+
					numOfAttrs*sizeof(BMLOVMetaItemData));

		xlLovMeta->bm_node = rel->rd_node;
		xlLovMeta->bm_num_of_attrs = numOfAttrs;

		copyMetaItems = (BMLOVMetaItem)
			(((char*)xlLovMeta) + sizeof(xl_bm_lovmetapage)); 
		memcpy(copyMetaItems, metaItems, numOfAttrs*sizeof(BMLOVMetaItemData));

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char*)xlLovMeta;
		rdata[0].len = 
			sizeof(xl_bm_lovmetapage) + numOfAttrs*sizeof(BMLOVMetaItemData);
		rdata[0].next = NULL;

		recptr = XLogInsert(RM_BITMAP_ID, 
							XLOG_BITMAP_INSERT_LOVMETA, rdata);

		PageSetLSN(lovMetapage, recptr);
		PageSetTLI(lovMetapage, ThisTimeLineID);
		pfree(xlLovMeta);
	}

	END_CRIT_SECTION();
}

