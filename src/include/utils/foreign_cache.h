/* src/include/utils/foreign_cache.h */

#ifndef FOREIGN_CACHE_H
#define FOREIGN_CACHE_H

#include "postgres.h"
#include "utils/hsearch.h"

/*
 * Per-cache-table metadata stored in shared memory.
 * Keyed by cache table relid (Oid).
 */
typedef struct CacheEntry
{
    Oid relid;     /* cache table OID (key) */
    int count;     /* current number of rows (approx, but good enough) */
} CacheEntry;

/* Called during shared memory initialization (see below) */
extern void ForeignCacheShmemInit(void);

/* Lookup or create a CacheEntry for a given cache table OID */
extern CacheEntry *GetCacheEntryForCacheRel(Oid relid);

extern int RecountCacheTableRows(Oid relid);


#endif /* FOREIGN_CACHE_H */
