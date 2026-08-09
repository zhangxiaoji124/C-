#include "orbit/http_server.hpp"
#include "orbit/agent.hpp"
#include "orbit/database.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace orbit {

namespace {

void send_json(httplib::Response& response, const json& body, int status = 200) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

json parse_body(const httplib::Request& request) {
    if (request.body.empty()) return json::object();
    return json::parse(request.body);
}

int path_id(const httplib::Request& request) {
    return std::stoi(request.matches[1].str());
}

std::optional<int> int_param(const httplib::Request& request, const char* name) {
    if (!request.has_param(name)) return std::nullopt;
    return std::stoi(request.get_param_value(name));
}

} // namespace

HttpServer::HttpServer(Database& database, AgentWorkflow& agent, std::filesystem::path web_root)
    : database_(database), agent_(agent), web_root_(std::move(web_root)) {}

bool HttpServer::listen(const std::string& host, int port) {
    httplib::Server server;
    server.set_read_timeout(10, 0);
    server.set_write_timeout(10, 0);
    server.set_payload_max_length(1024 * 1024);

    server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& response) {
        response.set_header("Access-Control-Allow-Origin", "*");
        response.set_header("Access-Control-Allow-Headers", "Content-Type, Idempotency-Key");
        response.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("X-Frame-Options", "DENY");
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
    });

    server.set_exception_handler([](const httplib::Request&, httplib::Response& response,
                                    std::exception_ptr error) {
        try {
            if (error) std::rethrow_exception(error);
        } catch (const nlohmann::json::exception& exception) {
            send_json(response, {{"error", "请求 JSON 格式或字段无效"}, {"detail", exception.what()}}, 400);
        } catch (const std::invalid_argument& exception) {
            send_json(response, {{"error", exception.what()}}, 400);
        } catch (const std::exception& exception) {
            std::cerr << "[api] " << exception.what() << '\n';
            send_json(response, {{"error", "服务器处理请求失败"}, {"detail", exception.what()}}, 500);
        }
    });
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (response.status == 404) send_json(response, {{"error", "资源不存在"}}, 404);
    });

    server.Get("/api/health", [&](const httplib::Request&, httplib::Response& response) {
        send_json(response, {{"status", "ok"}, {"service", "orbitops-api"}, {"version", "1.1.0"}});
    });
    server.Get("/api/dashboard", [&](const httplib::Request&, httplib::Response& response) {
        send_json(response, database_.dashboard());
    });
    server.Get("/api/cluster", [&](const httplib::Request&, httplib::Response& response) {
        send_json(response, database_.cluster_status());
    });
    server.Get("/api/agent/provider", [&](const httplib::Request&, httplib::Response& response) {
        send_json(response, agent_.provider_status());
    });
    server.Get("/metrics", [&](const httplib::Request&, httplib::Response& response) {
        const json dashboard = database_.dashboard();
        const json cluster = database_.cluster_status();
        std::string metrics =
            "# HELP orbitops_tasks_total Total tasks\n# TYPE orbitops_tasks_total gauge\n"
            "orbitops_tasks_total " + std::to_string(dashboard["stats"].value("total_tasks", 0)) + "\n"
            "# HELP orbitops_active_projects Active projects\n# TYPE orbitops_active_projects gauge\n"
            "orbitops_active_projects " + std::to_string(dashboard["stats"].value("active_projects", 0)) + "\n";
        for (const auto& [status, count] : cluster["queue"].items()) {
            metrics += "orbitops_jobs{status=\"" + status + "\"} " + std::to_string(count.get<int>()) + "\n";
        }
        response.set_content(metrics, "text/plain; version=0.0.4; charset=utf-8");
    });

    server.Get("/api/projects", [&](const httplib::Request&, httplib::Response& response) {
        send_json(response, database_.list_projects());
    });
    server.Get(R"(/api/projects/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        const json project = database_.get_project(path_id(request));
        project.is_null() ? send_json(response, {{"error", "项目不存在"}}, 404) : send_json(response, project);
    });
    server.Post("/api/projects", [&](const httplib::Request& request, httplib::Response& response) {
        const json body = parse_body(request);
        if (!body.contains("name") || body["name"].get<std::string>().empty()) {
            throw std::invalid_argument("项目名称不能为空");
        }
        const int id = database_.create_project(body);
        send_json(response, database_.get_project(id), 201);
    });
    server.Patch(R"(/api/projects/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        const int id = path_id(request);
        database_.update_project(id, parse_body(request))
            ? send_json(response, database_.get_project(id))
            : send_json(response, {{"error", "项目不存在"}}, 404);
    });
    server.Delete(R"(/api/projects/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        database_.delete_project(path_id(request))
            ? send_json(response, {{"deleted", true}})
            : send_json(response, {{"error", "项目不存在"}}, 404);
    });

    server.Get("/api/tasks", [&](const httplib::Request& request, httplib::Response& response) {
        std::optional<std::string> status;
        if (request.has_param("status")) status = request.get_param_value("status");
        send_json(response, database_.list_tasks(int_param(request, "project_id"), status));
    });
    server.Get(R"(/api/tasks/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        const json task = database_.get_task(path_id(request));
        task.is_null() ? send_json(response, {{"error", "任务不存在"}}, 404) : send_json(response, task);
    });
    server.Post("/api/tasks", [&](const httplib::Request& request, httplib::Response& response) {
        const json body = parse_body(request);
        if (!body.contains("project_id") || !body.contains("title") || body["title"].get<std::string>().empty()) {
            throw std::invalid_argument("project_id 和任务标题不能为空");
        }
        const int id = database_.create_task(body);
        send_json(response, database_.get_task(id), 201);
    });
    server.Patch(R"(/api/tasks/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        const int id = path_id(request);
        database_.update_task(id, parse_body(request))
            ? send_json(response, database_.get_task(id))
            : send_json(response, {{"error", "任务不存在"}}, 404);
    });
    server.Delete(R"(/api/tasks/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        database_.delete_task(path_id(request))
            ? send_json(response, {{"deleted", true}})
            : send_json(response, {{"error", "任务不存在"}}, 404);
    });

    server.Get("/api/activities", [&](const httplib::Request& request, httplib::Response& response) {
        const int limit = int_param(request, "limit").value_or(30);
        send_json(response, database_.list_activities(limit));
    });
    server.Get("/api/agent/runs", [&](const httplib::Request& request, httplib::Response& response) {
        send_json(response, database_.list_agent_runs(int_param(request, "limit").value_or(20)));
    });
    server.Get(R"(/api/agent/runs/(\d+))", [&](const httplib::Request& request, httplib::Response& response) {
        const json run = database_.get_agent_run(path_id(request));
        run.is_null() ? send_json(response, {{"error", "Agent 运行记录不存在"}}, 404) : send_json(response, run);
    });
    server.Post("/api/agent/runs", [&](const httplib::Request& request, httplib::Response& response) {
        const json body = parse_body(request);
        if (!body.contains("project_id") || !body.contains("goal")) {
            throw std::invalid_argument("project_id 和 goal 不能为空");
        }
        const int project_id = body["project_id"].get<int>();
        if (database_.get_project(project_id).is_null()) throw std::invalid_argument("项目不存在");
        const std::string goal = body["goal"].get<std::string>();
        if (goal.empty() || goal.size() > 1000) throw std::invalid_argument("目标长度应为 1-1000 个字符");
        const std::string mode = body.value("mode", "preview");
        if (mode != "preview" && mode != "execute") throw std::invalid_argument("mode 只能是 preview 或 execute");
        const std::string key = request.get_header_value("Idempotency-Key");
        const int run_id = database_.create_agent_run(project_id, goal, mode, key);
        const int job_id = database_.enqueue_job("agent_run",
            {{"run_id", run_id}, {"project_id", project_id}, {"goal", goal}, {"mode", mode}},
            3, "agent-run-" + std::to_string(run_id));
        const json persisted = database_.get_agent_run(run_id);
        send_json(response, {{"run_id", run_id}, {"job_id", job_id}, {"status", persisted.value("status", "queued")}}, 202);
    });

    if (!std::filesystem::exists(web_root_ / "index.html")) {
        throw std::runtime_error("Web root is missing index.html: " + web_root_.string());
    }
    if (!server.set_mount_point("/", web_root_.string())) {
        throw std::runtime_error("Cannot mount web directory: " + web_root_.string());
    }
    server.set_file_extension_and_mimetype_mapping("js", "text/javascript; charset=utf-8");
    server.set_file_extension_and_mimetype_mapping("css", "text/css; charset=utf-8");

    std::cout << "OrbitOps API listening on http://" << host << ':' << port << '\n';
    return server.listen(host, port);
}

} // namespace orbit
