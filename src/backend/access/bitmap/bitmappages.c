/*-------------------------------------------------------------------------
 *
 * bitmappage.c
 *	  Bitmap index page management code for the bitmap index.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tupdesc.h"
#include "access/bitmap.h"
#include "miscadmin.h"

/*
 * _bitmap_getbuf() -- return the buffer for the given block number and
 * 					   the access method.
 * Note: This function does not support "P_NEW" block number.
 */
Buffer
_bitmap_getbuf(Relation rel, BlockNumber blkno, int access)
{
	Buffer buf;

/*
	if (blkno == P_NEW)
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bitmap index does not use P_NEW")));
*/

	buf = ReadBuffer(rel, blkno);
	if (access != BM_NOLOCK)
		LockBuffer(buf, access);

	return buf;
}

/*
 * _bitmap_wrtbuf() -- write a buffer page to disk.
 *
 * Release the lock and the pin held on the buffer.
 */
void
_bitmap_wrtbuf(Buffer buf)
{
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buf);
}

/*
 * _bitmap_wrtnorelbuf() -- write a buffer page to disk without still holding
 *		the pin on this page.
 */
void
_bitmap_wrtnorelbuf(Buffer buf)
{
	WriteNoReleaseBuffer(buf);
}

/*
 * _bitmap_relbuf() -- release the buffer without writing.
 */
void
_bitmap_relbuf(Buffer buf)
{
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
}

/*
 * _bitmap_lovmetapageinit() -- initialize a new LOV metapage.
 */
void
_bitmap_lovmetapageinit(Relation rel, Buffer buf)
{
	int				attNo;
	Page			page;
	BMLOVMetaItem	metaItems;

	page = BufferGetPage(buf);
	Assert (PageIsNew(page));

	PageInit(page, BufferGetPageSize(buf), 0);

	metaItems = (BMLOVMetaItem) PageGetContents(page);

	for (attNo=0; attNo<RelationGetNumberOfAttributes(rel); attNo++)
	{
		Oid heapId;
		Oid indexId;

		_bitmap_create_lov_heapandindex(rel, attNo, 
										&heapId, &indexId);

		metaItems[attNo].bm_lov_heapId = heapId;
		metaItems[attNo].bm_lov_indexId = indexId;
		metaItems[attNo].bm_lov_lastpage = BM_LOV_STARTPAGE + attNo;
	}

ereport(DEBUG1, (errmsg_internal("completely done in _bitmap_lovmetapageinit [src/backend/access/bitmap/bitmappages.c] --> index_create")));
}

/*
 * _bitmap_lovpageinit -- initialize a new LOV page.
 */
void
_bitmap_lovpageinit(Relation rel, Buffer buf)
{
	Page			page;

	page = (Page) BufferGetPage(buf);
	Assert (PageIsNew(page));

	PageInit(page, BufferGetPageSize(buf), 0);

}

/*
 * _bitmap_bitmappageinit() -- initialize a new page to store the bitmap.
 *
 * Note: This function requires an exclusive lock on the metapage.
 */
void
_bitmap_bitmappageinit(Relation rel, Buffer buf)
{
	Page			page;
	BMBitmapOpaque	opaque;

	page = (Page) BufferGetPage(buf);
	Assert (PageIsNew(page));

	PageInit(page, BufferGetPageSize(buf), sizeof(BMBitmapOpaqueData));

	opaque = (BMBitmapOpaque) PageGetSpecialPointer(page);
	opaque->bm_hrl_words_used = 0;
	opaque->bm_bitmap_next = InvalidBlockNumber;
}

/*
 * _bitmap_init() -- initialize the bitmap index.
 */
void
_bitmap_init(Relation rel)
{
	BMMetaPage		metapage;
	Buffer			metabuf;
	Page			page;
	Buffer			buf;

	int 			attNo;
  
  ereport(DEBUG1, (errmsg_internal("into --> _bitmap_init")));
	/* sanity check */
	if (RelationGetNumberOfBlocks(rel) != 0)
		ereport (ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				errmsg("cannot initialize non-empty bitmap index \"%s\"",
				RelationGetRelationName(rel))));

	/* create the metapage */

	/* obtain the buffer for the metapage */
	/* metabuf = _bitmap_getbuf(rel, BM_METAPAGE, BM_WRITE);*/
	metabuf = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
	page = BufferGetPage(metabuf);

	Assert (PageIsNew(page)) ;

	/* initialize the metapage */
	PageInit(page, BufferGetPageSize(metabuf), 0);
	metapage = (BMMetaPage) page;
	
	/* allocate LOV metapage */
	buf = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
	_bitmap_lovmetapageinit(rel, buf);

	_bitmap_log_newpage(rel, XLOG_BITMAP_INSERT_NEWLOVMETA, buf, metapage);

	_bitmap_wrtbuf(buf);

  ereport(DEBUG1, (errmsg_internal("completed --> _bitmap_wrtbuf")));
	/* allocate the first pages of LOVs for all attributes to be indexed */
	for (attNo=0; attNo<RelationGetNumberOfAttributes(rel); attNo++)
	{
		BMLOVItem 		lovItem;
		OffsetNumber	newOffset;
		Page			currLovPage;

		buf = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
		_bitmap_lovpageinit(rel, buf);

		_bitmap_log_newpage(rel, XLOG_BITMAP_INSERT_NEWLOV, buf, metapage);

		currLovPage = BufferGetPage(buf);

		/* set the first item to support NULL value */
		lovItem = _bitmap_formitem(0);
		newOffset = OffsetNumberNext(PageGetMaxOffsetNumber(currLovPage));

		if (PageAddItem(currLovPage, (Item)lovItem, 
						sizeof(BMLOVItemData), newOffset,
						LP_USED) == InvalidOffsetNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("failed to add LOV item to \"%s\"",
					RelationGetRelationName(rel))));

		_bitmap_log_lovitem(rel, buf, true, newOffset, lovItem);

		pfree(lovItem);

		_bitmap_wrtbuf(buf);
	}

	_bitmap_log_metapage(rel, metapage);

  ereport(DEBUG1, (errmsg_internal("completed --> _bitmap_log_metapage")));
	/* write the metapage */
	_bitmap_wrtbuf(metabuf);
	
  ereport(DEBUG1, (errmsg_internal("completed --> _bitmap_init")));
}
