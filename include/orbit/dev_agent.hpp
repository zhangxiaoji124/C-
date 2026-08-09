#pragma once

#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

#include "orbit/ollama_client.hpp"

namespace orbit {

class Database;
using json = nlohmann::json;

class DeveloperAgent {
public:
    DeveloperAgent(Database& database, OllamaConfig config, std::filesystem::path workspace);

    json run(int run_id, const std::string& goal);
    json status() const;

private:
    Database& database_;
    OllamaClient ollama_;
    std::filesystem::path workspace_;

    json snapshot() const;
    json validate_plan(const json& candidate) const;
    json apply_files(int run_id, int& sequence, const json& files);
    json run_profile(int run_id, int& sequence, const std::string& profile);
    json run_command(const std::string& command) const;
    std::filesystem::path safe_path(const std::string& relative) const;
};

} // namespace orbit

