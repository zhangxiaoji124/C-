#include "orbit/worker.hpp"
#include "orbit/agent.hpp"
#include "orbit/database.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace orbit {

AgentWorker::AgentWorker(Database& database, AgentWorkflow& agent, std::string worker_id)
    : database_(database), agent_(agent), worker_id_(std::move(worker_id)) {}

bool AgentWorker::process_once() {
    const json job = database_.claim_job(worker_id_, 60);
    if (job.is_null()) return false;

    const int job_id = job.at("id").get<int>();
    try {
        if (job.value("type", "") != "agent_run") {
            throw std::runtime_error("Unsupported job type: " + job.value("type", "unknown"));
        }
        const json& payload = job.at("payload");
        const json result = agent_.run(payload.at("run_id").get<int>(),
                                       payload.at("project_id").get<int>(),
                                       payload.at("goal").get<std::string>(),
                                       payload.value("mode", "preview"));
        database_.complete_job(job_id, worker_id_, result);
        return true;
    } catch (const std::exception& error) {
        database_.fail_job(job_id, worker_id_, error.what());
        std::cerr << "[worker " << worker_id_ << "] job " << job_id << " failed: " << error.what() << '\n';
        return true;
    }
}

void AgentWorker::run(std::atomic_bool& stop, int poll_interval_ms) {
    auto last_heartbeat = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    while (!stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= std::chrono::seconds(5)) {
            database_.heartbeat_node(worker_id_, "worker", "local", {{"queue", "agent_run"}});
            last_heartbeat = now;
        }
        if (!process_once()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, poll_interval_ms)));
        }
    }
}

} // namespace orbit
