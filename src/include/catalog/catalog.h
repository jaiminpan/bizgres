/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/catalog.h,v 1.34 2005/10/15 02:49:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "utils/rel.h"


extern char *relpath(RelFileNode rnode);
extern char *GetDatabasePath(Oid dbNode, Oid spcNode);

extern bool IsSystemRelation(Relation relation);
extern bool IsToastRelation(Relation relation);

extern bool IsSystemClass(Form_pg_class reltuple);
extern bool IsToastClass(Form_pg_class reltuple);

extern bool IsSystemNamespace(Oid namespaceId);
extern bool IsToastNamespace(Oid namespaceId);

extern bool IsReservedName(const char *name);

extern Oid	GetNewOid(Relation relation);
extern Oid	GetNewOidWithIndex(Relation relation, Relation indexrel);
extern Oid GetNewRelFileNode(Oid reltablespace, bool relisshared,
				  Relation pg_class);

#endif   /* CATALOG_H */
