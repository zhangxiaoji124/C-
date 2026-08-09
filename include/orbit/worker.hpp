#pragma once

#include <atomic>
#include <string>

namespace orbit {

class Database;
class AgentWorkflow;

class AgentWorker {
public:
    AgentWorker(Database& database, AgentWorkflow& agent, std::string worker_id);
    void run(std::atomic_bool& stop, int poll_interval_ms = 250);
    bool process_once();
    const std::string& id() const { return worker_id_; }

private:
    Database& database_;
    AgentWorkflow& agent_;
    std::string worker_id_;
};

} // namespace orbit
