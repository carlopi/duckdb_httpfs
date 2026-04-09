#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

enum class RangeStatus {
	NEEDS_VALIDATION, // copied from persistent cache, not yet checked this transaction
	VALIDATED,        // checked this transaction, data is fresh
	INVALID           // checked this transaction, data changed
};

//! A cached range of a bucket listing — the fundamental caching unit
struct BucketCacheRange {
	// --- Validation fields ---
	string start_after;
	idx_t count = 0;
	string max_last_modified;
	string last_key;
	timestamp_t snapshot_timestamp;

	// --- Cached file list ---
	vector<OpenFileInfo> files;

	// --- Transaction-scoped status ---
	RangeStatus status = RangeStatus::NEEDS_VALIDATION;
};

//! Per-bucket glob cache. Used both in ObjectCache (persistent) and ClientContext (transaction-scoped).
class BucketGlobCache {
public:
	//! Get ranges overlapping a key prefix (returns copies)
	vector<BucketCacheRange> GetOverlappingRanges(const string &key_prefix) {
		lock_guard<mutex> guard(lock);
		vector<BucketCacheRange> result;
		for (auto &range : ranges) {
			bool range_ends_after_prefix = range.last_key >= key_prefix;
			bool range_starts_in_prefix =
			    range.start_after.empty() || range.start_after <= key_prefix ||
			    range.start_after.substr(0, key_prefix.size()) == key_prefix;
			if (range_ends_after_prefix && range_starts_in_prefix) {
				result.push_back(range);
			}
		}
		return result;
	}

	//! Add/replace ranges
	void AddRanges(vector<BucketCacheRange> new_ranges) {
		lock_guard<mutex> guard(lock);
		for (auto &range : new_ranges) {
			InsertRangeLocked(std::move(range));
		}
	}

	//! Get cache data, applying default_status to VALIDATED ranges.
	//! VALIDATED → default_status, NEEDS_VALIDATION → NEEDS_VALIDATION, INVALID → skipped.
	vector<BucketCacheRange> GetCacheData(RangeStatus default_status) {
		lock_guard<mutex> guard(lock);
		vector<BucketCacheRange> result;
		for (auto &range : ranges) {
			if (range.status == RangeStatus::INVALID) {
				continue;
			}
			BucketCacheRange copy = range;
			if (copy.status == RangeStatus::VALIDATED) {
				copy.status = default_status;
			}
			result.push_back(std::move(copy));
		}
		return result;
	}

	//! Merge ranges from another BucketGlobCache into this one
	void MergeFrom(BucketGlobCache &other) {
		auto ranges_to_merge = other.GetCacheData(RangeStatus::NEEDS_VALIDATION);
		AddRanges(std::move(ranges_to_merge));
	}

	//! Update status of a range identified by start_after
	void SetRangeStatus(const string &start_after, RangeStatus new_status) {
		lock_guard<mutex> guard(lock);
		for (auto &range : ranges) {
			if (range.start_after == start_after) {
				range.status = new_status;
				break;
			}
		}
	}

	//! Check if cache has any ranges
	bool IsEmpty() {
		lock_guard<mutex> guard(lock);
		return ranges.empty();
	}

private:
	void InsertRangeLocked(BucketCacheRange range) {
		// Remove ranges fully covered by the new range
		auto it = ranges.begin();
		while (it != ranges.end()) {
			if (it->start_after >= range.start_after && it->last_key <= range.last_key) {
				it = ranges.erase(it);
			} else {
				++it;
			}
		}
		// Insert in sorted order by start_after
		auto insert_pos = ranges.begin();
		while (insert_pos != ranges.end() && insert_pos->start_after < range.start_after) {
			++insert_pos;
		}
		ranges.insert(insert_pos, std::move(range));
	}

	mutex lock;
	vector<BucketCacheRange> ranges;
};

//! ObjectCache entry wrapping a BucketGlobCache for persistent storage
class BucketGlobCacheEntry : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "bucket_glob_cache";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// Non-evictable for now
		return optional_idx();
	}

	BucketGlobCache cache;
};

//! Transaction-scoped glob cache state, registered on ClientContext
class GlobCacheState : public ClientContextState {
public:
	//! Get or create a transaction-scoped BucketGlobCache for a bucket.
	//! On first access, copies from the persistent ObjectCache instance.
	BucketGlobCache &GetBucketCache(const string &bucket, ClientContext &context) {
		auto it = bucket_caches.find(bucket);
		if (it != bucket_caches.end()) {
			return it->second;
		}
		// First access this transaction — copy from persistent cache
		auto &obj_cache = ObjectCache::GetObjectCache(context);
		auto persistent = obj_cache.Get<BucketGlobCacheEntry>(bucket);
		auto &local = bucket_caches[bucket];
		if (persistent) {
			auto data = persistent->cache.GetCacheData(RangeStatus::NEEDS_VALIDATION);
			local.AddRanges(std::move(data));
		}
		return local;
	}

	//! On transaction commit — merge validated ranges back to persistent cache
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		MergeBack(context);
	}

	//! On transaction rollback — still merge, ranges are valid observations
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		MergeBack(context);
	}

private:
	void MergeBack(ClientContext &context) {
		auto &obj_cache = ObjectCache::GetObjectCache(context);
		for (auto &entry : bucket_caches) {
			auto persistent = obj_cache.GetOrCreate<BucketGlobCacheEntry>(entry.first);
			persistent->cache.MergeFrom(entry.second);
		}
		bucket_caches.clear();
	}

	unordered_map<string, BucketGlobCache> bucket_caches;
};

} // namespace duckdb
