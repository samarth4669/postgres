/* src/backend/utils/cache/foreign_cache.c */

#include "postgres.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "common/hashfn.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "utils/rel.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/foreign_cache.h"


static HTAB *CacheHash = NULL;

void
ForeignCacheShmemInit(void)
{
    HASHCTL ctl;

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(Oid);
    ctl.entrysize = sizeof(CacheEntry);
    ctl.hash      = tag_hash;  /* default hash for Oid keys */

    CacheHash = ShmemInitHash("foreign table cache hash",
                              128,   /* initial size */
                              1024,  /* max size */
                              &ctl,
                              HASH_ELEM | HASH_BLOBS | HASH_FUNCTION);
}

/*
 * Lookup/Create per-cache-table entry.
 */
CacheEntry *
GetCacheEntryForCacheRel(Oid relid)
{
    bool        found;
    CacheEntry *entry;

    if (CacheHash == NULL)
        elog(ERROR, "ForeignCacheShmemInit not called before GetCacheEntryForCacheRel");

    entry = (CacheEntry *) hash_search(CacheHash,
                                       &relid,
                                       HASH_ENTER,
                                       &found);
    if (!found)
    {
        entry->count = RecountCacheTableRows(relid);
    }

    return entry;
}

int RecountCacheTableRows(Oid relid)
{
    int count = 0;
    Relation rel = table_open(relid, AccessShareLock);
    TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
    HeapTuple tup;

    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
        count++;

    table_endscan(scan);
    table_close(rel, AccessShareLock);

    elog(LOG, "Rebuilt cache counter for relid=%u count=%d", relid, count);
    return count;
}

