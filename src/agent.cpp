#include "orbit/agent.hpp"
#include "orbit/database.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace orbit {

namespace {

std::string today() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d");
    return stream.str();
}

bool contains_any(const std::string& value, const std::initializer_list<std::string>& terms) {
    return std::any_of(terms.begin(), terms.end(), [&](const std::string& term) {
        return value.find(term) != std::string::npos;
    });
}

std::string compact_goal(const std::string& goal) {
    if (goal.size() <= 36) return goal;
    return goal.substr(0, 36) + "…";
}

} // namespace

AgentWorkflow::AgentWorkflow(Database& database) : database_(database) {}

json AgentWorkflow::observe(int project_id) {
    const json project = database_.get_project(project_id);
    if (project.is_null()) throw std::invalid_argument("项目不存在");

    json context;
    context["project"] = {
        {"id", project["id"]}, {"name", project["name"]},
        {"description", project["description"]}, {"due_date", project["due_date"]}
    };
    context["tasks"] = project["tasks"];
    context["today"] = today();
    context["signals"] = {
        {"total", project["tasks"].size()}, {"todo", 0}, {"in_progress", 0},
        {"review", 0}, {"done", 0}, {"overdue", 0}, {"unassigned", 0},
        {"urgent_open", 0}, {"estimated_open_hours", 0.0}
    };
    context["risks"] = json::array();
    for (const auto& task : project["tasks"]) {
        const std::string status = task.value("status", "todo");
        context["signals"][status] = context["signals"].value(status, 0) + 1;
        if (status != "done") {
            context["signals"]["estimated_open_hours"] =
                context["signals"]["estimated_open_hours"].get<double>() + task.value("estimate_hours", 0.0);
            if (task.value("assignee", "未分配") == "未分配") {
                context["signals"]["unassigned"] = context["signals"]["unassigned"].get<int>() + 1;
            }
            if (task.value("priority", "medium") == "urgent") {
                context["signals"]["urgent_open"] = context["signals"]["urgent_open"].get<int>() + 1;
            }
            if (!task["due_date"].is_null() && task["due_date"].get<std::string>() < context["today"].get<std::string>()) {
                context["signals"]["overdue"] = context["signals"]["overdue"].get<int>() + 1;
                context["risks"].push_back({
                    {"type", "overdue"}, {"task_id", task["id"]}, {"title", task["title"]},
                    {"due_date", task["due_date"]}, {"priority", task["priority"]}
                });
            }
        }
    }
    return context;
}

json AgentWorkflow::plan(const json& context, const std::string& goal) {
    json result = {
        {"intent", "项目健康度分析与行动编排"},
        {"summary", "已结合项目进度、截止日期、优先级和人员分配生成执行计划。"},
        {"actions", json::array()},
        {"insights", json::array()}
    };
    const auto& signals = context["signals"];
    const auto& project = context["project"];

    if (signals.value("overdue", 0) > 0) {
        result["insights"].push_back("发现 " + std::to_string(signals["overdue"].get<int>()) + " 个逾期任务，应优先处理。");
        int promoted = 0;
        for (const auto& risk : context["risks"]) {
            if (risk.value("priority", "medium") != "urgent" && promoted < 3) {
                result["actions"].push_back({
                    {"tool", "update_task"}, {"args", {{"task_id", risk["task_id"]}, {"priority", "urgent"}}},
                    {"reason", "逾期任务需要提升可见性"}
                });
                ++promoted;
            }
        }
    }
    if (signals.value("unassigned", 0) > 0) {
        result["insights"].push_back("有 " + std::to_string(signals["unassigned"].get<int>()) + " 个任务尚未分配负责人。");
    }
    if (signals.value("review", 0) >= 2) {
        result["insights"].push_back("评审列存在堆积，建议设置集中评审时段。");
    }

    if (contains_any(goal, {"计划", "规划", "拆解", "迭代", "plan", "sprint"})) {
        const std::string subject = compact_goal(goal);
        const std::vector<std::pair<std::string, std::string>> templates = {
            {"明确验收标准 · " + subject, "先定义可验证的完成条件，减少返工。"},
            {"执行核心工作 · " + subject, "将目标转为可跟踪的交付任务。"},
            {"复盘与效果验证 · " + subject, "用数据验证结果并沉淀改进项。"}
        };
        const std::vector<std::string> priorities = {"high", "high", "medium"};
        for (std::size_t i = 0; i < templates.size(); ++i) {
            result["actions"].push_back({
                {"tool", "create_task"},
                {"args", {
                    {"project_id", project["id"]}, {"title", templates[i].first},
                    {"description", templates[i].second}, {"priority", priorities[i]},
                    {"status", "todo"}, {"assignee", "未分配"},
                    {"estimate_hours", i == 1 ? 8 : 3}, {"tags", json::array({"Agent 生成"})}
                }},
                {"reason", templates[i].second}
            });
        }
    }

    if (result["actions"].empty()) {
        result["actions"].push_back({
            {"tool", "create_task"},
            {"args", {
                {"project_id", project["id"]}, {"title", "跟进：" + compact_goal(goal)},
                {"description", "由 Orbit Agent 根据当前项目上下文生成的跟进项。"},
                {"priority", signals.value("urgent_open", 0) > 0 ? "high" : "medium"},
                {"status", "todo"}, {"assignee", "未分配"}, {"estimate_hours", 2},
                {"tags", json::array({"Agent 生成"})}
            }},
            {"reason", "将自然语言目标转为可跟踪任务"}
        });
    }
    result["action_count"] = result["actions"].size();
    return result;
}

json AgentWorkflow::execute(int, int, const json& plan_data, const std::string& mode) {
    json execution = {{"mode", mode}, {"applied", json::array()}, {"skipped", json::array()}};
    for (const auto& action : plan_data["actions"]) {
        if (mode != "execute") {
            execution["skipped"].push_back({{"tool", action["tool"]}, {"reason", "预览模式未写入数据"}});
            continue;
        }
        const std::string tool = action["tool"];
        const json& args = action["args"];
        if (tool == "create_task") {
            const int id = database_.create_task(args, "agent");
            execution["applied"].push_back({{"tool", tool}, {"entity_id", id}, {"title", args["title"]}});
        } else if (tool == "update_task") {
            const int id = args.at("task_id").get<int>();
            if (database_.update_task(id, args, "agent")) {
                execution["applied"].push_back({{"tool", tool}, {"entity_id", id}});
            } else {
                execution["skipped"].push_back({{"tool", tool}, {"reason", "任务不存在或无需变更"}});
            }
        } else {
            execution["skipped"].push_back({{"tool", tool}, {"reason", "工具不在允许列表"}});
        }
    }
    execution["applied_count"] = execution["applied"].size();
    return execution;
}

json AgentWorkflow::verify(int project_id, const json& execution) {
    const json latest = observe(project_id);
    json checks = json::array({
        {{"name", "project_accessible"}, {"passed", !latest["project"].is_null()}},
        {{"name", "actions_accounted_for"}, {"passed", execution.contains("applied") && execution.contains("skipped")}},
        {{"name", "database_consistent"}, {"passed", latest["signals"]["total"].get<int>() >= latest["signals"]["done"].get<int>()}}
    });
    const bool passed = std::all_of(checks.begin(), checks.end(), [](const json& check) {
        return check.value("passed", false);
    });
    return {{"passed", passed}, {"checks", checks}, {"final_signals", latest["signals"]}};
}

json AgentWorkflow::run(int run_id, int project_id, const std::string& goal, const std::string& mode) {
    database_.mark_agent_run_running(run_id);
    try {
        database_.add_agent_step(run_id, 1, "intake", "completed", "理解目标并确定安全执行边界",
                                 {{"goal", goal}, {"mode", mode}});

        const json context = observe(project_id);
        database_.add_agent_step(run_id, 2, "observe", "completed", "读取项目、任务与风险信号", context["signals"]);

        const json plan_data = plan(context, goal);
        database_.add_agent_step(run_id, 3, "plan", "completed", "生成可解释的工具调用计划", plan_data);

        const json execution = execute(run_id, project_id, plan_data, mode);
        database_.add_agent_step(run_id, 4, "act", "completed",
                                 mode == "execute" ? "执行白名单数据库工具" : "预览计划，未修改数据", execution);

        const json verification = verify(project_id, execution);
        database_.add_agent_step(run_id, 5, "verify", verification["passed"].get<bool>() ? "completed" : "failed",
                                 "校验执行结果与数据一致性", verification);

        json output = {
            {"summary", plan_data["summary"]}, {"insights", plan_data["insights"]},
            {"plan", plan_data["actions"]}, {"execution", execution}, {"verification", verification}
        };
        database_.update_agent_run(run_id, verification["passed"].get<bool>() ? "completed" : "failed", output);
        return output;
    } catch (const std::exception& error) {
        const json output = {{"error", error.what()}};
        database_.add_agent_step(run_id, 99, "error", "failed", "工作流异常终止", output);
        database_.update_agent_run(run_id, "failed", output);
        throw;
    }
}

} // namespace orbit
