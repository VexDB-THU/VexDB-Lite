#import "VexDBBridge.h"

#include <sqlite3.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vexdb_sqlite.h"

@interface VexSearchResult ()
@property(nonatomic, readwrite) NSInteger rowID;
@property(nonatomic, copy, readwrite) NSString *title;
@property(nonatomic, copy, readwrite) NSString *category;
@property(nonatomic, readwrite) double distance;
@end

@implementation VexSearchResult
@end

@interface VexDemoSnapshot ()
@property(nonatomic, readwrite) BOOL success;
@property(nonatomic, copy, readwrite) NSString *message;
@property(nonatomic, copy, readwrite) NSString *mode;
@property(nonatomic, copy, readwrite) NSString *version;
@property(nonatomic, copy, readwrite) NSString *queryPlan;
@property(nonatomic, readwrite) NSInteger rowCount;
@property(nonatomic, readwrite) long long databaseBytes;
@property(nonatomic, readwrite) double buildMilliseconds;
@property(nonatomic, readwrite) double queryMilliseconds;
@property(nonatomic, copy, readwrite) NSArray<VexSearchResult *> *results;
@end

@implementation VexDemoSnapshot
@end

namespace {

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

NSString *SQLiteMessage(sqlite3 *db, NSString *context) {
    const char *detail = db ? sqlite3_errmsg(db) : "database is not open";
    return [NSString stringWithFormat:@"%@: %s", context, detail ?: "unknown error"];
}

struct Preset {
    const char *key;
    const char *title;
    float values[8];
};

constexpr Preset kPresets[] = {
    {"focus", "专注工作", {0.92f, 0.81f, 0.18f, 0.12f, 0.38f, 0.22f, 0.14f, 0.31f}},
    {"nature", "自然放松", {0.12f, 0.24f, 0.94f, 0.85f, 0.28f, 0.18f, 0.33f, 0.19f}},
    {"energy", "运动能量", {0.31f, 0.18f, 0.16f, 0.22f, 0.95f, 0.86f, 0.42f, 0.37f}},
    {"city", "城市探索", {0.28f, 0.37f, 0.19f, 0.16f, 0.34f, 0.45f, 0.93f, 0.82f}},
    {"coffee", "咖啡时刻", {0.84f, 0.63f, 0.28f, 0.21f, 0.37f, 0.32f, 0.48f, 0.29f}},
    {"music", "音乐灵感", {0.62f, 0.91f, 0.22f, 0.31f, 0.48f, 0.72f, 0.25f, 0.46f}},
};

const Preset &FindPreset(NSString *key) {
    for (const auto &preset : kPresets) {
        if ([key isEqualToString:[NSString stringWithUTF8String:preset.key]]) return preset;
    }
    return kPresets[0];
}

std::string VectorJSON(const float *values) {
    char buf[256];
    int used = std::snprintf(buf, sizeof(buf), "[");
    for (int d = 0; d < 8; d++) {
        used += std::snprintf(buf + used, sizeof(buf) - size_t(used),
                             "%s%.6f", d ? "," : "", values[d]);
    }
    std::snprintf(buf + used, sizeof(buf) - size_t(used), "]");
    return buf;
}

bool VectorJSON(NSArray<NSNumber *> *values, std::string *json) {
    json->clear();
    json->push_back('[');
    char number[64];
    for (NSUInteger i = 0; i < values.count; i++) {
        double value = values[i].doubleValue;
        if (!std::isfinite(value)) return false;
        if (i) json->push_back(',');
        int written = std::snprintf(number, sizeof(number), "%.9g", value);
        if (written <= 0 || written >= int(sizeof(number))) return false;
        json->append(number, size_t(written));
    }
    json->push_back(']');
    return true;
}

constexpr size_t kMaxTextVectorValues = 2'500'000;

bool FloatVectorJSON(const uint8_t *bytes, size_t dimensions, std::string *json) {
    json->clear();
    json->reserve(dimensions * 12 + 2);
    json->push_back('[');
    char number[64];
    for (size_t i = 0; i < dimensions; i++) {
        float value = 0;
        std::memcpy(&value, bytes + i * sizeof(float), sizeof(value));
        if (!std::isfinite(value)) return false;
        if (i) json->push_back(',');
        int written = std::snprintf(number, sizeof(number), "%.9g", double(value));
        if (written <= 0 || written >= int(sizeof(number))) return false;
        json->append(number, size_t(written));
    }
    json->push_back(']');
    return true;
}

NSString *MediaTableForScope(NSString *scope) {
    if ([scope isEqualToString:@"bundled"]) return @"bundled_media_vectors";
    if ([scope isEqualToString:@"user"]) return @"user_media_vectors";
    return nil;
}

}  // namespace

@interface VexDBBridge () {
    sqlite3 *_db;
    NSString *_mode;
}
@property(nonatomic, copy, readwrite) NSString *databasePath;
- (NSString *)queryPlanForVector:(const std::string &)query table:(NSString *)table;
@end

@implementation VexDBBridge

- (instancetype)initWithDatabasePath:(NSString *)databasePath {
    self = [super init];
    if (!self) return nil;
    _databasePath = [databasePath copy];
    _mode = @"plain";
    [self openDatabase];
    return self;
}

- (void)dealloc {
    if (_db) sqlite3_close_v2(_db);
}

- (BOOL)openDatabase {
    if (_db) return YES;
    int rc = sqlite3_open_v2(self.databasePath.fileSystemRepresentation, &_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) return NO;
    if (vexdb_sqlite_register(_db) != SQLITE_OK) return NO;
    sqlite3_busy_timeout(_db, 3000);
    return [self exec:@"PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"];
}

- (BOOL)exec:(NSString *)sql {
    char *error = nullptr;
    int rc = sqlite3_exec(_db, sql.UTF8String, nullptr, nullptr, &error);
    if (error) sqlite3_free(error);
    return rc == SQLITE_OK;
}

- (VexDemoSnapshot *)errorSnapshot:(NSString *)message {
    VexDemoSnapshot *snapshot = [VexDemoSnapshot new];
    snapshot.success = NO;
    snapshot.message = message;
    snapshot.mode = _mode ?: @"unknown";
    snapshot.version = @"";
    snapshot.queryPlan = @"";
    snapshot.results = @[];
    return snapshot;
}

- (VexDemoSnapshot *)rebuildWithMode:(NSString *)mode {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    auto started = Clock::now();

    NSString *options = nil;
    if ([mode isEqualToString:@"pq"]) {
        options = @"quantizer=pq, pq_m=4, memory_mode=compact";
    } else if ([mode isEqualToString:@"rabitq"]) {
        options = @"quantizer=rabitq, memory_mode=compact";
    } else {
        mode = @"plain";
        options = @"quantizer=none, memory_mode=full";
    }
    _mode = [mode copy];

    if (![self exec:@"DROP TABLE IF EXISTS demo_vectors;"]) {
        return [self errorSnapshot:SQLiteMessage(_db, @"清理旧索引失败")];
    }
    NSString *create = [NSString stringWithFormat:
        @"CREATE VIRTUAL TABLE demo_vectors USING GRAPH_INDEX("
         "embedding FLOAT[8], title TEXT, category TEXT, metric=l2, "
         "m=16, ef_construction=160, ef_search=128, brute_force_threshold=0, %@);",
        options];
    if (![self exec:create]) return [self errorSnapshot:SQLiteMessage(_db, @"创建向量表失败")];
    if (![self exec:@"BEGIN IMMEDIATE;"]) return [self errorSnapshot:SQLiteMessage(_db, @"开始事务失败")];

    sqlite3_stmt *insert = nullptr;
    const char *sql = "INSERT INTO demo_vectors(rowid,title,category,embedding) VALUES(?,?,?,?)";
    if (sqlite3_prepare_v2(_db, sql, -1, &insert, nullptr) != SQLITE_OK) {
        [self exec:@"ROLLBACK;"];
        return [self errorSnapshot:SQLiteMessage(_db, @"准备写入失败")];
    }

    BOOL inserted = YES;
    sqlite3_int64 rowID = 1;
    for (int group = 0; group < 6 && inserted; group++) {
        const Preset &preset = kPresets[group];
        NSString *category = [NSString stringWithUTF8String:preset.title];
        for (int sample = 0; sample < 100; sample++, rowID++) {
            float vector[8];
            for (int d = 0; d < 8; d++) {
                int code = (sample * 37 + d * 17 + group * 11) % 101;
                vector[d] = preset.values[d] + float(code - 50) / 2500.0f;
            }
            std::string json = VectorJSON(vector);
            NSString *title = [NSString stringWithFormat:@"%@ · %03d", category, sample + 1];
            sqlite3_bind_int64(insert, 1, rowID);
            sqlite3_bind_text(insert, 2, title.UTF8String, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 3, category.UTF8String, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 4, json.c_str(), -1, SQLITE_TRANSIENT);
            inserted = sqlite3_step(insert) == SQLITE_DONE;
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
            if (!inserted) break;
        }
    }
    sqlite3_finalize(insert);
    if (!inserted || ![self exec:@"COMMIT;"]) {
        [self exec:@"ROLLBACK;"];
        return [self errorSnapshot:SQLiteMessage(_db, @"写入演示向量失败")];
    }

    VexDemoSnapshot *snapshot = [self searchWithPreset:@"focus" limit:5];
    snapshot.buildMilliseconds = MillisecondsSince(started);
    snapshot.message = snapshot.success ? @"600 条向量已在设备本地建立索引" : snapshot.message;
    return snapshot;
}

- (VexDemoSnapshot *)searchWithPreset:(NSString *)presetKey limit:(NSInteger)limit {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    const Preset &preset = FindPreset(presetKey);
    std::string query = VectorJSON(preset.values);
    auto started = Clock::now();

    sqlite3_stmt *statement = nullptr;
    const char *sql =
        "SELECT rowid,title,category,distance FROM demo_vectors "
        "WHERE embedding MATCH ? AND k=? ORDER BY distance";
    if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return [self errorSnapshot:SQLiteMessage(_db, @"准备向量查询失败")];
    }
    sqlite3_bind_text(statement, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, int(limit));

    NSMutableArray<VexSearchResult *> *results = [NSMutableArray array];
    int stepRC = SQLITE_OK;
    while ((stepRC = sqlite3_step(statement)) == SQLITE_ROW) {
        VexSearchResult *result = [VexSearchResult new];
        result.rowID = NSInteger(sqlite3_column_int64(statement, 0));
        const char *title = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
        const char *category = reinterpret_cast<const char *>(sqlite3_column_text(statement, 2));
        result.title = title ? [NSString stringWithUTF8String:title] : @"未命名";
        result.category = category ? [NSString stringWithUTF8String:category] : @"";
        result.distance = sqlite3_column_double(statement, 3);
        [results addObject:result];
    }
    sqlite3_finalize(statement);
    if (stepRC != SQLITE_DONE) {
        return [self errorSnapshot:SQLiteMessage(_db, @"执行向量查询失败")];
    }

    VexDemoSnapshot *snapshot = [self baseSnapshot];
    snapshot.success = YES;
    snapshot.message = @"检索完全在本机完成";
    snapshot.queryMilliseconds = MillisecondsSince(started);
    snapshot.results = results;
    snapshot.queryPlan = [self queryPlanForVector:query];
    return snapshot;
}

- (VexDemoSnapshot *)reopenAndSearchWithPreset:(NSString *)preset limit:(NSInteger)limit {
    if (_db) {
        sqlite3_close_v2(_db);
        _db = nullptr;
    }
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"重新打开失败")];
    VexDemoSnapshot *snapshot = [self searchWithPreset:preset limit:limit];
    if (snapshot.success) snapshot.message = @"关闭并重开成功，索引仍可查询";
    return snapshot;
}

- (VexDemoSnapshot *)importUserChunks:(NSArray<NSString *> *)chunks
                        embeddingData:(NSData *)embeddingData
                           dimensions:(NSInteger)dimensions {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    if (chunks.count == 0 || chunks.count > 5000) {
        return [self errorSnapshot:@"导入失败：文本分片数量不正确"];
    }
    if (dimensions <= 0 || dimensions > 8192) {
        return [self errorSnapshot:@"导入失败：Embedding 向量维度必须在 1 到 8192 之间"];
    }
    const size_t dimensionCount = size_t(dimensions);
    if (chunks.count > kMaxTextVectorValues / dimensionCount) {
        return [self errorSnapshot:@"导入失败：分片数量与向量维度的组合超过手机安全上限"];
    }
    const size_t vectorValues = size_t(chunks.count) * dimensionCount;
    const size_t expectedBytes = vectorValues * sizeof(float);
    if (embeddingData.length != expectedBytes) {
        return [self errorSnapshot:@"导入失败：Float32 向量数据长度不正确"];
    }
    const auto *bytes = static_cast<const uint8_t *>(embeddingData.bytes);
    for (size_t i = 0; i < vectorValues; i++) {
        float value = 0;
        std::memcpy(&value, bytes + i * sizeof(float), sizeof(value));
        if (!std::isfinite(value)) {
            return [self errorSnapshot:@"导入失败：Embedding API 返回了无效数值"];
        }
    }

    auto started = Clock::now();
    if (![self exec:@"BEGIN IMMEDIATE;"]) {
        return [self errorSnapshot:SQLiteMessage(_db, @"开始导入事务失败")];
    }
    BOOL completed = [self exec:@"DROP TABLE IF EXISTS user_vectors;"];
    if (completed) {
        NSString *create = [NSString stringWithFormat:
            @"CREATE VIRTUAL TABLE user_vectors USING GRAPH_INDEX("
             "embedding FLOAT[%lu], content TEXT, metric=l2, m=16, "
             "ef_construction=160, ef_search=128, brute_force_threshold=0, "
             "quantizer=none, memory_mode=full);",
            (unsigned long)dimensionCount];
        completed = [self exec:create];
    }

    sqlite3_stmt *insert = nullptr;
    if (completed) {
        completed = sqlite3_prepare_v2(
            _db, "INSERT INTO user_vectors(rowid,content,embedding) VALUES(?,?,?)",
            -1, &insert, nullptr) == SQLITE_OK;
    }
    for (NSUInteger i = 0; completed && i < chunks.count; i++) {
        std::string vector;
        completed = FloatVectorJSON(bytes + size_t(i) * dimensionCount * sizeof(float),
                                    dimensionCount, &vector);
        if (!completed) break;
        sqlite3_bind_int64(insert, 1, sqlite3_int64(i + 1));
        sqlite3_bind_text(insert, 2, chunks[i].UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, vector.c_str(), -1, SQLITE_TRANSIENT);
        completed = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
    }
    if (insert) sqlite3_finalize(insert);

    if (!completed) {
        NSString *failure = SQLiteMessage(_db, @"写入用户文本索引失败");
        [self exec:@"ROLLBACK;"];
        return [self errorSnapshot:failure];
    }
    if (![self exec:@"COMMIT;"]) {
        NSString *failure = SQLiteMessage(_db, @"提交用户文本索引失败");
        [self exec:@"ROLLBACK;"];
        return [self errorSnapshot:failure];
    }

    VexDemoSnapshot *snapshot = [self userIndexStatus];
    snapshot.success = YES;
    snapshot.mode = @"user";
    snapshot.buildMilliseconds = MillisecondsSince(started);
    snapshot.message = [NSString stringWithFormat:@"%lu 个文本分片已写入本地向量索引",
                                                  (unsigned long)chunks.count];
    return snapshot;
}

- (VexDemoSnapshot *)searchUserWithEmbedding:(NSArray<NSNumber *> *)embedding
                                        limit:(NSInteger)limit {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    if (embedding.count == 0 || limit < 1 || limit > 100) {
        return [self errorSnapshot:@"查询失败：查询向量或返回数量不正确"];
    }
    std::string query;
    if (!VectorJSON(embedding, &query)) {
        return [self errorSnapshot:@"查询失败：Embedding API 返回了无效向量"];
    }

    auto started = Clock::now();
    sqlite3_stmt *statement = nullptr;
    const char *sql =
        "SELECT rowid,content,distance FROM user_vectors "
        "WHERE embedding MATCH ? AND k=? ORDER BY distance";
    if (sqlite3_prepare_v2(_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return [self errorSnapshot:SQLiteMessage(_db, @"准备用户文本查询失败")];
    }
    sqlite3_bind_text(statement, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, int(limit));

    NSMutableArray<VexSearchResult *> *results = [NSMutableArray array];
    int stepRC = SQLITE_OK;
    while ((stepRC = sqlite3_step(statement)) == SQLITE_ROW) {
        sqlite3_int64 rowID = sqlite3_column_int64(statement, 0);
        const char *content = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
        VexSearchResult *result = [VexSearchResult new];
        result.rowID = NSInteger(rowID);
        result.title = [NSString stringWithFormat:@"文本片段 %lld", rowID];
        result.category = content ? [NSString stringWithUTF8String:content] : @"";
        result.distance = sqlite3_column_double(statement, 2);
        [results addObject:result];
    }
    sqlite3_finalize(statement);
    if (stepRC != SQLITE_DONE) {
        return [self errorSnapshot:SQLiteMessage(_db, @"执行用户文本查询失败")];
    }

    VexDemoSnapshot *snapshot = [self userIndexStatus];
    snapshot.success = YES;
    snapshot.mode = @"user";
    snapshot.message = @"查询向量已在本机完成相似度检索";
    snapshot.queryMilliseconds = MillisecondsSince(started);
    snapshot.results = results;
    snapshot.queryPlan = [self queryPlanForVector:query table:@"user_vectors"];
    return snapshot;
}

- (VexDemoSnapshot *)userIndexStatus {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    VexDemoSnapshot *snapshot = [self baseSnapshot];
    snapshot.mode = @"user";
    snapshot.rowCount = 0;
    sqlite3_stmt *statement = nullptr;
    int rc = sqlite3_prepare_v2(_db, "SELECT count(*) FROM user_vectors", -1,
                                &statement, nullptr);
    if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        snapshot.success = YES;
        snapshot.rowCount = NSInteger(sqlite3_column_int64(statement, 0));
        snapshot.message = snapshot.rowCount > 0 ? @"已找到本地用户文本索引" : @"用户文本索引为空";
    } else {
        snapshot.success = YES;
        snapshot.message = @"尚未导入用户文本";
    }
    if (statement) sqlite3_finalize(statement);
    return snapshot;
}

- (VexDemoSnapshot *)importMediaWithScope:(NSString *)scope
                                   labels:(NSArray<NSString *> *)labels
                               embeddings:(NSArray<NSArray<NSNumber *> *> *)embeddings {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    NSString *table = MediaTableForScope(scope);
    if (!table) return [self errorSnapshot:@"导入失败：未知的图片索引类型"];
    if (labels.count == 0 || labels.count != embeddings.count || labels.count > 5000)
        return [self errorSnapshot:@"导入失败：图片和向量数量不正确"];
    NSUInteger dimensions = embeddings.firstObject.count;
    if (dimensions == 0 || dimensions > 8192)
        return [self errorSnapshot:@"导入失败：图片 Embedding 向量维度必须在 1 到 8192 之间"];
    for (NSArray<NSNumber *> *embedding in embeddings) {
        if (embedding.count != dimensions) return [self errorSnapshot:@"导入失败：图片向量维度不一致"];
        std::string ignored;
        if (!VectorJSON(embedding, &ignored)) return [self errorSnapshot:@"导入失败：图片向量包含无效数值"];
    }
    auto started = Clock::now();
    if (![self exec:@"BEGIN IMMEDIATE;"]) return [self errorSnapshot:SQLiteMessage(_db, @"开始导入事务失败")];
    BOOL completed = [self exec:[NSString stringWithFormat:@"DROP TABLE IF EXISTS %@;", table]];
    if (completed) {
        NSString *create = [NSString stringWithFormat:
            @"CREATE VIRTUAL TABLE %@ USING GRAPH_INDEX(embedding FLOAT[%lu], label TEXT, metric=l2, m=16, ef_construction=160, ef_search=128, brute_force_threshold=0, quantizer=none, memory_mode=full);",
            table, (unsigned long)dimensions];
        completed = [self exec:create];
    }
    sqlite3_stmt *insert = nullptr;
    NSString *insertSQL = [NSString stringWithFormat:
        @"INSERT INTO %@(rowid,label,embedding) VALUES(?,?,?)", table];
    if (completed) completed = sqlite3_prepare_v2(_db, insertSQL.UTF8String, -1, &insert, nullptr) == SQLITE_OK;
    for (NSUInteger i = 0; completed && i < labels.count; i++) {
        std::string vector; completed = VectorJSON(embeddings[i], &vector); if (!completed) break;
        sqlite3_bind_int64(insert, 1, sqlite3_int64(i + 1));
        sqlite3_bind_text(insert, 2, labels[i].UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, vector.c_str(), -1, SQLITE_TRANSIENT);
        completed = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_reset(insert); sqlite3_clear_bindings(insert);
    }
    if (insert) sqlite3_finalize(insert);
    if (!completed) { NSString *failure = SQLiteMessage(_db, @"写入图片索引失败"); [self exec:@"ROLLBACK;"]; return [self errorSnapshot:failure]; }
    if (![self exec:@"COMMIT;"]) { NSString *failure = SQLiteMessage(_db, @"提交图片索引失败"); [self exec:@"ROLLBACK;"]; return [self errorSnapshot:failure]; }
    VexDemoSnapshot *snapshot = [self mediaIndexStatusWithScope:scope];
    snapshot.success = YES;
    snapshot.mode = [@"media-" stringByAppendingString:scope];
    snapshot.buildMilliseconds = MillisecondsSince(started);
    snapshot.message = [NSString stringWithFormat:@"%lu 张图片已写入本地向量索引", (unsigned long)labels.count];
    return snapshot;
}

- (VexDemoSnapshot *)searchMediaWithScope:(NSString *)scope
                                embedding:(NSArray<NSNumber *> *)embedding
                                    limit:(NSInteger)limit {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    NSString *table = MediaTableForScope(scope);
    if (!table) return [self errorSnapshot:@"查询失败：未知的图片索引类型"];
    if (embedding.count == 0 || limit < 1 || limit > 100) return [self errorSnapshot:@"查询失败：图片查询向量不正确"];
    std::string query; if (!VectorJSON(embedding, &query)) return [self errorSnapshot:@"查询失败：图片向量无效"];
    auto started = Clock::now();
    sqlite3_stmt *statement = nullptr;
    NSString *searchSQL = [NSString stringWithFormat:
        @"SELECT rowid,label,distance FROM %@ WHERE embedding MATCH ? AND k=? ORDER BY distance", table];
    if (sqlite3_prepare_v2(_db, searchSQL.UTF8String, -1, &statement, nullptr) != SQLITE_OK)
        return [self errorSnapshot:SQLiteMessage(_db, @"准备图片查询失败")];
    sqlite3_bind_text(statement, 1, query.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int(statement, 2, int(limit));
    NSMutableArray<VexSearchResult *> *results = [NSMutableArray array]; int stepRC = SQLITE_OK;
    while ((stepRC = sqlite3_step(statement)) == SQLITE_ROW) {
        VexSearchResult *result = [VexSearchResult new]; result.rowID = NSInteger(sqlite3_column_int64(statement, 0));
        const char *label = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
        result.title = label ? [NSString stringWithUTF8String:label] : @"图片"; result.category = @"图片向量"; result.distance = sqlite3_column_double(statement, 2); [results addObject:result];
    }
    sqlite3_finalize(statement); if (stepRC != SQLITE_DONE) return [self errorSnapshot:SQLiteMessage(_db, @"执行图片查询失败")];
    VexDemoSnapshot *snapshot = [self mediaIndexStatusWithScope:scope];
    snapshot.success = YES;
    snapshot.mode = [@"media-" stringByAppendingString:scope];
    snapshot.message = @"图片查询已在本机完成相似度检索";
    snapshot.results = results;
    snapshot.queryMilliseconds = MillisecondsSince(started);
    snapshot.queryPlan = [self queryPlanForVector:query table:table];
    return snapshot;
}

- (VexDemoSnapshot *)mediaIndexStatusWithScope:(NSString *)scope {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    NSString *table = MediaTableForScope(scope);
    if (!table) return [self errorSnapshot:@"读取失败：未知的图片索引类型"];
    VexDemoSnapshot *snapshot = [self baseSnapshot]; snapshot.mode = [@"media-" stringByAppendingString:scope]; snapshot.rowCount = 0;
    NSString *statusSQL = [NSString stringWithFormat:@"SELECT count(*) FROM %@", table];
    sqlite3_stmt *statement = nullptr; int rc = sqlite3_prepare_v2(_db, statusSQL.UTF8String, -1, &statement, nullptr);
    if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) { snapshot.success = YES; snapshot.rowCount = NSInteger(sqlite3_column_int64(statement, 0)); snapshot.message = snapshot.rowCount > 0 ? @"已找到本地图片索引" : @"图片索引为空"; }
    else { snapshot.success = YES; snapshot.message = @"尚未导入图片"; }
    if (statement) sqlite3_finalize(statement); return snapshot;
}

- (VexDemoSnapshot *)clearMediaWithScope:(NSString *)scope {
    if (![self openDatabase]) return [self errorSnapshot:SQLiteMessage(_db, @"打开数据库失败")];
    NSString *table = MediaTableForScope(scope);
    if (!table) return [self errorSnapshot:@"清理失败：未知的图片索引类型"];
    if (![self exec:[NSString stringWithFormat:@"DROP TABLE IF EXISTS %@;", table]])
        return [self errorSnapshot:SQLiteMessage(_db, @"清理图片索引失败")];
    VexDemoSnapshot *snapshot = [self mediaIndexStatusWithScope:scope];
    snapshot.success = YES;
    snapshot.rowCount = 0;
    snapshot.message = @"图片索引已清空";
    return snapshot;
}

- (VexDemoSnapshot *)importUserMedia:(NSArray<NSString *> *)labels
                          embeddings:(NSArray<NSArray<NSNumber *> *> *)embeddings {
    return [self importMediaWithScope:@"user" labels:labels embeddings:embeddings];
}

- (VexDemoSnapshot *)searchUserMediaWithEmbedding:(NSArray<NSNumber *> *)embedding limit:(NSInteger)limit {
    return [self searchMediaWithScope:@"user" embedding:embedding limit:limit];
}

- (VexDemoSnapshot *)userMediaIndexStatus {
    return [self mediaIndexStatusWithScope:@"user"];
}

- (VexDemoSnapshot *)baseSnapshot {
    VexDemoSnapshot *snapshot = [VexDemoSnapshot new];
    snapshot.mode = _mode ?: @"unknown";
    snapshot.results = @[];

    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(_db, "SELECT vexdb_version()", -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        const char *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        snapshot.version = value ? [NSString stringWithUTF8String:value] : @"VexDB Lite";
    } else {
        snapshot.version = @"VexDB Lite";
    }
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(_db, "SELECT count(*) FROM demo_vectors", -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        snapshot.rowCount = NSInteger(sqlite3_column_int64(statement, 0));
    }
    sqlite3_finalize(statement);

    long long total = 0;
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *suffix in @[@"", @"-wal", @"-shm"]) {
        NSString *path = [self.databasePath stringByAppendingString:suffix];
        NSDictionary *attrs = [files attributesOfItemAtPath:path error:nil];
        total += [attrs[NSFileSize] longLongValue];
    }
    snapshot.databaseBytes = total;
    return snapshot;
}

- (NSString *)queryPlanForVector:(const std::string &)query {
    return [self queryPlanForVector:query table:@"demo_vectors"];
}

- (NSString *)queryPlanForVector:(const std::string &)query table:(NSString *)table {
    sqlite3_stmt *statement = nullptr;
    NSString *sql = [NSString stringWithFormat:
        @"EXPLAIN QUERY PLAN SELECT rowid FROM %@ "
         "WHERE embedding MATCH ? AND k=5 ORDER BY distance", table];
    if (sqlite3_prepare_v2(_db, sql.UTF8String, -1, &statement, nullptr) != SQLITE_OK) return @"不可用";
    sqlite3_bind_text(statement, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *detail = reinterpret_cast<const char *>(sqlite3_column_text(statement, 3));
        if (detail) [parts addObject:[NSString stringWithUTF8String:detail]];
    }
    sqlite3_finalize(statement);
    return [parts componentsJoinedByString:@" · "];
}

@end
