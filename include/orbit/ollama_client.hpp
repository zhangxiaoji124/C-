#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace orbit {

using json = nlohmann::json;

struct OllamaConfig {
    bool enabled = true;
    std::string base_url = "http://127.0.0.1:11434";
    std::string model = "qwen3:8b";
    int timeout_seconds = 120;
    bool fallback_to_rules = true;
};

class OllamaClient {
public:
    explicit OllamaClient(OllamaConfig config = {});

    json generate_plan(const json& context, const std::string& goal) const;
    json status() const;
    const OllamaConfig& config() const { return config_; }

private:
    OllamaConfig config_;

    json request(const std::string& path, const json& body) const;
    json get(const std::string& path) const;
    static json response_schema();
    static std::string system_prompt();
};

} // namespace orbit

