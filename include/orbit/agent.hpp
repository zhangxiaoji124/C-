#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace orbit {

class Database;
using json = nlohmann::json;

class AgentWorkflow {
public:
    explicit AgentWorkflow(Database& database);
    json run(int run_id, int project_id, const std::string& goal, const std::string& mode);

private:
    Database& database_;

    json observe(int project_id);
    json plan(const json& context, const std::string& goal);
    json execute(int run_id, int project_id, const json& plan, const std::string& mode);
    json verify(int project_id, const json& execution);
};

} // namespace orbit
