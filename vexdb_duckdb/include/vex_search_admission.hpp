#pragma once

#include "vex_search_queue.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/object_cache.hpp"
#include <thread>

namespace duckdb {

class VexSearchAdmission : public ObjectCacheEntry {
public:
    explicit VexSearchAdmission(DatabaseInstance &db)
        : concurrency_index(SettingIndex(db, "vexdb_search_concurrency")),
          timeout_index(SettingIndex(db, "vexdb_search_queue_timeout_ms")) {}
    static const char *ObjectType() { return "vexdb_search_admission_v1"; }
    string GetObjectType() override { return ObjectType(); }
    optional_idx GetEstimatedCacheMemory() const override { return optional_idx(); }

    static shared_ptr<VexSearchAdmission> Get(DatabaseInstance &db) {
        auto result = db.GetObjectCache().GetOrCreate<VexSearchAdmission>(ObjectType(), db);
        if (!result) throw InternalException("VexDB search queue cache entry has an incompatible type");
        return result;
    }

    uint64_t GetLimit(DatabaseInstance &db) const {
        Value value;
        int64_t limit = 0;
        if (db.config.user_settings.TryGetSetting(concurrency_index, value)) {
            if (value.IsNull()) throw InvalidInputException("vexdb_search_concurrency cannot be NULL");
            limit = value.GetValue<int64_t>();
        }
        if (limit < -1) throw InvalidInputException("vexdb_search_concurrency must be -1, 0 or a positive integer");
        return limit == -1 ? cpu_count : uint64_t(limit);
    }

    VexSearchQueue::Permit Acquire(ClientContext &context) {
        Value timeout;
        uint64_t timeout_ms = 0;
        if (context.config.user_settings.TryGetSetting(context.db->config.user_settings, timeout_index, timeout)) {
            if (timeout.IsNull()) throw InvalidInputException("vexdb_search_queue_timeout_ms cannot be NULL");
            timeout_ms = timeout.GetValue<uint64_t>();
        }
        try {
            return queue.Acquire([&] { return GetLimit(*context.db); }, [&] { return context.IsInterrupted(); }, timeout_ms);
        } catch (const VexSearchQueue::Cancelled &) {
            throw InterruptException();
        } catch (const VexSearchQueue::Timeout &) {
            throw InvalidInputException("VexDB search queue wait timed out (vexdb_search_queue_timeout_ms)");
        }
    }

    VexSearchQueue queue;

private:
    static idx_t SettingIndex(DatabaseInstance &db, const string &name) {
        ExtensionOption option;
        if (!db.config.TryGetExtensionOption(name, option) || !option.setting_index.IsValid()) {
            throw InternalException("VexDB search queue setting is not registered: %s", name);
        }
        return option.setting_index.GetIndex();
    }
    const idx_t concurrency_index, timeout_index;
    // A hardware hint, independent of the session's DuckDB worker setting.
    // Administrators can override it for quotas or other work on the machine.
    const uint64_t cpu_count = std::max(1u, std::thread::hardware_concurrency());
};

} // namespace duckdb
