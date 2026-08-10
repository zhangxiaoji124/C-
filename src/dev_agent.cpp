#include "orbit/dev_agent.hpp"
#include "orbit/database.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace orbit {

namespace {

std::string json_safe_log(std::string value) {
    try {
        const json probe = value;
        (void)probe.dump();
        return value;
    } catch (const json::type_error&) {
        for (char& character : value) {
            if (static_cast<unsigned char>(character) >= 0x80) character = '?';
        }
        return value;
    }
}

std::string normalize_makefile(std::string content) {
    const std::array<std::string, 4> old_standards = {
        "-std=c++98", "-std=c++11", "-std=c++14", "-std=c++17"
    };
    for (const auto& standard : old_standards) {
        std::size_t position = 0;
        while ((position = content.find(standard, position)) != std::string::npos) {
            content.replace(position, standard.size(), "-std=c++20");
            position += 10;
        }
    }
    std::istringstream input(content);
    std::ostringstream output;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("//", 0) == 0) line.replace(0, 2, "#");
        if (line.rfind("all:", 0) == 0) {
            std::string prerequisite = line.substr(4);
            prerequisite.erase(0, prerequisite.find_first_not_of(" \t"));
            const auto end = prerequisite.find_first_of(" \t");
            if (end != std::string::npos) prerequisite.resize(end);
            if (!prerequisite.empty() && content.find("\n" + prerequisite + ":") == std::string::npos &&
                content.find("-o " + prerequisite) != std::string::npos) {
                line = "all:";
            }
        }
        const std::array<std::string, 7> recipes = {
            "g++ ", "gcc ", "clang++ ", "$(CXX) ", "$(CC) ", "./", ".\\"
        };
        if (!line.empty() && line.front() != '\t') {
            for (const auto& prefix : recipes) {
                if (line.rfind(prefix, 0) == 0) {
                    if (prefix == "gcc " && line.find(".cpp") != std::string::npos) line.replace(0, 3, "g++");
                    line.insert(line.begin(), '\t');
                    break;
                }
            }
        }
        output << line << '\n';
    }
    return output.str();
}

} // namespace

DeveloperAgent::DeveloperAgent(Database& database, OllamaConfig config, std::filesystem::path workspace)
    : database_(database), ollama_(std::move(config)), workspace_(std::filesystem::absolute(std::move(workspace))) {
    std::filesystem::create_directories(workspace_);
}

json DeveloperAgent::status() const {
    json result = ollama_.status();
    result["workspace"] = workspace_.string();
    result["max_rounds"] = 7;
    result["tools"] = json::array({"workspace_snapshot", "write_file", "build", "test", "git_diff"});
    result["execution_scope"] = "fixed_workspace";
    return result;
}

std::filesystem::path DeveloperAgent::safe_path(const std::string& relative) const {
    if (relative.empty()) throw std::invalid_argument("File path cannot be empty");
    const std::filesystem::path requested(relative);
    if (requested.is_absolute()) throw std::invalid_argument("Absolute paths are not allowed");
    const auto root = std::filesystem::weakly_canonical(workspace_);
    const auto candidate = std::filesystem::weakly_canonical(root / requested);
    const auto inside = candidate.lexically_relative(root);
    if (inside.empty() || *inside.begin() == "..") {
        throw std::invalid_argument("Path escapes the developer workspace: " + relative);
    }
    return candidate;
}

json DeveloperAgent::snapshot() const {
    json result = {{"root", workspace_.string()}, {"files", json::array()}};
    const std::set<std::string> readable = {
        ".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".txt", ".md", ".json", ".yaml", ".yml"
    };
    std::size_t total_content = 0;
    if (!std::filesystem::exists(workspace_)) return result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             workspace_, std::filesystem::directory_options::skip_permission_denied)) {
        if (result["files"].size() >= 40) break;
        if (entry.is_directory()) continue;
        const auto relative = std::filesystem::relative(entry.path(), workspace_).generic_string();
        if (relative.rfind("build/", 0) == 0 || relative.rfind(".git/", 0) == 0 ||
            relative.rfind("node_modules/", 0) == 0) continue;
        json file = {{"path", relative}, {"size", entry.file_size()}};
        const std::string filename = entry.path().filename().string();
        const bool is_makefile = filename == "Makefile" || filename == "CMakeLists.txt";
        if ((is_makefile || readable.contains(entry.path().extension().string())) &&
            entry.file_size() <= 32768 && total_content < 100000) {
            std::ifstream stream(entry.path(), std::ios::binary);
            std::ostringstream content;
            content << stream.rdbuf();
            file["content"] = content.str();
            total_content += file["content"].get_ref<const std::string&>().size();
        }
        result["files"].push_back(std::move(file));
    }
    result["file_count"] = result["files"].size();
    return result;
}

json DeveloperAgent::validate_plan(const json& candidate) const {
    if (!candidate.is_object() || !candidate.contains("files") || !candidate["files"].is_array()) {
        throw std::runtime_error("Development plan has an invalid structure");
    }
    json result = {
        {"summary", candidate.value("summary", "本地模型已生成开发方案。")},
        {"files", json::array()}, {"profiles", json::array()},
        {"completion_criteria", candidate.value("completion_criteria", json::array())},
        {"provider", candidate.value("provider", json::object())}
    };
    std::size_t total_bytes = 0;
    for (const auto& file : candidate["files"]) {
        if (!file.is_object() || !file.contains("path") || !file.contains("content") ||
            !file["path"].is_string() || !file["content"].is_string() || result["files"].size() >= 10) continue;
        const std::string path = file["path"].get<std::string>();
        safe_path(path);
        std::string content = file["content"].get<std::string>();
        if (std::filesystem::path(path).filename() == "Makefile") {
            content = normalize_makefile(std::move(content));
        }
        if (content.size() > 131072 || total_bytes + content.size() > 524288) continue;
        const auto extension = std::filesystem::path(path).extension().string();
        const std::string filename = std::filesystem::path(path).filename().string();
        const std::set<std::string> allowed_extensions = {
            ".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".txt", ".md", ".json", ".yaml", ".yml"
        };
        if (!allowed_extensions.contains(extension) && filename != "Makefile" && filename != "CMakeLists.txt") continue;
        json normalized = {
            {"path", path}, {"content", content},
            {"reason", file.value("reason", "实现开发目标")}
        };
        auto existing = std::find_if(result["files"].begin(), result["files"].end(), [&](const json& item) {
            return item.value("path", "") == path;
        });
        if (existing == result["files"].end()) result["files"].push_back(std::move(normalized));
        else *existing = std::move(normalized);
        total_bytes += content.size();
    }
    const std::set<std::string> profiles = {"build", "test", "git_diff"};
    if (candidate.contains("profiles") && candidate["profiles"].is_array()) {
        for (const auto& profile : candidate["profiles"]) {
            if (profile.is_string() && profiles.contains(profile.get<std::string>()) &&
                std::find(result["profiles"].begin(), result["profiles"].end(), profile) == result["profiles"].end()) {
                result["profiles"].push_back(profile);
            }
        }
    }
    if (std::find(result["profiles"].begin(), result["profiles"].end(), "build") == result["profiles"].end()) {
        result["profiles"].push_back("build");
    }
    if (std::find(result["profiles"].begin(), result["profiles"].end(), "test") == result["profiles"].end()) {
        result["profiles"].push_back("test");
    }
    return result;
}

json DeveloperAgent::apply_files(int run_id, int& sequence, const json& files) {
    json written = json::array();
    for (const auto& file : files) {
        const auto path = safe_path(file["path"].get<std::string>());
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write file: " + path.string());
        const std::string content = file["content"].get<std::string>();
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        const json result = {{"path", file["path"]}, {"bytes", content.size()}};
        database_.add_dev_step(run_id, sequence++, "write_file", "completed",
                               {{"path", file["path"]}, {"reason", file["reason"]}}, result);
        written.push_back(result);
    }
    return written;
}

json DeveloperAgent::run_command(const std::string& command) const {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        if (read_pipe) CloseHandle(read_pipe);
        if (write_pipe) CloseHandle(write_pipe);
        throw std::runtime_error("Cannot create build output pipe");
    }
    HANDLE null_input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : null_input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    std::string command_line = "cmd.exe /D /S /C " + command + " 2>&1";
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        if (job) CloseHandle(job);
        if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        throw std::runtime_error("Cannot start build tool");
    }
    CloseHandle(write_pipe);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 125);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(read_pipe);
        CloseHandle(job);
        throw std::runtime_error("Cannot isolate build process tree");
    }
    ResumeThread(process.hThread);

    std::string output;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    bool timed_out = false;
    while (true) {
        DWORD available = 0;
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(read_pipe, buffer.data(), requested, &read, nullptr) || read == 0) break;
            if (output.size() < 100000) output.append(buffer.data(), read);
        }
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            TerminateJobObject(job, 124);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    DWORD available = 0;
    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        DWORD read = 0;
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(read_pipe, buffer.data(), requested, &read, nullptr) || read == 0) break;
        if (output.size() < 100000) output.append(buffer.data(), read);
    }
    DWORD native_exit = 125;
    GetExitCodeProcess(process.hProcess, &native_exit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    CloseHandle(job);
    if (timed_out) output += "\nCommand timed out after 120 seconds; the process tree was terminated.\n";
    const int exit_code = timed_out ? 124 : static_cast<int>(native_exit);
    return {{"command", command}, {"exit_code", exit_code}, {"success", exit_code == 0},
            {"timed_out", timed_out}, {"output", json_safe_log(std::move(output))}};
#else
    const std::string shell_command = "timeout 120s " + command + " 2>&1";
    FILE* pipe = popen(shell_command.c_str(), "r");
    if (!pipe) throw std::runtime_error("Cannot start build tool");
    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        if (output.size() < 100000) output += buffer.data();
    }
    const int raw = pclose(pipe);
    const int exit_code = WIFEXITED(raw) ? WEXITSTATUS(raw) : raw;
    return {{"command", command}, {"exit_code", exit_code}, {"success", exit_code == 0},
            {"timed_out", exit_code == 124}, {"output", json_safe_log(std::move(output))}};
#endif
}

json DeveloperAgent::run_profile(int run_id, int& sequence, const std::string& profile) {
    std::string command;
    std::error_code relative_error;
    const auto relative_workspace = std::filesystem::relative(workspace_, std::filesystem::current_path(), relative_error);
    const bool safe_relative = !relative_error && !relative_workspace.empty() && *relative_workspace.begin() != "..";
    const std::string command_workspace = safe_relative ? relative_workspace.string() : workspace_.string();
    if (profile == "build") {
        if (!std::filesystem::exists(workspace_ / "Makefile")) {
            json missing = {{"profile", profile}, {"success", false}, {"exit_code", -1},
                            {"output", "Makefile is missing; create a complete Makefile with build and test targets."}};
            database_.add_dev_step(run_id, sequence++, profile, "failed", json::object(), missing);
            return missing;
        }
#ifdef _WIN32
        command = "mingw32-make -C \"" + command_workspace + "\" -j2";
#else
        command = "make -C \"" + command_workspace + "\" -j2";
#endif
    } else if (profile == "test") {
#ifdef _WIN32
        command = "mingw32-make -C \"" + command_workspace + "\" test";
#else
        command = "make -C \"" + command_workspace + "\" test";
#endif
    } else if (profile == "git_diff") {
        if (!std::filesystem::exists(workspace_ / ".git")) {
            json skipped = {{"profile", profile}, {"success", true}, {"skipped", true}, {"output", "Workspace is not a Git repository"}};
            database_.add_dev_step(run_id, sequence++, profile, "completed", json::object(), skipped);
            return skipped;
        }
        command = "git -C \"" + command_workspace + "\" diff --check";
    } else {
        throw std::invalid_argument("Unknown execution profile: " + profile);
    }
    json result = run_command(command);
    result["profile"] = profile;
    database_.add_dev_step(run_id, sequence++, profile, result["success"].get<bool>() ? "completed" : "failed",
                           {{"command", command}}, result);
    return result;
}

json DeveloperAgent::run(int run_id, const std::string& goal) {
    database_.mark_dev_run_running(run_id);
    int sequence = 1;
    std::string feedback;
    json all_written = json::array();
    json all_tools = json::array();
    json last_plan;
    json last_review;
    try {
        for (int round = 1; round <= 7; ++round) {
            const json current = snapshot();
            database_.add_dev_step(run_id, sequence++, "observe", "completed",
                                   {{"round", round}}, {{"file_count", current["file_count"]}});
            last_plan = validate_plan(ollama_.generate_development_plan(current, goal, feedback, round));
            database_.add_dev_step(run_id, sequence++, "plan", "completed",
                                   {{"round", round}, {"feedback", feedback}},
                                   {{"summary", last_plan["summary"]}, {"file_count", last_plan["files"].size()},
                                    {"profiles", last_plan["profiles"]}, {"provider", last_plan["provider"]}});
            const json written = apply_files(run_id, sequence, last_plan["files"]);
            for (const auto& item : written) all_written.push_back(item);

            bool success = !last_plan["files"].empty() || round > 1;
            std::ostringstream failures;
            for (const auto& profile : last_plan["profiles"]) {
                const json tool = run_profile(run_id, sequence, profile.get<std::string>());
                all_tools.push_back(tool);
                if (!tool.value("success", false)) {
                    success = false;
                    failures << "Profile " << profile.get<std::string>() << " failed:\n"
                             << tool.value("output", "") << "\n"
                             << "The executed build/test command comes from the current Makefile. "
                                "If entrypoints, source files, or targets changed, update the Makefile consistently. "
                                "Do not repeat unchanged files that produced this failure.\n";
                    break;
                }
            }
            if (success) {
                last_review = ollama_.review_development_result(snapshot(), goal);
                const bool goal_passed = last_review.value("passed", false);
                database_.add_dev_step(run_id, sequence++, "review", goal_passed ? "completed" : "failed",
                                       {{"goal", goal}}, last_review);
                if (!goal_passed) {
                    std::ostringstream review_feedback;
                    review_feedback << "Independent goal review failed: "
                                    << last_review.value("summary", "requirements are not satisfied") << "\n";
                    for (const auto& issue : last_review.value("issues", json::array())) {
                        if (issue.is_string()) review_feedback << "- " << issue.get<std::string>() << "\n";
                    }
                    feedback = review_feedback.str();
                    continue;
                }
                json output = {
                    {"success", true}, {"summary", last_plan["summary"]}, {"rounds", round},
                    {"workspace", workspace_.string()}, {"written_files", all_written},
                    {"tool_results", all_tools}, {"completion_criteria", last_plan["completion_criteria"]},
                    {"provider", last_plan["provider"]}, {"review", last_review}
                };
                database_.add_dev_step(run_id, sequence++, "verify", "completed", json::object(),
                                       {{"build_and_test_passed", true}, {"goal_review_passed", true}});
                database_.update_dev_run(run_id, "completed", output);
                return output;
            }
            feedback = failures.str();
            if (feedback.empty()) feedback = "模型没有生成可写入文件，请提供完整实现。";
        }
        json output = {
            {"success", false}, {"summary", "七轮自动修复后，构建、测试或目标审查仍未通过。"},
            {"rounds", 7},
            {"workspace", workspace_.string()}, {"written_files", all_written},
            {"tool_results", all_tools}, {"last_feedback", feedback},
            {"provider", last_plan.value("provider", json::object())}, {"review", last_review}
        };
        database_.add_dev_step(run_id, sequence++, "verify", "failed", json::object(),
                               {{"accepted", false}, {"feedback", feedback}});
        database_.update_dev_run(run_id, "failed", output);
        return output;
    } catch (const std::exception& error) {
        const json output = {{"success", false}, {"error", error.what()}, {"workspace", workspace_.string()}};
        database_.add_dev_step(run_id, sequence, "error", "failed", json::object(), output);
        database_.update_dev_run(run_id, "failed", output);
        throw;
    }
}

} // namespace orbit
