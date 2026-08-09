#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace orbit {

using json = nlohmann::json;

class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void migrate();
    void seed_if_empty();

    json dashboard();
    json list_projects();
    json get_project(int id);
    int create_project(const json& input);
    bool update_project(int id, const json& input);
    bool delete_project(int id);

    json list_tasks(std::optional<int> project_id = std::nullopt,
                    const std::optional<std::string>& status = std::nullopt);
    json get_task(int id);
    int create_task(const json& input, const std::string& actor = "user");
    bool update_task(int id, const json& input, const std::string& actor = "user");
    bool delete_task(int id);

    json list_activities(int limit = 30);
    int create_agent_run(int project_id, const std::string& goal, const std::string& mode,
                         const std::string& idempotency_key = "");
    void mark_agent_run_running(int id);
    void update_agent_run(int id, const std::string& status, const json& output);
    void add_agent_step(int run_id, int sequence, const std::string& stage,
                        const std::string& status, const std::string& summary,
                        const json& detail = json::object());
    json list_agent_runs(int limit = 20);
    json get_agent_run(int id);

    int enqueue_job(const std::string& type, const json& payload, int max_attempts = 3,
                    const std::string& dedupe_key = "");
    json claim_job(const std::string& worker_id, int lease_seconds = 30);
    void renew_job_lease(int job_id, const std::string& worker_id, int lease_seconds = 60);
    void complete_job(int job_id, const std::string& worker_id, const json& result);
    void fail_job(int job_id, const std::string& worker_id, const std::string& error);
    void heartbeat_node(const std::string& node_id, const std::string& role,
                        const std::string& address, const json& metadata = json::object());
    json cluster_status();

    int create_dev_run(const std::string& goal, const std::string& workspace,
                       const std::string& idempotency_key = "");
    void mark_dev_run_running(int id);
    void update_dev_run(int id, const std::string& status, const json& output);
    void add_dev_step(int run_id, int sequence, const std::string& stage,
                      const std::string& status, const json& input, const json& output);
    json list_dev_runs(int limit = 20);
    json get_dev_run(int id);

    int scalar_int(const std::string& sql);
    void execute(const std::string& sql);

private:
    sqlite3* db_ = nullptr;
    std::recursive_mutex mutex_;

    void bind_json(sqlite3_stmt* statement, int index, const json& value);
    void log_activity(const std::string& actor, const std::string& action,
                      const std::string& entity_type, int entity_id,
                      const std::string& detail);
    json query(const std::string& sql, const std::vector<json>& params = {});
    int insert(const std::string& sql, const std::vector<json>& params = {});
    bool change(const std::string& sql, const std::vector<json>& params = {});
};

} // namespace orbit
