/*-------------------------------------------------------------------------
 *
 * bitmapattutil.c
 *	Defines the routines to maintain all attribute values which are
 *	indexed in the bitmap index.
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
#include "access/nbtree.h"
#include "nodes/execnodes.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "access/heapam.h"
#include "optimizer/clauses.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"

TupleDesc _bitmap_create_lov_heapTupleDesc(Relation rel, int attNo);

/*
 * _bitmap_create_lov_heapandindex() -- create a new heap relation and
 *	a btree index for LOV.
 */
void
_bitmap_create_lov_heapandindex(Relation rel,
								int attNo,
								Oid *lovHeapId, Oid *lovIndexId)
{
	char		lovHeapName[NAMEDATALEN];
	char		lovIndexName[NAMEDATALEN];
	TupleDesc	tupDesc;
	IndexInfo  *indexInfo;
	ObjectAddress	objAddr, referenced;
	char*		accessMethodName = "btree";
	Oid			accessMethodId = BTREE_AM_OID;
	Oid*		classObjectId;

	/* create a new empty heap to store all attribute values with their
	   corresponding block number and offset in LOV. */
	snprintf(lovHeapName, sizeof(lovHeapName), 
			 "bm_internal_t%u%s_%d", RelationGetNamespace(rel), 
			 RelationGetRelationName(rel), attNo);

	tupDesc = _bitmap_create_lov_heapTupleDesc(rel, attNo);

  ereport(DEBUG1, (errmsg_internal("into --> heap_create_with_catalog")));
	*lovHeapId = 
		heap_create_with_catalog(lovHeapName, RelationGetNamespace(rel),
					rel->rd_rel->reltablespace, InvalidOid, rel->rd_rel->relowner, tupDesc, RELKIND_RELATION,
					rel->rd_rel->relisshared, false, 1, ONCOMMIT_NOOP, false);

  ereport(DEBUG1, (errmsg_internal("done --> heap_create_with_catalog")));
	/* rel->rd_rel->reltablespace, InvalidOid,rel->rd_rel->relowner, tupDesc, RELKIND_RELATION, */

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	objAddr.classId = RelationRelationId;
	objAddr.objectId = *lovHeapId;
	objAddr.objectSubId = 0 ;

	referenced.classId = RelationRelationId;
	referenced.objectId = RelationGetRelid(rel);
	referenced.objectSubId = 0;

	recordDependencyOn(&objAddr, &referenced, DEPENDENCY_INTERNAL);

	/* create a new index on the newly-created heap. 
	   The key is the attribute value. */
	snprintf(lovIndexName, sizeof(lovIndexName), 
			 "bm_internal_i%u%s_%d", RelationGetNamespace(rel), 
			 RelationGetRelationName(rel), attNo);

	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = 1;
	indexInfo->ii_Expressions = NIL;
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = make_ands_implicit(NULL);
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_Unique = true;

	indexInfo->ii_KeyAttrNumbers[0] = 1;
	
	classObjectId = (Oid*)palloc(1*sizeof(Oid));
	classObjectId[0] = 
		GetDefaultOpClass(tupDesc->attrs[0]->atttypid, 
						  accessMethodId);

  ereport(DEBUG1, (errmsg_internal("into --> index_create")));
	*lovIndexId = 
		index_create(*lovHeapId, lovIndexName, InvalidOid,
					 indexInfo, accessMethodId, rel->rd_rel->reltablespace, 
					 classObjectId, false, false, false, false);


	objAddr.classId = RelationRelationId;
	objAddr.objectId = *lovIndexId;
	objAddr.objectSubId = 0 ;

	recordDependencyOn(&objAddr, &referenced, DEPENDENCY_INTERNAL);
  ereport(DEBUG1, (errmsg_internal("done --> index_create")));
}

/*
 * _bitmap_create_lov_heapTupleDesc() -- create the new heap tuple descriptor.
 */
TupleDesc
_bitmap_create_lov_heapTupleDesc(Relation rel, int attNo)
{
	TupleDesc	tupDesc;
	TupleDesc	oldTupDesc;
	AttrNumber	attnum;

	const int	numOfAttrs = 3;

	oldTupDesc = RelationGetDescr(rel);

	tupDesc = CreateTemplateTupleDesc(numOfAttrs, false);

	tupDesc->attrs[0] = 
			(Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);

	attnum = 1;

	/* cope the attribute to be indexed. */
	memcpy(tupDesc->attrs[0], oldTupDesc->attrs[attNo], 
		   ATTRIBUTE_TUPLE_SIZE);
	(tupDesc->attrs[0])->attnum = attnum;

	attnum++;

	/* the block number */
	TupleDescInitEntry(tupDesc, attnum, "blockNumber", INT4OID, -1, 0);
	
	attnum++;

	/* the offset number */
	TupleDescInitEntry(tupDesc,attnum, "offsetNumber", INT4OID, -1, 0);

	return tupDesc;
}

/*
 * _bitmap_open_lov_heapandindex() -- open the heap relation and the btree
 *		index for LOV.
 */
void
_bitmap_open_lov_heapandindex(Relation rel, Page lovMetapage, uint32 attNo,
							  Relation *lovHeapP, Relation *lovIndexP,
							  LOCKMODE lockMode)
{
	BMLOVMetaItem	metaItems;

	metaItems = (BMLOVMetaItem) PageGetContents(lovMetapage);

	*lovHeapP = heap_open(metaItems[attNo].bm_lov_heapId, lockMode);
	*lovIndexP = index_open(metaItems[attNo].bm_lov_indexId);
}

/*
 * _bitmap_insert_lov() -- insert a new tuple into the heap and index.
 */
void
_bitmap_insert_lov(Relation lovHeap, Relation lovIndex,
				   Datum* datum, char* nulls)
{
	TupleDesc	tupDesc;
	HeapTuple	tuple;
	Datum		indexDatum[1];
	bool		indexNulls[1];
	InsertIndexResult result;

	tupDesc = RelationGetDescr(lovHeap);

	/* insert this tuple into the heap */
	tuple = heap_formtuple(tupDesc, datum, nulls);
	simple_heap_insert(lovHeap, tuple);

	/* insert a new tuple into the index */
	indexDatum[0] = datum[0];
	if (nulls[0] == 'n')
		indexNulls[0] = true;
	else
		indexNulls[0] = false;

	result = 
		index_insert(lovIndex, indexDatum, indexNulls, 
					 &(tuple->t_self), lovHeap, true);

	Assert(result);

	heap_freetuple(tuple);

}


/*
 * _bitmap_close_lov_heapandindex() -- close the heap and the index.
 */
void
_bitmap_close_lov_heapandindex
	(Relation lovHeap, Relation lovIndex, LOCKMODE lockMode)
{
	heap_close(lovHeap, lockMode);
	index_close(lovIndex);
}

/*
 * _bitmap_findvalue() -- find the given value in the heap.
 *		If this value exists, return true. Otherwise return false.
 *
 * The block number and the offset of this value in LOV are also returned.
 */
bool
_bitmap_findvalue(Relation lovHeap, Relation lovIndex, Datum datum, bool isnull,
				  ScanKey scanKey, IndexScanDesc scanDesc,
				  BlockNumber *lovBlock, bool *blockNull,
				  OffsetNumber *lovOffset, bool *offsetNull)
{
	TupleDesc		tupDesc;
	HeapTuple		tuple;
	bool			found = false;

	tupDesc = RelationGetDescr(lovIndex);

	if (isnull)
	{
		scanKey->sk_flags = SK_ISNULL;
		scanKey->sk_argument = datum;
	}
	else
	{
		scanKey->sk_flags = 0;
		scanKey->sk_argument = datum;
	}

	index_rescan(scanDesc, scanKey);

	tuple = index_getnext(scanDesc, ForwardScanDirection);

	if (tuple != NULL)
	{
		TupleDesc heapTupDesc;

		found = true;

		heapTupDesc = RelationGetDescr(lovHeap);

		*lovBlock = DatumGetInt32(heap_getattr(tuple, 2, heapTupDesc, blockNull));
		*lovOffset = DatumGetInt16(heap_getattr(tuple, 3, heapTupDesc, offsetNull));
	}

	return found;
}

