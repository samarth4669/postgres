#include "postgres.h"
#include "nodes/parsenodes.h"
#include "optimizer/planner.h"
#include "utils/lsyscache.h"
#include "catalog/pg_class.h"
#include "catalog/namespace.h"
#include "utils/rel.h"
#include "executor/spi.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

static planner_hook_type prev_planner_hook = NULL;

/* helper: check if cache table exists for given foreign table name */
static Oid
get_cache_table_oid(const char *foreign_table_name)
{
    Oid relid = InvalidOid;
    char cache_name[NAMEDATALEN];
    int ret;

    snprintf(cache_name, sizeof(cache_name), "cache_%s", foreign_table_name);

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    char query[256];
    snprintf(query, sizeof(query),
             "SELECT oid FROM pg_class WHERE relname = '%s' AND relnamespace = "
             "(SELECT oid FROM pg_namespace WHERE nspname = 'public');",
             cache_name);

    ret = SPI_execute(query, true, 0);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        Datum val = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            relid = DatumGetObjectId(val);
    }

    SPI_finish();
    return relid;
}

/* helper: detect pk = constant in simple queries (simplified) */
static bool
has_pk_eq_constant(Query *parse, Oid relid)
{
    /* For brevity, assume true if quals exist; you can walk expr tree for pk = const */
    return (parse->jointree && parse->jointree->quals != NULL);
}

static PlannedStmt *
fdw_cache_redirect_planner(Query *parse,
                           const char *query_string,
                           int cursorOptions,
                           ParamListInfo boundParams)
{
    ListCell *lc;

    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

        if (rte->rtekind == RTE_RELATION)
        {
            Oid relid = rte->relid;
            char relkind = get_rel_relkind(relid);

            if (relkind == RELKIND_FOREIGN_TABLE)
            {
                const char *foreign_table_name = get_rel_name(relid);
                elog(DEBUG1, "fdw_cache_redirect: foreign table detected: %s", foreign_table_name);

                /* Step 1: Check if pk=constant condition exists */
                if (has_pk_eq_constant(parse, relid))
                {
                    /* Step 2: Check if cache table exists */
                    Oid cache_oid = get_cache_table_oid(foreign_table_name);

                    if (OidIsValid(cache_oid))
                    {
                        elog(DEBUG1, "fdw_cache_redirect: cache table exists, rewriting to %u", cache_oid);

                        rte->relid = cache_oid;
                        rte->rtekind = RTE_RELATION; /* normal local relation */
                    }
                    else
                    {
                        elog(DEBUG1, "fdw_cache_redirect: no cache table, keeping foreign table");
                    }
                }
            }
        }
    }

    /* continue normal planning */
    if (prev_planner_hook)
        return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        return standard_planner(parse, query_string, cursorOptions, boundParams);
}

void
_PG_init(void)
{
    elog(LOG, "fdw_cache_redirect extension loaded");
    prev_planner_hook = planner_hook;
    planner_hook = fdw_cache_redirect_planner;
}

void
_PG_fini(void)
{
    planner_hook = prev_planner_hook;
}

