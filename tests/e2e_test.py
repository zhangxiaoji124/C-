"""End-to-end test: independent API and Worker processes sharing the durable queue."""

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
DB = ROOT / "build" / "orbitops-e2e.db"
PORT = 18081
BASE = f"http://127.0.0.1:{PORT}"


def request(path: str, method: str = "GET", payload: dict | None = None, headers: dict | None = None):
    data = json.dumps(payload).encode() if payload is not None else None
    merged = {"Content-Type": "application/json", **(headers or {})}
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=merged)
    with urllib.request.urlopen(req, timeout=3) as response:
        body = response.read().decode()
        return response.status, json.loads(body) if "json" in response.headers.get("Content-Type", "") else body


def wait_for_api(timeout: float = 8) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if request("/api/health")[1]["status"] == "ok":
                return
        except (OSError, urllib.error.URLError):
            time.sleep(0.1)
    raise RuntimeError("API did not become healthy")


def main() -> int:
    if not EXE.exists():
        raise RuntimeError(f"Build binary first: {EXE}")
    for suffix in ("", "-wal", "-shm"):
        try:
            pathlib.Path(str(DB) + suffix).unlink()
        except FileNotFoundError:
            pass

    # Keep process arguments relative so the same test also covers non-ASCII workspaces on MinGW.
    common = ["--db", "build/orbitops-e2e.db", "--web", "web", "--no-ollama"]
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    api_process = subprocess.Popen([str(EXE), "--role", "api", "--node-id", "e2e-api", "--port", str(PORT), *common],
                                   cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=flags)
    worker = None
    assertions = 0
    try:
        wait_for_api()
        worker = subprocess.Popen([str(EXE), "--role", "worker", "--node-id", "e2e-worker", *common],
                                  cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=flags)
        status, health = request("/api/health")
        assert status == 200 and health["service"] == "orbitops-api"; assertions += 1

        status, html = request("/")
        assert status == 200 and "OrbitOps" in html; assertions += 1

        status, dev_status = request("/api/dev/status")
        assert status == 200 and not dev_status["enabled"] and "workspace" in dev_status; assertions += 1

        _, project = request("/api/projects", "POST", {
            "name": "E2E 分布式测试", "description": "API 与 Worker 跨进程协作", "color": "#0984E3"
        })
        assert project["id"] > 0; assertions += 1

        _, task = request("/api/tasks", "POST", {
            "project_id": project["id"], "title": "验证任务持久化", "priority": "high", "estimate_hours": 2
        })
        assert task["title"] == "验证任务持久化"; assertions += 1

        _, queued = request("/api/agent/runs", "POST", {
            "project_id": project["id"], "goal": "为下一个迭代制定计划并拆解关键任务", "mode": "execute"
        }, {"Idempotency-Key": "e2e-agent-run"})
        assert queued["status"] == "queued"; assertions += 1
        _, duplicate = request("/api/agent/runs", "POST", {
            "project_id": project["id"], "goal": "重复请求不会重复执行", "mode": "execute"
        }, {"Idempotency-Key": "e2e-agent-run"})
        assert duplicate["run_id"] == queued["run_id"] and duplicate["job_id"] == queued["job_id"]; assertions += 1

        deadline = time.time() + 10
        run = None
        while time.time() < deadline:
            _, run = request(f"/api/agent/runs/{queued['run_id']}")
            if run["status"] in ("completed", "failed"):
                break
            time.sleep(0.15)
        assert run and run["status"] == "completed"; assertions += 1
        assert len(run["steps"]) == 5 and run["output"]["verification"]["passed"]; assertions += 1
        assert run["output"]["execution"]["applied_count"] >= 3; assertions += 1

        _, tasks = request(f"/api/tasks?project_id={project['id']}")
        assert len(tasks) >= 4; assertions += 1

        cluster = None
        deadline = time.time() + 3
        while time.time() < deadline:
            _, cluster = request("/api/cluster")
            if cluster["queue"]["completed"] >= 1:
                break
            time.sleep(0.1)
        node_ids = {node["node_id"] for node in cluster["nodes"]}
        assert "e2e-worker" in node_ids and "e2e-api-api" in node_ids; assertions += 1
        assert cluster["queue"]["completed"] >= 1; assertions += 1

        _, metrics = request("/metrics")
        assert "orbitops_tasks_total" in metrics and "orbitops_jobs" in metrics; assertions += 1
        print(f"E2E passed: {assertions} assertions; API -> queue -> worker -> database -> API verified.")
        return 0
    finally:
        for process in (api_process, worker):
            if process is not None and process.poll() is None:
                process.terminate()
        for process in (api_process, worker):
            if process is None:
                continue
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"E2E failed: {exc}", file=sys.stderr)
        raise
