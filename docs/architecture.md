# OrbitOps 架构说明

## 设计目标

OrbitOps 同时满足两个场景：单机上开箱即用，以及在不修改业务代码的前提下将 API 与 Agent 执行器拆开部署。核心设计原则是请求快速返回、工作可靠落盘、执行可重试、结果可解释。

## 进程角色

| 角色 | 职责 | 是否有状态 |
|---|---|---|
| `api` | REST 接口、静态 Web、参数校验、任务入队、查询结果 | 无本地会话状态 |
| `worker` | 领取 Agent 任务、运行五阶段工作流、写入步骤与结果 | 仅持有短期任务租约 |
| `all` | 在一个进程内同时运行 API 与 Worker，方便本地开发 | 同上 |

API 创建 `agent_runs` 后，将带唯一 `dedupe_key` 的任务写入 `jobs`。Worker 使用 `BEGIN IMMEDIATE` 事务领取最早可用任务，同时写入 `worker_id`、尝试次数和 `lease_until`。只有持有对应租约的 Worker 能完成或回退任务。

## 任务状态机

```mermaid
stateDiagram-v2
    [*] --> queued
    queued --> processing: Worker 获取租约
    processing --> completed: 执行与验证成功
    processing --> queued: 失败且仍可重试
    processing --> queued: 租约超时，被其他 Worker 接管
    processing --> failed: 达到最大尝试次数
    completed --> [*]
    failed --> [*]
```

重试等待时间按 `attempts²` 秒增加，最大 30 秒。默认最多尝试三次。Worker 异常退出时不需要显式恢复，租约到期后任务会自动重新进入可领取范围。

## 数据模型

```mermaid
erDiagram
    PROJECTS ||--o{ TASKS : contains
    PROJECTS ||--o{ AGENT_RUNS : scopes
    AGENT_RUNS ||--o{ AGENT_STEPS : records
    AGENT_RUNS ||--|| JOBS : dispatched_as
    DEV_RUNS ||--o{ DEV_STEPS : records
    DEV_RUNS ||--|| JOBS : dispatched_as
    SERVICE_NODES {
      string node_id PK
      string role
      datetime last_heartbeat
    }
    JOBS {
      int id PK
      string status
      string worker_id
      datetime lease_until
      int attempts
    }
```

SQLite 启用 WAL、外键、5 秒 busy timeout、NORMAL synchronous 和针对队列/任务查询的组合索引。初始化演示数据也在 `BEGIN IMMEDIATE` 中完成，避免多个服务首次并发启动时重复写入。

## 本地模型规划

`Plan` 阶段通过 Ollama 原生 `/api/chat` 接口调用本地模型，关闭流式传输并传入 JSON Schema。响应包含模型、输入/输出 Token 数和推理耗时，随 Agent 运行结果一同持久化。默认模型为 `llama3.2:3b`，可通过环境变量替换。

当本地模型不可连接、模型不存在或返回内容无法通过校验时，系统默认降级至确定性规则规划器。`--ollama-required` 可用于集成测试和严格生产环境，防止静默降级。

## Agent 安全边界

- `preview` 模式生成完整计划但不修改业务数据。
- `execute` 模式只调用 C++ 注册的 `create_task` 和 `update_task` 工具。
- 模型生成内容必须通过 JSON Schema 和二次 C++ 语义验证。
- 更新操作只能引用当前项目中真实存在的任务 ID，不能直接标记完成。
- 单次规划最多提升三个风险任务，并生成有限数量的拆解任务。
- 所有动作记录执行实体 ID，完成后重新观察项目并运行一致性检查。
- 任一阶段异常会记录 `error` 步骤，将运行标记为失败，并交由队列决定是否重试。

## 自主开发执行器

`DeveloperAgent` 与项目规划工作流共用持久化队列，但拥有独立的 `dev_runs/dev_steps` 审计模型。它将工作区快照和用户目标发送给 Ollama，使用 JSON Schema 约束文件列表与工具配置。C++ 层随后再次验证相对路径、扩展名、单文件大小、总写入量和工具白名单。

```mermaid
flowchart LR
    Goal["开发目标"] --> Observe["读取沙箱"]
    Observe --> Plan["Ollama 结构化计划"]
    Plan --> Validate["C++ 安全校验"]
    Validate --> Write["写入完整文件"]
    Write --> Build["固定构建配置"]
    Build --> Test["固定测试配置"]
    Build -->|失败日志| Plan
    Test -->|失败日志| Plan
    Test -->|通过| Review["独立目标审查"]
    Review -->|不满足原始目标| Plan
    Review -->|通过| Done["持久化结果"]
```

Worker 每 20 秒续租正在运行的长任务，进程崩溃后租约到期即可由其他节点接管。`claim_job` 对 `dev_run` 额外施加数据库级互斥：任一时刻只有一个未过期开发作业可以写共享工作区，而普通项目规划任务仍可由其他 Worker 并行处理。

## 扩展建议

当前实现适合单机多进程和同一 Docker 主机。更高规模下建议保持 `Database` 与任务接口不变，替换以下基础设施：

- SQLite → PostgreSQL，使用 `SELECT ... FOR UPDATE SKIP LOCKED` 领取任务；
- 数据库队列 → Redis Streams、NATS JetStream 或 Kafka；
- 单点静态前端 → CDN；
- `/metrics` → Prometheus，节点日志 → Loki/ELK；
- API 前增加 Nginx、Traefik 或云负载均衡器。

业务接口、前端和 `AgentWorkflow` 不依赖具体队列产品，因此迁移边界清晰。
