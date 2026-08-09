#include "orbit/agent.hpp"
#include "orbit/database.hpp"
#include "orbit/dev_agent.hpp"
#include "orbit/http_server.hpp"
#include "orbit/worker.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::atomic_bool stop_requested{false};

void on_signal(int) {
    stop_requested.store(true);
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool env_bool(const char* name, bool fallback) {
    std::string value = env_or(name, fallback ? "true" : "false");
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

struct Config {
    std::string role = "all";
    std::string host = env_or("ORBITOPS_HOST", "127.0.0.1");
    int port = std::stoi(env_or("ORBITOPS_PORT", "8080"));
    std::filesystem::path database = env_or("ORBITOPS_DB", "data/orbitops.db");
    std::filesystem::path web = env_or("ORBITOPS_WEB", "web");
    std::filesystem::path dev_workspace = env_or("ORBITOPS_DEV_WORKSPACE", "agent_workspace");
    orbit::OllamaConfig ollama = {
        env_bool("ORBITOPS_OLLAMA_ENABLED", true),
        env_or("ORBITOPS_OLLAMA_URL", "http://127.0.0.1:11434"),
        env_or("ORBITOPS_OLLAMA_MODEL", "qwen3:8b"),
        std::stoi(env_or("ORBITOPS_OLLAMA_TIMEOUT", "240")),
        env_bool("ORBITOPS_OLLAMA_FALLBACK", true)
    };
    std::string node_id = env_or("COMPUTERNAME", env_or("HOSTNAME", "node")) + "-" + std::to_string(process_id());
};

void print_help() {
    std::cout << R"HELP(OrbitOps 1.2.0 - distributed local-AI project operations platform

Usage: orbitops [options]
  --role <all|api|worker>  Process role (default: all)
  --host <address>         API bind address (default: 127.0.0.1)
  --port <number>          API port (default: 8080)
  --db <path>              SQLite database path
  --web <path>             Static web directory
  --node-id <id>           Cluster node identity
  --dev-workspace <path>   Fixed workspace for autonomous development
  --ollama-url <url>       Local Ollama base URL
  --ollama-model <name>    Ollama model (default: qwen3:8b)
  --no-ollama              Disable model inference and use rules
  --ollama-required        Fail workflow instead of rule fallback
  --help                   Show this help
)HELP";
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("Missing value after " + arg);
            return argv[i];
        };
        if (arg == "--role") config.role = next();
        else if (arg == "--host") config.host = next();
        else if (arg == "--port") config.port = std::stoi(next());
        else if (arg == "--db") config.database = next();
        else if (arg == "--web") config.web = next();
        else if (arg == "--node-id") config.node_id = next();
        else if (arg == "--dev-workspace") config.dev_workspace = next();
        else if (arg == "--ollama-url") config.ollama.base_url = next();
        else if (arg == "--ollama-model") config.ollama.model = next();
        else if (arg == "--no-ollama") config.ollama.enabled = false;
        else if (arg == "--ollama-required") config.ollama.fallback_to_rules = false;
        else if (arg == "--help" || arg == "-h") { print_help(); std::exit(0); }
        else throw std::invalid_argument("Unknown option: " + arg);
    }
    if (config.role != "all" && config.role != "api" && config.role != "worker") {
        throw std::invalid_argument("Role must be all, api, or worker");
    }
    if (config.port < 1 || config.port > 65535) throw std::invalid_argument("Invalid port");
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_args(argc, argv);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        orbit::Database database(config.database);
        database.migrate();
        database.seed_if_empty();
        orbit::AgentWorkflow agent(database, config.ollama);
        orbit::DeveloperAgent dev_agent(database, config.ollama, config.dev_workspace);

        if (config.role == "worker") {
            orbit::AgentWorker worker(database, agent, dev_agent, config.node_id);
            std::cout << "OrbitOps worker started: " << worker.id() << '\n';
            worker.run(stop_requested);
            return 0;
        }

        std::thread worker_thread;
        if (config.role == "all") {
            worker_thread = std::thread([&] {
                orbit::AgentWorker worker(database, agent, dev_agent, config.node_id + "-worker");
                worker.run(stop_requested);
            });
        }

        std::thread heartbeat([&] {
            const std::string node = config.node_id + "-api";
            while (!stop_requested.load()) {
                database.heartbeat_node(node, "api", config.host + ":" + std::to_string(config.port),
                                        {{"version", "1.2.0"}, {"role", config.role},
                                         {"agent_provider", config.ollama.enabled ? "ollama" : "rules"},
                                         {"agent_model", config.ollama.model}});
                for (int i = 0; i < 50 && !stop_requested.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        orbit::HttpServer server(database, agent, dev_agent, config.web);
        const bool ok = server.listen(config.host, config.port);
        stop_requested.store(true);
        if (heartbeat.joinable()) heartbeat.join();
        if (worker_thread.joinable()) worker_thread.join();
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "OrbitOps failed: " << error.what() << '\n';
        return 1;
    }
}
