#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace orbit {

using json = nlohmann::json;

struct OllamaConfig {
    bool enabled = true;
    std::string base_url = "http://127.0.0.1:11434";
    std::string model = "llama3.2:3b";
    int timeout_seconds = 360;
    bool fallback_to_rules = true;
    std::string review_model = "llama3.2:3b";
};

class OllamaClient {
public:
    explicit OllamaClient(OllamaConfig config = {});

    json generate_plan(const json& context, const std::string& goal) const;
    json generate_development_plan(const json& workspace, const std::string& goal,
                                   const std::string& feedback, int round) const;
    json review_development_result(const json& workspace, const std::string& goal) const;
    json status() const;
    const OllamaConfig& config() const { return config_; }

private:
    OllamaConfig config_;

    json request(const std::string& path, const json& body) const;
    json get(const std::string& path) const;
    static json response_schema();
    static json development_schema();
    static json development_review_schema();
    static std::string system_prompt();
};

} // namespace orbit
