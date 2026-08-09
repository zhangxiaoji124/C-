#pragma once

#include <filesystem>
#include <string>

namespace orbit {

class Database;
class AgentWorkflow;

class HttpServer {
public:
    HttpServer(Database& database, AgentWorkflow& agent, std::filesystem::path web_root);
    bool listen(const std::string& host, int port);

private:
    Database& database_;
    AgentWorkflow& agent_;
    std::filesystem::path web_root_;
};

} // namespace orbit

