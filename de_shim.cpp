/*
 * de_shim.cpp
 *		C++ half of the shim between the dynamic_extension index AM and the
 *		dynamic extension framework (DynamicExtension over the external
 *		ISAM shard, queried with the library's de::pl point lookup).
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
#include <string>
#include <vector>

#include <unistd.h>

#include "framework/DynamicExtension.h"
#include "query/pointlookup.h"
#include "shard/ExternalISAMTree.h"

namespace
{

/*
 * Index entry: the int4 key widened to int64, and the heap TID packed into
 * 64 bits (never 0, since offset numbers start at 1).
 *
 * A record's identity is its key AND its TID: several rows can share a key.
 * Lookups go through range_scan, which groups records with operator==, so
 * comparing only the key would merge different rows with the same key.
 */
struct PgRec
{
	int64_t key;
	uint64_t value;

	bool operator<(const PgRec &other) const
	{
		return key < other.key || (key == other.key && value < other.value);
	}

	bool operator==(const PgRec &other) const
	{
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

/* Buffer and layout parameters for the framework. */
constexpr size_t DE_BUFFER_LWM = 1000;
constexpr size_t DE_BUFFER_HWM = 2000;
constexpr size_t DE_SCALE_FACTOR = 8;

/*
 * Leveling, not tiering: after a checkpoint reload, InternalLevel is sized
 * to the number of shards it had, so a tiered level has no free slot for the
 * next flush.  Serial scheduler, because a backend must not run work on
 * background threads.
 */
typedef de::DynamicExtension<Shard, Query, de::LayoutPolicy::LEVELING,
							 de::DeletePolicy::TOMBSTONE, de::SerialScheduler>
	Extension;

std::string last_error;

template <typename F>
int guarded(F &&f)
{
	try
	{
		f();
		return 0;
	}
	catch (const std::exception &e)
	{
		last_error = e.what();
	}
	catch (...)
	{
		last_error = "unknown C++ exception";
	}
	return -1;
}

/*
 * The framework takes no directory argument: it keeps checkpoint.json,
 * wal.log, shards/ and tmp_shards/ relative to the working directory.  But
 * a Postgres backend must stay in the data directory, because Postgres opens
 * its own files (base/..., pg_wal/...) by relative path too.  So step into
 * the index directory only for the length of one call, and always step back.
 */
template <typename F>
int
in_dir(const std::string &dir, F &&f)
{
	char	   *old_dir = getcwd(nullptr, 0);

	if (old_dir == nullptr)
	{
		last_error = std::string("cannot read working directory: ") + strerror(errno);
		return -1;
	}

	int			rc;

	if (chdir(dir.c_str()) != 0)
	{
		last_error = std::string("cannot enter index directory: ") + strerror(errno);
		rc = -1;
	}
	else
	{
		rc = guarded(f);
		if (chdir(old_dir) != 0)
		{
			last_error = std::string("cannot return to working directory: ") + strerror(errno);
			rc = -1;
		}
	}

	free(old_dir);
	return rc;
}

}								/* namespace */

struct DEIndex
{
	Extension  *ext;
	std::byte  *lookup_buf;
	std::string dir;
};

extern "C" int
de_create(const char *dir)
{
	return guarded([&] {
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
	});
}

extern "C" DEIndex *
de_open(const char *dir)
{
	DEIndex    *idx = nullptr;

	int rc = guarded([&] {
		idx = new DEIndex{nullptr, nullptr, dir};
		psudb::sf_aligned_alloc(PAGE_SZ, LOOKUP_BUF_SZ, &idx->lookup_buf);
		if (idx->lookup_buf == nullptr)
			throw std::bad_alloc();
	});

	if (rc == 0)
		rc = in_dir(idx->dir, [&] {
			idx->ext = new Extension(DE_BUFFER_LWM, DE_BUFFER_HWM, DE_SCALE_FACTOR, 0, 1, false);
		});

	if (rc != 0)
	{
		if (idx)
			free(idx->lookup_buf);
		delete idx;
		return nullptr;
	}
	return idx;
}

extern "C" void
de_close(DEIndex *idx)
{
	if (idx == nullptr)
		return;

	/*
	 * Closing is not dropping: keep the shard files that checkpoint.json
	 * lists, or the next de_open() finds nothing to load.
	 */
	in_dir(idx->dir, [&] {
		de::isam_keep_files = true;
		delete idx->ext;
	});
	de::isam_keep_files = false;
	free(idx->lookup_buf);
	delete idx;
}

extern "C" int
de_insert(DEIndex *idx, int64_t key, uint64_t tid)
{
	return in_dir(idx->dir, [&] {
		/* returns 0 only when the buffer is at its high watermark */
		if (!idx->ext->insert({key, tid}))
			throw std::runtime_error("dynamic extension buffer is full");
	});
}

/*
 * Every TID whose key is in [low, high], in key order.  *tids is a malloc'd
 * array the caller must free(), even when *count is 0.
 */
extern "C" int
de_range(DEIndex *idx, int64_t low, int64_t high, uint64_t **tids, size_t *count)
{
	*tids = nullptr;
	*count = 0;

	return in_dir(idx->dir, [&] {
		std::vector<uint64_t> found;
		auto		it = idx->ext->range_scan({low, 0}, {high, 0});

		while (it.has_next())
			found.push_back(it.next().value);

		/* +1 so an empty result is still a real allocation */
		*tids = (uint64_t *) malloc(found.size() * sizeof(uint64_t) + 1);
		if (*tids == nullptr)
			throw std::bad_alloc();
		memcpy(*tids, found.data(), found.size() * sizeof(uint64_t));
		*count = found.size();
	});
}

/* One TID with this key, if any: a range of a single key. */
extern "C" int
de_lookup(DEIndex *idx, int64_t key, uint64_t *tid, bool *found)
{
	uint64_t   *tids;
	size_t		count;
	int			rc = de_range(idx, key, key, &tids, &count);

	*found = (rc == 0 && count > 0);
	if (*found)
		*tid = tids[0];
	free(tids);
	return rc;
}

extern "C" const char *
de_last_error(void)
{
	return last_error.c_str();
}
