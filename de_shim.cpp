/*
 * de_shim.cpp
 *		C++ half of the shim between the dynamic_extension index AM and
 * the dynamic extension framework (DynamicExtension over the external ISAM
 * shard, queried with the library's de::pl point lookup).
 *
 * No Postgres headers are included here.  Every entry point catches all
 * exceptions and reports failure through its return value and
 * de_last_error(), so control never unwinds or longjmps across the C/C++
 * boundary.
 */
#include "de_shim.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "framework/DynamicExtension.h"
#include "query/pointlookup.h"
#include "shard/ExternalISAMTree.h"

namespace {

/*
 * Index entry: the int4 key widened to int64, and the heap TID packed into
 * 64 bits (never 0, since offset numbers start at 1).
 *
 * A record's identity is its key AND its TID: several rows can share a key.
 * Lookups go through range_scan, which groups records with operator==, so
 * comparing only the key would merge different rows with the same key.
 */
struct PgRec {
  int64_t key;
  uint64_t value;

  bool operator<(const PgRec &other) const {
    return key < other.key || (key == other.key && value < other.value);
  }

  bool operator==(const PgRec &other) const {
    return key == other.key && value == other.value;
  }
};

typedef de::ISAMTree<PgRec> Shard;
typedef de::pl::Query<Shard> Query;

/*
 * Page buffer handed to point_lookup.  It reads one 4 KiB ISAM page, but
 * may then look at the record just past the end of that page (when the
 * lower bound falls off a leaf), so allocate two pages and zero them.
 */
constexpr size_t PAGE_SZ = 4096;
constexpr size_t LOOKUP_BUF_SZ = 2 * PAGE_SZ;

/*
 * Leveling, not tiering: after a checkpoint reload, InternalLevel is sized
 * to the number of shards it had, so a tiered level has no free slot for the
 * next flush.  Serial scheduler, because a backend must not run work on
 * background threads.
 */
typedef de::DynamicExtension<Shard, Query, de::LayoutPolicy::TEIRING,
                             de::DeletePolicy::TOMBSTONE, de::SerialScheduler>
    Extension;

std::string last_error;

/* Postgres's working directory (the data directory); it never changes */
int home_fd = -1;

template <typename F> int guarded(F &&f) {
  try {
    f();
    return 0;
  } catch (const std::exception &e) {
    last_error = e.what();
  } catch (...) {
    last_error = "unknown C++ exception";
  }
  return -1;
}

} /* namespace */

/*
 * Defined after the namespace (it needs Extension) and outside it, because
 * de_shim.h declares it as the global "struct DEIndex".
 */
struct DEIndex {
  Extension *ext;
  std::byte *lookup_buf;
  std::string dir;
  int dir_fd; /* the index folder, opened once in de_open */
};

namespace {

/*
 * The framework takes no directory argument: it keeps checkpoint.json,
 * shards/ and tmp_shards/ relative to the working directory.  But a Postgres
 * backend must stay in the data directory, because Postgres opens its own
 * files (base/..., pg_wal/...) by relative path too.  So step into the index
 * directory only for the length of one call, and always step back.
 *
 * fchdir on directory fds opened once, not chdir(path): chdir walks every
 * component of the path in the kernel, twice per call, which was ~20% of
 * the time of a bulk INSERT.
 */
template <typename F> int in_index_dir(DEIndex *idx, F &&f) {
  if (home_fd < 0 &&
      (home_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
    last_error =
        std::string("cannot open working directory: ") + strerror(errno);
    return -1;
  }
  if (fchdir(idx->dir_fd) != 0) {
    last_error =
        std::string("cannot enter index directory: ") + strerror(errno);
    return -1;
  }

  int rc = guarded(f);

  if (fchdir(home_fd) != 0) {
    last_error =
        std::string("cannot return to working directory: ") + strerror(errno);
    rc = -1;
  }
  return rc;
}

/* free everything de_open allocated; safe on a half-built DEIndex */
void free_index(DEIndex *idx) {
  if (idx == nullptr)
    return;
  if (idx->dir_fd >= 0)
    close(idx->dir_fd);
  free(idx->lookup_buf);
  delete idx;
}

} /* namespace */

extern "C" int de_create(const char *dir) {
  /* absolute path, no directory change needed */
  return guarded([&] {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  });
}

extern "C" DEIndex *de_open(const char *dir, size_t buffer_lwm,
                            size_t buffer_hwm, size_t scale_factor) {
  DEIndex *idx = nullptr;

  int rc = guarded([&] {
    idx = new DEIndex{nullptr, nullptr, dir, -1};
    psudb::sf_aligned_alloc(PAGE_SZ, LOOKUP_BUF_SZ, &idx->lookup_buf);
    if (idx->lookup_buf == nullptr)
      throw std::bad_alloc();

    idx->dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (idx->dir_fd < 0)
      throw std::runtime_error(std::string("cannot open index directory: ") +
                               strerror(errno));
  });

  if (rc == 0)
    rc = in_index_dir(idx, [&] {
      /*
       * last argument: use_wal = false.  The framework writes no
       * wal.log; its buffer is refilled from the Postgres buffer pages
       * with de_replay() instead.
       */
      idx->ext = new Extension(buffer_lwm, buffer_hwm, scale_factor, 0, 1,
                               false, false);
    });

  if (rc != 0) {
    free_index(idx);
    return nullptr;
  }
  return idx;
}

extern "C" void de_close(DEIndex *idx) {
  if (idx == nullptr)
    return;

  /*
   * Closing is not dropping: ~ISAMTree() no longer removes files, so the
   * shard files that checkpoint.json lists stay for the next de_open().
   */
  in_index_dir(idx, [&] { delete idx->ext; });
  free_index(idx);
}

extern "C" int de_insert(DEIndex *idx, int64_t key, uint64_t tid) {
  return in_index_dir(idx, [&] {
    /* returns 0 only when the buffer is at its high watermark */
    if (!idx->ext->insert({key, tid}))
      throw std::runtime_error("dynamic extension buffer is full");
  });
}

/*
 * Every TID whose key is in [low, high], in key order.  *tids is a malloc'd
 * array the caller must free(), even when *count is 0.
 */
extern "C" int de_range(DEIndex *idx, int64_t low, int64_t high,
                        uint64_t **tids, size_t *count) {
  *tids = nullptr;
  *count = 0;

  return in_index_dir(idx, [&] {
    std::vector<uint64_t> found;
    auto it = idx->ext->range_scan(
        {low, 0}, {high, std::numeric_limits<uint64_t>::max()});

    while (it.has_next())
      found.push_back(it.next().value);

    /* +1 so an empty result is still a real allocation */
    *tids = (uint64_t *)malloc(found.size() * sizeof(uint64_t) + 1);
    if (*tids == nullptr)
      throw std::bad_alloc();
    memcpy(*tids, found.data(), found.size() * sizeof(uint64_t));
    *count = found.size();
  });
}

/*
 * Every entry in the index, as two parallel malloc'd arrays: keys[i] goes
 * with tids[i].  The caller must free() both, even when *count is 0.
 * Used by VACUUM, which has to look at every entry.
 */
extern "C" int de_scan_all(DEIndex *idx, int64_t **keys, uint64_t **tids,
                           size_t *count) {
  *keys = nullptr;
  *tids = nullptr;
  *count = 0;

  return in_index_dir(idx, [&] {
    std::vector<PgRec> found;

    /* keys are int4 values widened to int64, so this covers them all */
    auto it = idx->ext->range_scan({std::numeric_limits<int32_t>::min(), 0},
                                   {std::numeric_limits<int32_t>::max(),
                                    std::numeric_limits<uint64_t>::max()});

    while (it.has_next())
      found.push_back(it.next());

    /* +1 so an empty result is still a real allocation */
    *keys = (int64_t *)malloc(found.size() * sizeof(int64_t) + 1);
    *tids = (uint64_t *)malloc(found.size() * sizeof(uint64_t) + 1);
    if (*keys == nullptr || *tids == nullptr) {
      free(*keys);
      free(*tids);
      *keys = nullptr;
      *tids = nullptr;
      throw std::bad_alloc();
    }
    for (size_t i = 0; i < found.size(); i++) {
      (*keys)[i] = found[i].key;
      (*tids)[i] = found[i].value;
    }
    *count = found.size();
  });
}

/*
 * Remove one entry.  With DeletePolicy::TOMBSTONE this does not touch the
 * shards: it inserts a tombstone {key, tid}, and the next merge drops the
 * tombstone together with the record it matches.  Matching uses
 * PgRec::operator==, which compares key AND tid, so only this one row's
 * entry goes, never another row with the same key.
 */
extern "C" int de_erase(DEIndex *idx, int64_t key, uint64_t tid) {
  return in_index_dir(idx, [&] {
    /* like insert: returns 0 only when the buffer is at its high watermark */
    if (!idx->ext->erase({key, tid}))
      throw std::runtime_error("dynamic extension buffer is full");
  });
}

/*
 * Put one entry from the Postgres buffer pages back into the extension's
 * in-memory buffer, without logging (there is no wal.log any more).
 *
 * In the index directory because a replayed entry can fill the buffer to
 * buffer_lwm, and the next one then flushes, which writes shard files.
 */
extern "C" int de_replay(DEIndex *idx, int64_t key, uint64_t tid,
                         bool tombstone) {
  return in_index_dir(idx, [&] {
    int ok = tombstone ? idx->ext->erase({key, tid}, nullptr, false)
                       : idx->ext->insert({key, tid}, false);

    if (!ok)
      throw std::runtime_error(
          "dynamic extension buffer is full during replay");
  });
}

/*
 * How many times the buffer has been flushed into a shard (saved in
 * checkpoint.json).  If it went up during de_insert / de_erase, everything
 * on the Postgres buffer pages is now in a shard.  Reads memory only, so no
 * directory change.
 */
extern "C" uint64_t de_flush_seq(DEIndex *idx) {
  uint64_t seq = 0;

  guarded([&] { seq = idx->ext->get_flush_seq(); });
  return seq;
}

/* One TID with this key, if any: a range of a single key. */
extern "C" int de_lookup(DEIndex *idx, int64_t key, uint64_t *tid,
                         bool *found) {
  uint64_t *tids;
  size_t count;
  int rc = de_range(idx, key, key, &tids, &count);

  *found = (rc == 0 && count > 0);
  if (*found)
    *tid = tids[0];
  free(tids);
  return rc;
}

extern "C" const char *de_last_error(void) { return last_error.c_str(); }
