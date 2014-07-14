/*-------------------------------------------------------------------------
 *
 * bitmap.h
 *	header file for Postgres bitmap access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	$PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */

#ifndef BITMAP_H
#define BITMAP_H

#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/xlogutils.h"

#define BM_READ		BUFFER_LOCK_SHARE
#define BM_WRITE	BUFFER_LOCK_EXCLUSIVE
#define BM_NOLOCK	(-1)

/* the encoding schemes for a bitmap index */
#define BM_EQUALITY		1

/* the start extent number */
#define BM_START_EXTENT_NUMBER		0

/*
 * Metapage, always the first page (page 0) in the index.
 *
 * This page stores some meta-data information about this index.
 */
typedef struct BMMetaPageData 
{
	PageHeaderData	bm_phdr;		/* pad for page header (do not use) */
} BMMetaPageData;
typedef BMMetaPageData* BMMetaPage;

#define BM_METAPAGE 	0
#define BM_MAX_NUM_OF_EXTENTS		((uint8) \
	(((BLCKSZ) - MAXALIGN(sizeof(BMMetaPageData))) / (sizeof(BlockNumber))))
#define BM_EXTENT_SIZE(pos)		(((uint32)1)<<(pos))

/* the maximum number of heap tuples in one page */
#define MaxNumHeapTuples	\
	((BLCKSZ - 1) / MAXALIGN(offsetof(HeapTupleHeaderData, t_bits) + \
	sizeof(ItemIdData)) + 1)

/* the size of a hybrid run-length(HRL) word */
#define BM_HRL_WORD_SIZE		8
/* the type for a word in the HRL scheme */
typedef uint8			BM_HRL_WORD;
#define BM_HRL_WORD_LEFTMOST	(BM_HRL_WORD_SIZE-1)

/*
 * LOV page -- pages to store list of attribute values and the pointers
 * 			   to their bitmaps.
 */

/*
 * We maintain a heap and index relations for all values in each attribute
 * to support better search performance.
 *
 * The LOV meta page contains BMLOVMetaItemData, defined in below.
 */
typedef struct BMLOVMetaItemData
{
	Oid			bm_lov_heapId;		/* the relation id for the heap */
	Oid			bm_lov_indexId;		/* the relation id for the index */

	/* the last block number for this attribute in LOV. */
	BlockNumber	bm_lov_lastpage;
} BMLOVMetaItemData;
typedef BMLOVMetaItemData*	BMLOVMetaItem;

#define		BM_LOV_STARTPAGE	2

/*
 * Items in a LOV page.
 */
typedef struct BMLOVItemData 
{
	BlockNumber		bm_lov_head;	/* the first page of the bitmap */
	BlockNumber 	bm_lov_tail;	/* the last page of the bitmap */

	/* Additional information to be used to append new bits into
	   existing bitmap that this value is associated with. 
	   The following two words do not store in the regular bitmap page
	   (see below). */

	/* Represent the last complete word in this bitmap. */
	BM_HRL_WORD		bm_last_compword;

	/* Represent the last word in the associated bitmap. This word is not
	   a complete word. All new bits will be appended to this word.*/
	BM_HRL_WORD		bm_last_word;

	/* the last bit whose value is 1 (a set bit), starting from 1. */
	uint64			bm_last_setbit;

	uint8			bm_last_two_headerbits;
	 
} BMLOVItemData;

typedef BMLOVItemData* BMLOVItem;

#define	BMLOVITEM_SIZE(itup)	(IndexTupleSize(itup) + \
	(sizeof(BMLOVItemData) - sizeof(IndexTupleData)))

/*
 * Bitmap page -- pages to store the bitmap for each attribute value.
 *
 * This bitmap page stores two parts of information: one is called header
 * words, each bit of which corresponds to a word in the content word
 * (described below). If the bit is a set bit (1), then its corresponding
 * content word is a compressed word. Otherwise, it is a literal word.
 */

/*
 * Opaque data for a bitmap page.
 */
typedef struct BMBitmapOpaqueData 
{
	uint32		bm_hrl_words_used;		/* the number of words used */
	BlockNumber	bm_bitmap_next;		/* the next page for this bitmap */
} BMBitmapOpaqueData;
typedef BMBitmapOpaqueData*	BMBitmapOpaque;

#define BM_MAX_NUM_OF_HRL_WORDS_PER_PAGE \
	((BLCKSZ - \
	MAXALIGN(sizeof(PageHeaderData)) - \
	MAXALIGN(sizeof(BMBitmapOpaqueData)))/sizeof(BM_HRL_WORD))
#define BM_MAX_NUM_OF_HEADER_WORDS \
	(BM_MAX_NUM_OF_HRL_WORDS_PER_PAGE/(BM_HRL_WORD_SIZE+1))
/* We want to limit this number to the multiplication of the word size.*/
#define BM_NUM_OF_HRL_WORDS_PER_PAGE \
	(((BM_MAX_NUM_OF_HRL_WORDS_PER_PAGE - \
	  BM_MAX_NUM_OF_HEADER_WORDS)/BM_HRL_WORD_SIZE) * BM_HRL_WORD_SIZE)

typedef struct BMBitmapData
{
	BM_HRL_WORD bm_headerWords[BM_MAX_NUM_OF_HEADER_WORDS];
	BM_HRL_WORD bm_contentWords[BM_NUM_OF_HRL_WORDS_PER_PAGE];
} BMBitmapData;
typedef BMBitmapData*	BMBitmap;

#define BM_MAKE_FILL_WORD(bit, length) \
	((((BM_HRL_WORD)bit) << (BM_HRL_WORD_SIZE-1)) | (length))

#define ALL_ZERO			0

#define LITERAL_ALL_ZERO	0
#define LITERAL_ALL_ONE		((BM_HRL_WORD)(~((BM_HRL_WORD)0)))

#define FILL_LENGTH(w)		(((BM_HRL_WORD)(((BM_HRL_WORD)(w))<<1))>>1)
#define MAX_FILL_LENGTH		((((BM_HRL_WORD)1)<<(BM_HRL_WORD_SIZE-1))-1)
#define GET_FILL_BIT(w)		(((BM_HRL_WORD)(w))>>BM_HRL_WORD_LEFTMOST)
#define IS_FILL_WORD(words,wordNo) \
	((((words)[(wordNo)/BM_HRL_WORD_SIZE]) & \
	 (((BM_HRL_WORD)1) << (BM_HRL_WORD_SIZE-1-((wordNo)%BM_HRL_WORD_SIZE)))) != 0)

/*
 * the state for build 
 */
typedef struct BMBuildState
{
	Buffer			bm_metabuf;
	Buffer			bm_lov_metabuf;

	TupleDesc*		bm_tupDescs;
	Relation*		bm_lov_heaps;
	Relation*		bm_lov_indexes;
	ScanKeyData*	bm_lov_scanKeys;
	IndexScanDesc*	bm_lov_scanDescs;

	double 			ituples ;	/* the number of index tuples */
} BMBuildState ;

/*
 * Scan opaque data
 */
typedef struct BMBitmapScanPositionData
{
	Buffer			bm_lovBuffer;/* the buffer that contains the LOV item. */
	OffsetNumber	bm_lovOffset;	/* the offset of the LOV item */

	BlockNumber		bm_nextBlockNo; /* the next bitmap page block */

	/* the number of words (in uncompressed form) that have been read */
	uint32			bm_numOfWordsRead;

	/* the starting position of words in the latest block. */
	uint32			bm_startWordNo;

	/* the number of words in the latest block */
	uint32			bm_num_of_words;

	/* indicate if the last two words in the bitmap has been read. 
		These two words are stored inside a BMLovItem. */
	bool			bm_readLastWords;

	/* the words in the latest block */
	BM_HRL_WORD		bm_headerWordsABlock[BM_MAX_NUM_OF_HEADER_WORDS];
	BM_HRL_WORD		bm_wordsABlock[BM_NUM_OF_HRL_WORDS_PER_PAGE];

} BMBitmapScanPositionData;
typedef BMBitmapScanPositionData* BMBitmapScanPosition;

/* the batch size of tids to be found before returning them to
	the caller. */
#define BATCH_TIDS	1024*16

/*
 * The position of a scan, including the position of the tid in the array
 * of tids, and the bit position in all related bitmaps.
 *
 * This structure is followed by N BMBitmapScanPosition, where N is the number
 * of attributes in the scan key.
 */
typedef struct BMScanPositionData
{
	uint64			bm_tid_pos;	/* the position of the current tid */

	/* the latest block of ANDED words */
	BM_HRL_WORD		bm_last_scanHeaderWords[BM_MAX_NUM_OF_HEADER_WORDS];
	BM_HRL_WORD		bm_last_scanwords[BM_NUM_OF_HRL_WORDS_PER_PAGE]; 
	/* the number of words in the latest block of ANDED words. */
	uint32			bm_num_of_words;
	/* the last scan word no */
	uint32			bm_lastScanWordNo;
	/* the last scan position in bm_last_scanword */
	uint32			bm_last_scanpos;

	/* the next read word position in uncompressed bitmap form */
	uint32			bm_nextReadNo;

	/* indicate if this scan is over. */
	bool			bm_done;

	/* the tid offserts that are right after the current scan */
	uint64			bm_tidOffsets[BATCH_TIDS];

	/* the total number of such tids. */
	uint32			bm_num_of_tids;
	/* the current scan position in this array. */
	uint32			bm_curr_tid_pos;

	/* followed by one or more BMBitmapScanPositionData, depending on
	   the values in the index predicates. */
} BMScanPositionData;
typedef BMScanPositionData* BMScanPosition;

typedef struct BMScanOpaqueData
{
	BMScanPosition		bm_currPos;
	BMScanPosition		bm_markPos;
} BMScanOpaqueData;
typedef BMScanOpaqueData* BMScanOpaque;

/*
 * XLOG records for bitmap index operations
 *
 * Some information in high 4 bits of log record xl_info field.
 */
#define XLOG_BITMAP_INSERT_NEWLOV	0x00 /* add a new LOV page */
#define XLOG_BITMAP_INSERT_LOVITEM	0x10 /* add a new entry into the LOV list */
#define XLOG_BITMAP_INSERT_META		0x20 /* update the metapage */
#define XLOG_BITMAP_INSERT_NEWLOVMETA	0x30 /* add a new LOV metapage */
#define XLOG_BITMAP_INSERT_LOVMETA	0x40 /* update the whole LOV metapage */
#define XLOG_BITMAP_INSERT_LOVMETA_UPDATE	0x50 /* update the bm_lastpage in the 
													LOV metapage */
#define XLOG_BITMAP_INSERT_NEWBITMAP	0x60 /* add a new bitmap page */
#define XLOG_BITMAP_INSERT_BITMAP	0x70 /* add a new set bit */
#define XLOG_BITMAP_INSERT_BITMAP_LASTWORDS	0x80 /* update the last 2 words
													in a bitmap */

/* The information about inserting a new lovitem into the LOV list. */
typedef struct xl_bm_lovitem
{
	RelFileNode 	bm_node;
	BlockNumber		bm_lov_blkno;
	bool			bm_isNewItem;
	OffsetNumber	bm_lov_offset;
	BMLOVItemData	bm_lovItem;
} xl_bm_lovitem;

/* The information about the LOV metapage */
typedef struct xl_bm_lovmetapage
{
	RelFileNode 	bm_node;
	uint8	bm_num_of_attrs;

	/* follow by a set of BMLOVMetaItemData */
} xl_bm_lovmetapage;

/* The information about changes to the LOV metapage */
typedef struct xl_bm_lovmeta_update
{
	uint32			bm_attno;
	BlockNumber		bm_lov_lastpage;
} xl_bm_lovmeta_update;

/* The information about adding a new page */
typedef struct xl_bm_newpage{
	RelFileNode 	bm_node;
	BlockNumber		bm_new_blkno;
} xl_bm_newpage;

/* The information about a new LOV page */
/*typedef struct xl_bm_newlovpage{
	BlockNumber		bm_lov_blkno;
} xl_bm_newlovpage;*/

/* The information about a new bitmap page */
/* typedef struct xl_bm_newbitmappage{
	BlockNumber		bm_bitmap_blkno;
} xl_bm_newbitmappage; */

/* The information about a new LOV page */
/*typedef struct xl_bm_newlovpage{
	BlockNumber		bm_lov_blkno;
} xl_bm_newlovpage; */

/* The information about a new LOV metapage */
/*typedef struct xl_bm_newlovmetapage{
	BlockNumber		bm_lovmeta_blkno;
} xl_bm_newlovmetapage; */

/* The information about changes on a bitmap page. 
   If bm_isOpaque is true, then bm_next_blkno is set. Otherwise,
   bm_lastword_pos and bm_lastword_in_block are set. 
 */
typedef struct xl_bm_bitmappage{
	RelFileNode 	bm_node;
	BlockNumber		bm_bitmap_blkno;

	bool			bm_isOpaque;

	OffsetNumber	bm_lastword_pos;
	BM_HRL_WORD		bm_lastword_in_block;
	bool			bm_isFillWord;
	BlockNumber		bm_next_blkno;
} xl_bm_bitmappage;

/* The information about changes to the last 2 words in a bitmap */
typedef struct xl_bm_bitmap_lastwords
{
	RelFileNode 	bm_node;
	BM_HRL_WORD		bm_last_compword;
	BM_HRL_WORD		bm_last_word;
	uint8			bm_last_two_headerbits;

	BlockNumber		bm_lov_blkno;
	OffsetNumber	bm_lov_offset;
} xl_bm_bitmap_lastwords;

/* The information about the changes in the metapage.
   If the value does not change, then set it to 0.
 */
typedef struct xl_bm_metapage
{
	RelFileNode 	bm_node;
} xl_bm_metapage;



/* public routines */
extern Datum bmbuild(PG_FUNCTION_ARGS);
extern Datum bminsert(PG_FUNCTION_ARGS);
extern Datum bmbeginscan(PG_FUNCTION_ARGS);
extern Datum bmgettuple(PG_FUNCTION_ARGS);
extern Datum bmgetmulti(PG_FUNCTION_ARGS);
extern Datum bmrescan(PG_FUNCTION_ARGS);
extern Datum bmendscan(PG_FUNCTION_ARGS);
extern Datum bmmarkpos(PG_FUNCTION_ARGS);
extern Datum bmrestrpos(PG_FUNCTION_ARGS);
extern Datum bmbulkdelete(PG_FUNCTION_ARGS);
extern Datum bmvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum bmgetbitmapwords(PG_FUNCTION_ARGS);

/* bitmappages.c */
extern Buffer _bitmap_getbuf(Relation rel, BlockNumber blkno, int access);
extern void _bitmap_wrtbuf(Buffer buf);
extern void _bitmap_relbuf(Buffer buf);
extern void _bitmap_wrtnorelbuf(Buffer buf);
extern void _bitmap_lovmetapageinit(Relation rel, Buffer buf);
extern void _bitmap_lovpageinit(Relation rel, Buffer buf);
extern void _bitmap_bitmappageinit(Relation rel, Buffer buf);
extern void _bitmap_init(Relation rel);

/* bitmapinsert.c */
extern void _bitmap_buildinsert
	(Relation rel, ItemPointerData ht_ctid, Datum* attdata, bool* nulls,
	 BMBuildState*	state);
extern void _bitmap_doinsert
	(Relation rel, ItemPointerData ht_ctid, Datum* attdata, bool* nulls);

/* bitmaputil.c */
extern BMLOVItem _bitmap_formitem(uint64 currTidNumber);
extern TupleDesc _bitmap_copyOneInTupleDesc
	(TupleDesc newTupDesc, TupleDesc oldTupDesc, uint32 attno);
extern void _bitmap_log_newpage
	(Relation rel, uint8 info, Buffer buf, BMMetaPage metapage);
extern void _bitmap_log_metapage(Relation rel, BMMetaPage metapage);
extern void _bitmap_log_bitmappage
	(Relation rel, Buffer bitmapBuffer, bool isOpaque);
extern void _bitmap_log_bitmap_lastwords
	(Relation rel, Buffer lovBuffer, OffsetNumber lovOffset, BMLOVItem lovItem);
extern void _bitmap_log_lovitem(Relation rel, Buffer lovBuffer, bool isNewItem,
								OffsetNumber offset, BMLOVItem lovItem);
extern void _bitmap_log_lovmetapage
	(Relation rel, Buffer lovMetaBuffer, uint8 numOfAttrs);

/* bitmapcompare.c */
extern Datum bmint2cmp(PG_FUNCTION_ARGS);
extern Datum bmint4cmp(PG_FUNCTION_ARGS);
extern Datum bmint8cmp(PG_FUNCTION_ARGS);
extern Datum bmint48cmp(PG_FUNCTION_ARGS);
extern Datum bmint84cmp(PG_FUNCTION_ARGS);
extern Datum bmint24cmp(PG_FUNCTION_ARGS);
extern Datum bmint42cmp(PG_FUNCTION_ARGS);
extern Datum bmint28cmp(PG_FUNCTION_ARGS);
extern Datum bmint82cmp(PG_FUNCTION_ARGS);
extern Datum bmboolcmp(PG_FUNCTION_ARGS);
extern Datum bmcharcmp(PG_FUNCTION_ARGS);
extern Datum bmoidcmp(PG_FUNCTION_ARGS);
extern Datum bmoidvectorcmp(PG_FUNCTION_ARGS);
extern Datum bmnamecmp(PG_FUNCTION_ARGS);
extern Datum bmname_pattern_cmp(PG_FUNCTION_ARGS);

/* bitmapsearch.c */
extern void _bitmap_searchinit(IndexScanDesc scan, ScanDirection dir);
extern bool _bitmap_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bitmap_next(IndexScanDesc scan, ScanDirection dir);
extern bool _bitmap_firstblockwords(IndexScanDesc scan, ScanDirection dir);
extern bool _bitmap_nextblockwords(IndexScanDesc scan, ScanDirection dir);
extern uint8 _bitmap_find_bitset(BM_HRL_WORD word, uint8 lastPos);

/* bitmapattutil.c */
extern void _bitmap_create_lov_heapandindex
	(Relation rel, int attNo, Oid *heapId, Oid *indexId);
extern void _bitmap_open_lov_heapandindex
	(Relation rel, Page lovMetapage, uint32 attNo,
	 Relation *lovHeapP, Relation *lovIndexP, LOCKMODE lockMode);
extern void _bitmap_insert_lov
	(Relation lovHeap, Relation lovIndex, Datum* datum, char* nulls);
extern void _bitmap_close_lov_heapandindex
	(Relation lovHeap, Relation lovIndex, LOCKMODE lockMode);
extern bool _bitmap_findvalue
	(Relation lovHeap, Relation lovIndex,
	 Datum datum, bool isnull, ScanKey scanKey, IndexScanDesc scanDesc,
	 BlockNumber *lovBlock, bool *blockNull,
	 OffsetNumber *lovOffset, bool *offsetNull);

/*
 * prototypes for functions in bitmapxlog.c
 */
extern void bitmap_redo(XLogRecPtr lsn, XLogRecord *record);
extern void bitmap_undo(XLogRecPtr lsn, XLogRecord *record);
extern void bitmap_desc(char *buf, uint8 xl_info, char *rec);

#endif
