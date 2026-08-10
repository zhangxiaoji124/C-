# OrbitOps · 分布式 AI 项目运营台

一个适合 C++ 实习展示与二次开发的全栈项目：C++20 编写 HTTP API、持久化任务队列和 AI Agent Worker，SQLite 负责事务与数据存储，原生 Web 前端提供项目总览、看板、Agent 工作流追踪和集群监控。

默认不依赖云服务或 API Key，一条命令即可运行完整系统。API 与 Worker 也可以拆成独立进程或容器横向扩展。

## 核心能力

- 完整业务闭环：项目与任务 CRUD、筛选、看板拖拽、优先级、进度和活动日志
- 本地模型 Agent：Ollama `llama3.2:3b` 分别以开发、项目规划和独立验收角色运行；也可切换 `qwen3:8b` 等代码模型，完整数据留在本机
- 可解释工作流：`Intake → Observe → Plan → Act → Verify` 五阶段全程留痕
- 自主开发闭环：观察代码 → 生成完整文件 → 编译 → 测试 → 目标审查 → 错误反馈，最多自动修复七轮
- 分布式执行：API/Worker 角色分离、SQLite WAL 持久化队列、任务租约、超时接管、指数退避与最多三次重试
- 一致性保护：请求幂等键、任务去重键、白名单工具调用、执行后数据校验
- 可观测性：节点注册与心跳、队列状态、Agent 步骤留痕、`/metrics` Prometheus 指标
- 工程化：CMake 与 Make 双构建、单元测试、跨进程 E2E 测试、Docker Compose 多节点示例
- 高效运行：原生二进制、HTTP 线程池、SQLite WAL、预编译语句、索引与内存临时表

## 架构

```mermaid
flowchart LR
    Browser["Web 前端"] --> API1["API 节点 A"]
    Browser --> API2["API 节点 B"]
    API1 --> DB[("SQLite WAL")]
    API2 --> DB
    DB --> Q["持久化任务队列"]
    Q --> W1["Agent Worker 1"]
    Q --> W2["Agent Worker 2"]
    W1 --> DB
    W2 --> DB
    W1 --> Flow["观察 → 规划 → 工具 → 校验"]
    W2 --> Flow
    Flow --> Sandbox["隔离开发工作区"]
    Flow --> Ollama["本地 Ollama"]
```

本地演示使用 SQLite，实现同一主机或共享 Docker Volume 上的多进程协调。若要跨主机大规模部署，持久层接口已集中封装在 `Database` 类中，可替换为 PostgreSQL，任务层可迁移到 Redis Streams 或 Kafka；业务 API 与 Agent 工作流无需改变。

## 快速启动

### Windows（MinGW）

要求：支持 C++20 的 GCC、GNU Make、PowerShell。

```powershell
.\scripts\build.ps1
.\scripts\run.ps1
```

浏览器访问 <http://127.0.0.1:8080>。

### Linux / macOS

```bash
make -j$(nproc)
./build/orbitops --role all
```

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/orbitops
```

首次运行会自动创建 `data/orbitops.db`、执行数据库迁移并写入演示数据。

### 启用本地 Ollama

安装并启动 [Ollama](https://ollama.com/)，准备默认模型：

```bash
ollama pull llama3.2:3b
ollama pull qwen3:8b  # 可选：硬件性能充足时用作开发模型
ollama serve
```

OrbitOps 默认连接 `http://127.0.0.1:11434`。Agent 页面会显示模型在线状态。可通过环境变量切换模型：

```powershell
$env:ORBITOPS_OLLAMA_MODEL='llama3.2:3b'
.\scripts\run.ps1
```

Ollama 不可用时系统会自动降级到规则规划器；使用 `--ollama-required` 可以关闭降级，让模型错误直接反映在工作流结果中。

## 分布式运行

在不同终端启动 API 和多个 Worker。它们共享同一个数据库文件：

```powershell
# 终端 1：只运行 Web/API
.\build\orbitops.exe --role api --port 8080 --node-id api-1

# 终端 2、3：运行两个 Agent Worker
.\build\orbitops.exe --role worker --node-id worker-1
.\build\orbitops.exe --role worker --node-id worker-2
```

或用 Docker Compose 启动一个 API 和两个 Worker：

```bash
docker compose up --build
```

可在网页「集群状态」查看节点心跳和队列，也可以访问 `/metrics` 接入 Prometheus。

## AI Agent 如何工作

用户提交自然语言目标后，API 返回 `202 Accepted` 并立即将工作放入队列：

1. **Intake**：理解目标，确定 `preview` 或 `execute` 安全边界。
2. **Observe**：读取项目、任务、截止日期、工作量与负责人等实时上下文。
3. **Plan**：把实时上下文交给本地 Ollama，使用 JSON Schema 约束模型输出，生成带理由的结构化工具计划。
4. **Act**：预览模式不写数据；执行模式只允许调用已注册的任务创建/更新工具。
5. **Verify**：重新读取数据库，核对工具结果与项目数据一致性。

模型输出不会直接执行：C++ 会过滤未知工具、校验任务 ID、限制动作数量、移除危险字段，并禁止模型将任务直接标记为完成。Ollama 不可用时会降级到确定性规则规划器，因此系统依然可以离线运行且无需付费 API。

### 自主开发 Agent

Agent 页面的「自主开发 Agent」处理真实代码，不只是生成任务卡片。提交目标后，Worker 会读取固定的 `agent_workspace`，要求 Ollama 按 JSON Schema 返回完整文件和执行方案，然后只通过下列受控工具执行：

1. `workspace_snapshot`：读取最多 40 个源码/配置文件，并限制上下文总量；
2. `write_file`：只写入工作区内的 C/C++、Markdown、JSON、YAML、Make/CMake 文件；
3. `build`：执行固定的 `make -j2` 构建配置；
4. `test`：执行固定的 `make test`；
5. `git_diff`：仅在工作区本身是 Git 仓库时执行 `git diff --check`。

构建或测试失败时，真实日志会进入下一轮模型上下文，最多自动修复七轮。工具层会规范化 Makefile 的注释、Tab、C++ 编译器和语言标准等确定性格式错误；语义仍由模型完成。即使工具返回成功，独立目标审查器仍会重新读取最终源码和测试，防止用 Hello World、空测试或删除原功能伪装完成；审查不通过同样会触发下一轮修复。所有观察、计划、写文件、命令结果、目标审查和验证结论分别写入 `dev_runs` 与 `dev_steps`。多个分布式 Worker 会续租长任务，但同一工作区的开发作业严格串行，避免并发覆盖。开发 Agent 必须使用在线 Ollama，不会用规则引擎伪装成代码生成。

工作区可通过 `ORBITOPS_DEV_WORKSPACE` 或 `--dev-workspace` 修改；建议始终指向专用目录。开发 Agent 会执行模型生成的 C++ 代码和 Makefile，因此生产环境还应在独立的低权限账户或容器中运行 Worker；固定工作区限制的是文件写入路径，不等同于操作系统级隔离。

## API 概览

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/api/health` | 服务健康检查 |
| `GET` | `/api/dashboard` | 总览聚合数据 |
| `GET/POST` | `/api/projects` | 查询、创建项目 |
| `GET/PATCH/DELETE` | `/api/projects/{id}` | 项目详情、更新、删除 |
| `GET/POST` | `/api/tasks` | 查询、创建任务 |
| `GET/PATCH/DELETE` | `/api/tasks/{id}` | 任务详情、更新、删除 |
| `POST` | `/api/agent/runs` | 异步提交 Agent 工作流 |
| `GET` | `/api/agent/runs/{id}` | 查询工作流和步骤 |
| `GET` | `/api/agent/provider` | Ollama 与模型在线状态 |
| `GET` | `/api/dev/status` | 开发 Agent、模型、工具与沙箱状态 |
| `POST` | `/api/dev/runs` | 异步提交自主开发作业 |
| `GET` | `/api/dev/runs/{id}` | 查询文件、构建、测试与修复时间线 |
| `GET` | `/api/cluster` | 节点与队列状态 |
| `GET` | `/metrics` | Prometheus 文本指标 |

创建 Agent 工作流示例：

```bash
curl -X POST http://127.0.0.1:8080/api/agent/runs \
  -H "Content-Type: application/json" \
  -H "Idempotency-Key: sprint-plan-001" \
  -d '{"project_id":1,"goal":"为下轮迭代制定计划并识别风险","mode":"execute"}'
```

## 测试

```powershell
.\scripts\test.ps1
```

测试分为三层：

- `orbitops_tests`：27 项数据层、两类 Agent、幂等、开发任务串行化、队列租约与节点发现断言。
- `tests/e2e_test.py`：启动独立 API/Worker 进程，验证 Web、REST、开发 Agent 状态、持久化队列、Agent 执行、集群发现和监控指标，共 14 项断言。
- `tests/ollama_integration_test.py`：如果本机存在配置模型，启动独立服务并验证真实模型推理、结构化计划、安全工具执行与 Token/耗时指标；没有模型时自动跳过。
- `orbitops_dev_review_test`：向本地模型提供一个“Hello World 冒充计算器”的退化工作区，验证独立目标审查必须拒绝空洞实现；模型不可用时自动跳过。

## 项目结构

```text
include/orbit/       C++ 公共接口
src/                 数据库、HTTP、Agent、Worker 与程序入口
web/                 零构建的响应式 Web 前端
tests/               单元测试与跨进程端到端测试
scripts/             Windows 构建、运行、测试脚本
third_party/         固定版本的 httplib、nlohmann/json、SQLite
CMakeLists.txt       CMake 构建定义
Makefile             GNU Make 构建定义
Dockerfile           生产镜像
docker-compose.yml   API + 双 Worker 演示集群
```

## 技术选型

- C++20、[cpp-httplib](https://github.com/yhirose/cpp-httplib)
- SQLite 3.50.4（WAL 模式）
- [nlohmann/json](https://github.com/nlohmann/json)
- HTML5、CSS3、原生 JavaScript

## License

本项目使用 MIT License。第三方依赖保留其各自许可证。
