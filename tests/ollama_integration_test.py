"""Optional real-model integration test for a locally running Ollama instance."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.error
import urllib.request


ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / ("orbitops.exe" if os.name == "nt" else "orbitops")
DB = ROOT / "build" / "orbitops-ollama-test.db"
PORT = 18083
BASE = f"http://127.0.0.1:{PORT}"
MODEL = os.getenv("ORBITOPS_OLLAMA_MODEL", "llama3.2:3b")
PLAN_MODEL = os.getenv("ORBITOPS_OLLAMA_REVIEW_MODEL", "llama3.2:3b")


def call(base: str, path: str, method: str = "GET", payload: dict | None = None, headers: dict | None = None):
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
    request = urllib.request.Request(base + path, data=data, method=method,
                                     headers={"Content-Type": "application/json; charset=utf-8", **(headers or {})})
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def ollama_ready() -> bool:
    try:
        tags = call("http://127.0.0.1:11434", "/api/tags")
        models = {item.get("name") or item.get("model") for item in tags.get("models", [])}
        return MODEL in models and PLAN_MODEL in models
    except (OSError, urllib.error.URLError):
        return False


def wait_api() -> None:
    for _ in range(100):
        try:
            if call(BASE, "/api/health")["status"] == "ok":
                return
        except (OSError, urllib.error.URLError):
            time.sleep(0.1)
    raise RuntimeError("OrbitOps API did not start")


def main() -> int:
    if not ollama_ready():
        print(f"SKIP: local Ollama model {MODEL!r} is unavailable")
        return 0
    for suffix in ("", "-wal", "-shm"):
        try:
            pathlib.Path(str(DB) + suffix).unlink()
        except FileNotFoundError:
            pass

    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    common = ["--db", "build/orbitops-ollama-test.db", "--web", "web",
              "--ollama-model", MODEL, "--review-model", PLAN_MODEL, "--ollama-required"]
    api_process = subprocess.Popen([str(EXE), "--role", "api", "--node-id", "ollama-test-api",
                                    "--port", str(PORT), *common], cwd=ROOT, creationflags=flags)
    worker = None
    try:
        wait_api()
        worker = subprocess.Popen([str(EXE), "--role", "worker", "--node-id", "ollama-test-worker", *common],
                                  cwd=ROOT, creationflags=flags)
        provider = call(BASE, "/api/agent/provider")
        assert (provider["available"] and provider["model_available"] and
                provider["review_model_available"] and provider["model"] == MODEL)

        project = call(BASE, "/api/projects", "POST", {
            "name": "珠宝电商智能客服", "description": "提升高客单价珠宝咨询的准确率与成交转化", "color": "#6754d9"
        })
        call(BASE, "/api/tasks", "POST", {
            "project_id": project["id"], "title": "整理钻石参数与售后政策知识库",
            "status": "in_progress", "priority": "high", "estimate_hours": 8
        })
        queued = call(BASE, "/api/agent/runs", "POST", {
            "project_id": project["id"],
            "goal": "请规划珠宝电商智能客服上线前的质量保障工作，创建三个具体任务，分别覆盖专业术语准确性、敏感承诺拦截和真实对话验收，不要生成泛化任务。",
            "mode": "execute"
        }, {"Idempotency-Key": "real-ollama-integration"})

        deadline = time.time() + 180
        run = None
        while time.time() < deadline:
            run = call(BASE, f"/api/agent/runs/{queued['run_id']}")
            if run["status"] in ("completed", "failed"):
                break
            time.sleep(0.5)
        assert run and run["status"] == "completed", run
        assert run["output"]["provider"]["type"] == "ollama", run["output"]
        assert run["output"]["provider"]["model"] == PLAN_MODEL
        assert run["output"]["execution"]["applied_count"] >= 1
        generated = [task for task in call(BASE, f"/api/tasks?project_id={project['id']}")
                     if "Ollama 生成" in task.get("tags", [])]
        assert generated
        print(json.dumps({
            "status": "passed", "model": PLAN_MODEL, "run_id": run["id"],
            "duration_ms": run["output"]["provider"]["total_duration_ms"],
            "prompt_tokens": run["output"]["provider"]["prompt_tokens"],
            "completion_tokens": run["output"]["provider"]["completion_tokens"],
            "generated_tasks": [task["title"] for task in generated]
        }, ensure_ascii=False, indent=2))
        return 0
    finally:
        for process in (api_process, worker):
            if process is not None and process.poll() is None:
                process.terminate()
        for process in (api_process, worker):
            if process is None:
                continue
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Ollama integration failed: {error}", file=sys.stderr)
        raise
