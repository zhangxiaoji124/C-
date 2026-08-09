#pragma once

#include <atomic>
#include <string>

namespace orbit {

class Database;
class AgentWorkflow;
class DeveloperAgent;

class AgentWorker {
public:
    AgentWorker(Database& database, AgentWorkflow& agent, DeveloperAgent& dev_agent,
                std::string worker_id);
    void run(std::atomic_bool& stop, int poll_interval_ms = 250);
    bool process_once();
    const std::string& id() const { return worker_id_; }

private:
    Database& database_;
    AgentWorkflow& agent_;
    DeveloperAgent& dev_agent_;
    std::string worker_id_;
};

} // namespace orbit
