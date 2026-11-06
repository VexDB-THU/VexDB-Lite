#include "store/sqlite_record_reader.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (0)

namespace {
struct TempFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("vexdb-record-reader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    ~TempFile() {
        std::error_code e;
        for (auto suffix : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + suffix, e);
    }
};
struct Db {
    sqlite3 *db = nullptr;
    explicit Db(const std::filesystem::path &path,
                int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
        CHECK(sqlite3_open_v2(path.string().c_str(), &db, flags, nullptr) == SQLITE_OK);
        sqlite3_busy_timeout(db, 100);
    }
    ~Db() { sqlite3_close(db); }
    void Exec(const char *sql) {
        char *message = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
        const std::string error = message ? message : "";
        sqlite3_free(message);
        if (rc != SQLITE_OK) throw std::runtime_error(error);
    }
};
void Checkpoint(Db &db) {
    int log = -1, done = -1;
    CHECK(sqlite3_wal_checkpoint_v2(db.db, "main", SQLITE_CHECKPOINT_TRUNCATE,
                                   &log, &done) == SQLITE_OK);
}
std::shared_ptr<vexdb_sqlite::SqliteRecordReader> Reader(Db &db) {
    sqlite3_stmt *stmt = nullptr;
    CHECK(sqlite3_prepare_v2(db.db, "SELECT rowid FROM graph WHERE kind=?1 AND seg=?2",
                             -1, &stmt, nullptr) == SQLITE_OK);
    return std::make_shared<vexdb_sqlite::SqliteRecordReader>(db.db, stmt, "main", "graph");
}
void Expect(vexdb_sqlite::SqliteRecordReader &reader, int kind, unsigned seg,
            std::size_t offset, const std::string &value) {
    std::string got(value.size(), '\0');
    CHECK(reader.Read(kind, seg, offset, got.size(), got.data()));
    CHECK(got == value);
}
} // namespace

int main() {
    try {
        TempFile file;
        Db a(file.path), b(file.path);
        a.Exec("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
               "CREATE TABLE graph(kind INTEGER,seg INTEGER,data BLOB,UNIQUE(kind,seg));"
               "INSERT INTO graph(rowid,kind,seg,data) VALUES"
               "(0,3,0,CAST('0123456789abcdef' AS BLOB)),"
               "(100,4,1,CAST('abcdefghijklmnop' AS BLOB));");
        auto reader = Reader(a);
        for (int i = 0; i < 100; ++i) Expect(*reader, 3, 0, 1, "123");
        CHECK(reader->GetStats().opens == 100);
        CHECK(reader->GetStats().closes == 100);
        b.Exec("INSERT INTO graph VALUES(9,9,zeroblob(32));");
        Checkpoint(b); // An idle ordinary reader must not pin a WAL snapshot.

        a.Exec("BEGIN IMMEDIATE;");
        const auto opens = reader->GetStats().opens;
        {
            vexdb_sqlite::ScopedRecordReadReuse scope(reader);
            for (int i = 0; i < 1000; ++i) Expect(*reader, 3, 0, 4, "4567");
            CHECK(reader->GetStats().opens == opens + 1);
            Expect(*reader, 4, 1, 2, "cde"); // Reposition the same handle.
            CHECK(reader->GetStats().reopens > 0);
            a.Exec("UPDATE graph SET data=CAST('ZYXWVUTSRQPONMLK' AS BLOB) WHERE rowid=100;");
            Expect(*reader, 4, 1, 0, "ZYX"); // Expired handle must read new data.
            a.Exec("DELETE FROM graph WHERE kind=4 AND seg=1;"
                   "INSERT INTO graph(rowid,kind,seg,data) VALUES(300,4,1,CAST('new-value' AS BLOB));");
            Expect(*reader, 4, 1, 0, "new"); // Cached rowid must be refreshed.
            char out[3];
            CHECK(!reader->Read(99, 99, 0, sizeof(out), out));
            CHECK(!reader->Read(4, 1, 100, sizeof(out), out));
        }
        a.Exec("COMMIT;");
        CHECK(sqlite3_get_autocommit(a.db));
        b.Exec("INSERT INTO graph VALUES(8,8,zeroblob(32));");
        Checkpoint(b); // Scope exit must close retained handles before commit.

        const auto before = reader->GetStats().opens;
        Expect(*reader, 4, 1, 0, "new");
        Expect(*reader, 4, 1, 0, "new");
        CHECK(reader->GetStats().opens == before + 2); // Query behavior restored.

        a.Exec("BEGIN IMMEDIATE;");
        {
            vexdb_sqlite::ScopedRecordReadReuse scope(reader);
            a.Exec("UPDATE graph SET data=CAST('rolled-back' AS BLOB) WHERE rowid=300;");
            Expect(*reader, 4, 1, 0, "rolled");
        }
        a.Exec("ROLLBACK;");
        Expect(*reader, 4, 1, 0, "new");
        Checkpoint(b);
        {
            Db readonly(file.path, SQLITE_OPEN_READONLY);
            auto r = Reader(readonly);
            {
                vexdb_sqlite::ScopedRecordReadReuse scope(r);
                for (int i = 0; i < 100; ++i) Expect(*r, 4, 1, 0, "new");
            }
            // Keep both the read-only connection and reader alive/idle.
            b.Exec("INSERT INTO graph VALUES(6,6,zeroblob(32));");
            Checkpoint(b);
        }
        reader.reset();
        std::cout << "SQLite record reader: values, rowid0, replacement, expired BLOB, "
                     "WAL release, commit and rollback passed; 1000 scoped reads used one open\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
