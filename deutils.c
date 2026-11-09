// This is currently a base that only prints out the functions calls, the
// arguments, and timestamp

#include "c.h"
#include "common/relpath.h"
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/reloptions.h"
#include "dynamic_ext.h"
#include "miscadmin.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "storage/off.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(dynamic_ext);

static relopt_kind de_relopt_kind;
static relopt_parse_elt de_relopt_tab[3];

void _PG_init(void) {
  de_relopt_kind = add_reloption_kind();

  add_int_reloption(de_relopt_kind, "buffer_lwm",
                    "The number of records that can be inserted before a "
                    "buffer flush is initiated",
                    DEFAULT_DE_BUFFER_LWM, MIN_DE_BUFFER_LWM, MAX_DE_BUFFER_LWM,
                    AccessExclusiveLock);
  add_int_reloption(de_relopt_kind, "buffer_hwm",
                    "The maximum buffer capacity, inserts will begin to fail "
                    "once this number is reached, until the buffer flush has "
                    "completed. Has no effect in single-threaded operation",
                    DEFAULT_DE_BUFFER_HWM, MIN_DE_BUFFER_HWM, MAX_DE_BUFFER_HWM,
                    AccessExclusiveLock);
  add_int_reloption(de_relopt_kind, "scale_factor",
                    "The rate at which the capacity of levels grows; should be "
                    "at least 2 for reasonable performance",
                    DEFAULT_DE_SCALE_FACTOR, MIN_DE_SCALE_FACTOR,
                    MAX_DE_SCALE_FACTOR, AccessExclusiveLock);

  de_relopt_tab[0].optname = "buffer_lwm";
  de_relopt_tab[0].opttype = RELOPT_TYPE_INT;
  de_relopt_tab[0].offset = offsetof(DEOptions, buffer_lwm);

  de_relopt_tab[1].optname = "buffer_hwm";
  de_relopt_tab[1].opttype = RELOPT_TYPE_INT;
  de_relopt_tab[1].offset = offsetof(DEOptions, buffer_hwm);

  de_relopt_tab[2].optname = "scale_factor";
  de_relopt_tab[2].opttype = RELOPT_TYPE_INT;
  de_relopt_tab[2].offset = offsetof(DEOptions, scale_factor);
}

Datum dynamic_ext(PG_FUNCTION_ARGS) {
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  /*
   * Total number of strategies (operators) by which we can traverse/search
   * this AM.  Zero if AM does not have a fixed set of strategy assignments.
   */
  amroutine->amstrategies = 5;
  /* total number of support functions that this AM uses */
  amroutine->amsupport = 5;
  /* opclass options support function number or 0 */
  amroutine->amoptsprocnum = 0;
  /* does AM support ORDER BY indexed column's value? */
  amroutine->amcanorder = false;
  /* does AM support ORDER BY result of an operator on indexed column? */
  amroutine->amcanorderbyop = false;
  /* does AM support hashing using API consistent with the hash AM? */
  amroutine->amcanhash = false;
  /* do operators within an opfamily have consistent equality semantics? */
  amroutine->amconsistentequality = false;
  /* do operators within an opfamily have consistent ordering semantics? */
  amroutine->amconsistentordering = false;
  /* does AM support backward scanning? */
  amroutine->amcanbackward = false;
  /* does AM support UNIQUE indexes? */
  amroutine->amcanunique = false;
  /* does AM support multi-column indexes? */
  amroutine->amcanmulticol = false;
  /* does AM require scans to have a constraint on the first index column? */
  amroutine->amoptionalkey = false;
  /* does AM handle ScalarArrayOpExpr quals? */
  amroutine->amsearcharray = false;
  /* does AM handle IS NULL/IS NOT NULL quals? */
  amroutine->amsearchnulls = false;
  /* can index storage data type differ from column data type? */
  amroutine->amstorage = false;
  /* can an index of this type be clustered on? */
  amroutine->amclusterable = false;
  /* does AM handle predicate locks? */
  amroutine->ampredlocks = false;
  /* does AM support parallel scan? */
  amroutine->amcanparallel = false;
  /* does AM support parallel build? */
  amroutine->amcanbuildparallel = false;
  /* does AM support columns included with clause INCLUDE? */
  amroutine->amcaninclude = false;
  /* does AM use maintenance_work_mem? */
  amroutine->amusemaintenanceworkmem = false;
  /* does AM summarize tuples, with at least all tuples in the block
   * summarized in one summary */
  amroutine->amsummarizing = false;
  /* OR of parallel vacuum flags */
  amroutine->amparallelvacuumoptions = 0;
  /* type of data stored in index, or InvalidOid if variable */
  amroutine->amkeytype = InvalidOid;

  /* interface functions */
  amroutine->ambuild = debuild;
  amroutine->ambuildempty = debuildempty;
  amroutine->aminsert = deinsert;
  amroutine->ambulkdelete = debulkdelete;
  amroutine->amvacuumcleanup = devacuumcleanup;
  amroutine->amcostestimate = decostestimate;
  amroutine->amoptions = deoptions;
  amroutine->amvalidate = devalidate;
  amroutine->ambeginscan = debeginscan;
  amroutine->amrescan = derescan;
  amroutine->amgettuple = degettuple;
  amroutine->amendscan = deendscan;

  PG_RETURN_POINTER(amroutine);
}

/*
 * Fill in metapage for DE index.
 */

void DEFillMetapage(Relation index, Page metaPage) {
  DEMetaPageData *metadata;

  PageInit(metaPage, BLCKSZ, 0);
  metadata = (DEMetaPageData *)PageGetContents(metaPage);
  metadata->magic = DE_MAGICK_NUMBER;
  metadata->npages = 0;
  metadata->flush_seq = 0;

  ((PageHeader)metaPage)->pd_lower += sizeof(DEMetaPageData);

  Assert(((PageHeader)metaPage)->pd_lower <= ((PageHeader)metaPage)->pd_upper);
}

/*
 * Initialize metapage for DE index.
 */

void DEInitMetapage(Relation index) {
  Buffer metaBuffer;
  Page metaPage;
  GenericXLogState *state;

  metaBuffer =
      ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
  Assert(BufferGetBlockNumber(metaBuffer) == DE_METAPAGE_BLKNO);

  state = GenericXLogStart(index);
  metaPage =
      GenericXLogRegisterBuffer(state, metaBuffer, GENERIC_XLOG_FULL_IMAGE);

  DEFillMetapage(index, metaPage);
  GenericXLogFinish(state);
  UnlockReleaseBuffer(metaBuffer);
}

void DEBufferAppend(Relation index, const DEBufEntry *e, uint64 flush_seq) {
  Buffer metaBuffer;
  Page metaPage;
  DEMetaPageData *metadata;
  GenericXLogState *state;
  Buffer buffer = InvalidBuffer;
  Page page = NULL;
  bool need_new_pages = true;

  metaBuffer = ReadBuffer(index, DE_METAPAGE_BLKNO);
  LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);

  state = GenericXLogStart(index);
  metaPage = GenericXLogRegisterBuffer(state, metaBuffer, 0);
  metadata = (DEMetaPageData *)PageGetContents(metaPage);

  if (metadata->flush_seq != flush_seq) {
    metadata->flush_seq = flush_seq;
    metadata->npages = 0;
  }

  if (metadata->npages > 0) {
    buffer = ReadBuffer(index, metadata->npages);
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

    if (PageGetFreeSpace(BufferGetPage(buffer)) >=
        MAXALIGN(sizeof(DEBufEntry))) {
      page = GenericXLogRegisterBuffer(state, buffer, 0);
      need_new_pages = false;
    } else {
      UnlockReleaseBuffer(buffer);
    }
  }

  if (need_new_pages) {
    BlockNumber blkno = metadata->npages + 1;

    if (blkno < RelationGetNumberOfBlocks(index)) {
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    } else {
      buffer =
          ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
    }

    page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
    PageInit(page, BLCKSZ, 0);
    metadata->npages = blkno;
  }
  if (PageAddItem(page, (Item)e, sizeof(DEBufEntry), InvalidOffsetNumber, false,
                  false) == InvalidOffsetNumber)
    elog(ERROR, "could not add entry to buffer page");

  GenericXLogFinish(state);
  UnlockReleaseBuffer(buffer);
  UnlockReleaseBuffer(metaBuffer);
}

DEBufEntry *DEBufferRead(Relation index, uint64 ckpt_flush_seq, int *n) {
  Buffer metaBuffer;
  Page metaPage;
  uint64 flush_seq;
  BlockNumber npages;
  DEMetaPageData *metadata;
  BlockNumber blkno;
  DEBufEntry *entries;
  int cap = 64;

  *n = 0;

  metaBuffer = ReadBuffer(index, DE_METAPAGE_BLKNO);
  LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
  metaPage = BufferGetPage(metaBuffer);
  metadata = (DEMetaPageData *)PageGetContents(metaPage);

  if (metadata->magic != DE_MAGICK_NUMBER) {
    ereport(ERROR, errmsg("WRONG MAGIC NUMBER"));
  }
  flush_seq = metadata->flush_seq;
  npages = metadata->npages;

  UnlockReleaseBuffer(metaBuffer);

  if (flush_seq > ckpt_flush_seq)
    ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg("index \"%s\": buffer pages are at flush %llu but "
                           "checkpoint.json is at %llu",
                           RelationGetRelationName(index),
                           (unsigned long long)flush_seq,
                           (unsigned long long)ckpt_flush_seq),
                    errhint("REINDEX the index.")));

  if (flush_seq < ckpt_flush_seq)
    npages = 0;

  entries = palloc(cap * sizeof(DEBufEntry));

  for (blkno = 1; blkno <= npages; blkno++) {
    Buffer buffer = ReadBuffer(index, blkno);
    Page page;
    OffsetNumber off;
    OffsetNumber maxoff;

    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);
    maxoff = PageGetMaxOffsetNumber(page);

    for (off = FirstOffsetNumber; off <= maxoff; off++) {
      Item item = PageGetItem(page, PageGetItemId(page, off));

      if (*n == cap) {
        cap *= 2;
        entries = repalloc(entries, cap * sizeof(DEBufEntry));
      }

      memcpy(&entries[*n], item, sizeof(DEBufEntry));
      (*n)++;
    }
    UnlockReleaseBuffer(buffer);
  }

  return entries;
}

void de_index_dir(char *path, Oid dbOid, RelFileNumber relNumber) {
  snprintf(path, MAXPGPATH, "%s/dynamic_ext/%u/%u", DataDir, dbOid, relNumber);
}

DEIndex *de_open_index(Relation index) {
  DEOptions *rdopts = (DEOptions *)index->rd_options;
  size_t lwm = DEFAULT_DE_BUFFER_LWM;
  size_t hwm = DEFAULT_DE_BUFFER_HWM;
  size_t sf = DEFAULT_DE_SCALE_FACTOR;
  char dir[MAXPGPATH];
  DEIndex *idx;
  DEBufEntry *entries;
  int n, i;

  LockRelation(index, ShareUpdateExclusiveLock);

  if (rdopts != NULL) {
    lwm = rdopts->buffer_lwm;
    hwm = rdopts->buffer_hwm;
    sf = rdopts->scale_factor;
  }

  de_index_dir(dir, index->rd_locator.dbOid, index->rd_locator.relNumber);
  idx = de_open(dir, lwm, hwm, sf);

  if (idx == NULL) {
    ereport(ERROR, errmsg("Could not open index"));
  }

  entries = DEBufferRead(index, de_flush_seq(idx), &n);

  for (i = 0; i < n; i++) {
    if (de_replay(idx, entries[i].key, entries[i].tid, entries[i].tombstone) !=
        0) {
      de_close_index(index, idx);

      ereport(ERROR, errmsg("dynamic_ext: could not open \"%s\": %s", dir,
                            de_last_error()));
    }
  }
  pfree(entries);

  return idx;
}

void de_close_index(Relation index, DEIndex *idx) {
  de_close(idx);
  UnlockRelation(index, ShareUpdateExclusiveLock);
}

/*
 * Parse reloptions for index.
 */
bytea *deoptions(Datum reloptions, bool validate) {
  DEOptions *rdopts;

  rdopts = (DEOptions *)build_reloptions(reloptions, validate, de_relopt_kind,
                                         sizeof(DEOptions), de_relopt_tab,
                                         lengthof(de_relopt_tab));

  if (validate && rdopts != NULL && rdopts->buffer_lwm > rdopts->buffer_hwm) {
    ereport(
        ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg(
             "dynamic_ext: buffer_lwm must be less or equal to buffer_hwm")));
  }

  return (bytea *)rdopts;
}
