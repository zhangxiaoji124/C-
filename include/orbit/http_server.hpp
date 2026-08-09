#pragma once

#include <filesystem>
#include <string>

namespace orbit {

class Database;
class AgentWorkflow;
class DeveloperAgent;

class HttpServer {
public:
    HttpServer(Database& database, AgentWorkflow& agent, DeveloperAgent& dev_agent,
               std::filesystem::path web_root);
    bool listen(const std::string& host, int port);

private:
    Database& database_;
    AgentWorkflow& agent_;
    DeveloperAgent& dev_agent_;
    std::filesystem::path web_root_;
};

} // namespace orbit
