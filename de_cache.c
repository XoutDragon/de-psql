#include "postgres.h"

#include "dynamic_ext.h"
#include "storage/block.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/syscache.h"

typedef struct DECacheEntry {
  Oid indexoid;
  DEIndex *idx;
  RelFileNumber relnumber;
  size_t lwm, hwm, sf;
  uint64 seq;
  uint32 nentries;
  bool maybe_dropped;
  uint32 epoch;
} DECacheEntry;

static bool de_check_dropped = false;
static HTAB *de_cache = NULL;

static void de_cache_relcache_cb(Datum arg, Oid relid) {
  HASH_SEQ_STATUS status;
  DECacheEntry *e;

  if (de_cache == NULL)
    return;

  if (OidIsValid(relid)) {
    e = hash_search(de_cache, &relid, HASH_FIND, NULL);
    if (e != NULL) {
      e->maybe_dropped = true;
      de_check_dropped = true;
    }
    return;
  }

  hash_seq_init(&status, de_cache);
  while ((e = hash_seq_search(&status)) != NULL) {
    e->maybe_dropped = true;
    de_check_dropped = true;
  }
}

static void de_cache_sweep_dropped(Oid keep) {
  HASH_SEQ_STATUS status;
  DECacheEntry *e;

  de_check_dropped = false;
  hash_seq_init(&status, de_cache);

  while ((e = hash_seq_search(&status)) != NULL) {
    if (!e->maybe_dropped || e->indexoid == keep)
      continue;
    e->maybe_dropped = false;
    if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(e->indexoid))) {
      Oid oid = e->indexoid;

      if (e->idx != NULL)
        de_close(e->idx);
      hash_search(de_cache, &oid, HASH_REMOVE, NULL);
    }
  }
}

static void de_cache_init(void) {
  HASHCTL ctl;
  ctl.keysize = sizeof(Oid);
  ctl.entrysize = sizeof(DECacheEntry);
  de_cache =
      hash_create("dynamic_ext open indexes", 16, &ctl, HASH_ELEM | HASH_BLOBS);
  CacheRegisterRelcacheCallback(de_cache_relcache_cb, (Datum)0);
}

static int de_replay_entries(Relation index, DEIndex *idx, DEBufEntry *entries,
                             int n) {
  uint64 before = de_flush_seq(idx);
  uint64 seq = before;
  int start = 0;
  int i;

  for (i = 0; i < n; i++) {
    if (de_replay(idx, entries[i].key, entries[i].tid, entries[i].tombstone) !=
        0) {
      return -1;
    }

    if (de_flush_seq(idx) != seq) {
      seq = de_flush_seq(idx);
      start = i;
    }
  }

  if (seq != before)
    for (i = start; i < n; i++)
      DEBufferAppendMany(index, &entries[i], 1, seq);
  return 0;
}

DEIndex *de_acquire_index(Relation index) {
  Oid oid = RelationGetRelid(index);
  DEOptions *opts = (DEOptions *)index->rd_options;
  size_t lwm = DEFAULT_DE_BUFFER_LWM;
  size_t hwm = DEFAULT_DE_BUFFER_HWM;
  size_t sf = DEFAULT_DE_SCALE_FACTOR;
  DECacheEntry *e;
  bool found;
  bool fresh = false;
  uint64 seq;
  uint32 nentries;
  uint32 epoch;
  BlockNumber npages;

  LockRelation(index, ShareUpdateExclusiveLock);
  if (opts != NULL) {
    lwm = opts->buffer_lwm;
    hwm = opts->buffer_hwm;
    sf = opts->scale_factor;
  }

  if (de_cache == NULL)
    de_cache_init();

  if (de_check_dropped)
    de_cache_sweep_dropped(oid);

  e = hash_search(de_cache, &oid, HASH_ENTER, &found);

  if (!found)
    e->idx = NULL;
  e->maybe_dropped = false;

  DEReadMeta(index, &seq, &nentries, &npages, &epoch);

  if (e->idx != NULL &&
      (e->relnumber != index->rd_locator.relNumber || e->lwm != lwm ||
       e->hwm != hwm || e->sf != sf || e->seq != seq ||
       nentries < e->nentries || e->epoch != epoch)) {
    de_close(e->idx);
    e->idx = NULL;
  }

  if (e->idx == NULL) {
    char dir[MAXPGPATH];
    de_index_dir(dir, index->rd_locator.dbOid, index->rd_locator.relNumber);
    e->idx = de_open(dir, lwm, hwm, sf);
    if (e->idx == NULL) {
      hash_search(de_cache, &oid, HASH_REMOVE, NULL);
      ereport(ERROR, errmsg("dynamic_ext: could not open \"%s\": %s", dir,
                            de_last_error()));
    }
    e->relnumber = index->rd_locator.relNumber;
    e->lwm = lwm;
    e->hwm = hwm;
    e->sf = sf;
    e->seq = de_flush_seq(e->idx);
    e->nentries = 0;
    e->epoch = epoch;
    fresh = true;
  }

  if (fresh || nentries > e->nentries) {
    int n;

    DEBufEntry *entries = DEBufferRead(index, e->seq, e->nentries, &n);

    if (de_replay_entries(index, e->idx, entries, n) != 0) {
      char *msg = pstrdup(de_last_error());

      de_discard_index(index, e->idx);
      ereport(ERROR, errmsg("dynamic_ext: could not replay index \"%s\" %s",
                            RelationGetRelationName(index), msg));
    }
    pfree(entries);
    DEReadMeta(index, &e->seq, &e->nentries, &npages, &e->epoch);
  }

  return e->idx;
}

void de_release_index(Relation index, DEIndex *idx) {
  UnlockRelation(index, ShareUpdateExclusiveLock);
}

void de_cache_sync(Relation index) {
  Oid oid = RelationGetRelid(index);
  DECacheEntry *e = hash_search(de_cache, &oid, HASH_FIND, NULL);
  BlockNumber npages;

  if (e != NULL && e->idx != NULL) {
    DEReadMeta(index, &e->seq, &e->nentries, &npages, &e->epoch);
  }
}

void de_discard_index(Relation index, DEIndex *idx) {
  Oid oid = RelationGetRelid(index);
  DECacheEntry *e = hash_search(de_cache, &oid, HASH_FIND, NULL);

  if (e != NULL) {
    if (e->idx != NULL) {
      DEBumpEpoch(index);
      de_close(e->idx);
    }
    hash_search(de_cache, &oid, HASH_REMOVE, NULL);
  }
}