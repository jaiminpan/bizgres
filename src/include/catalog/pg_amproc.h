/*-------------------------------------------------------------------------
 *
 * pg_amproc.h
 *	  definition of the system "amproc" relation (pg_amproc)
 *	  along with the relation's initial contents.
 *
 * The amproc table identifies support procedures associated with index
 * opclasses.  These procedures can't be listed in pg_amop since they are
 * not the implementation of any indexable operator for the opclass.
 *
 * The primary key for this table is <amopclaid, amprocsubtype, amprocnum>.
 * amprocsubtype is equal to zero for an opclass's "default" procedures.
 * Usually a nondefault amprocsubtype indicates a support procedure to be
 * used with operators having the same nondefault amopsubtype.	The exact
 * behavior depends on the index AM, however, and some don't pay attention
 * to subtype at all.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_amproc.h,v 1.54 2005/07/01 19:19:03 tgl Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMPROC_H
#define PG_AMPROC_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_amproc definition.  cpp turns this into
 *		typedef struct FormData_pg_amproc
 * ----------------
 */
#define AccessMethodProcedureRelationId  2603

CATALOG(pg_amproc,2603) BKI_WITHOUT_OIDS
{
	Oid			amopclaid;		/* the index opclass this entry is for */
	Oid			amprocsubtype;	/* procedure subtype, or zero if default */
	int2		amprocnum;		/* support procedure index */
	regproc		amproc;			/* OID of the proc */
} FormData_pg_amproc;

/* ----------------
 *		Form_pg_amproc corresponds to a pointer to a tuple with
 *		the format of pg_amproc relation.
 * ----------------
 */
typedef FormData_pg_amproc *Form_pg_amproc;

/* ----------------
 *		compiler constants for pg_amproc
 * ----------------
 */
#define Natts_pg_amproc					4
#define Anum_pg_amproc_amopclaid		1
#define Anum_pg_amproc_amprocsubtype	2
#define Anum_pg_amproc_amprocnum		3
#define Anum_pg_amproc_amproc			4

/* ----------------
 *		initial contents of pg_amproc
 * ----------------
 */

/* rtree */
DATA(insert (	 425	0 1 193 ));
DATA(insert (	 425	0 2 194 ));
DATA(insert (	 425	0 3 195 ));
DATA(insert (	1993	0 1 197 ));
DATA(insert (	1993	0 2 198 ));
DATA(insert (	1993	0 3 199 ));


/* btree */
DATA(insert (	 397	0 1 382 ));
DATA(insert (	 421	0 1 357 ));
DATA(insert (	 423	0 1 1596 ));
DATA(insert (	 424	0 1 1693 ));
DATA(insert (	 426	0 1 1078 ));
DATA(insert (	 428	0 1 1954 ));
DATA(insert (	 429	0 1 358 ));
DATA(insert (	 432	0 1 926 ));
DATA(insert (	 434	0 1 1092 ));
DATA(insert (	 434 1114 1 2344 ));
DATA(insert (	 434 1184 1 2357 ));
DATA(insert (	1970	0 1 354 ));
DATA(insert (	1970  701 1 2194 ));
DATA(insert (	1972	0 1 355 ));
DATA(insert (	1972  700 1 2195 ));
DATA(insert (	1974	0 1 926 ));
DATA(insert (	1976	0 1 350 ));
DATA(insert (	1976   23 1 2190 ));
DATA(insert (	1976   20 1 2192 ));
DATA(insert (	1978	0 1 351 ));
DATA(insert (	1978   20 1 2188 ));
DATA(insert (	1978   21 1 2191 ));
DATA(insert (	1980	0 1 842 ));
DATA(insert (	1980   23 1 2189 ));
DATA(insert (	1980   21 1 2193 ));
DATA(insert (	1982	0 1 1315 ));
DATA(insert (	1984	0 1 836 ));
DATA(insert (	1986	0 1 359 ));
DATA(insert (	1988	0 1 1769 ));
DATA(insert (	1989	0 1 356 ));
DATA(insert (	1991	0 1 404 ));
DATA(insert (	1994	0 1 360 ));
DATA(insert (	1996	0 1 1107 ));
DATA(insert (	1998	0 1 1314 ));
DATA(insert (	1998 1082 1 2383 ));
DATA(insert (	1998 1114 1 2533 ));
DATA(insert (	2000	0 1 1358 ));
DATA(insert (	2002	0 1 1672 ));
DATA(insert (	2003	0 1 360 ));
DATA(insert (	2039	0 1 2045 ));
DATA(insert (	2039 1082 1 2370 ));
DATA(insert (	2039 1184 1 2526 ));
DATA(insert (	2095	0 1 2166 ));
DATA(insert (	2096	0 1 2166 ));
DATA(insert (	2097	0 1 2180 ));
DATA(insert (	2098	0 1 2187 ));
DATA(insert (	2099	0 1  377 ));
DATA(insert (	2233	0 1  380 ));
DATA(insert (	2234	0 1  381 ));


/* hash */
DATA(insert (	 427	0 1 1080 ));
DATA(insert (	 431	0 1 454 ));
DATA(insert (	 433	0 1 422 ));
DATA(insert (	 435	0 1 450 ));
DATA(insert (	1971	0 1 451 ));
DATA(insert (	1973	0 1 452 ));
DATA(insert (	1975	0 1 422 ));
DATA(insert (	1977	0 1 449 ));
DATA(insert (	1979	0 1 450 ));
DATA(insert (	1981	0 1 949 ));
DATA(insert (	1983	0 1 1697 ));
DATA(insert (	1985	0 1 399 ));
DATA(insert (	1987	0 1 455 ));
DATA(insert (	1990	0 1 453 ));
DATA(insert (	1992	0 1 457 ));
DATA(insert (	1995	0 1 400 ));
DATA(insert (	1997	0 1 452 ));
DATA(insert (	1999	0 1 452 ));
DATA(insert (	2001	0 1 1696 ));
DATA(insert (	2004	0 1 400 ));
DATA(insert (	2040	0 1 452 ));
DATA(insert (	2222	0 1 454 ));
DATA(insert (	2223	0 1 456 ));
DATA(insert (	2224	0 1 398 ));
DATA(insert (	2225	0 1 450 ));
DATA(insert (	2226	0 1 450 ));
DATA(insert (	2227	0 1 450 ));
DATA(insert (	2228	0 1 450 ));
DATA(insert (	2229	0 1 456 ));
DATA(insert (	2230	0 1 456 ));
DATA(insert (	2231	0 1 456 ));
DATA(insert (	2232	0 1 455 ));
DATA(insert (	2235	0 1 329 ));


/* gist */
DATA(insert (	2593	0 1 2578 ));
DATA(insert (	2593	0 2 2583 ));
DATA(insert (	2593	0 3 2579 ));
DATA(insert (	2593	0 4 2580 ));
DATA(insert (	2593	0 5 2581 ));
DATA(insert (	2593	0 6 2582 ));
DATA(insert (	2593	0 7 2584 ));
DATA(insert (	2594	0 1 2585 ));
DATA(insert (	2594	0 2 2583 ));
DATA(insert (	2594	0 3 2586 ));
DATA(insert (	2594	0 4 2580 ));
DATA(insert (	2594	0 5 2581 ));
DATA(insert (	2594	0 6 2582 ));
DATA(insert (	2594	0 7 2584 ));
DATA(insert (	2595	0 1 2591 ));
DATA(insert (	2595	0 2 2583 ));
DATA(insert (	2595	0 3 2592 ));
DATA(insert (	2595	0 4 2580 ));
DATA(insert (	2595	0 5 2581 ));
DATA(insert (	2595	0 6 2582 ));
DATA(insert (	2595	0 7 2584 ));

/* the operator routines for the bitmap index */
DATA(insert (	2849	0 1 2833 ));	/* abstime */
DATA(insert (	2850	0 1 2834 ));	/* array */
DATA(insert (	2851	0 1 1596 ));	/* bit */
DATA(insert (	2852	0 1 2824 ));	/* bool */
DATA(insert (	2853	0 1 1078 ));	/* bpchar */
DATA(insert (	2854	0 1 1954 ));	/* bytea */
DATA(insert (	2855	0 1 2825 ));	/* char */
DATA(insert (	2856	0 1  926 ));	/* cidr */
DATA(insert (	2857	0 1 1092 ));	/* date */
DATA(insert (	2857 1114 1 2344 ));	/* date-timestamp */
DATA(insert (	2857 1184 1 2357 ));	/* date-timestamptz */
DATA(insert (	2858	0 1 2839 ));	/* float4 */
DATA(insert (	2858  701 1 2842 ));	/* float48 */
DATA(insert (	2859	0 1 2840 ));	/* float8 */
DATA(insert (	2859  700 1 2843 ));	/* float84 */
DATA(insert (	2860	0 1  926 ));	/* inet */
DATA(insert (	2861	0 1 2821 ));	/* int2 */
DATA(insert (	2861   23 1 2828 ));	/* int24 */
DATA(insert (	2861   20 1 2830 ));	/* int28 */
DATA(insert (	2862	0 1 2822 ));	/* int4 */
DATA(insert (	2862   20 1 2829 ));	/* int42 */
DATA(insert (	2862   21 1 2826 ));	/* int48 */
DATA(insert (	2863	0 1 2823 ));	/* int8 */
DATA(insert (	2863   21 1 2831 ));	/* int82 */
DATA(insert (	2863   23 1 2827 ));	/* int84 */
DATA(insert (	2864	0 1 1315 ));	/* interval */
DATA(insert (	2865	0 1  836 ));	/* macaddr */
DATA(insert (	2866	0 1 2837 ));	/* name */
DATA(insert (	2867	0 1 1769 ));	/* numeric */
DATA(insert (	2868	0 1 2835 ));	/* oid */
DATA(insert (	2869	0 1 2836 ));	/* oidvector */
DATA(insert (	2870	0 1 2832 ));	/* text */
DATA(insert (	2871	0 1 1107 ));	/* time */
DATA(insert (	2872	0 1 1314 ));	/* timestamptz */
DATA(insert (	2872 1082 1 2383 ));	/* timestamptz-date */
DATA(insert (	2872 1114 1 2533 ));	/* timestamptz-timestamp */
DATA(insert (	2873	0 1 1358 ));	/* timetz */
DATA(insert (	2874	0 1 1672 ));	/* varbit */
DATA(insert (	2875	0 1 2832 ));	/* varchar */
DATA(insert (	2876	0 1 2045 ));	/* timestamp */
DATA(insert (	2876 1082 1 2370 ));	/* timestamp-date */
DATA(insert (	2876 1184 1 2526 ));	/* timestamp-timestamptz */
DATA(insert (	2877	0 1 2837 ));	/* text pattern */
DATA(insert (	2878	0 1 2166 ));	/* varchar pattern */
DATA(insert (	2879	0 1 2847 ));	/* bpchar pattern */
DATA(insert (	2880	0 1 2838 ));	/* name pattern */
DATA(insert (	2881	0 1  377 ));	/* money */
DATA(insert (	2882	0 1 2844 ));	/* reltime */
DATA(insert (	2883	0 1 2845 ));	/* tinterval */

#endif   /* PG_AMPROC_H */
