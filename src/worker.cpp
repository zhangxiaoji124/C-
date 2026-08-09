#include "orbit/worker.hpp"
#include "orbit/agent.hpp"
#include "orbit/database.hpp"
#include "orbit/dev_agent.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace orbit {

AgentWorker::AgentWorker(Database& database, AgentWorkflow& agent, DeveloperAgent& dev_agent,
                         std::string worker_id)
    : database_(database), agent_(agent), dev_agent_(dev_agent), worker_id_(std::move(worker_id)) {}

bool AgentWorker::process_once() {
    const json job = database_.claim_job(worker_id_, 60);
    if (job.is_null()) return false;

    const int job_id = job.at("id").get<int>();
    std::mutex lease_mutex;
    std::condition_variable lease_changed;
    bool lease_done = false;
    std::thread lease_keeper([&] {
        std::unique_lock lock(lease_mutex);
        while (!lease_changed.wait_for(lock, std::chrono::seconds(20), [&] { return lease_done; })) {
            lock.unlock();
            try { database_.renew_job_lease(job_id, worker_id_, 60); }
            catch (const std::exception& error) {
                std::cerr << "[worker " << worker_id_ << "] lease renewal failed: " << error.what() << '\n';
            }
            lock.lock();
        }
    });
    const auto stop_lease = [&] {
        {
            std::lock_guard lock(lease_mutex);
            lease_done = true;
        }
        lease_changed.notify_one();
        if (lease_keeper.joinable()) lease_keeper.join();
    };
    try {
        const json& payload = job.at("payload");
        json result;
        if (job.value("type", "") == "agent_run") {
            result = agent_.run(payload.at("run_id").get<int>(),
                                payload.at("project_id").get<int>(),
                                payload.at("goal").get<std::string>(),
                                payload.value("mode", "preview"));
        } else if (job.value("type", "") == "dev_run") {
            result = dev_agent_.run(payload.at("run_id").get<int>(), payload.at("goal").get<std::string>());
        } else {
            throw std::runtime_error("Unsupported job type: " + job.value("type", "unknown"));
        }
        stop_lease();
        database_.complete_job(job_id, worker_id_, result);
        return true;
    } catch (const std::exception& error) {
        stop_lease();
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
            database_.heartbeat_node(worker_id_, "worker", "local", {{"queues", json::array({"agent_run", "dev_run"})}});
            last_heartbeat = now;
        }
        if (!process_once()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, poll_interval_ms)));
        }
    }
}

} // namespace orbit
