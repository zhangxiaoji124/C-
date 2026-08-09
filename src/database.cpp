#include "orbit/database.hpp"

#include <algorithm>
#include <stdexcept>

namespace orbit {

namespace {

void ensure_ok(int code, sqlite3* db, const std::string& context) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW) {
        throw std::runtime_error(context + ": " + sqlite3_errmsg(db));
    }
}

} // namespace

Database::Database(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const auto code = sqlite3_open_v2(path.string().c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (code != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown SQLite error";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Cannot open database: " + message);
    }
    sqlite3_busy_timeout(db_, 5000);
    execute("PRAGMA foreign_keys = ON;");
    execute("PRAGMA journal_mode = WAL;");
    execute("PRAGMA synchronous = NORMAL;");
    execute("PRAGMA temp_store = MEMORY;");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::execute(const std::string& sql) {
    std::lock_guard lock(mutex_);
    char* error = nullptr;
    const int code = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error("SQLite execute failed: " + message);
    }
}

void Database::migrate() {
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS projects (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT '',
            color TEXT NOT NULL DEFAULT '#6C5CE7',
            status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','paused','completed')),
            due_date TEXT,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
            title TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT 'todo' CHECK(status IN ('todo','in_progress','review','done')),
            priority TEXT NOT NULL DEFAULT 'medium' CHECK(priority IN ('low','medium','high','urgent')),
            assignee TEXT NOT NULL DEFAULT '未分配',
            due_date TEXT,
            estimate_hours REAL NOT NULL DEFAULT 0,
            tags TEXT NOT NULL DEFAULT '[]',
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE TABLE IF NOT EXISTS activities (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            actor TEXT NOT NULL,
            action TEXT NOT NULL,
            entity_type TEXT NOT NULL,
            entity_id INTEGER NOT NULL,
            detail TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE TABLE IF NOT EXISTS agent_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
            goal TEXT NOT NULL,
            mode TEXT NOT NULL CHECK(mode IN ('preview','execute')),
            status TEXT NOT NULL DEFAULT 'queued',
            idempotency_key TEXT,
            output TEXT NOT NULL DEFAULT '{}',
            started_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            completed_at TEXT
        );
        CREATE TABLE IF NOT EXISTS agent_steps (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id INTEGER NOT NULL REFERENCES agent_runs(id) ON DELETE CASCADE,
            sequence INTEGER NOT NULL,
            stage TEXT NOT NULL,
            status TEXT NOT NULL,
            summary TEXT NOT NULL,
            detail TEXT NOT NULL DEFAULT '{}',
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE TABLE IF NOT EXISTS jobs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            type TEXT NOT NULL,
            payload TEXT NOT NULL,
            dedupe_key TEXT,
            status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','processing','completed','failed')),
            attempts INTEGER NOT NULL DEFAULT 0,
            max_attempts INTEGER NOT NULL DEFAULT 3,
            worker_id TEXT,
            lease_until TEXT,
            available_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            result TEXT,
            last_error TEXT,
            created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE TABLE IF NOT EXISTS service_nodes (
            node_id TEXT PRIMARY KEY,
            role TEXT NOT NULL,
            address TEXT NOT NULL,
            metadata TEXT NOT NULL DEFAULT '{}',
            started_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
            last_heartbeat TEXT NOT NULL DEFAULT (datetime('now','localtime'))
        );
        CREATE INDEX IF NOT EXISTS idx_tasks_project ON tasks(project_id);
        CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
        CREATE INDEX IF NOT EXISTS idx_tasks_due_date ON tasks(due_date);
        CREATE INDEX IF NOT EXISTS idx_activities_created ON activities(created_at DESC);
        CREATE INDEX IF NOT EXISTS idx_agent_steps_run ON agent_steps(run_id, sequence);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_agent_runs_idempotency ON agent_runs(idempotency_key) WHERE idempotency_key IS NOT NULL AND idempotency_key != '';
        CREATE INDEX IF NOT EXISTS idx_jobs_claim ON jobs(status, available_at, lease_until, id);
        CREATE UNIQUE INDEX IF NOT EXISTS idx_jobs_dedupe ON jobs(dedupe_key) WHERE dedupe_key IS NOT NULL AND dedupe_key != '';
    )SQL");
}

void Database::seed_if_empty() {
    execute("BEGIN IMMEDIATE;");
    try {
        if (scalar_int("SELECT COUNT(*) FROM projects") != 0) {
            execute("COMMIT;");
            return;
        }
        const int p1 = create_project({
            {"name", "智能客服升级"}, {"description", "将 FAQ、工单与知识库整合为智能服务入口"},
            {"color", "#6C5CE7"}, {"due_date", "2026-09-18"}
        });
        const int p2 = create_project({
            {"name", "增长数据中台"}, {"description", "统一渠道指标，缩短经营分析链路"},
            {"color", "#00B894"}, {"due_date", "2026-10-05"}
        });
        const int p3 = create_project({
            {"name", "移动端体验优化"}, {"description", "围绕核心路径提升性能与转化率"},
            {"color", "#E17055"}, {"due_date", "2026-08-28"}
        });

        create_task({{"project_id", p1}, {"title", "梳理高频咨询意图"}, {"status", "done"},
                     {"priority", "high"}, {"assignee", "林一"}, {"due_date", "2026-08-06"},
                     {"estimate_hours", 6}, {"tags", json::array({"调研", "NLP"})}});
        create_task({{"project_id", p1}, {"title", "搭建知识库检索接口"}, {"status", "in_progress"},
                     {"priority", "urgent"}, {"assignee", "陈辰"}, {"due_date", "2026-08-12"},
                     {"estimate_hours", 16}, {"tags", json::array({"后端", "RAG"})}});
        create_task({{"project_id", p1}, {"title", "设计客服质检规则"}, {"status", "review"},
                     {"priority", "high"}, {"assignee", "周悦"}, {"due_date", "2026-08-11"},
                     {"estimate_hours", 8}, {"tags", json::array({"AI", "质量"})}});
        create_task({{"project_id", p1}, {"title", "灰度发布与反馈回收"}, {"status", "todo"},
                     {"priority", "medium"}, {"assignee", "未分配"}, {"due_date", "2026-08-20"},
                     {"estimate_hours", 10}, {"tags", json::array({"发布"})}});

        create_task({{"project_id", p2}, {"title", "定义北极星指标口径"}, {"status", "done"},
                     {"priority", "high"}, {"assignee", "王岚"}, {"due_date", "2026-08-03"},
                     {"estimate_hours", 5}, {"tags", json::array({"数据"})}});
        create_task({{"project_id", p2}, {"title", "接入广告渠道数据"}, {"status", "in_progress"},
                     {"priority", "high"}, {"assignee", "赵新"}, {"due_date", "2026-08-16"},
                     {"estimate_hours", 20}, {"tags", json::array({"ETL", "API"})}});
        create_task({{"project_id", p2}, {"title", "搭建经营分析大屏"}, {"status", "todo"},
                     {"priority", "medium"}, {"assignee", "李念"}, {"due_date", "2026-08-25"},
                     {"estimate_hours", 18}, {"tags", json::array({"可视化"})}});

        create_task({{"project_id", p3}, {"title", "首屏性能基线测试"}, {"status", "done"},
                     {"priority", "urgent"}, {"assignee", "高远"}, {"due_date", "2026-08-04"},
                     {"estimate_hours", 4}, {"tags", json::array({"性能"})}});
        create_task({{"project_id", p3}, {"title", "图片资源按需加载"}, {"status", "review"},
                     {"priority", "high"}, {"assignee", "高远"}, {"due_date", "2026-08-10"},
                     {"estimate_hours", 8}, {"tags", json::array({"前端", "性能"})}});
        create_task({{"project_id", p3}, {"title", "结算流程可用性测试"}, {"status", "todo"},
                     {"priority", "urgent"}, {"assignee", "周悦"}, {"due_date", "2026-08-13"},
                     {"estimate_hours", 12}, {"tags", json::array({"UX", "测试"})}});
        execute("COMMIT;");
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

void Database::bind_json(sqlite3_stmt* statement, int index, const json& value) {
    int code = SQLITE_OK;
    if (value.is_null()) code = sqlite3_bind_null(statement, index);
    else if (value.is_boolean()) code = sqlite3_bind_int(statement, index, value.get<bool>() ? 1 : 0);
    else if (value.is_number_integer()) code = sqlite3_bind_int64(statement, index, value.get<sqlite3_int64>());
    else if (value.is_number()) code = sqlite3_bind_double(statement, index, value.get<double>());
    else {
        const std::string string_value = value.is_string() ? value.get<std::string>() : value.dump();
        code = sqlite3_bind_text(statement, index, string_value.c_str(), -1, SQLITE_TRANSIENT);
    }
    ensure_ok(code, db_, "Failed to bind value");
}

json Database::query(const std::string& sql, const std::vector<json>& params) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    ensure_ok(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr), db_, "Failed to prepare query");
    try {
        for (std::size_t i = 0; i < params.size(); ++i) bind_json(statement, static_cast<int>(i + 1), params[i]);
        json rows = json::array();
        int code;
        while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
            json row = json::object();
            for (int column = 0; column < sqlite3_column_count(statement); ++column) {
                const std::string name = sqlite3_column_name(statement, column);
                switch (sqlite3_column_type(statement, column)) {
                    case SQLITE_INTEGER: row[name] = sqlite3_column_int64(statement, column); break;
                    case SQLITE_FLOAT: row[name] = sqlite3_column_double(statement, column); break;
                    case SQLITE_TEXT: row[name] = reinterpret_cast<const char*>(sqlite3_column_text(statement, column)); break;
                    case SQLITE_NULL: row[name] = nullptr; break;
                    default: row[name] = nullptr;
                }
            }
            rows.push_back(std::move(row));
        }
        ensure_ok(code, db_, "Failed to execute query");
        sqlite3_finalize(statement);
        return rows;
    } catch (...) {
        sqlite3_finalize(statement);
        throw;
    }
}

int Database::insert(const std::string& sql, const std::vector<json>& params) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    ensure_ok(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr), db_, "Failed to prepare insert");
    try {
        for (std::size_t i = 0; i < params.size(); ++i) bind_json(statement, static_cast<int>(i + 1), params[i]);
        ensure_ok(sqlite3_step(statement), db_, "Failed to execute insert");
        sqlite3_finalize(statement);
        return static_cast<int>(sqlite3_last_insert_rowid(db_));
    } catch (...) {
        sqlite3_finalize(statement);
        throw;
    }
}

bool Database::change(const std::string& sql, const std::vector<json>& params) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    ensure_ok(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr), db_, "Failed to prepare update");
    try {
        for (std::size_t i = 0; i < params.size(); ++i) bind_json(statement, static_cast<int>(i + 1), params[i]);
        ensure_ok(sqlite3_step(statement), db_, "Failed to execute update");
        const bool changed = sqlite3_changes(db_) > 0;
        sqlite3_finalize(statement);
        return changed;
    } catch (...) {
        sqlite3_finalize(statement);
        throw;
    }
}

int Database::scalar_int(const std::string& sql) {
    const json rows = query(sql);
    if (rows.empty() || rows[0].empty()) return 0;
    return rows[0].begin().value().get<int>();
}

json Database::dashboard() {
    json result;
    const auto stats = query(R"SQL(
        SELECT
          COUNT(*) AS total_tasks,
          SUM(CASE WHEN status='done' THEN 1 ELSE 0 END) AS completed_tasks,
          SUM(CASE WHEN status='in_progress' THEN 1 ELSE 0 END) AS active_tasks,
          SUM(CASE WHEN due_date < date('now','localtime') AND status != 'done' THEN 1 ELSE 0 END) AS overdue_tasks,
          ROUND(COALESCE(SUM(estimate_hours),0),1) AS estimated_hours
        FROM tasks
    )SQL");
    result["stats"] = stats.empty() ? json::object() : stats[0];
    result["stats"]["active_projects"] = scalar_int("SELECT COUNT(*) FROM projects WHERE status='active'");
    result["status_distribution"] = query(R"SQL(
        SELECT status, COUNT(*) AS count FROM tasks GROUP BY status
        ORDER BY CASE status WHEN 'todo' THEN 1 WHEN 'in_progress' THEN 2 WHEN 'review' THEN 3 ELSE 4 END
    )SQL");
    result["priority_distribution"] = query(R"SQL(
        SELECT priority, COUNT(*) AS count FROM tasks WHERE status != 'done' GROUP BY priority
        ORDER BY CASE priority WHEN 'urgent' THEN 1 WHEN 'high' THEN 2 WHEN 'medium' THEN 3 ELSE 4 END
    )SQL");
    result["projects"] = list_projects();
    result["activities"] = list_activities(8);
    result["upcoming"] = query(R"SQL(
        SELECT t.id, t.title, t.priority, t.status, t.due_date, t.assignee,
               p.name AS project_name, p.color AS project_color
        FROM tasks t JOIN projects p ON p.id=t.project_id
        WHERE t.status != 'done' AND t.due_date IS NOT NULL
        ORDER BY t.due_date ASC LIMIT 6
    )SQL");
    return result;
}

json Database::list_projects() {
    return query(R"SQL(
        SELECT p.*,
               COUNT(t.id) AS task_count,
               SUM(CASE WHEN t.status='done' THEN 1 ELSE 0 END) AS completed_count,
               CASE WHEN COUNT(t.id)=0 THEN 0 ELSE ROUND(100.0 * SUM(CASE WHEN t.status='done' THEN 1 ELSE 0 END) / COUNT(t.id)) END AS progress
        FROM projects p LEFT JOIN tasks t ON t.project_id=p.id
        GROUP BY p.id ORDER BY CASE p.status WHEN 'active' THEN 1 WHEN 'paused' THEN 2 ELSE 3 END, p.updated_at DESC
    )SQL");
}

json Database::get_project(int id) {
    auto rows = query("SELECT * FROM projects WHERE id=?", {id});
    if (rows.empty()) return nullptr;
    json project = rows[0];
    project["tasks"] = list_tasks(id);
    return project;
}

int Database::create_project(const json& input) {
    const int id = insert(R"SQL(
        INSERT INTO projects(name, description, color, status, due_date)
        VALUES(?,?,?,?,?)
    )SQL", {
        input.at("name"), input.value("description", ""), input.value("color", "#6C5CE7"),
        input.value("status", "active"), input.value("due_date", json(nullptr))
    });
    log_activity("user", "created", "project", id, "创建项目「" + input.at("name").get<std::string>() + "」");
    return id;
}

bool Database::update_project(int id, const json& input) {
    auto existing = get_project(id);
    if (existing.is_null()) return false;
    const bool changed = change(R"SQL(
        UPDATE projects SET name=?, description=?, color=?, status=?, due_date=?, updated_at=datetime('now','localtime') WHERE id=?
    )SQL", {
        input.value("name", existing["name"]), input.value("description", existing["description"]),
        input.value("color", existing["color"]), input.value("status", existing["status"]),
        input.contains("due_date") ? input["due_date"] : existing["due_date"], id
    });
    if (changed) log_activity("user", "updated", "project", id, "更新项目「" + existing["name"].get<std::string>() + "」");
    return changed;
}

bool Database::delete_project(int id) {
    auto existing = get_project(id);
    if (existing.is_null()) return false;
    const bool changed = change("DELETE FROM projects WHERE id=?", {id});
    if (changed) log_activity("user", "deleted", "project", id, "删除项目「" + existing["name"].get<std::string>() + "」");
    return changed;
}

json Database::list_tasks(std::optional<int> project_id, const std::optional<std::string>& status) {
    std::string sql = R"SQL(
        SELECT t.*, p.name AS project_name, p.color AS project_color
        FROM tasks t JOIN projects p ON p.id=t.project_id WHERE 1=1
    )SQL";
    std::vector<json> params;
    if (project_id) { sql += " AND t.project_id=?"; params.emplace_back(*project_id); }
    if (status) { sql += " AND t.status=?"; params.emplace_back(*status); }
    sql += " ORDER BY CASE t.priority WHEN 'urgent' THEN 1 WHEN 'high' THEN 2 WHEN 'medium' THEN 3 ELSE 4 END, t.due_date ASC, t.id DESC";
    auto rows = query(sql, params);
    for (auto& row : rows) {
        try { row["tags"] = json::parse(row.value("tags", "[]")); }
        catch (...) { row["tags"] = json::array(); }
    }
    return rows;
}

json Database::get_task(int id) {
    auto rows = query(R"SQL(
        SELECT t.*, p.name AS project_name, p.color AS project_color
        FROM tasks t JOIN projects p ON p.id=t.project_id WHERE t.id=?
    )SQL", {id});
    if (rows.empty()) return nullptr;
    try { rows[0]["tags"] = json::parse(rows[0].value("tags", "[]")); }
    catch (...) { rows[0]["tags"] = json::array(); }
    return rows[0];
}

int Database::create_task(const json& input, const std::string& actor) {
    const json tags = input.value("tags", json::array());
    const int id = insert(R"SQL(
        INSERT INTO tasks(project_id,title,description,status,priority,assignee,due_date,estimate_hours,tags)
        VALUES(?,?,?,?,?,?,?,?,?)
    )SQL", {
        input.at("project_id"), input.at("title"), input.value("description", ""), input.value("status", "todo"),
        input.value("priority", "medium"), input.value("assignee", "未分配"), input.value("due_date", json(nullptr)),
        input.value("estimate_hours", 0.0), tags.dump()
    });
    log_activity(actor, "created", "task", id, "创建任务「" + input.at("title").get<std::string>() + "」");
    return id;
}

bool Database::update_task(int id, const json& input, const std::string& actor) {
    auto existing = get_task(id);
    if (existing.is_null()) return false;
    const json tags = input.contains("tags") ? input["tags"] : existing["tags"];
    const bool changed = change(R"SQL(
        UPDATE tasks SET project_id=?, title=?, description=?, status=?, priority=?, assignee=?, due_date=?,
                         estimate_hours=?, tags=?, updated_at=datetime('now','localtime') WHERE id=?
    )SQL", {
        input.value("project_id", existing["project_id"]), input.value("title", existing["title"]),
        input.value("description", existing["description"]), input.value("status", existing["status"]),
        input.value("priority", existing["priority"]), input.value("assignee", existing["assignee"]),
        input.contains("due_date") ? input["due_date"] : existing["due_date"],
        input.value("estimate_hours", existing["estimate_hours"]), tags.dump(), id
    });
    if (changed) {
        std::string detail = "更新任务「" + existing["title"].get<std::string>() + "」";
        if (input.contains("status") && input["status"] != existing["status"]) {
            detail += "，状态变更为 " + input["status"].get<std::string>();
        }
        log_activity(actor, "updated", "task", id, detail);
    }
    return changed;
}

bool Database::delete_task(int id) {
    auto existing = get_task(id);
    if (existing.is_null()) return false;
    const bool changed = change("DELETE FROM tasks WHERE id=?", {id});
    if (changed) log_activity("user", "deleted", "task", id, "删除任务「" + existing["title"].get<std::string>() + "」");
    return changed;
}

void Database::log_activity(const std::string& actor, const std::string& action,
                            const std::string& entity_type, int entity_id,
                            const std::string& detail) {
    insert("INSERT INTO activities(actor,action,entity_type,entity_id,detail) VALUES(?,?,?,?,?)",
           {actor, action, entity_type, entity_id, detail});
}

json Database::list_activities(int limit) {
    limit = std::clamp(limit, 1, 100);
    return query("SELECT * FROM activities ORDER BY id DESC LIMIT ?", {limit});
}

int Database::create_agent_run(int project_id, const std::string& goal, const std::string& mode,
                               const std::string& idempotency_key) {
    if (!idempotency_key.empty()) {
        auto existing = query("SELECT id FROM agent_runs WHERE idempotency_key=?", {idempotency_key});
        if (!existing.empty()) return existing[0]["id"].get<int>();
    }
    const int id = insert("INSERT INTO agent_runs(project_id,goal,mode,idempotency_key) VALUES(?,?,?,?)",
                          {project_id, goal, mode, idempotency_key.empty() ? json(nullptr) : json(idempotency_key)});
    log_activity("agent", "started", "agent_run", id, "Agent 开始处理：「" + goal + "」");
    return id;
}

int Database::enqueue_job(const std::string& type, const json& payload, int max_attempts,
                          const std::string& dedupe_key) {
    if (!dedupe_key.empty()) {
        auto existing = query("SELECT id FROM jobs WHERE dedupe_key=?", {dedupe_key});
        if (!existing.empty()) return existing[0]["id"].get<int>();
    }
    return insert("INSERT INTO jobs(type,payload,max_attempts,dedupe_key) VALUES(?,?,?,?)",
                  {type, payload.dump(), std::clamp(max_attempts, 1, 10),
                   dedupe_key.empty() ? json(nullptr) : json(dedupe_key)});
}

json Database::claim_job(const std::string& worker_id, int lease_seconds) {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE;");
    try {
        auto rows = query(R"SQL(
            SELECT * FROM jobs
            WHERE attempts < max_attempts AND available_at <= datetime('now','localtime')
              AND (status='queued' OR (status='processing' AND lease_until < datetime('now','localtime')))
            ORDER BY id LIMIT 1
        )SQL");
        if (rows.empty()) {
            execute("COMMIT;");
            return nullptr;
        }
        const int id = rows[0]["id"].get<int>();
        change(R"SQL(
            UPDATE jobs SET status='processing',worker_id=?,attempts=attempts+1,
              lease_until=datetime('now','localtime', ?),updated_at=datetime('now','localtime') WHERE id=?
        )SQL", {worker_id, "+" + std::to_string(std::max(5, lease_seconds)) + " seconds", id});
        execute("COMMIT;");
        rows[0]["worker_id"] = worker_id;
        rows[0]["attempts"] = rows[0].value("attempts", 0) + 1;
        try { rows[0]["payload"] = json::parse(rows[0].value("payload", "{}")); }
        catch (...) { rows[0]["payload"] = json::object(); }
        return rows[0];
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

void Database::complete_job(int job_id, const std::string& worker_id, const json& result) {
    change(R"SQL(
        UPDATE jobs SET status='completed',result=?,lease_until=NULL,updated_at=datetime('now','localtime')
        WHERE id=? AND worker_id=? AND status='processing'
    )SQL", {result.dump(), job_id, worker_id});
}

void Database::fail_job(int job_id, const std::string& worker_id, const std::string& error) {
    change(R"SQL(
        UPDATE jobs SET
          status=CASE WHEN attempts >= max_attempts THEN 'failed' ELSE 'queued' END,
          last_error=?,worker_id=NULL,lease_until=NULL,
          available_at=datetime('now','localtime', '+' || MIN(30, attempts * attempts) || ' seconds'),
          updated_at=datetime('now','localtime')
        WHERE id=? AND worker_id=? AND status='processing'
    )SQL", {error, job_id, worker_id});
}

void Database::heartbeat_node(const std::string& node_id, const std::string& role,
                              const std::string& address, const json& metadata) {
    change(R"SQL(
        INSERT INTO service_nodes(node_id,role,address,metadata) VALUES(?,?,?,?)
        ON CONFLICT(node_id) DO UPDATE SET role=excluded.role,address=excluded.address,
          metadata=excluded.metadata,last_heartbeat=datetime('now','localtime')
    )SQL", {node_id, role, address, metadata.dump()});
}

json Database::cluster_status() {
    json result;
    result["nodes"] = query(R"SQL(
        SELECT node_id,role,address,metadata,started_at,last_heartbeat,
          CASE WHEN last_heartbeat >= datetime('now','localtime','-15 seconds') THEN 'online' ELSE 'stale' END AS status
        FROM service_nodes ORDER BY role,node_id
    )SQL");
    for (auto& node : result["nodes"]) {
        try { node["metadata"] = json::parse(node.value("metadata", "{}")); }
        catch (...) { node["metadata"] = json::object(); }
    }
    const auto jobs = query(R"SQL(
        SELECT status,COUNT(*) AS count FROM jobs GROUP BY status
    )SQL");
    result["queue"] = {{"queued", 0}, {"processing", 0}, {"completed", 0}, {"failed", 0}};
    for (const auto& row : jobs) result["queue"][row["status"].get<std::string>()] = row["count"];
    result["database"] = {{"engine", "SQLite WAL"}, {"status", "healthy"}};
    return result;
}

void Database::update_agent_run(int id, const std::string& status, const json& output) {
    change("UPDATE agent_runs SET status=?,output=?,completed_at=datetime('now','localtime') WHERE id=?",
           {status, output.dump(), id});
    log_activity("agent", status, "agent_run", id,
                 status == "completed" ? "Agent 工作流执行完成" : "Agent 工作流执行失败");
}

void Database::mark_agent_run_running(int id) {
    change("UPDATE agent_runs SET status='running',completed_at=NULL WHERE id=?", {id});
}

void Database::add_agent_step(int run_id, int sequence, const std::string& stage,
                              const std::string& status, const std::string& summary,
                              const json& detail) {
    insert("INSERT INTO agent_steps(run_id,sequence,stage,status,summary,detail) VALUES(?,?,?,?,?,?)",
           {run_id, sequence, stage, status, summary, detail.dump()});
}

json Database::list_agent_runs(int limit) {
    limit = std::clamp(limit, 1, 100);
    auto rows = query(R"SQL(
        SELECT r.*, p.name AS project_name FROM agent_runs r
        LEFT JOIN projects p ON p.id=r.project_id ORDER BY r.id DESC LIMIT ?
    )SQL", {limit});
    for (auto& row : rows) {
        try { row["output"] = json::parse(row.value("output", "{}")); }
        catch (...) { row["output"] = json::object(); }
    }
    return rows;
}

json Database::get_agent_run(int id) {
    auto rows = query(R"SQL(
        SELECT r.*, p.name AS project_name FROM agent_runs r
        LEFT JOIN projects p ON p.id=r.project_id WHERE r.id=?
    )SQL", {id});
    if (rows.empty()) return nullptr;
    try { rows[0]["output"] = json::parse(rows[0].value("output", "{}")); }
    catch (...) { rows[0]["output"] = json::object(); }
    rows[0]["steps"] = query("SELECT * FROM agent_steps WHERE run_id=? ORDER BY sequence", {id});
    for (auto& step : rows[0]["steps"]) {
        try { step["detail"] = json::parse(step.value("detail", "{}")); }
        catch (...) { step["detail"] = json::object(); }
    }
    return rows[0];
}

} // namespace orbit
