#include "orbit/agent.hpp"
#include "orbit/database.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int passed = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Assertion failed: " + message);
    ++passed;
}

} // namespace

int main() {
    const std::filesystem::path path = "build/orbitops-unit-test.db";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);

    try {
        orbit::Database db(path);
        db.migrate();

        const int project_id = db.create_project({
            {"name", "测试项目"}, {"description", "验证数据层"}, {"color", "#6754d9"}
        });
        expect(project_id > 0, "project is created");
        expect(db.get_project(project_id)["name"] == "测试项目", "project can be read");

        const int task_id = db.create_task({
            {"project_id", project_id}, {"title", "测试任务"}, {"priority", "high"},
            {"estimate_hours", 3.5}, {"tags", orbit::json::array({"测试"})}
        });
        expect(task_id > 0, "task is created");
        expect(db.update_task(task_id, {{"status", "in_progress"}}), "task is updated");
        expect(db.get_task(task_id)["status"] == "in_progress", "task status persisted");
        expect(db.list_tasks(project_id).size() == 1, "task filter works");

        const int preview_run = db.create_agent_run(project_id, "分析风险", "preview", "unit-preview");
        expect(db.create_agent_run(project_id, "不会重复", "preview", "unit-preview") == preview_run,
               "agent idempotency key returns existing run");
        orbit::OllamaConfig offline;
        offline.enabled = false;
        orbit::AgentWorkflow agent(db, offline);
        expect(!agent.provider_status()["enabled"].get<bool>(), "rule-only provider can be selected");
        const auto preview = agent.run(preview_run, project_id, "分析风险", "preview");
        expect(preview["verification"]["passed"], "preview workflow verifies result");
        expect(db.get_agent_run(preview_run)["steps"].size() == 5, "workflow records five stages");

        const int execute_run = db.create_agent_run(project_id, "为下轮迭代制定计划并拆解任务", "execute", "unit-execute");
        const auto output = agent.run(execute_run, project_id, "为下轮迭代制定计划并拆解任务", "execute");
        expect(output["execution"]["applied_count"].get<int>() >= 3, "execute workflow calls tools");
        expect(db.list_tasks(project_id).size() >= 4, "agent-created tasks persist");

        const int job_id = db.enqueue_job("agent_run", {{"run_id", 99}}, 3, "unit-job");
        expect(job_id > 0, "job is enqueued");
        expect(db.enqueue_job("agent_run", {{"run_id", 100}}, 3, "unit-job") == job_id,
               "job dedupe key returns existing job");
        const auto job = db.claim_job("worker-test", 30);
        expect(job["id"] == job_id, "worker claims queued job");
        expect(job["attempts"] == 1, "claim increments attempt");
        db.complete_job(job_id, "worker-test", {{"ok", true}});
        expect(db.cluster_status()["queue"]["completed"] == 1, "completed job is counted");

        db.heartbeat_node("node-test", "worker", "local", {{"test", true}});
        const auto cluster = db.cluster_status();
        expect(cluster["nodes"].size() == 1, "service discovery stores heartbeat");
        expect(cluster["nodes"][0]["status"] == "online", "fresh node is online");

        expect(db.delete_task(task_id), "task is deleted");
        std::cout << "All " << passed << " unit assertions passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
