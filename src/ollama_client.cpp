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

json OllamaClient::development_schema() {
    const json file_schema = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}}},
            {"content", {{"type", "string"}}},
            {"reason", {{"type", "string"}}}
        }},
        {"required", json::array({"path", "content", "reason"})},
        {"additionalProperties", false}
    };
    return {
        {"type", "object"},
        {"properties", {
            {"summary", {{"type", "string"}}},
            {"files", {{"type", "array"}, {"items", file_schema}, {"maxItems", 10}}},
            {"profiles", {{"type", "array"}, {"items", {
                {"type", "string"}, {"enum", json::array({"build", "test", "git_diff"})}
            }}, {"maxItems", 4}}},
            {"completion_criteria", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 6}}}
        }},
        {"required", json::array({"summary", "files", "profiles", "completion_criteria"})},
        {"additionalProperties", false}
    };
}

json OllamaClient::development_review_schema() {
    return {
        {"type", "object"},
        {"properties", {
            {"passed", {{"type", "boolean"}}},
            {"summary", {{"type", "string"}}},
            {"issues", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 10}}}
        }},
        {"required", json::array({"passed", "summary", "issues"})},
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
        {"model", config_.review_model},
        {"stream", false},
        {"think", false},
        {"keep_alive", "10m"},
        {"format", schema},
        {"options", {{"temperature", 0.1}, {"num_predict", 700}}},
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
        {"type", "ollama"}, {"model", response.value("model", config_.review_model)},
        {"prompt_tokens", response.value("prompt_eval_count", 0)},
        {"completion_tokens", response.value("eval_count", 0)},
        {"total_duration_ms", response.value("total_duration", 0LL) / 1000000.0}
    };
    return plan;
}

json OllamaClient::generate_development_plan(const json& workspace, const std::string& goal,
                                             const std::string& feedback, int round) const {
    const json schema = development_schema();
    const std::string system = R"PROMPT(你是 OrbitOps 自主 C++ 开发 Agent。你在一个固定工作区中完成用户的开发目标。
你必须观察现有文件，给出需要创建或修改的完整文件内容，然后选择 build 和 test 工具进行验证。
如果收到上一轮构建或测试错误，该日志是事实依据：先定位根因，再只修改必要文件，并保留上一轮已经通过的能力。
在输出前做一次静态自检：每个声明都有定义、每个可执行程序恰好有一个 main、入口函数不能因测试重构而丢失、可能抛异常的解析必须在异常处理范围内、Makefile 的 test 必须真正运行测试程序。
链接关系示例：如果 core.cpp 定义业务函数、cli_main.cpp 定义产品 main，那么产品目标必须同时编译 core.cpp 和 cli_main.cpp；测试目标应编译 core.cpp 和测试源码，但不能链接 cli_main.cpp。创建或重命名源码时必须同步修改 Makefile。
修复轮次的 files 数组只能包含内容确实需要改变的文件，未修改文件必须省略，不能原样重复输出。
禁止用 Hello World、空测试、只返回成功的测试或删除原有功能来绕过失败。必须逐项满足原始开发目标，保持 C++20 标准，并为新增与原有行为提供有意义的断言。
禁止声称构建或测试已经通过，是否通过只能由后续工具决定。禁止访问工作区外路径，禁止网络请求，禁止启动长期服务。
项目必须包含可重复执行的自动化测试和 Makefile；Windows 使用 mingw32-make，Linux 使用 make。
Makefile 必须使用 `#` 注释，所有配方命令必须以真正的 Tab 字符开头，C++ 源码必须用 `CXX = g++` 和 `-std=c++20`，并提供能编译且运行真实测试程序的 `test` 目标；禁止使用 `//` 注释或用 gcc 链接 .cpp。
不要只解释代码，必须提供能够直接写入磁盘的完整文件。代码保持简洁，不写冗长注释或重复内容。输出严格符合 JSON Schema。)PROMPT";
    std::string user = "开发目标：\n" + goal + "\n\n当前工作区快照：\n" + workspace.dump(2) +
                       "\n\n当前迭代轮次：" + std::to_string(round);
    if (!feedback.empty()) user += "\n\n上一轮工具反馈（请修复）：\n" + feedback;
    user += "\n\n请输出完整开发方案，并至少选择 build 和 test。JSON Schema：\n" + schema.dump();
    const json body = {
        {"model", config_.model}, {"stream", false}, {"think", false}, {"keep_alive", "10m"},
        {"format", schema}, {"options", {{"temperature", 0.1}, {"num_predict", 1600}}},
        {"messages", json::array({
            {{"role", "system"}, {"content", system}},
            {{"role", "user"}, {"content", user}}
        })}
    };
    const json response = request("/api/chat", body);
    if (!response.contains("message") || !response["message"].contains("content")) {
        throw std::runtime_error("Ollama development response is missing content");
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

json OllamaClient::review_development_result(const json& workspace, const std::string& goal) const {
    const json schema = development_review_schema();
    const std::string system = R"PROMPT(你是独立的软件验收审查员，不参与代码生成。
请严格对照原始开发目标检查最终工作区的实际源码、构建文件和测试内容。构建与测试退出码为零只是必要条件，不是充分条件。
如果项目被替换为 Hello World、删除了原有功能、降低了要求、测试没有真实断言、未覆盖关键错误路径、标准版本不符，必须判定 passed=false。
只有每项明确需求都有实现证据和有效测试证据时才能通过。输出严格符合 JSON Schema。)PROMPT";
    const std::string user = "原始开发目标：\n" + goal +
        "\n\n构建和测试工具已返回成功。请审查最终工作区是否真的完成目标：\n" + workspace.dump(2) +
        "\n\nJSON Schema：\n" + schema.dump();
    const json body = {
        {"model", config_.review_model}, {"stream", false}, {"think", false}, {"keep_alive", "10m"},
        {"format", schema}, {"options", {{"temperature", 0.0}, {"num_predict", 300}}},
        {"messages", json::array({
            {{"role", "system"}, {"content", system}},
            {{"role", "user"}, {"content", user}}
        })}
    };
    const json response = request("/api/chat", body);
    if (!response.contains("message") || !response["message"].contains("content")) {
        throw std::runtime_error("Ollama development review response is missing content");
    }
    json review = json::parse(response["message"]["content"].get<std::string>());
    review["provider"] = {
        {"type", "ollama"}, {"model", response.value("model", config_.review_model)},
        {"prompt_tokens", response.value("prompt_eval_count", 0)},
        {"completion_tokens", response.value("eval_count", 0)},
        {"total_duration_ms", response.value("total_duration", 0LL) / 1000000.0}
    };
    return review;
}

json OllamaClient::status() const {
    json result = {
        {"type", "ollama"}, {"enabled", config_.enabled}, {"base_url", config_.base_url},
        {"model", config_.model}, {"review_model", config_.review_model}, {"available", false},
        {"model_available", false}, {"review_model_available", false}
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
            if (name == config_.review_model || model.value("model", "") == config_.review_model) {
                result["review_model_available"] = true;
            }
        }
    } catch (const std::exception& error) {
        result["error"] = error.what();
    }
    return result;
}

} // namespace orbit
