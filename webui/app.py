"""Same-origin HTTP API and buildless frontend for ProtoBridge."""

from __future__ import annotations

import io
import os
import zipfile
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import urlsplit

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles

from .catalog import RunRequest
from .simulation import SimulationService, TERMINAL, git_identity

STATIC = Path(__file__).parent/"static"


def create_app(repo: Path | None = None, data_dir: Path | None = None, binary: Path | None = None) -> FastAPI:
    root = repo or Path(os.environ.get("PROTOBRIDGE_WEB_REPO", Path(__file__).resolve().parents[1]))
    storage = data_dir or Path(os.environ.get("PROTOBRIDGE_WEB_DATA_DIR", Path.home()/".cache/protobridge-web/runs"))

    @asynccontextmanager
    async def lifespan(application: FastAPI):
        service = SimulationService(root, storage, binary)
        application.state.service = service
        await service.start()
        yield
        await service.close()

    application = FastAPI(title="ProtoBridge Web Lab", version="0.1.0", lifespan=lifespan)

    @application.middleware("http")
    async def request_policy(request: Request, call_next):
        if request.method in {"POST", "PUT", "DELETE"}:
            origin = request.headers.get("origin")
            if origin and urlsplit(origin).netloc != request.headers.get("host"):
                return JSONResponse({"detail": "请从当前网页提交任务。"}, status_code=403)
            try:
                if int(request.headers.get("content-length", "0")) > 200000:
                    return JSONResponse({"detail": "请求超过 200 KB。"}, status_code=413)
            except ValueError:
                return JSONResponse({"detail": "无效的请求长度。"}, status_code=400)
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "same-origin"
        if request.url.path.startswith("/api/"):
            response.headers["Cache-Control"] = "no-store"
        return response

    def service() -> SimulationService:
        return application.state.service

    def job(identifier: str):
        try:
            return service().require_job(identifier)
        except KeyError:
            raise HTTPException(404, "找不到这次运行。") from None

    @application.get("/api/status")
    async def status():
        values = list(service().jobs.values())
        return {**service().binary_identity(), **git_identity(root),
                "running": sum(j["status"] in {"running", "cancelling"} for j in values),
                "queued": sum(j["status"] == "queued" for j in values), "workers": 1,
                "total_runs": len(values)}

    @application.get("/api/catalog")
    async def catalog():
        return service().catalog.listing()

    @application.post("/api/preview")
    async def preview(body: RunRequest):
        try:
            return service().catalog.prepare(body)
        except ValueError as error:
            raise HTTPException(422, str(error)) from None

    @application.get("/api/runs")
    async def runs():
        return {"runs": service().summaries()}

    @application.get("/api/runs/export.csv")
    async def export_csv():
        return Response("\ufeff"+service().export_csv(), media_type="text/csv; charset=utf-8",
                        headers={"Content-Disposition": 'attachment; filename="protobridge-runs.csv"'})

    @application.post("/api/runs", status_code=202)
    async def submit(body: RunRequest):
        try:
            return service().submit(body)
        except ValueError as error:
            raise HTTPException(422, str(error)) from None
        except RuntimeError as error:
            raise HTTPException(503, str(error)) from None
        except OverflowError as error:
            raise HTTPException(429, str(error)) from None

    @application.get("/api/runs/{identifier}")
    async def get_run(identifier: str):
        return job(identifier)

    @application.post("/api/runs/{identifier}/cancel")
    async def cancel(identifier: str):
        job(identifier)
        return await service().cancel(identifier)

    @application.get("/api/runs/{identifier}/trace")
    async def trace(identifier: str):
        job(identifier)
        return service().trace(identifier)

    @application.get("/api/runs/{identifier}/log")
    async def log(identifier: str, offset: int = Query(0, ge=0)):
        job(identifier)
        path = service().data_dir/identifier/"stdout.log"
        with path.open("rb") as stream:
            stream.seek(min(offset, path.stat().st_size))
            content = stream.read(65536)
            cursor = stream.tell()
        return {"text": content.decode("utf-8", errors="replace"), "cursor": cursor,
                "has_more": cursor < path.stat().st_size}

    @application.get("/api/runs/{identifier}/artifacts/{filename}")
    async def artifact(identifier: str, filename: str):
        current = job(identifier)
        allowed = {"config.cfg", "profile.profile", "stdout.log", "steps.csv", "result.json", "run.zip"}
        if filename not in allowed:
            raise HTTPException(404, "没有这个文件。")
        directory = service().data_dir/identifier
        if filename == "run.zip":
            if current["status"] not in TERMINAL:
                raise HTTPException(409, "运行结束后可下载完整记录。")
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for name in sorted(allowed-{"run.zip"}):
                    path = directory/name
                    if path.is_file():
                        archive.write(path, name)
            return Response(data.getvalue(), media_type="application/zip",
                            headers={"Content-Disposition": f'attachment; filename="protobridge-{identifier[:8]}.zip"'})
        path = directory/filename
        if not path.is_file():
            raise HTTPException(404, "这次运行没有生成这个文件。")
        return FileResponse(path, filename=filename, media_type="application/octet-stream")

    @application.get("/")
    async def index():
        return FileResponse(STATIC/"index.html")

    application.mount("/static", StaticFiles(directory=STATIC), name="static")
    return application


app = create_app()
