/*
   Koya Plugin: SQLite

   What this is:
   - Application code that exposes minimal SQLite access to Koya's JS runtime
     using an async worker thread to avoid blocking the engine.

   Koya vs Application responsibilities:
   - Koya provides QuickJS and lifecycle hooks (update/cleanup).
   - The plugin (application) manages sqlite3 handles, jobs, and marshaling of
     results back to JS in `update`.

   Exposed JS API (selected):
   - open(path) -> db, openInMemory() -> db, close(db)
   - exec(db, sql) -> Promise<void>
   - query(db, sql) -> Promise<Array<RowObject>>

   Plugin structure reference:
   - `integrate(...)` registers an `update` hook that drains completion lambdas
     posted by the worker.
*/
#include <sqlite3.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <queue>
#include <thread>
#include <condition_variable>

#include "../../sdk/quickjs/quickjs.h"
#include "../../sdk/module_hooks.h"
#include <functional>

// RAII wrapper for sqlite3* handle tracked by id.
struct SqliteDbHandle {
    sqlite3* db = nullptr;
};

static std::mutex g_sql_mutex;
static std::unordered_map<uint32_t, std::unique_ptr<SqliteDbHandle>> g_dbs;
static uint32_t g_next_db_id = 1;
static JSContext* g_ctx = nullptr;

// Async worker infrastructure
// Background job types processed by the worker thread.
enum class JobType { Exec, Query };
struct JobExec { uint32_t id; std::string sql; JSValue resolve; JSValue reject; };
struct JobQuery { uint32_t id; std::string sql; JSValue resolve; JSValue reject; };
struct Job { JobType type; JobExec e; JobQuery q; };
static std::mutex g_job_mutex;
static std::condition_variable g_job_cv;
static std::queue<Job> g_jobs;
// Row with (name,value,type) pairs preserving SQLite types
enum class SqliteValueType { Null, Integer, Float, Text, Blob };
struct QueryResultColumn {
    std::string name;
    std::string value;
    SqliteValueType type;
};
struct QueryResultRow {
    std::vector<QueryResultColumn> cols;
};
// Completed query result; delivered as JS array of row objects.
struct QueryResult {
    bool ok;
    int code;
    std::string message;
    std::vector<QueryResultRow> rows;
};
// Completed exec result; delivered as resolve(void) or reject({message, code}).
struct ExecResult {
    bool ok; int code; std::string message;
};
static std::queue<std::function<void()>> g_completions; // run on main thread via update hook
static std::mutex g_comp_mutex;
static std::thread g_worker;
static std::atomic<bool> g_worker_running{false};

static void ensure_worker_started();

static std::string js_to_std_string(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    std::string s = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return s;
}

static JSValue make_error(JSContext* ctx, const std::string& msg, int code) {
    JSValue err = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
    JS_SetPropertyStr(ctx, err, "code", JS_NewInt32(ctx, code));
    return err;
}

// JS: open(path) -> db object with `id` to be passed back to exec/query/close.
static JSValue js_open(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "open(path: string)");
    }
    std::string path = js_to_std_string(ctx, argv[0]);
    auto handle = std::make_unique<SqliteDbHandle>();
    int rc = sqlite3_open(path.c_str(), &handle->db);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(handle->db);
        if (handle->db) { sqlite3_close(handle->db); handle->db = nullptr; }
        return JS_Throw(ctx, make_error(ctx, msg.empty()?"sqlite open failed":msg, rc));
    }
    std::lock_guard<std::mutex> lk(g_sql_mutex);
    uint32_t id = g_next_db_id++;
    g_dbs[id] = std::move(handle);
    JSValue dbObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dbObj, "id", JS_NewInt32(ctx, (int32_t)id));
    return dbObj;
}

// JS: openInMemory() -> db.
static JSValue js_open_memory(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    auto handle = std::make_unique<SqliteDbHandle>();
    int rc = sqlite3_open(":memory:", &handle->db);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(handle->db);
        if (handle->db) { sqlite3_close(handle->db); handle->db = nullptr; }
        return JS_Throw(ctx, make_error(ctx, msg.empty()?"sqlite open failed":msg, rc));
    }
    std::lock_guard<std::mutex> lk(g_sql_mutex);
    uint32_t id = g_next_db_id++;
    g_dbs[id] = std::move(handle);
    JSValue dbObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dbObj, "id", JS_NewInt32(ctx, (int32_t)id));
    return dbObj;
}

// Helper: resolve db id from JS object and return the native handle.
static SqliteDbHandle* get_handle(JSContext* ctx, JSValueConst dbObj) {
    JSValue idv = JS_GetPropertyStr(ctx, dbObj, "id");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
    std::lock_guard<std::mutex> lk(g_sql_mutex);
    auto it = g_dbs.find((uint32_t)id64);
    if (it == g_dbs.end()) return nullptr;
    return it->second.get();
}

// JS: close(db) — closes and removes the handle from the map.
static JSValue js_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "close(db)");
    }
    JSValue dbObj = argv[0];
    JSValue idv = JS_GetPropertyStr(ctx, dbObj, "id");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
    std::unique_ptr<SqliteDbHandle> to_close;
    {
        std::lock_guard<std::mutex> lk(g_sql_mutex);
        auto it = g_dbs.find((uint32_t)id64);
        if (it == g_dbs.end()) return JS_ThrowTypeError(ctx, "close: invalid db");
        to_close = std::move(it->second);
        g_dbs.erase(it);
    }
    if (to_close && to_close->db) sqlite3_close(to_close->db);
    return JS_UNDEFINED;
}

// JS: exec(db, sql) -> Promise<void>
static JSValue js_exec(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx, "exec(db, sql)");
    }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject = funcs[1];
    Job job; job.type = JobType::Exec;
    JSValue idv = JS_GetPropertyStr(ctx, argv[0], "id");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
    job.e.id = (uint32_t)id64;
    job.e.sql = js_to_std_string(ctx, argv[1]);
    job.e.resolve = JS_DupValue(ctx, resolve);
    job.e.reject = JS_DupValue(ctx, reject);
    {
        std::lock_guard<std::mutex> lk(g_job_mutex);
        g_jobs.push(std::move(job));
    }
    ensure_worker_started();
    g_job_cv.notify_one();
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, reject);
    return promise;
}

// JS: query(db, sql) -> Promise<Array<Row>>
static JSValue js_query(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx, "query(db, sql)");
    }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject = funcs[1];
    Job job; job.type = JobType::Query;
    JSValue idv = JS_GetPropertyStr(ctx, argv[0], "id");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
    job.q.id = (uint32_t)id64;
    job.q.sql = js_to_std_string(ctx, argv[1]);
    job.q.resolve = JS_DupValue(ctx, resolve);
    job.q.reject = JS_DupValue(ctx, reject);
    {
        std::lock_guard<std::mutex> lk(g_job_mutex);
        g_jobs.push(std::move(job));
    }
    ensure_worker_started();
    g_job_cv.notify_one();
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, reject);
    return promise;
}

static int sqlite_module_init(JSContext* ctx, JSModuleDef* m) {
    JS_SetModuleExport(ctx, m, "open", JS_NewCFunction(ctx, js_open, "open", 1));
    JS_SetModuleExport(ctx, m, "openInMemory", JS_NewCFunction(ctx, js_open_memory, "openInMemory", 0));
    JS_SetModuleExport(ctx, m, "close", JS_NewCFunction(ctx, js_close, "close", 1));
    JS_SetModuleExport(ctx, m, "exec", JS_NewCFunction(ctx, js_exec, "exec", 2));
    JS_SetModuleExport(ctx, m, "query", JS_NewCFunction(ctx, js_query, "query", 2));
    return 0;
}

extern "C" {
// Required entry point
JSModuleDef* integrateV1(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook, const KoyaRendererV1*) {
    g_ctx = ctx;
        // Hook: update — runs completions (Promise resolve/reject) on engine thread.
        registerHook("update", [](void*){
        std::queue<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lk(g_comp_mutex);
            std::swap(local, g_completions);
        }
        while(!local.empty()) { local.front()(); local.pop(); }
    });
    registerHook("cleanup", [](void*){
        g_worker_running = false;
        g_job_cv.notify_all();
        if (g_worker.joinable()) g_worker.join();
        // free any pending completion lambdas by draining
        std::queue<std::function<void()>> drain;
        {
            std::lock_guard<std::mutex> lk(g_comp_mutex);
            std::swap(drain, g_completions);
        }
    });
    JSModuleDef* m = JS_NewCModule(ctx, module_name, sqlite_module_init);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "open");
    JS_AddModuleExport(ctx, m, "openInMemory");
    JS_AddModuleExport(ctx, m, "close");
    JS_AddModuleExport(ctx, m, "exec");
    JS_AddModuleExport(ctx, m, "query");
    return m;
}
}

static void worker_thread_body() {
    while (g_worker_running) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_job_mutex);
            g_job_cv.wait(lk, []{ return !g_worker_running || !g_jobs.empty(); });
            if (!g_worker_running) break;
            if (g_jobs.empty()) continue;
            job = std::move(g_jobs.front());
            g_jobs.pop();
        }
        if (job.type == JobType::Exec) {
            ExecResult res{true, SQLITE_OK, std::string()};
            sqlite3* db = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_sql_mutex);
                auto it = g_dbs.find(job.e.id);
                if (it == g_dbs.end() || !it->second || !it->second->db) {
                    res.ok = false; res.code = SQLITE_MISUSE; res.message = "invalid db";
                } else {
                    db = it->second->db;
                }
            }
            if (db) {
                char* errmsg = nullptr;
                int rc = sqlite3_exec(db, job.e.sql.c_str(), nullptr, nullptr, &errmsg);
                if (rc != SQLITE_OK) {
                    res.ok = false; res.code = rc; res.message = errmsg ? errmsg : "sqlite exec failed";
                    if (errmsg) sqlite3_free(errmsg);
                }
            }
            // post completion
            {
                std::lock_guard<std::mutex> lk(g_comp_mutex);
                JSValue resolve = job.e.resolve; JSValue reject = job.e.reject;
                g_completions.push([resolve, reject, res]() mutable {
                    if (!g_ctx) { JS_FreeValueRT(JS_GetRuntime(g_ctx), resolve); JS_FreeValueRT(JS_GetRuntime(g_ctx), reject); return; }
                    if (res.ok) {
                        JSValue undef = JS_UNDEFINED;
                        JSValue ret = JS_Call(g_ctx, resolve, JS_UNDEFINED, 1, &undef);
                        JS_FreeValue(g_ctx, ret);
                    } else {
                        JSValue err = JS_NewObject(g_ctx);
                        JS_SetPropertyStr(g_ctx, err, "message", JS_NewString(g_ctx, res.message.c_str()));
                        JS_SetPropertyStr(g_ctx, err, "code", JS_NewInt32(g_ctx, res.code));
                        JSValue ret = JS_Call(g_ctx, reject, JS_UNDEFINED, 1, &err);
                        JS_FreeValue(g_ctx, err);
                        JS_FreeValue(g_ctx, ret);
                    }
                    JS_FreeValue(g_ctx, resolve);
                    JS_FreeValue(g_ctx, reject);
                });
            }
        } else if (job.type == JobType::Query) {
            QueryResult res; res.ok = true; res.code = SQLITE_OK;
            sqlite3* db = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_sql_mutex);
                auto it = g_dbs.find(job.q.id);
                if (it == g_dbs.end() || !it->second || !it->second->db) { res.ok = false; res.code = SQLITE_MISUSE; res.message = "invalid db"; }
                else { db = it->second->db; }
            }
            if (db) {
                sqlite3_stmt* stmt = nullptr;
                int rc = sqlite3_prepare_v2(db, job.q.sql.c_str(), -1, &stmt, nullptr);
                if (rc != SQLITE_OK) {
                    res.ok = false; res.code = rc; res.message = sqlite3_errmsg(db);
                } else {
                    int col_count = sqlite3_column_count(stmt);
                    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                        QueryResultRow row;
                        row.cols.reserve((size_t)col_count);
                        for (int c = 0; c < col_count; ++c) {
                            const char* name = sqlite3_column_name(stmt, c);
                            int type = sqlite3_column_type(stmt, c);
                            QueryResultColumn col;
                            col.name = name ? name : "";
                            
                            switch (type) {
                                case SQLITE_INTEGER: {
                                    long long v = sqlite3_column_int64(stmt, c);
                                    col.value = std::to_string(v);
                                    col.type = SqliteValueType::Integer;
                                    break; }
                                case SQLITE_FLOAT: {
                                    double v = sqlite3_column_double(stmt, c);
                                    col.value = std::to_string(v);
                                    col.type = SqliteValueType::Float;
                                    break; }
                                case SQLITE_TEXT: {
                                    const unsigned char* txt = sqlite3_column_text(stmt, c);
                                    col.value = std::string((const char*)txt);
                                    col.type = SqliteValueType::Text;
                                    break; }
                                case SQLITE_NULL: {
                                    col.value = "";
                                    col.type = SqliteValueType::Null;
                                    break; }
                                case SQLITE_BLOB:
                                default: {
                                    const void* blob = sqlite3_column_blob(stmt, c);
                                    int bytes = sqlite3_column_bytes(stmt, c);
                                    col.value = std::string((const char*)blob, (size_t)bytes);
                                    col.type = SqliteValueType::Blob;
                                    break; }
                            }
                            row.cols.push_back(std::move(col));
                        }
                        res.rows.push_back(std::move(row));
                    }
                    if (rc != SQLITE_DONE) { res.ok = false; res.code = rc; res.message = sqlite3_errmsg(db); }
                    sqlite3_finalize(stmt);
                }
            }
            // post completion
            {
                std::lock_guard<std::mutex> lk(g_comp_mutex);
                JSValue resolve = job.q.resolve; JSValue reject = job.q.reject;
                g_completions.push([resolve, reject, res]() mutable {
                    if (!g_ctx) { JS_FreeValueRT(JS_GetRuntime(g_ctx), resolve); JS_FreeValueRT(JS_GetRuntime(g_ctx), reject); return; }
                    if (res.ok) {
                        JSValue arr = JS_NewArray(g_ctx);
                        uint32_t idx = 0;
                        for (const auto& r : res.rows) {
                            JSValue obj = JS_NewObject(g_ctx);
                            for (const auto& col : r.cols) {
                                JSValue jsValue;
                                switch (col.type) {
                                    case SqliteValueType::Null:
                                        jsValue = JS_NULL;
                                        break;
                                    case SqliteValueType::Integer: {
                                        long long intVal = std::stoll(col.value);
                                        jsValue = JS_NewInt64(g_ctx, intVal);
                                        break; }
                                    case SqliteValueType::Float: {
                                        double floatVal = std::stod(col.value);
                                        jsValue = JS_NewFloat64(g_ctx, floatVal);
                                        break; }
                                    case SqliteValueType::Text:
                                        jsValue = JS_NewStringLen(g_ctx, col.value.data(), col.value.size());
                                        break;
                                    case SqliteValueType::Blob:
                                        // For BLOB data, we'll return it as a string representation
                                        // In a more sophisticated implementation, this could be a Uint8Array
                                        jsValue = JS_NewStringLen(g_ctx, col.value.data(), col.value.size());
                                        break;
                                }
                                JS_SetPropertyStr(g_ctx, obj, col.name.c_str(), jsValue);
                            }
                            JS_SetPropertyUint32(g_ctx, arr, idx++, obj);
                        }
                        JSValue ret = JS_Call(g_ctx, resolve, JS_UNDEFINED, 1, &arr);
                        JS_FreeValue(g_ctx, arr);
                        JS_FreeValue(g_ctx, ret);
                    } else {
                        JSValue err = JS_NewObject(g_ctx);
                        JS_SetPropertyStr(g_ctx, err, "message", JS_NewString(g_ctx, res.message.c_str()));
                        JS_SetPropertyStr(g_ctx, err, "code", JS_NewInt32(g_ctx, res.code));
                        JSValue ret = JS_Call(g_ctx, reject, JS_UNDEFINED, 1, &err);
                        JS_FreeValue(g_ctx, err);
                        JS_FreeValue(g_ctx, ret);
                    }
                    JS_FreeValue(g_ctx, resolve);
                    JS_FreeValue(g_ctx, reject);
                });
            }
        }
    }
}

static void ensure_worker_started() {
    if (g_worker_running) return;
    g_worker_running = true;
    g_worker = std::thread(worker_thread_body);
}


