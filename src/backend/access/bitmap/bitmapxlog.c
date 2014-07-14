/*-------------------------------------------------------------------------
 *
 * bitmapxlog.c
 *	WAL replay logic for the bitmap index.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	$PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/bitmap.h"
#include "access/xlogutils.h"

static void
bitmap_xlog_newpage(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_bm_newpage	*xlrec = (xl_bm_newpage*) XLogRecGetData(record);

	Relation		reln;
	Page			page;
	uint8			info;

	/* xl_bm_metapage	*xlrecMeta = (xl_bm_metapage*)
		((char*)xlrec+sizeof(xl_bm_newpage)); */

	info = record->xl_info & ~XLR_INFO_MASK;

  ereport(DEBUG1, (errmsg_internal("into --> XLogOpenRelation")));
	reln = XLogOpenRelation(xlrec->bm_node);
  ereport(DEBUG1, (errmsg_internal("done --> XLogOpenRelation")));
	if (!RelationIsValid(reln))
		return;
  ereport(DEBUG1, (errmsg_internal("crash1")));

	if (redo)
	{
		Buffer		buffer;

#ifdef BM_DEBUG
		ereport(LOG, (errcode(LOG), 
			errmsg("call bitmap_xlog_newpage: redo=%d, info=%x\n", redo, info)));
#endif

		buffer = XLogReadBuffer(true, reln, xlrec->bm_new_blkno);
		if (!BufferIsValid(buffer))
			elog(PANIC, "bm_insert_redo: block unfound: %d", 
				 xlrec->bm_new_blkno);

		page = BufferGetPage(buffer);

		if (XLByteLT(PageGetLSN(page), lsn))
		{
			Buffer		metabuf;
			BMMetaPage	metapage;

			switch (info)
			{
				case XLOG_BITMAP_INSERT_NEWLOV:
					_bitmap_lovpageinit(reln, buffer);
					break;
				case XLOG_BITMAP_INSERT_NEWLOVMETA:
					_bitmap_lovmetapageinit(reln, buffer);
					break;
				case XLOG_BITMAP_INSERT_NEWBITMAP:
					_bitmap_bitmappageinit(reln, buffer);
					break;
				default:
					elog(PANIC, "bitmap_redo: unknown newpage op code %u", info);
			}

			PageSetLSN(page, lsn);
			PageSetTLI(page, ThisTimeLineID);
			_bitmap_wrtbuf(buffer);

			metabuf = XLogReadBuffer(true, reln, BM_METAPAGE);
			if (!BufferIsValid(metabuf))
				elog(PANIC, "bm_insert_redo: block unfound: %d", BM_METAPAGE);
			metapage = (BMMetaPage)BufferGetPage(metabuf);

			if (XLByteLT(PageGetLSN(metapage), lsn))
			{
				PageSetLSN(metapage, lsn);
				PageSetTLI(metapage, ThisTimeLineID);
				_bitmap_wrtbuf(metabuf);
			}
			else
				_bitmap_relbuf(metabuf);
		}

		else {
			_bitmap_relbuf(buffer);
		}
	}
	else
		elog(PANIC, "bm_insert_undo: not implemented.");
  /* elog(PANIC, "call completely done for _bitmap_lovmetapageinit from  bitmap_xlog_newpage[src/backend/access/bitmap/bitmapxlog.c]", info); */
}

static void
bitmap_xlog_insert_lovitem(bool redo, XLogRecPtr lsn, XLogRecord* record)
{
	xl_bm_lovitem	*xlrec = (xl_bm_lovitem*) XLogRecGetData(record);
	Relation reln;

	reln = XLogOpenRelation(xlrec->bm_node);
	if (!RelationIsValid(reln))
		return;

	if (redo)
	{
		Buffer			lovBuffer;
		Page			lovPage;

#ifdef BM_DEBUG
		ereport(LOG, (errcode(LOG), 
			errmsg("call bitmap_xlog_insert_lovitem: redo=%d, blkno=%d\n", 
					redo, xlrec->bm_lov_blkno)));
#endif

		lovBuffer = XLogReadBuffer(false, reln, xlrec->bm_lov_blkno);
		if (!BufferIsValid(lovBuffer))
			elog(PANIC, "bm_insert_redo: block unfound: %d",
				 xlrec->bm_lov_blkno);

		lovPage = BufferGetPage(lovBuffer);

		if (XLByteLT(PageGetLSN(lovPage), lsn))
		{
			if(xlrec->bm_isNewItem)
			{
				OffsetNumber	newOffset, itemSize;

				newOffset = OffsetNumberNext(PageGetMaxOffsetNumber(lovPage));
				if (newOffset != xlrec->bm_lov_offset)
					elog(PANIC, 
				"bm_insert_redo: LOV item is not inserted in pos %d(requested %d)",
						 newOffset, xlrec->bm_lov_offset);

				itemSize = sizeof(BMLOVItemData);
				if (itemSize > PageGetFreeSpace(lovPage))
					elog(PANIC, 
						 "bm_insert_redo: not enough space in LOV page %d",
						 xlrec->bm_lov_blkno);
		
				if (PageAddItem(lovPage, (Item)&(xlrec->bm_lovItem), itemSize, 
								newOffset, LP_USED) == InvalidOffsetNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("failed to add LOV item to \"%s\"",
							RelationGetRelationName(reln))));
			}

			else{
				BMLOVItem oldLovItem;
				oldLovItem = (BMLOVItem)
					PageGetItem(lovPage, 
								PageGetItemId(lovPage, xlrec->bm_lov_offset));

				memcpy(oldLovItem, &(xlrec->bm_lovItem), sizeof(BMLOVItemData));
			}

			PageSetLSN(lovPage, lsn);
			PageSetTLI(lovPage, ThisTimeLineID);
			_bitmap_wrtbuf(lovBuffer);
		}

		else {
			_bitmap_relbuf(lovBuffer);
		}
	}

	else
		elog(PANIC, "bm_insert_undo: not implemented.");
}

static void
bitmap_xlog_insert_meta(bool redo, XLogRecPtr lsn, XLogRecord* record)
{
	xl_bm_metapage	*xlrec = (xl_bm_metapage*) XLogRecGetData(record);
	Relation		reln;

	reln = XLogOpenRelation(xlrec->bm_node);
	
	if (!RelationIsValid(reln))
		return;

	if (redo)
	{
		Buffer			metabuf;
		BMMetaPage			metapage;

#ifdef BM_DEBUG
		ereport(LOG, (errcode(LOG), 
			errmsg("call bitmap_xlog_insert_meta: redo=%d\n", redo)));
#endif

		metabuf = XLogReadBuffer(false, reln, BM_METAPAGE);
		if (!BufferIsValid(metabuf))
			elog(PANIC, "bm_insert_redo: block unfound: %d", BM_METAPAGE);

		/* restore the page */
		metapage = (BMMetaPage)BufferGetPage(metabuf);

		if (XLByteLT(PageGetLSN(metapage), lsn))
		{
			PageSetLSN(metapage, lsn);
			PageSetTLI(metapage, ThisTimeLineID);
			_bitmap_wrtbuf(metabuf);
		}

		else
			_bitmap_relbuf(metabuf);
	}

	else
		elog(PANIC, "bm_insert_undo: not implemented.");
}

static void
bitmap_xlog_insert_lovmeta(bool redo, XLogRecPtr lsn, XLogRecord* record)
{
	xl_bm_lovmetapage	*xlrec = (xl_bm_lovmetapage*)XLogRecGetData(record);
	Relation reln;

	reln = XLogOpenRelation(xlrec->bm_node);
	/* reln = XLogOpenRelation(redo, RM_BITMAP_ID, xlrec->bm_node);*/
	
	if (!RelationIsValid(reln))
		return;

	if (redo)
	{
		Buffer	lovMetabuf;
		Page	lovMetapage;
		BMLOVMetaItem	copyMetaItems, metaItems;

#ifdef BM_DEBUG
		ereport(LOG, (errcode(LOG), 
			errmsg("call bitmap_xlog_insert_lovmeta: redo=%d\n", redo)));
#endif

		lovMetabuf = XLogReadBuffer(false, reln, BM_LOV_STARTPAGE-1);
		if (!BufferIsValid(lovMetabuf))
			elog(PANIC, "bm_insert_redo: block unfound: %d -- at (%d,%d,%d)", 
				 BM_LOV_STARTPAGE-1, xlrec->bm_node.spcNode, 
				 xlrec->bm_node.dbNode, xlrec->bm_node.relNode);

		lovMetapage = BufferGetPage(lovMetabuf);

		if (XLByteLT(PageGetLSN(lovMetapage), lsn))
		{
#ifdef BM_DEBUG
			uint32 attno;
#endif

			copyMetaItems = (BMLOVMetaItem)PageGetContents(lovMetapage);

			metaItems = (BMLOVMetaItem)
				((char*)xlrec + sizeof(xl_bm_lovmetapage));
			memcpy(copyMetaItems, metaItems, 
					xlrec->bm_num_of_attrs * sizeof(BMLOVMetaItemData));

#ifdef BM_DEBUG
			for(attno=0; attno<xlrec->bm_num_of_attrs; attno++)
				elog(LOG, "metaItems=%d, %d, %d", 
					 copyMetaItems[attno].bm_lov_heapId,
					 copyMetaItems[attno].bm_lov_indexId, 
					 copyMetaItems[attno].bm_lov_lastpage);
#endif

			PageSetLSN(lovMetapage, lsn);
			PageSetTLI(lovMetapage, ThisTimeLineID);
			_bitmap_wrtbuf(lovMetabuf);
		}

		else
			_bitmap_relbuf(lovMetabuf);
	}

	else
		elog(PANIC, "bm_insert_undo: not implemented.");		
}

static void
bitmap_xlog_insert_bitmap(bool redo, XLogRecPtr lsn, XLogRecord* record)
{
	xl_bm_bitmappage	*xlrec = (xl_bm_bitmappage*) XLogRecGetData(record);
	Relation reln;

	reln = XLogOpenRelation(xlrec->bm_node);
	if (!RelationIsValid(reln))
		return;

	if (redo)
	{
		Buffer	bitmapBuffer;
		Page	bitmapPage;
		BMBitmapOpaque	bitmapPageOpaque ;

		bitmapBuffer = XLogReadBuffer(false, reln, xlrec->bm_bitmap_blkno);
		if (!BufferIsValid(bitmapBuffer))
			elog(PANIC, "bm_insert_redo: block unfound: %d",
				 xlrec->bm_bitmap_blkno);

		bitmapPage = BufferGetPage(bitmapBuffer);

		if (XLByteLT(PageGetLSN(bitmapPage), lsn))
		{
			bitmapPageOpaque = (BMBitmapOpaque)PageGetSpecialPointer(bitmapPage);;

#ifdef BM_DEBUG
			ereport(LOG, (errcode(LOG), 
				errmsg("call bitmap_xlog_insert_bitmap: redo=%d, blkno=%d, isOpaque=%d, words_used=%d, lastword=%d, next_blkno=%d\n", redo, xlrec->bm_bitmap_blkno, xlrec->bm_isOpaque, xlrec->bm_lastword_pos, xlrec->bm_lastword_in_block, xlrec->bm_next_blkno)));
#endif

			if (xlrec->bm_isOpaque)
			{
				if (bitmapPageOpaque->bm_bitmap_next != InvalidBlockNumber)
					elog(PANIC, 
						"%s next bitmap page for blkno %d is already set",
						"bm_insert_redo: ",
						xlrec->bm_bitmap_blkno);
				Assert(bitmapPageOpaque->bm_hrl_words_used == 
						BM_NUM_OF_HRL_WORDS_PER_PAGE);

				bitmapPageOpaque->bm_bitmap_next = xlrec->bm_next_blkno;
			}

			else 
			{
				BMBitmap	bitmap;

				if (bitmapPageOpaque->bm_hrl_words_used != 
					xlrec->bm_lastword_pos - 1)
					elog(PANIC, 
						"bm_insert_redo: a bit has been inserted in the pos %d",
						xlrec->bm_lastword_pos);

				Assert (xlrec->bm_lastword_in_block != 0);

				bitmap = (BMBitmap) PageGetContents(bitmapPage);
				
				bitmap->bm_headerWords
					[(bitmapPageOpaque->bm_hrl_words_used/BM_HRL_WORD_SIZE)] |=
					(1<<(BM_HRL_WORD_SIZE-1-
					(bitmapPageOpaque->bm_hrl_words_used%BM_HRL_WORD_SIZE)));
				bitmap->bm_contentWords[bitmapPageOpaque->bm_hrl_words_used] = 
					xlrec->bm_lastword_in_block;
				bitmapPageOpaque->bm_hrl_words_used ++;
			}

			PageSetLSN(bitmapPage, lsn);
			PageSetTLI(bitmapPage, ThisTimeLineID);
			_bitmap_wrtbuf(bitmapBuffer);
		}
		else
			_bitmap_relbuf(bitmapBuffer);
	}

	else
		elog(PANIC, "bm_insert_undo: not implemented.");
}

static void
bitmap_xlog_insert_bitmap_lastwords(bool redo, XLogRecPtr lsn, XLogRecord* record)
{
	xl_bm_bitmap_lastwords	*xlrec = 
		(xl_bm_bitmap_lastwords*) XLogRecGetData(record);
	Relation reln;

	reln = XLogOpenRelation(xlrec->bm_node);
	if (!RelationIsValid(reln))
		return;

	if (redo)
	{
		Buffer		lovBuffer;
		Page		lovPage;
		BMLOVItem	lovItem;

#ifdef BM_DEBUG
		ereport(LOG, (errcode(LOG), 
			errmsg("call bitmap_xlog_insert_bitmap_lastwords: redo=%d\n", 
					redo)));
#endif

		lovBuffer = XLogReadBuffer(false, reln, xlrec->bm_lov_blkno);
		if (!BufferIsValid(lovBuffer))
			elog(PANIC, "bm_insert_redo: block unfound: %d",
				 xlrec->bm_lov_blkno);

		lovPage = BufferGetPage(lovBuffer);

		if (XLByteLT(PageGetLSN(lovPage), lsn))
		{
			lovItem = (BMLOVItem)
				PageGetItem(lovPage, PageGetItemId(lovPage, xlrec->bm_lov_offset));

			lovItem->bm_last_compword = xlrec->bm_last_compword;
			lovItem->bm_last_word = xlrec->bm_last_word;
			lovItem->bm_last_two_headerbits = xlrec->bm_last_two_headerbits;

			PageSetLSN(lovPage, lsn);
			PageSetTLI(lovPage, ThisTimeLineID);
			_bitmap_wrtbuf(lovBuffer);
		}

		else
			_bitmap_relbuf(lovBuffer);
	}

	else
		elog(PANIC, "bm_insert_undo: not implemented.");
}

void
bitmap_redo(XLogRecPtr lsn, XLogRecord* record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BITMAP_INSERT_NEWLOV:
			bitmap_xlog_newpage(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_LOVITEM:
			bitmap_xlog_insert_lovitem(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_META:
			bitmap_xlog_insert_meta(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_NEWLOVMETA:
			bitmap_xlog_newpage(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_LOVMETA:
			bitmap_xlog_insert_lovmeta(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_NEWBITMAP:
			bitmap_xlog_newpage(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_BITMAP:
			bitmap_xlog_insert_bitmap(true, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_BITMAP_LASTWORDS:
			bitmap_xlog_insert_bitmap_lastwords(true, lsn, record);
			break;
		default:
			elog(PANIC, "bitmap_redo: unknown op code %u", info);
	}
}

void
bitmap_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BITMAP_INSERT_NEWLOV:
			bitmap_xlog_newpage(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_LOVITEM:
			bitmap_xlog_insert_lovitem(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_META:
			bitmap_xlog_insert_meta(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_NEWLOVMETA:
			bitmap_xlog_newpage(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_LOVMETA:
			bitmap_xlog_insert_lovmeta(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_NEWBITMAP:
			bitmap_xlog_newpage(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_BITMAP:
			bitmap_xlog_insert_bitmap(false, lsn, record);
			break;
		case XLOG_BITMAP_INSERT_BITMAP_LASTWORDS:
			bitmap_xlog_insert_bitmap_lastwords(false, lsn, record);
			break;

		default:
			elog(PANIC, "bitmap_undo: unknown op code %u", info);
	}
}

static void
out_target(char *buf, RelFileNode* node)
{
	sprintf(buf + strlen(buf), "rel %u/%u/%u",
			node->spcNode, node->dbNode, node->relNode);
}

void
bitmap_desc(char* buf, uint8 xl_info, char* rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BITMAP_INSERT_NEWLOV:
		{
			xl_bm_newpage *xlrec = (xl_bm_newpage*)rec;

			strcat(buf, "insert a new LOV page: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_LOVITEM:
		{
			xl_bm_lovitem *xlrec = (xl_bm_lovitem*)rec;

			strcat(buf, "insert a new LOV item: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_META:
		{
			xl_bm_metapage *xlrec = (xl_bm_metapage*)rec;

			strcat(buf, "update the metapage: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_NEWLOVMETA:
		{
			xl_bm_newpage *xlrec = (xl_bm_newpage*)rec;

			strcat(buf, "insert a new LOV metapage: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_LOVMETA:
		{
			xl_bm_lovmetapage *xlrec = (xl_bm_lovmetapage*)rec;

			strcat(buf, "update the LOV metapage: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_NEWBITMAP:
		{
			xl_bm_newpage *xlrec = (xl_bm_newpage*)rec;

			strcat(buf, "insert a new bitmap page: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_BITMAP:
		{
			xl_bm_bitmappage *xlrec = (xl_bm_bitmappage*)rec;

			strcat(buf, "update a bitmap page: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_BITMAP_LASTWORDS:
		{
			xl_bm_bitmap_lastwords *xlrec = (xl_bm_bitmap_lastwords*)rec;

			strcat(buf, "update the last two words in a bitmap: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}

		default:
			strcat(buf, "UNKNOWN");
			break;
	}
}
