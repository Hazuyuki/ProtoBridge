"""A bounded, persistent queue that executes the real ns-3 binary."""

from __future__ import annotations

import asyncio
import csv
import hashlib
import io
import json
import os
import re
import shutil
import signal
import subprocess
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .catalog import Catalog, RunRequest

TERMINAL = {"completed", "incomplete", "failed", "cancelled", "timed_out", "interrupted"}
MAX_PENDING = 16
MAX_LOG_BYTES = 16 * 1024 * 1024


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_result(output: str) -> dict[str, Any]:
    blocks = re.findall(r"^RESULT_START\s*$\n(.*?)^RESULT_END\s*$", output, re.M | re.S)
    if len(blocks) != 1 or len(re.findall(r"^RESULT_START\s*$", output, re.M)) != 1:
        raise ValueError("程序需要输出且仅输出一个完整 RESULT 结果块。")
    result: dict[str, Any] = {}
    for line in blocks[0].splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in result:
            raise ValueError(f"结果重复包含 {key}。")
        if re.fullmatch(r"-?\d+", value):
            parsed: Any = int(value)
        elif re.fullmatch(r"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?", value):
            parsed = float(value)
        elif value in {"true", "false"}:
            parsed = value == "true"
        else:
            parsed = value
        result[key] = parsed
    if result.get("status") not in {"complete", "incomplete"}:
        raise ValueError("结果缺少有效的 status。")
    if type(result.get("simTimeNs")) is not int or result["simTimeNs"] < 0:
        raise ValueError("结果缺少有效的整数 simTimeNs。")
    return result


def find_binary(repo: Path) -> Path | None:
    candidates = sorted((repo/"build/scratch").glob("*gpu-cluster-sim*"))
    candidates = [p for p in candidates if p.is_file() and os.access(p, os.X_OK)
                  and p.suffix not in {".o", ".so", ".py", ".cc"}]
    candidates.sort(key=lambda p: ("debug" in p.name, p.name))
    return candidates[0] if candidates else None


def git_identity(repo: Path) -> dict[str, str]:
    result = {}
    for key, arguments in [("commit", ["rev-parse", "HEAD"]), ("branch", ["branch", "--show-current"])]:
        try:
            result[key] = subprocess.check_output(["git", *arguments], cwd=repo, text=True, timeout=3, stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.SubprocessError):
            result[key] = "unknown"
    return result


class SimulationService:
    def __init__(self, repo: Path, data_dir: Path, binary: Path | None = None):
        self.repo = repo.resolve()
        self.data_dir = data_dir.expanduser().resolve()
        self.catalog = Catalog(self.repo)
        self.binary_override = binary
        self.jobs: dict[str, dict[str, Any]] = {}
        self.queue: asyncio.Queue[str] = asyncio.Queue()
        self.processes: dict[str, asyncio.subprocess.Process] = {}
        self.cancelled: set[str] = set()
        self.worker: asyncio.Task | None = None
        self._binary_fingerprint: tuple | None = None
        self._binary_sha = ""

    def binary(self) -> Path | None:
        candidate = self.binary_override or find_binary(self.repo)
        return candidate if candidate and candidate.is_file() and os.access(candidate, os.X_OK) else None

    def binary_identity(self) -> dict[str, Any]:
        binary = self.binary()
        if binary is None:
            return {"ready": False, "build_command": "./ns3 build gpu-cluster-sim"}
        stat = binary.stat()
        fingerprint = (str(binary), stat.st_size, stat.st_mtime_ns)
        if fingerprint != self._binary_fingerprint:
            self._binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
            self._binary_fingerprint = fingerprint
        return {"ready": True, "name": binary.name, "sha256": self._binary_sha,
                "modified_at": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat()}

    async def start(self):
        self.data_dir.mkdir(parents=True, exist_ok=True)
        paths = sorted(self.data_dir.glob("*/result.json"), key=lambda p: p.stat().st_mtime, reverse=True)[:500]
        for path in reversed(paths):
            try:
                job = json.loads(path.read_text())
                if not re.fullmatch(r"[0-9a-f]{32}", job["id"]) or path.parent.name != job["id"]:
                    continue
                self.jobs[job["id"]] = job
                if job["status"] not in TERMINAL:
                    job.update(status="interrupted", error="服务在上次运行结束前停止，请重新提交。", finished_at=now())
                    self.save(job)
            except (OSError, ValueError, KeyError, TypeError):
                continue
        self.worker = asyncio.create_task(self._work())

    def save(self, job: dict[str, Any]):
        directory = self.data_dir/job["id"]
        temporary = directory/"result.json.tmp"
        temporary.write_text(json.dumps(job, ensure_ascii=False, indent=2)+"\n")
        temporary.replace(directory/"result.json")

    def require_job(self, identifier: str) -> dict[str, Any]:
        if not re.fullmatch(r"[0-9a-f]{32}", identifier) or identifier not in self.jobs:
            raise KeyError("找不到这次运行。")
        return self.jobs[identifier]

    def summaries(self) -> list[dict[str, Any]]:
        fields = ["id", "name", "status", "created_at", "started_at", "finished_at", "wall_seconds", "summary", "result", "error"]
        return [{k: job.get(k) for k in fields} for job in reversed(list(self.jobs.values()))]

    def submit(self, request: RunRequest) -> dict[str, Any]:
        if self.binary() is None:
            raise RuntimeError("请先在仓库根目录执行 ./ns3 build gpu-cluster-sim。")
        if sum(j["status"] not in TERMINAL for j in self.jobs.values()) >= MAX_PENDING:
            raise OverflowError("队列已有 16 个未完成任务，请等待或取消任务后再提交。")
        prepared = self.catalog.prepare(request)
        identifier = uuid.uuid4().hex
        directory = self.data_dir/identifier
        directory.mkdir()
        (directory/"profile.profile").write_text(prepared["profile"])
        actual_config = prepared["config"].replace("profile = profile.profile", f"profile = {directory/'profile.profile'}", 1)
        (directory/"config.cfg").write_text(actual_config)
        (directory/"stdout.log").touch()
        job = {"id": identifier, "name": prepared["title"], "status": "queued", "created_at": now(),
               "started_at": None, "finished_at": None, "wall_seconds": None,
               "request": request.model_dump(), "summary": prepared["summary"],
               "notices": prepared["notices"], "config": actual_config, "profile": prepared["profile"], "cli": prepared["cli"],
               "result": None, "error": None, "source": git_identity(self.repo),
               "binary": None, "command": None, "launch_command": None}
        self.jobs[identifier] = job
        self.save(job)
        self.queue.put_nowait(identifier)
        return job

    async def cancel(self, identifier: str) -> dict[str, Any]:
        job = self.require_job(identifier)
        if job["status"] in TERMINAL:
            return job
        self.cancelled.add(identifier)
        if job["status"] == "queued":
            job.update(status="cancelled", error="用户取消了这次运行。", finished_at=now())
        else:
            job["status"] = "cancelling"
            process = self.processes.get(identifier)
            if process:
                await self._terminate(process)
        self.save(job)
        return job

    @staticmethod
    async def _terminate(process: asyncio.subprocess.Process):
        if process.returncode is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            await asyncio.wait_for(process.wait(), timeout=2)
        except asyncio.TimeoutError:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            await process.wait()

    async def _work(self):
        while True:
            identifier = await self.queue.get()
            try:
                job = self.jobs[identifier]
                if job["status"] != "queued":
                    continue
                await self._execute(job)
            finally:
                self.queue.task_done()

    async def _execute(self, job: dict[str, Any]):
        identifier = job["id"]
        directory = self.data_dir/identifier
        request = RunRequest(**job["request"])
        started = time.monotonic()
        job.update(status="running", started_at=now(), binary=self.binary_identity())
        try:
            binary = self.binary()
            if binary is None:
                raise RuntimeError("仿真程序不存在，请重新构建。")
            cli = job["cli"]
            command = [str(binary.resolve()), f"--protocolConfig={directory/'config.cfg'}",
                       f"--numGpus={job['summary']['ranks']}", f"--dataSize={request.data_size}",
                       f"--numNodes={request.num_nodes}", f"--gpusPerNode={request.gpus_per_node}",
                       f"--RngRun={request.seed}", f"--stepTraceFile={directory/'steps.csv'}",
                       *[f"--{key}={value}" for key, value in cli.items()]]
            # Linux prlimit bounds child memory/output and suppresses abort core files.
            # Other hosts retain timeout/cancellation and output-size monitoring.
            limiter = shutil.which("prlimit")
            launch = ([limiter, "--core=0", "--as=8589934592", "--fsize=33554432", "--"] if limiter else []) + command
            job.update(command=command, launch_command=launch)
            self.save(job)
            with (directory/"stdout.log").open("wb") as output:
                process = await asyncio.create_subprocess_exec(*launch, cwd=directory, stdout=output,
                                                               stderr=asyncio.subprocess.STDOUT, start_new_session=True)
                self.processes[identifier] = process
                while process.returncode is None:
                    if identifier in self.cancelled:
                        await self._terminate(process)
                        break
                    if time.monotonic()-started > request.timeout:
                        job.update(status="timed_out", error=f"运行超过 {request.timeout} 秒，已停止。")
                        await self._terminate(process)
                        break
                    if (directory/"stdout.log").stat().st_size > MAX_LOG_BYTES:
                        job.update(status="failed", error="运行日志超过 16 MiB，已停止任务。")
                        await self._terminate(process)
                        break
                    try:
                        await asyncio.wait_for(process.wait(), timeout=0.1)
                    except asyncio.TimeoutError:
                        pass
            job["exit_code"] = process.returncode
            if identifier in self.cancelled:
                job.update(status="cancelled", error="用户取消了这次运行。")
            elif job["status"] not in {"timed_out", "failed"}:
                output = (directory/"stdout.log").read_text(errors="replace")
                if process.returncode:
                    job.update(status="failed", error=f"仿真程序退出，退出码 {process.returncode}。请查看运行日志。")
                else:
                    result = parse_result(output)
                    job.update(result=result, status="completed" if result["status"] == "complete" else "incomplete")
                    if job["status"] == "incomplete":
                        job["error"] = "传输尚未全部交付，不能将该时长作为已完成操作的延迟。"
        except asyncio.CancelledError:
            process = self.processes.get(identifier)
            if process:
                await self._terminate(process)
            job.update(status="interrupted", error="服务停止，运行已中断。")
            raise
        except Exception as error:
            job.update(status="failed", error=str(error))
        finally:
            process = self.processes.get(identifier)
            if process and process.returncode is None:
                await self._terminate(process)
            self.processes.pop(identifier, None)
            self.cancelled.discard(identifier)
            job.update(finished_at=now(), wall_seconds=round(time.monotonic()-started, 4))
            self.save(job)

    def trace(self, identifier: str) -> dict[str, Any]:
        self.require_job(identifier)
        path = self.data_dir/identifier/"steps.csv"
        if not path.exists():
            return {"rows": [], "truncated": False}
        rows = []
        with path.open() as stream:
            for row in csv.DictReader(stream):
                try:
                    rows.append({key: int(row[key]) for key in ["gpu", "step", "collectiveStartNs", "completeNs", "sinceStartNs"]})
                except (ValueError, KeyError, TypeError):
                    continue
                if len(rows) >= 20000:
                    return {"rows": rows, "truncated": True}
        return {"rows": rows, "truncated": False}

    def export_csv(self) -> str:
        stream = io.StringIO()
        fields = ["id", "name", "status", "config", "ranks", "dataSizeBytes", "simTimeNs", "wallSeconds", "createdAt"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for job in reversed(list(self.jobs.values())):
            name = job["name"]
            if name.startswith(("=", "+", "-", "@", "\t", "\r")):
                name = "'" + name
            writer.writerow({"id": job["id"], "name": name, "status": job["status"],
                             "config": job["request"]["config_id"], "ranks": job["summary"]["ranks"],
                             "dataSizeBytes": job["request"]["data_size"], "simTimeNs": (job["result"] or {}).get("simTimeNs", ""),
                             "wallSeconds": job["wall_seconds"], "createdAt": job["created_at"]})
        return stream.getvalue()

    async def close(self):
        if self.worker:
            self.worker.cancel()
            await asyncio.gather(self.worker, return_exceptions=True)
        for job in self.jobs.values():
            if job["status"] not in TERMINAL:
                job.update(status="interrupted", error="服务停止，任务尚未运行。", finished_at=now())
                self.save(job)
