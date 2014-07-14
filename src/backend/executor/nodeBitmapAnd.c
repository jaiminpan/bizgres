/*-------------------------------------------------------------------------
 *
 * nodeBitmapAnd.c
 *	  routines to handle BitmapAnd nodes.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeBitmapAnd.c,v 1.4 2005/10/15 02:49:17 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitBitmapAnd	- initialize the BitmapAnd node
 *		MultiExecBitmapAnd	- retrieve the result bitmap from the node
 *		ExecEndBitmapAnd	- shut down the BitmapAnd node
 *		ExecReScanBitmapAnd - rescan the BitmapAnd node
 *
 *	 NOTES
 *		BitmapAnd nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans,
 *		much like Append nodes.  The logic is much simpler than
 *		Append, however, since we needn't cope with forward/backward
 *		execution.
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/instrument.h"
#include "executor/nodeBitmapAnd.h"

/* ----------------------------------------------------------------
 *		ExecInitBitmapAnd
 *
 *		Begin all of the subscans of the BitmapAnd node.
 * ----------------------------------------------------------------
 */
BitmapAndState *
ExecInitBitmapAnd(BitmapAnd *node, EState *estate, int eflags)
{
	BitmapAndState *bitmapandstate = makeNode(BitmapAndState);
	PlanState **bitmapplanstates;
	int			nplans;
	int			i;
	ListCell   *l;
	Plan	   *initNode;
	bool		inmem = false;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	CXT1_printf("ExecInitBitmapAnd: context is %d\n", CurrentMemoryContext);

	/*
	 * Set up empty vector of subplan states
	 */
	nplans = list_length(node->bitmapplans);

	bitmapplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * create new BitmapAndState for our BitmapAnd node
	 */
	bitmapandstate->ps.plan = (Plan *) node;
	bitmapandstate->ps.state = estate;
	bitmapandstate->bitmapplans = bitmapplanstates;
	bitmapandstate->nplans = nplans;

	/*
	 * Miscellaneous initialization
	 *
	 * BitmapAnd plans don't have expression contexts because they never call
	 * ExecQual or ExecProject.  They don't need any tuple slots either.
	 */

#define BITMAPAND_NSLOTS 0

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "bitmapplanstates".
	 */
	i = 0;
	foreach(l, node->bitmapplans)
	{
		initNode = (Plan *) lfirst(l);
    bitmapplanstates[i] = ExecInitNode(initNode, estate, eflags);

		if (!inmem && IsA(initNode, BitmapIndexScan))
			inmem = (((BitmapIndexScan*)initNode)->indexam != 
					 BITMAP_AM_OID);
		else if (!inmem && IsA(initNode, BitmapAnd))
			inmem = ((BitmapAnd*)initNode)->inmem;
		else if (!inmem && IsA(initNode, BitmapOr))
			inmem = ((BitmapOr*)initNode)->inmem;
		i++;
	}

	node->inmem = inmem;

	bitmapandstate->odbms = (OnDiskBitmapWords**)
		palloc0(nplans * sizeof(OnDiskBitmapWords*));
	for (i=0; i<nplans; i++)
		bitmapandstate->odbms[i] = odbm_create(ODBM_MAX_WORDS);
	bitmapandstate->resultOdbm = NULL;

	return bitmapandstate;
}

int
ExecCountSlotsBitmapAnd(BitmapAnd *node)
{
	ListCell   *plan;
	int			nSlots = 0;

	foreach(plan, node->bitmapplans)
		nSlots += ExecCountSlotsNode((Plan *) lfirst(plan));
	return nSlots + BITMAPAND_NSLOTS;
}

/* ----------------------------------------------------------------
 *	   MultiExecBitmapAnd
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapAnd(BitmapAndState *node)
{
	PlanState **bitmapplans;
	int			nplans;
	int			i;
	Node		*result = NULL;
	bool		inmem = ((BitmapAnd*)(((PlanState*)node)->plan))->inmem;

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	/*
	 * get information from the node
	 */
	bitmapplans = node->bitmapplans;
	nplans = node->nplans;


	/*
	 * Scan all the subplans and AND their result bitmaps
 	*/
	for (i = 0; i < nplans; i++)
	{
		PlanState	*subnode = bitmapplans[i];

		/* set the required bitmap type for the subnodes */
		odbm_set_bitmaptype(subnode->plan, inmem);

		if (inmem) 
		{
			TIDBitmap *subresult;

			subresult = (TIDBitmap *) MultiExecProcNode(subnode);

			if (!subresult || !IsA(subresult, TIDBitmap))
				elog(ERROR, "unrecognized result from subplan");

			if (result == NULL)
				result = (Node*)subresult;			/* first subplan */
			else
			{
				tbm_intersect((TIDBitmap*)result, subresult);
				tbm_free(subresult);
			}
		}

		else
		{
			OnDiskBitmapWords* subresult;

			/* if there is no leftover from previous scan, then
				read next list of words. */
			if ((node->odbms[i])->numOfWords == 0)
			{
				node->odbms[i]->startNo = 0;
				odbm_set_child_resultnode(subnode, node->odbms[i]);

				subresult = (OnDiskBitmapWords*)MultiExecProcNode(subnode);

				if (!subresult || !IsA(subresult, OnDiskBitmapWords))
					elog(ERROR, "unrecognized result from subplan");

				node->odbms[i] = subresult;
			}
		}
	}

	if (!inmem)
	{
		if (node->resultOdbm == NULL)
			node->resultOdbm = odbm_create(ODBM_MAX_WORDS);
		odbm_reset(node->resultOdbm);
		odbm_intersect(node->odbms, nplans, node->resultOdbm);
		result = (Node*)(node->resultOdbm);
	}

	if (result == NULL)
		elog(ERROR, "BitmapAnd doesn't support zero inputs");

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStopNodeMulti(node->ps.instrument, 0 /* XXX */);

	return (Node *) result;
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapAnd
 *
 *		Shuts down the subscans of the BitmapAnd node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapAnd(BitmapAndState *node)
{
	PlanState **bitmapplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	bitmapplans = node->bitmapplans;
	nplans = node->nplans;

	/*
	 * shut down each of the subscans (that we've initialized)
	 */
	for (i = 0; i < nplans; i++)
	{
		if (bitmapplans[i])
			ExecEndNode(bitmapplans[i]);
	}

	if (node->odbms != NULL)
	{
		for (i=0; i<nplans; i++)
			if (node->odbms[i] != NULL)
				odbm_free(node->odbms[i]);
		pfree(node->odbms);
	}
}

void
ExecReScanBitmapAnd(BitmapAndState *node, ExprContext *exprCtxt)
{
	int			i;

	for (i = 0; i < node->nplans; i++)
	{
		PlanState  *subnode = node->bitmapplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		if (node->odbms[i] != NULL)
		{
			node->odbms[i]->startNo = 0;
			node->odbms[i]->numOfWords = 0;
		}

		/*
		 * Always rescan the inputs immediately, to ensure we can pass down
		 * any outer tuple that might be used in index quals.
		 */
		ExecReScan(subnode, exprCtxt);
	}
}
