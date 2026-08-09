#include "orbit/ollama_client.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace orbit {

OllamaClient::OllamaClient(OllamaConfig config) : config_(std::move(config)) {}

json OllamaClient::response_schema() {
    const json args_properties = {
        {"task_id", {{"type", "integer"}}},
        {"project_id", {{"type", "integer"}}},
        {"title", {{"type", "string"}}},
        {"description", {{"type", "string"}}},
        {"status", {{"type", "string"}, {"enum", json::array({"todo", "in_progress", "review"})}}},
        {"priority", {{"type", "string"}, {"enum", json::array({"low", "medium", "high", "urgent"})}}},
        {"assignee", {{"type", "string"}}},
        {"estimate_hours", {{"type", "number"}, {"minimum", 0}, {"maximum", 80}}},
        {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 5}}}
    };
    const json action_schema = {
        {"type", "object"},
        {"properties", {
            {"tool", {{"type", "string"}, {"enum", json::array({"create_task", "update_task"})}}},
            {"args", {{"type", "object"}, {"properties", args_properties}, {"additionalProperties", false}}},
            {"reason", {{"type", "string"}}}
        }},
        {"required", json::array({"tool", "args", "reason"})},
        {"additionalProperties", false}
    };
    return {
        {"type", "object"},
        {"properties", {
            {"summary", {{"type", "string"}}},
            {"insights", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 5}}},
            {"actions", {{"type", "array"}, {"maxItems", 6}, {"items", action_schema}}}
        }},
        {"required", json::array({"summary", "insights", "actions"})},
        {"additionalProperties", false}
    };
}

std::string OllamaClient::system_prompt() {
    return R"PROMPT(你是 OrbitOps 项目规划 Agent。你会收到一个项目目标以及来自数据库的实时项目上下文。
你的职责是分析风险并输出少量、具体、可执行的工具计划。

安全规则：
1. 只能使用 create_task 或 update_task。
2. create_task 应创建有业务含义的具体任务，标题不要重复目标原文，必须包含清晰交付物。
3. update_task 只能操作上下文中已有的 task_id；不要把任务直接标记为 done。
4. 不要删除任务，不要虚构负责人，不要修改项目。
5. 每次最多六个动作，优先解决用户目标和真实风险，不需要为了凑数创建任务。
6. 使用简洁中文，给出决策理由。输出必须严格符合提供的 JSON Schema。)PROMPT";
}

json OllamaClient::request(const std::string& path, const json& body) const {
    if (!config_.enabled) throw std::runtime_error("Ollama provider is disabled");
    if (config_.base_url.rfind("http://", 0) != 0) {
        throw std::runtime_error("Only local HTTP Ollama endpoints are supported");
    }
    httplib::Client client(config_.base_url);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(std::max(5, config_.timeout_seconds), 0);
    client.set_write_timeout(10, 0);
    const auto response = client.Post(path, body.dump(), "application/json");
    if (!response) {
        throw std::runtime_error("Cannot connect to Ollama at " + config_.base_url);
    }
    if (response->status < 200 || response->status >= 300) {
        std::string detail = response->body;
        try { detail = json::parse(response->body).value("error", response->body); } catch (...) {}
        throw std::runtime_error("Ollama returned HTTP " + std::to_string(response->status) + ": " + detail);
    }
    return json::parse(response->body);
}

json OllamaClient::get(const std::string& path) const {
    if (!config_.enabled) throw std::runtime_error("Ollama provider is disabled");
    httplib::Client client(config_.base_url);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(3, 0);
    const auto response = client.Get(path);
    if (!response || response->status != 200) throw std::runtime_error("Ollama is unavailable");
    return json::parse(response->body);
}

json OllamaClient::generate_plan(const json& context, const std::string& goal) const {
    const json schema = response_schema();
    const json body = {
        {"model", config_.model},
        {"stream", false},
        {"think", false},
        {"keep_alive", "10m"},
        {"format", schema},
        {"options", {{"temperature", 0.1}, {"num_predict", 1200}}},
        {"messages", json::array({
            {{"role", "system"}, {"content", system_prompt()}},
            {{"role", "user"}, {"content",
                "用户目标：\n" + goal + "\n\n项目实时上下文：\n" + context.dump(2) +
                "\n\n请严格按这个 JSON Schema 输出：\n" + schema.dump()}}
        })}
    };
    const json response = request("/api/chat", body);
    if (!response.contains("message") || !response["message"].contains("content")) {
        throw std::runtime_error("Ollama response does not contain message.content");
    }
    json plan = json::parse(response["message"]["content"].get<std::string>());
    plan["provider"] = {
        {"type", "ollama"}, {"model", response.value("model", config_.model)},
        {"prompt_tokens", response.value("prompt_eval_count", 0)},
        {"completion_tokens", response.value("eval_count", 0)},
        {"total_duration_ms", response.value("total_duration", 0LL) / 1000000.0}
    };
    return plan;
}

json OllamaClient::status() const {
    json result = {
        {"type", "ollama"}, {"enabled", config_.enabled}, {"base_url", config_.base_url},
        {"model", config_.model}, {"available", false}, {"model_available", false}
    };
    if (!config_.enabled) return result;
    try {
        const json tags = get("/api/tags");
        result["available"] = true;
        result["models"] = json::array();
        for (const auto& model : tags.value("models", json::array())) {
            const std::string name = model.value("name", model.value("model", ""));
            result["models"].push_back(name);
            if (name == config_.model || model.value("model", "") == config_.model) result["model_available"] = true;
        }
    } catch (const std::exception& error) {
        result["error"] = error.what();
    }
    return result;
}

} // namespace orbit
