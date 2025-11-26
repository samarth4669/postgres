/*-------------------------------------------------------------------------
 *
 * nodeForeignscan.c
 *	  Routines to support scans of foreign tables
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeForeignscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecForeignScan			scans a foreign table.
 *		ExecInitForeignScan		creates and initializes state info.
 *		ExecReScanForeignScan	rescans the foreign relation.
 *		ExecEndForeignScan		releases any resources allocated.
 */
#include "postgres.h"
#include "executor/spi.h"

#include "executor/executor.h"
#include "executor/nodeForeignscan.h"
#include "foreign/fdwapi.h"
#include "utils/memutils.h"
#include "utils/rel.h"


#include "access/table.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/heap.h"
#include "catalog/dependency.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/acl.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "catalog/pg_am_d.h"

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "parser/parse_oper.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_constraint.h"
#include "utils/syscache.h"
#include "catalog/pg_index.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "utils/syscache.h"
#include "nodes/params.h"


#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "executor/executor.h"
#include "utils/builtins.h"

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* Recursive logger for WHERE clause */
#include "postgres.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "nodes/parsenodes.h"
#include "parser/parsetree.h"
#include "catalog/pg_constraint.h"
#include "catalog/indexing.h"
#include "access/htup_details.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"   /* for getTypeOutputInfo() */
#include "utils/builtins.h"    /* for OidOutputFunctionCall() */
#include "nodes/makefuncs.h"
#include "catalog/pg_type.h"   /* for type OIDs */
#include "utils/array.h"   /* For ARR_ELEMTYPE, ARR_DIMS, etc. */
#include "catalog/namespace.h"
#include "access/htup_details.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "nodes/pg_list.h"
#include "utils/foreign_cache.h"


static Oid cache_relid_global = InvalidOid;

typedef struct KeyLookupInfo
{
    AttrNumber key_attnum;  /* which column */
    Const *key_const;       /* constant value for lookup */
} KeyLookupInfo;

/* Helper: recursively inspect quals */

static void print_where_clause(Node *node, List *rtable)
{
    if (node == NULL)
        return;

    /* Case 1: The WHERE clause can be a List (e.g., multiple AND conditions) */
    if (IsA(node, List))
    {
        List *clauses = (List *) node;
        ListCell *lc;

        foreach(lc, clauses)
        {
            print_where_clause((Node *) lfirst(lc), rtable);
        }
        return;
    }

    /* Case 2: Handle operator expressions like id = 2 */
    if (IsA(node, OpExpr))
    {
        OpExpr *op = (OpExpr *) node;
        ListCell *lc;

        elog(LOG, "Found operator expression with %d args", list_length(op->args));

        foreach(lc, op->args)
        {
            Expr *arg = (Expr *) lfirst(lc);

            if (IsA(arg, Var))
            {
                Var *var = (Var *) arg;
                RangeTblEntry *rte = rt_fetch(var->varno, rtable);

                if (rte && rte->rtekind == RTE_RELATION)
                {
                    const char *relname = get_rel_name(rte->relid);
                    const char *attname = get_attname(rte->relid, var->varattno, false);
                    elog(LOG, "Column: %s.%s", relname, attname);
                }
                else
                {
                    elog(LOG, "Unknown Var node: varno=%d attno=%d", var->varno, var->varattno);
                }
            }
            else if (IsA(arg, Const))
            {
                Const *c = (Const *) arg;

                if (!c->constisnull)
                {
                    Oid typoutput;
                    bool typisvarlena;
                    getTypeOutputInfo(c->consttype, &typoutput, &typisvarlena);
                    char *valstr = OidOutputFunctionCall(typoutput, c->constvalue);
                    elog(LOG, "Constant value: %s (type OID: %u)", valstr, c->consttype);
                }
                else
                {
                    elog(LOG, "Constant is NULL");
                }
            }
            else
            {
                elog(LOG, "Unhandled argument node type: %d", nodeTag(arg));
            }
        }
    }

    /* Case 3: Handle boolean expressions (AND/OR/NOT) */
    else if (IsA(node, BoolExpr))
    {
        BoolExpr *bexpr = (BoolExpr *) node;
        char *typename;

        switch (bexpr->boolop)
        {
            case AND_EXPR:
                typename = "AND";
                break;
            case OR_EXPR:
                typename = "OR";
                break;
            case NOT_EXPR:
                typename = "NOT";
                break;
            default:
                typename = "UNKNOWN_BOOL";
                break;
        }

        elog(LOG, "Boolean expression type: %s with %d args",
             typename, list_length(bexpr->args));

        ListCell *lc;
        foreach(lc, bexpr->args)
        {
            print_where_clause((Node *) lfirst(lc), rtable);
        }
    }
  
	else if (IsA(node, ScalarArrayOpExpr))
    {
        ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
        elog(LOG, "Found ScalarArrayOpExpr (IN/ALL) operator");
    }
	else
    {
        elog(LOG, "Unhandled node type in WHERE clause: %d", nodeTag(node));
    }
}



/*
 * Returns the cache table name for a given executor state and logs it.
 * estate: ExecutorState containing the original query.
 * Returns a static buffer with the cache table name.
 */
static const char *
get_and_log_cache_table_name(EState *estate)
{
    static char cache_table_name[NAMEDATALEN];  // static so pointer is valid after return
    const char *foreign_table_name = NULL;

    if (estate->origQuery->rtable)
    {
        RangeTblEntry *rte = linitial_node(RangeTblEntry, estate->origQuery->rtable);
        if (rte->rtekind == RTE_RELATION)
        {
            foreign_table_name = get_rel_name(rte->relid);
			

            elog(LOG, "Foreign table: %s",foreign_table_name);
        }
    }

    if (foreign_table_name)
    {
        snprintf(cache_table_name, NAMEDATALEN, "%s_cache", foreign_table_name);
        elog(LOG, "Foreign table: %s, Cache table: %s", foreign_table_name, cache_table_name);
    }
    else
    {
        snprintf(cache_table_name, NAMEDATALEN, "unknown_cache");
        elog(LOG, "Foreign table not found, using cache table: %s", cache_table_name);
    }

    return cache_table_name;
}


static const char *
get_and_log_cache_table_space(EState *estate)
{
    
    const char *schema_name = NULL;

    if (estate->origQuery->rtable)
    {
        RangeTblEntry *rte = linitial_node(RangeTblEntry, estate->origQuery->rtable);
        if (rte->rtekind == RTE_RELATION)
        {
			Oid schema_oid = get_rel_namespace(rte->relid);
            schema_name = get_namespace_name(schema_oid);

            elog(LOG, "Foreign table_schema: %s", schema_name);
        }
    }

    
    
    return schema_name;
}


static bool
check_simple_equality(Node *node, List *rtable)
{
    if (node == NULL)
        return false;  /* empty WHERE is trivially simple */

    /* Case 1: List of expressions (usually from AND conditions) */
    if (IsA(node, List))
    {
        List *clauses = (List *) node;
        ListCell *lc;

        foreach(lc, clauses)
        {
            if (!check_simple_equality((Node *) lfirst(lc), rtable))
                return false;
        }
        return true;
    }

    /* Case 2: Operator expression (attr = val) */
    if (IsA(node, OpExpr))
    {
        OpExpr *op = (OpExpr *) node;

        /* Only allow "=" operator */
        char *opname = get_opname(op->opno);
        if (opname == NULL || strcmp(opname, "=") != 0)
            return false;

        /* Must have exactly 2 arguments */
        if (list_length(op->args) != 2)
            return false;

        ListCell *lc;
        foreach(lc, op->args)
        {
            Expr *arg = (Expr *) lfirst(lc);
            if (!(IsA(arg, Var) || IsA(arg, Const)))
                return false;  /* only allow Var = Const */
        }

        return true;
    }

    /* Case 3: Boolean expressions (AND only) */
    if (IsA(node, BoolExpr))
    {
        BoolExpr *bexpr = (BoolExpr *) node;

        /* Only allow AND, not OR/NOT */
        if (bexpr->boolop != AND_EXPR)
            return false;

        ListCell *lc;
        foreach(lc, bexpr->args)
        {
            if (!check_simple_equality((Node *) lfirst(lc), rtable))
                return false;
        }
        return true;
    }

    /* Any other node types are not allowed */
    return false;
}

int check_where_clause_exist(Query *query){

	FromExpr *fromExpr = (FromExpr *) query->jointree;
    if (fromExpr && fromExpr->quals)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void
inspect_query(Query *query)
{
    if (query->commandType == CMD_SELECT)
    {
        elog(LOG, "Query Type: SELECT");

        /* get FROM clause (range table) */
        if (query->rtable)
        {
            RangeTblEntry *rte = linitial_node(RangeTblEntry, query->rtable);
            if (rte->rtekind == RTE_RELATION)
            {
                char *relname = get_rel_name(rte->relid);
                elog(LOG, "Target relation: %s", relname);
            }
        }

        /* check WHERE clause */
        FromExpr *fromExpr = (FromExpr *) query->jointree;
        if (fromExpr && fromExpr->quals)
        {
            elog(LOG, "Parsing WHERE clause...");
            print_where_clause(fromExpr->quals, query->rtable);

        }
        else
        {
            elog(LOG, "No WHERE clause found.");
        }
    }
    else
    {
        elog(LOG, "Non-SELECT query (type = %d)", query->commandType);
    }
}


#include "postgres.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "utils/syscache.h"
#include "utils/elog.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

/*
 * get_primary_key_attnums
 *   Given a table OID, returns an array of AttrNumber corresponding to
 *   the primary key columns.
 */
#include "utils/syscache.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_constraint.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "nodes/pg_list.h"
#include "access/genam.h"
#include "catalog/pg_operator.h"
#include "utils/fmgroids.h"


/*
 * get_primary_key_attnums
 *   Returns a List of AttrNumber (int16) representing the columns
 *   that form the primary key of the table identified by relid.
 *   Returns NIL if no primary key exists.
 */
#include "nodes/pg_list.h"
#include "catalog/pg_constraint.h"
#include "utils/elog.h"
#include "nodes/bitmapset.h"

/* Returns a List* of AttrNumber for the primary key columns of a table */
Bitmapset *
get_primary_key_attnums(Oid relid)
{
    Oid constraintOid;
    Bitmapset *pk_attnos;

    /* Fetch primary key attribute numbers as a Bitmapset */
    pk_attnos = get_primary_key_attnos(relid, false, &constraintOid);

    if (pk_attnos == NULL)
        elog(LOG, "No primary key found for relation %u", relid);
    else
        elog(LOG, "Primary key found for relation %u", relid);

    return pk_attnos;  /* caller is responsible for freeing with bms_free() if needed */
}
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"
#include "nodes/nodes.h"

/* Extract the constant from a simple equality operator expression, e.g., WHERE col = 123 */

KeyLookupInfo *find_query(Node *node, List *rtable)
{
    if (node == NULL)
        return NULL;

    /* Case 1: Multiple AND conditions */
    if (IsA(node, List))
    {
        List *clauses = (List *) node;
        ListCell *lc;

        foreach(lc, clauses)
        {
            KeyLookupInfo *info = find_query((Node *) lfirst(lc), rtable);
            if (info)  /* return first match found */
                return info;
        }
        return NULL;
    }

    /* Case 2: Simple operator expressions (id = 2) */
    if (IsA(node, OpExpr))
    {
        OpExpr *op = (OpExpr *) node;

        if (list_length(op->args) == 2)
        {
            Node *lhs = linitial(op->args);
            Node *rhs = lsecond(op->args);

            if (IsA(lhs, Var) && IsA(rhs, Const))
            {
                Var *var = (Var *) lhs;
                Const *c = (Const *) rhs;

                RangeTblEntry *rte = rt_fetch(var->varno, rtable);
                if (rte && rte->rtekind == RTE_RELATION)
                {
                    const char *relname = get_rel_name(rte->relid);
                    const char *attname = get_attname(rte->relid, var->varattno, false);
                    elog(LOG, "Single-key lookup detected: %s.%s", relname, attname);

                    /* Allocate return structure */
                    KeyLookupInfo *info = palloc(sizeof(KeyLookupInfo));
                    info->key_attnum = var->varattno;
                    info->key_const = (Const *) palloc(sizeof(Const));
                    memcpy(info->key_const, c, sizeof(Const));

                    /* Log for debugging */
                    Oid typoutput;
                    bool typisvarlena;
                    getTypeOutputInfo(c->consttype, &typoutput, &typisvarlena);
                    char *valstr = OidOutputFunctionCall(typoutput, c->constvalue);
                    elog(LOG, " -> key_attnum: %d, const value: %s", info->key_attnum, valstr);

                    return info;
                }
            }
        }
        return NULL;
    }

    /* Case 3: Boolean expressions (AND/OR/NOT) */
    if (IsA(node, BoolExpr))
    {
        BoolExpr *bexpr = (BoolExpr *) node;
        ListCell *lc;
        foreach(lc, bexpr->args)
        {
            KeyLookupInfo *info = find_query((Node *) lfirst(lc), rtable);
            if (info)
                return info;
        }
        return NULL;
    }

    /* Case 4: ScalarArrayOpExpr (e.g., id IN (...)) */
    if (IsA(node, ScalarArrayOpExpr))
    {
        elog(LOG, "IN/ALL clause detected - ignoring for single-key lookup");
        return NULL;
    }

    return NULL;  /* fallback */
}


/* file: nodeForeignScan.c  OR foreign_cache.c */

/* Prototype (put near top of file, or in foreign_cache.h if used externally) */
static HeapTuple lookup_tuple_in_cache(Oid cache_rel_oid, AttrNumber attnum, Const *key_const);

/* Implementation */
static HeapTuple
lookup_tuple_in_cache(Oid cache_rel_oid, AttrNumber attnum, Const *key_const)
{
    Relation    rel;
    TupleDesc   tupdesc;
    TableScanDesc scan;
    HeapTuple   tuple;
    HeapTuple   result = NULL;

    Oid         key_type;
    Datum       key_val;
    bool        key_isnull;

    Oid typoutput;
    bool typisvarlena;
    char *key_str = NULL;

    /* Defensive checks */
    if (!OidIsValid(cache_rel_oid) || attnum <= 0 || key_const == NULL)
        return NULL;

    key_type = key_const->consttype;
    key_val  = key_const->constvalue;
    key_isnull = key_const->constisnull;

    if (key_isnull)
        return NULL;

    /* open relation and begin scan */
    rel = table_open(cache_rel_oid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    scan = table_beginscan_catalog(rel, 0, NULL);
    if (!scan)
    {
        table_close(rel, AccessShareLock);
        elog(ERROR, "lookup_tuple_in_cache: failed to begin scan on cache table");
    }

    /* optional human-readable key for logs */
    getTypeOutputInfo(key_type, &typoutput, &typisvarlena);
    key_str = OidOutputFunctionCall(typoutput, key_val);

    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        Datum val;
        bool isnull;
        bool match = false;

        val = heap_getattr(tuple, attnum, tupdesc, &isnull);
        if (isnull)
            continue;

        switch (key_type)
        {
            case INT4OID:
            {
                int32 tuple_val = DatumGetInt32(val);
                int32 const_val = DatumGetInt32(key_val);
                match = (tuple_val == const_val);
                break;
            }

            case TEXTOID:
            {
                text *tuple_text = DatumGetTextPP(val);
                text *const_text = DatumGetTextPP(key_val);
                match = (strcmp(text_to_cstring(tuple_text),
                                text_to_cstring(const_text)) == 0);
                break;
            }

            default:
                elog(DEBUG1, "lookup_tuple_in_cache: unsupported key type: %u", key_type);
                break;
        }

        if (match)
        {
            /* copy tuple so it outlives the scan's buffer */
            result = heap_copytuple(tuple);
            elog(INFO, "Cache hit for key value: %s", key_str);
            break;
        }
    }

    if (key_str)
        pfree(key_str);

    heap_endscan(scan);
    table_close(rel, AccessShareLock);

    return result; /* caller must store/free it */
}



#include "postgres.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "catalog/pg_operator.h"
#include "utils/typcache.h"





static void
cache_insert_tuple(TupleTableSlot *slot, ForeignScanState *node)
{
    HeapTuple tuple;
    Relation cache_rel;
    Oid cache_relid;
    Oid nspoid;
    const char *relname;
    char cache_name[NAMEDATALEN];

    /* Get foreign relation info */
    relname = RelationGetRelationName(node->ss.ss_currentRelation);
    nspoid = RelationGetNamespace(node->ss.ss_currentRelation);

    /* Generate cache table name */
    snprintf(cache_name, sizeof(cache_name), "%s_cache", relname);

    /* Find cache table OID */
    cache_relid = get_relname_relid(cache_name, nspoid);
    if (!OidIsValid(cache_relid))
    {
        elog(WARNING, "Cache table %s not found, skipping insert", cache_name);
        return;
    }
     /* Look up / create per-cache-table entry in shared hash */
    CacheEntry *entry = GetCacheEntryForCacheRel(cache_relid);

    cache_rel = table_open(cache_relid, RowExclusiveLock);

	
    TableScanDesc scan;
    int current = entry->count;

	int rowcount = 0;
    elog(LOG, "tuple%d",current);

    if (current >=3 )
    {
        const char *schema_name   = get_namespace_name(RelationGetNamespace(cache_rel));
        const char *cache_relname = RelationGetRelationName(cache_rel);
        char        cmd[512];
        int         excess = current - 3 + 1;

        elog(LOG, "cache_insert_tuple: cache %s.%s over limit (%d >= %d), deleting %d row(s)",
             schema_name, cache_relname, current,10, excess);

        snprintf(cmd, sizeof(cmd),
                 "DELETE FROM %s.%s WHERE ctid IN "
                 "(SELECT ctid FROM %s.%s ORDER BY ctid ASC LIMIT %d)",
                 schema_name, cache_relname,
                 schema_name, cache_relname,
                 excess);

        if (SPI_connect() != SPI_OK_CONNECT)
            elog(ERROR, "cache_insert_tuple: SPI_connect failed");

        int spi_rc = SPI_exec(cmd, 0);
        if (spi_rc != SPI_OK_DELETE)
            elog(WARNING, "cache_insert_tuple: SPI_exec(delete) returned %d", spi_rc);

        SPI_finish();

        /*
         * Decrement our counter approximately. If other backends are also
         * inserting/deleting, this is approximate, but fine for cache control.
         */
        entry->count -= excess;
        if (entry->count < 0)
            entry->count = 0;
    }

    /*
     * Step 2: Insert the new tuple into the cache table.
     */
    ExecMaterializeSlot(slot);
    tuple = ExecCopySlotHeapTuple(slot);

    simple_heap_insert(cache_rel, tuple);

    /* Update approximate count */
    entry->count++;

    elog(LOG, "cache_insert_tuple: cache %s now has approx %d row(s)",
         cache_name, entry->count);

    table_close(cache_rel, RowExclusiveLock);
}


static TupleTableSlot *
ForeignNext(ForeignScanState *node)
{
	TupleTableSlot *slot;
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	MemoryContext oldcontext;

	/* Call the Iterate function in short-lived context */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	if (plan->operation != CMD_SELECT)
	{
		/*
		 * direct modifications cannot be re-evaluated, so shouldn't get here
		 * during EvalPlanQual processing
		 */
		Assert(node->ss.ps.state->es_epq_active == NULL);

		slot = node->fdwroutine->IterateDirectModify(node);
	}
	else
		slot = node->fdwroutine->IterateForeignScan(node);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Insert valid value into tableoid, the only actually-useful system
	 * column.
	 */
	if (plan->fsSystemCol && !TupIsNull(slot))
		slot->tts_tableOid = RelationGetRelid(node->ss.ss_currentRelation);
    
	if (!TupIsNull(slot))
        cache_insert_tuple(slot, node);
	return slot;
}

/*
 * ForeignRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot)
{
	FdwRoutine *fdwroutine = node->fdwroutine;
	ExprContext *econtext;

	/*
	 * extract necessary information from foreign scan node
	 */
	econtext = node->ss.ps.ps_ExprContext;

	/* Does the tuple meet the remote qual condition? */
	econtext->ecxt_scantuple = slot;

	ResetExprContext(econtext);

	/*
	 * If an outer join is pushed down, RecheckForeignScan may need to store a
	 * different tuple in the slot, because a different set of columns may go
	 * to NULL upon recheck.  Otherwise, it shouldn't need to change the slot
	 * contents, just return true or false to indicate whether the quals still
	 * pass.  For simple cases, setting fdw_recheck_quals may be easier than
	 * providing this callback.
	 */
	if (fdwroutine->RecheckForeignScan &&
		!fdwroutine->RecheckForeignScan(node, slot))
		return false;

	return ExecQual(node->fdw_recheck_quals, econtext);
}

/* ----------------------------------------------------------------
 *		ExecForeignScan(node)
 *
 *		Fetches the next tuple from the FDW, checks local quals, and
 *		returns it.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */

static TupleTableSlot *
ExecForeignScan(PlanState *pstate)
{
    ForeignScanState *node = castNode(ForeignScanState, pstate);
    EState *estate = node->ss.ps.state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    /* Detect if simple equality lookup */
    if (estate->origQuery && estate->origQuery->commandType == CMD_SELECT)
    {

		
        FromExpr *fromExpr = (FromExpr *) estate->origQuery->jointree;
        if (check_simple_equality(fromExpr->quals, estate->origQuery->rtable))
        {   if (node->f_state.cache_returned)
            {
                elog(LOG, "Cache tuple already returned, ending scan.");
                ExecClearTuple(slot);
                return slot;
            }
            elog(LOG, "Performing cache table lookup");

            /* Hardcoded test: replace with parsed quals later */
            Const *key_const = palloc(sizeof(Const));
            key_const->consttype   = INT4OID;
            key_const->consttypmod = -1;
            key_const->constcollid = InvalidOid;
            key_const->constlen    = sizeof(int32);
            key_const->constisnull = false;
            key_const->constvalue  = Int32GetDatum(2);

            AttrNumber key_attnum = 1;  /* example: 4th column */
            Oid typid = INT4OID;
            Oid eq_op = 96; /* int4 = int4 operator */
            KeyLookupInfo *info = find_query(fromExpr->quals, estate->origQuery->rtable);
            key_attnum = info->key_attnum;
            if (info && info->key_const)
			{
				Const *c = info->key_const;
				Oid typoutput;
				bool typisvarlena;

				getTypeOutputInfo(c->consttype, &typoutput, &typisvarlena);
				char *valstr = OidOutputFunctionCall(typoutput, c->constvalue);

				elog(LOG, "Key column attnum = %d", info->key_attnum);
				elog(LOG, "Key column type = %u", c->consttype);
				elog(LOG, "Key column value = %s", valstr);

				key_attnum = info->key_attnum;
                key_const = info->key_const;

			}
            elog(LOG, "Looking up in cache table oid=%u, attnum=%d, constvalue=%lu",
                cache_relid_global,
                key_attnum,
                (unsigned long) DatumGetUInt32(key_const->constvalue));

            
            TupleTableSlot *cache_slot = NULL;

            /* inside ExecForeignScan, top of function ensure variables declared early */
            HeapTuple cache_ht = NULL;

            if (key_attnum == 1)   /* use your metadata to compute expected_pk_attnum */
            {
                cache_ht = lookup_tuple_in_cache(cache_relid_global, key_attnum, key_const);

                if (cache_ht != NULL)
                {
                    /* store tuple into FDW scan slot; slot will free tuple when cleared */
                    ExecClearTuple(slot);
                    ExecStoreHeapTuple(cache_ht, slot, true);  /* true => slot will free the tuple */
                    node->f_state.cache_returned = true;
                    elog(DEBUG2, "ExecForeignScan: cache hit returned for rel %u", cache_relid_global);
                    return slot;
                }
                else
                {
                    elog(DEBUG2, "ExecForeignScan: cache miss, falling back to remote fetch");
                }
            }
            else 
            {
                elog(DEBUG2, "ExecForeignScan: skipping cache lookup (multi-column PK or unexpected attnum)");
            }
        }
    }

    /* Normal FDW path */
    return ExecScan(&node->ss,
                    (ExecScanAccessMtd) ForeignNext,
                    (ExecScanRecheckMtd) ForeignRecheck);
}



/* ----------------------------------------------------------------
 *		ExecInitForeignScan
 * ----------------------------------------------------------------
 */
ForeignScanState *
ExecInitForeignScan(ForeignScan *node, EState *estate, int eflags)
{
	ForeignScanState *scanstate;
	Relation	currentRelation = NULL;
	Index		scanrelid = node->scan.scanrelid;
	int			tlistvarno;
	FdwRoutine *fdwroutine;
	Relation foreign_rel;
    const char *foreign_relname;
    Oid foreign_nsp;
    char cache_name[NAMEDATALEN];
    
	MyFdwScanState *myfdw_state = palloc0(sizeof(MyFdwScanState));
    myfdw_state->cache_returned = false;


	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	scanstate = makeNode(ForeignScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecForeignScan;
    scanstate->fdw_state =  myfdw_state;
	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation, if any; also acquire function pointers from the
	 * FDW's handler
	 */
	if (scanrelid > 0)
	{
		currentRelation = ExecOpenScanRelation(estate, scanrelid, eflags);
		scanstate->ss.ss_currentRelation = currentRelation;
		fdwroutine = GetFdwRoutineForRelation(currentRelation, true);
	}
	else
	{
		/* We can't use the relcache, so get fdwroutine the hard way */
		fdwroutine = GetFdwRoutineByServerId(node->fs_server);
	}


    /* Open the foreign relation */
	RangeTblEntry *rte = exec_rt_fetch(node->scan.scanrelid, estate);
	Oid relid = rte->relid;
	foreign_rel = table_open(rte->relid, AccessShareLock);

	foreign_relname = RelationGetRelationName(foreign_rel);
	

    foreign_nsp = RelationGetNamespace(foreign_rel);

    /* Prepare cache table name */
    snprintf(cache_name, NAMEDATALEN, "%s_cache", foreign_relname);
    /* Check if cache table exists */
    cache_relid_global = get_relname_relid(cache_name, foreign_nsp);
    if (cache_relid_global == InvalidOid)
    {
        elog(LOG, "Cache table %s does not exist, creating...", cache_name);

        TupleDesc tupdesc = RelationGetDescr(foreign_rel);

        heap_create_with_catalog(
            cache_name,
            foreign_nsp,
            InvalidOid,
            InvalidOid,
            InvalidOid,
            InvalidOid,
            GetUserId(),
            HEAP_TABLE_AM_OID,
            tupdesc,
            NIL,
            RELKIND_RELATION,
            RELPERSISTENCE_PERMANENT,
            false, false,
            ONCOMMIT_NOOP,
            (Datum)0,
            true, false, false,
            InvalidOid,
            NULL
        );
		CommandCounterIncrement();


        elog(LOG, "Cache table %s created successfully", cache_name);
    }

    table_close(foreign_rel, AccessShareLock);
	Bitmapset *pk_attnums = get_primary_key_attnums(relid);

    if (pk_attnums == NULL)
    {
        elog(WARNING, "Relation %u has no primary key", relid);
    }

	/*
	 * Determine the scan tuple type.  If the FDW provided a targetlist
	 * describing the scan tuples, use that; else use base relation's rowtype.
	 */
	if (node->fdw_scan_tlist != NIL || currentRelation == NULL)
	{
		TupleDesc	scan_tupdesc;

		scan_tupdesc = ExecTypeFromTL(node->fdw_scan_tlist);
		ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
							  &TTSOpsHeapTuple);
		/* Node's targetlist will contain Vars with varno = INDEX_VAR */
		tlistvarno = INDEX_VAR;
	}
	else
	{
		TupleDesc	scan_tupdesc;

		/* don't trust FDWs to return tuples fulfilling NOT NULL constraints */
		scan_tupdesc = CreateTupleDescCopy(RelationGetDescr(currentRelation));
		ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
							  &TTSOpsHeapTuple);
		/* Node's targetlist will contain Vars with varno = scanrelid */
		tlistvarno = scanrelid;
	}

	/* Don't know what an FDW might return */
	scanstate->ss.ps.scanopsfixed = false;
	scanstate->ss.ps.scanopsset = true;

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&scanstate->ss, tlistvarno);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);
	scanstate->fdw_recheck_quals =
		ExecInitQual(node->fdw_recheck_quals, (PlanState *) scanstate);

	/*
	 * Determine whether to scan the foreign relation asynchronously or not;
	 * this has to be kept in sync with the code in ExecInitAppend().
	 */
	scanstate->ss.ps.async_capable = (((Plan *) node)->async_capable &&
									  estate->es_epq_active == NULL);

	/*
	 * Initialize FDW-related state.
	 */
	scanstate->fdwroutine = fdwroutine;
	scanstate->fdw_state = NULL;

	/*
	 * For the FDW's convenience, look up the modification target relation's
	 * ResultRelInfo.  The ModifyTable node should have initialized it for us,
	 * see ExecInitModifyTable.
	 *
	 * Don't try to look up the ResultRelInfo when EvalPlanQual is active,
	 * though.  Direct modifications cannot be re-evaluated as part of
	 * EvalPlanQual.  The lookup wouldn't work anyway because during
	 * EvalPlanQual processing, EvalPlanQual only initializes the subtree
	 * under the ModifyTable, and doesn't run ExecInitModifyTable.
	 */
	if (node->resultRelation > 0 && estate->es_epq_active == NULL)
	{
		if (estate->es_result_relations == NULL ||
			estate->es_result_relations[node->resultRelation - 1] == NULL)
		{
			elog(ERROR, "result relation not initialized");
		}
		scanstate->resultRelInfo = estate->es_result_relations[node->resultRelation - 1];
	}

	/* Initialize any outer plan. */
	if (outerPlan(node))
		outerPlanState(scanstate) =
			ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * Tell the FDW to initialize the scan.
	 */
	if (node->operation != CMD_SELECT)
	{
		/*
		 * Direct modifications cannot be re-evaluated by EvalPlanQual, so
		 * don't bother preparing the FDW.
		 *
		 * In case of an inherited UPDATE/DELETE with foreign targets there
		 * can be direct-modify ForeignScan nodes in the EvalPlanQual subtree,
		 * so we need to ignore such ForeignScan nodes during EvalPlanQual
		 * processing.  See also ExecForeignScan/ExecReScanForeignScan.
		 */
		if (estate->es_epq_active == NULL)
			fdwroutine->BeginDirectModify(scanstate, eflags);
	}
	else
		fdwroutine->BeginForeignScan(scanstate, eflags);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndForeignScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndForeignScan(ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;

	/* Let the FDW shut down */
	if (plan->operation != CMD_SELECT)
	{
		if (estate->es_epq_active == NULL)
			node->fdwroutine->EndDirectModify(node);
	}
	else
		node->fdwroutine->EndForeignScan(node);

	/* Shut down any outer plan. */
	if (outerPlanState(node))
		ExecEndNode(outerPlanState(node));

	/* Free the exprcontext */
	ExecFreeExprContext(&node->ss.ps);

	/* clean out the tuple table */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecReScanForeignScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanForeignScan(ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * Ignore direct modifications when EvalPlanQual is active --- they are
	 * irrelevant for EvalPlanQual rechecking
	 */
	if (estate->es_epq_active != NULL && plan->operation != CMD_SELECT)
		return;

	node->fdwroutine->ReScanForeignScan(node);

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  outerPlan may also be NULL, in which case there is
	 * nothing to rescan at all.
	 */
	if (outerPlan != NULL && outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecForeignScanEstimate
 *
 *		Informs size of the parallel coordination information, if any
 * ----------------------------------------------------------------
 */
void
ExecForeignScanEstimate(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->EstimateDSMForeignScan)
	{
		node->pscan_len = fdwroutine->EstimateDSMForeignScan(node, pcxt);
		shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanInitializeDSM
 *
 *		Initialize the parallel coordination information
 * ----------------------------------------------------------------
 */
void
ExecForeignScanInitializeDSM(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->InitializeDSMForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_allocate(pcxt->toc, node->pscan_len);
		fdwroutine->InitializeDSMForeignScan(node, pcxt, coordinate);
		shm_toc_insert(pcxt->toc, plan_node_id, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecForeignScanReInitializeDSM(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->ReInitializeDSMForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pcxt->toc, plan_node_id, false);
		fdwroutine->ReInitializeDSMForeignScan(node, pcxt, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanInitializeWorker
 *
 *		Initialization according to the parallel coordination information
 * ----------------------------------------------------------------
 */
void
ExecForeignScanInitializeWorker(ForeignScanState *node,
								ParallelWorkerContext *pwcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->InitializeWorkerForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pwcxt->toc, plan_node_id, false);
		fdwroutine->InitializeWorkerForeignScan(node, pwcxt->toc, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownForeignScan
 *
 *		Gives FDW chance to stop asynchronous resource consumption
 *		and release any resources still held.
 * ----------------------------------------------------------------
 */
void
ExecShutdownForeignScan(ForeignScanState *node)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->ShutdownForeignScan)
		fdwroutine->ShutdownForeignScan(node);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanRequest
 *
 *		Asynchronously request a tuple from a designed async-capable node
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanRequest(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncRequest != NULL);
	fdwroutine->ForeignAsyncRequest(areq);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanConfigureWait
 *
 *		In async mode, configure for a wait
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanConfigureWait(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncConfigureWait != NULL);
	fdwroutine->ForeignAsyncConfigureWait(areq);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanNotify
 *
 *		Callback invoked when a relevant event has occurred
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanNotify(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncNotify != NULL);
	fdwroutine->ForeignAsyncNotify(areq);
}
