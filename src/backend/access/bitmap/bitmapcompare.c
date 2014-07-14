/*-------------------------------------------------------------------------
 *
 * bitmapcompare.c
 *	  Comparison functions for bitmap access method.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 * NOTES
 *
 *	These functions are stored in pg_amproc.  For each operator class
 *	defined on bitmap, they compute
 *
 *				compare(a, b):
 *						< 0 if a < b,
 *						= 0 if a == b,
 *						> 0 if a > b.
 *
 *	The result is always an int32 regardless of the input datatype.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"


Datum
bmint2cmp(PG_FUNCTION_ARGS)
{
	int16       a = PG_GETARG_INT16(0);
	int16       b = PG_GETARG_INT16(1);

	PG_RETURN_INT32((int32) a - (int32) b);
}

Datum
bmint4cmp(PG_FUNCTION_ARGS)
{
	int32	res;
	int32	a = PG_GETARG_INT32(0);
	int32	b = PG_GETARG_INT32(1);

	if (a > b)
		res = 1;
	else if (a == b)
		res = 0;
	else
		res = -1;

	PG_RETURN_INT32(res);
}

Datum
bmint8cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmint48cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmint84cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmint24cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmint42cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int16		b = PG_GETARG_INT16(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmint28cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmint82cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int16		b = PG_GETARG_INT16(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmboolcmp(PG_FUNCTION_ARGS)
{
	bool		a = PG_GETARG_BOOL(0);
	bool		b = PG_GETARG_BOOL(1);

	PG_RETURN_INT32((int32) a - (int32) b);
}

Datum
bmcharcmp(PG_FUNCTION_ARGS)
{
	char		a = PG_GETARG_CHAR(0);
	char		b = PG_GETARG_CHAR(1);

	/* Be careful to compare chars as unsigned */
	PG_RETURN_INT32((int32) ((uint8) a) - (int32) ((uint8) b));
}

Datum
bmoidcmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
bmoidvectorcmp(PG_FUNCTION_ARGS)
{
	Oid		   *a = (Oid *) PG_GETARG_POINTER(0);
	Oid		   *b = (Oid *) PG_GETARG_POINTER(1);
	int			i;

	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		if (a[i] != b[i])
		{
			if (a[i] > b[i])
				PG_RETURN_INT32(1);
			else
				PG_RETURN_INT32(-1);
		}
	}
	PG_RETURN_INT32(0);
}

Datum
bmnamecmp(PG_FUNCTION_ARGS)
{
	Name		a = PG_GETARG_NAME(0);
	Name		b = PG_GETARG_NAME(1);

	PG_RETURN_INT32(strncmp(NameStr(*a), NameStr(*b), NAMEDATALEN));
}

Datum
bmname_pattern_cmp(PG_FUNCTION_ARGS)
{
	Name		a = PG_GETARG_NAME(0);
	Name		b = PG_GETARG_NAME(1);

	PG_RETURN_INT32(memcmp(NameStr(*a), NameStr(*b), NAMEDATALEN));
}
