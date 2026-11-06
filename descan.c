#include "postgres.h"

#include "access/relscan.h"
#include "dynamic_ext.h"
#include "storage/itemptr.h"

/*
 * Begin scan of index.
 */
IndexScanDesc debeginscan(Relation r, int nkeys, int norderbys) {
  fprintf(stderr, "debeginscan\n");
  IndexScanDesc scan;
  DEScanOpaque so;

  so = palloc0_object(DEScanOpaqueData);
  so->done = false;

  scan = RelationGetIndexScan(r, nkeys, norderbys);

  scan->opaque = so;

  return scan;
}

/*
 * Rescan an index.
 */
void derescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
              ScanKey orderbys, int norderbys) {
  fprintf(stderr, "rescan\n");
  DEScanOpaque so = ((DEScanOpaque)scan->opaque);

  if (scankey && nscankeys > 0)
    memmove(scan->keyData, scankey, nscankeys * sizeof(ScanKeyData));

  free(so->tids);
  so->tids = NULL;
  so->count = 0;
  so->pos = 0;
  so->done = false;
}

/*
 * End scan of index.
 */
void deendscan(IndexScanDesc scan) {
  fprintf(stderr, "endscan\n");
  DEScanOpaque so = ((DEScanOpaque)scan->opaque);

  free(so->tids);
  pfree(so);
}

bool degettuple(IndexScanDesc scan, ScanDirection dir) {
  fprintf(stderr, "gettuple start\n");
  DEScanOpaque so = ((DEScanOpaque)scan->opaque);
  int64 low;
  int64 high;

  if (!so->done) {
    low = PG_INT32_MIN;
    high = PG_INT32_MAX;

    for (int i = 0; i < scan->numberOfKeys; i++) {
      int v;
      v = DatumGetInt32(scan->keyData[i].sk_argument);
      switch (scan->keyData[i].sk_strategy) {
      case 1:
        high = Min(high, v - 1);
        break;
      case 2:
        high = Min(high, v);
        break;
      case 3:
        low = Max(low, v);
        high = Min(high, v);
        break;
      case 4:
        low = Max(low, v);
        break;
      case 5:
        low = Max(low, v + 1);
        break;
      }
    }

    if (low <= high) {
      DEIndex *idx;
      Relation index = scan->indexRelation;

      idx = de_open_index(index);
      de_range(idx, low, high, &so->tids, &so->count);
      de_close_index(scan->indexRelation, idx);
    }

    so->pos = 0;
    so->done = true;
  }

  if (so->pos >= so->count)
    return false;

  fprintf(stderr, "pos=%zu\n", so->pos);
  ItemPointerSet(&scan->xs_heaptid, so->tids[so->pos] >> 16,
                 so->tids[so->pos] & 0xFFFF);
  so->pos++;
  scan->xs_recheck = true;
  return true;
}
