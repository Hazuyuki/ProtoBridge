"""Run with: python -m webui --port 8080."""

import argparse
import os


def main():
    parser = argparse.ArgumentParser(description="ProtoBridge local simulation web interface")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--repo", help="ProtoBridge checkout (defaults to this repository)")
    parser.add_argument("--data-dir", help="Run storage (default: ~/.cache/protobridge-web/runs)")
    args = parser.parse_args()
    if args.repo:
        os.environ["PROTOBRIDGE_WEB_REPO"] = args.repo
    if args.data_dir:
        os.environ["PROTOBRIDGE_WEB_DATA_DIR"] = args.data_dir
    import uvicorn

    uvicorn.run("webui.app:app", host=args.host, port=args.port, workers=1)


if __name__ == "__main__":
    main()
