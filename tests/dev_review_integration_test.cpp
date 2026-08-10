#include "orbit/ollama_client.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        orbit::OllamaConfig config;
        config.timeout_seconds = 180;
        orbit::OllamaClient client(config);
        const auto status = client.status();
        if (!status.value("available", false) || !status.value("review_model_available", false)) {
            std::cout << "Developer review integration skipped: local llama3.2:3b is unavailable.\n";
            return 0;
        }

        const orbit::json workspace = {
            {"root", "review-fixture"},
            {"file_count", 3},
            {"files", orbit::json::array({
                {{"path", "calculator.cpp"}, {"content", "#include <iostream>\nint main(){std::cout << \"Hello, World!\\n\";}\n"}},
                {{"path", "test_calculator.cpp"}, {"content", "int main(){return 0;}\n"}},
                {{"path", "Makefile"}, {"content", "all:\n\tg++ -std=c++11 calculator.cpp -o calculator\ntest: all\n\t./calculator\n"}}
            })}
        };
        const std::string goal =
            "创建 C++20 命令行计算器，支持 add、sub、mul、div、mod，覆盖除零、非数字和 mod 非整数测试。";
        const auto review = client.review_development_result(workspace, goal);
        if (review.value("passed", true)) {
            throw std::runtime_error("Reviewer incorrectly accepted a hollow Hello World implementation");
        }
        if (!review.contains("issues") || review["issues"].empty()) {
            throw std::runtime_error("Reviewer rejected the fixture without actionable issues");
        }
        std::cout << orbit::json({
            {"status", "passed"},
            {"review_passed", review["passed"]},
            {"summary", review["summary"]},
            {"issues", review["issues"]},
            {"model", review["provider"]["model"]}
        }).dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Developer review integration failed: " << error.what() << '\n';
        return 1;
    }
}
