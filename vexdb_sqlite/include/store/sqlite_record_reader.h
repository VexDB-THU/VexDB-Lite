#ifndef VEXDB_SQLITE_RECORD_READER_H
#define VEXDB_SQLITE_RECORD_READER_H

#include "sqlite3.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace vexdb_sqlite {

class ScopedRecordReadReuse;

// Owns the lookup statement and incremental BLOB handle for one graph table.
// Ordinary reads release the handle immediately. A bounded write operation
// may reuse it, provided its scope ends before returning to SQLite's caller.
class SqliteRecordReader {
public:
    struct Stats {
        std::uint64_t opens = 0;
        std::uint64_t reopens = 0;
        std::uint64_t reads = 0;
        std::uint64_t closes = 0;
    };

    SqliteRecordReader(sqlite3 *db, sqlite3_stmt *lookup, std::string schema,
                       std::string table)
        : db_(db), lookup_(lookup), schema_(std::move(schema)), table_(std::move(table)) {}
    ~SqliteRecordReader() {
        CloseBlob();
        sqlite3_finalize(lookup_);
    }
    SqliteRecordReader(const SqliteRecordReader &) = delete;
    SqliteRecordReader &operator=(const SqliteRecordReader &) = delete;

    const Stats &GetStats() const { return stats_; }

    bool Read(int kind, std::uint32_t seg, std::size_t offset, std::size_t len,
              char *dst) {
        if (!lookup_ || offset > INT_MAX || len > INT_MAX) return false;
        const auto key = (std::uint64_t(std::uint32_t(kind)) << 32) | seg;
        for (int pass = 0; pass < 2; ++pass) {
            sqlite3_int64 rowid;
            auto found = rowids_.find(key);
            if (pass == 0 && found != rowids_.end()) {
                rowid = found->second;
            } else {
                sqlite3_bind_int(lookup_, 1, kind);
                sqlite3_bind_int64(lookup_, 2, seg);
                const int rc = sqlite3_step(lookup_);
                if (rc != SQLITE_ROW) {
                    sqlite3_reset(lookup_);
                    CloseBlob();
                    return false;
                }
                rowid = sqlite3_column_int64(lookup_, 0);
                sqlite3_reset(lookup_);
                rowids_[key] = rowid;
            }
            if (blob_ && blob_rowid_ != rowid) {
                ++stats_.reopens;
                if (sqlite3_blob_reopen(blob_, rowid) != SQLITE_OK) {
                    CloseBlob();
                    continue; // Refresh a rowid invalidated by replacement.
                }
                blob_rowid_ = rowid;
            }
            if (!blob_) {
                ++stats_.opens;
                if (sqlite3_blob_open(db_, schema_.c_str(), table_.c_str(), "data",
                                      rowid, 0, &blob_) != SQLITE_OK) {
                    CloseBlob();
                    continue;
                }
                blob_rowid_ = rowid;
            }
            ++stats_.reads;
            const int rc = sqlite3_blob_read(blob_, dst, int(len), int(offset));
            if (!reuse_ || rc != SQLITE_OK) CloseBlob();
            if (rc == SQLITE_OK) return true;
            // UPDATE/DELETE expires a handle (SQLITE_ABORT). Refresh the
            // lookup and open a new handle; never return the previous bytes.
        }
        return false;
    }

private:
    friend class ScopedRecordReadReuse;
    void CloseBlob() {
        if (blob_) {
            sqlite3_blob_close(blob_);
            blob_ = nullptr;
            ++stats_.closes;
        }
    }
    bool BeginReuse() {
        const bool previous = reuse_;
        reuse_ = true;
        return previous;
    }
    void FinishReuse(bool previous) {
        CloseBlob();
        reuse_ = previous;
    }

    sqlite3 *db_;
    sqlite3_stmt *lookup_;
    std::string schema_, table_;
    std::unordered_map<std::uint64_t, sqlite3_int64> rowids_;
    sqlite3_blob *blob_ = nullptr;
    sqlite3_int64 blob_rowid_ = 0;
    bool reuse_ = false;
    Stats stats_;
};

class ScopedRecordReadReuse {
public:
    explicit ScopedRecordReadReuse(std::shared_ptr<SqliteRecordReader> reader)
        : reader_(std::move(reader)), previous_(reader_ && reader_->BeginReuse()) {}
    ~ScopedRecordReadReuse() {
        if (reader_) reader_->FinishReuse(previous_);
    }
    ScopedRecordReadReuse(const ScopedRecordReadReuse &) = delete;
    ScopedRecordReadReuse &operator=(const ScopedRecordReadReuse &) = delete;
private:
    std::shared_ptr<SqliteRecordReader> reader_;
    bool previous_;
};

} // namespace vexdb_sqlite

#endif
