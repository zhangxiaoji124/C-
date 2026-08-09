#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "orbit/ollama_client.hpp"

namespace orbit {

class Database;
using json = nlohmann::json;

class AgentWorkflow {
public:
    explicit AgentWorkflow(Database& database, OllamaConfig config = {});
    json run(int run_id, int project_id, const std::string& goal, const std::string& mode);
    json provider_status() const;

private:
    Database& database_;
    OllamaClient ollama_;

    json observe(int project_id);
    json plan(const json& context, const std::string& goal);
    json rule_plan(const json& context, const std::string& goal) const;
    json validate_plan(const json& candidate, const json& context) const;
    json execute(int run_id, int project_id, const json& plan, const std::string& mode);
    json verify(int project_id, const json& execution);
};

} // namespace orbit
