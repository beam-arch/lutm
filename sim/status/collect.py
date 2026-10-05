#!/usr/bin/env python3
"""Return bounded, credential-free build telemetry from the Depot host."""

from collections import deque
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys


STAGES = ("bootstrap", "build", "policy-non-ab", "inspect-non-ab", "upload-non-ab",
          "policy-ab", "inspect-ab", "upload-ab", "pipeline")
PROGRESS = re.compile(r"\[\s*(\d+)%\s+(\d+)/(\d+)")
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def collect(root):
    state_file = root / ".lutm-status.json"
    try:
        state = json.loads(state_file.read_text())
    except (OSError, ValueError):
        state = {"offset": 0}
    markers = {}
    for stage in STAGES:
        try:
            markers[stage] = int((root / f"{stage}.exit").read_text().strip())
        except (OSError, ValueError):
            pass

    log = root / "build.log"
    recent = deque(maxlen=6)
    if log.exists():
        if log.stat().st_size < state.get("offset", 0):
            state = {"offset": 0}
        with log.open("rb") as stream:
            stream.seek(state.get("offset", 0))
            for raw in stream:
                line = ANSI.sub("", raw.decode("utf8", "replace")).strip()
                if line.startswith("OUT_DIR="):
                    state["layout"] = "ab" if line.rstrip("/").endswith("/ab") else "non-ab"
                    state.pop("progress", None)
                if line.startswith("TARGET_BUILD_VARIANT="):
                    state["variant"] = line.split("=", 1)[1]
                match = PROGRESS.search(line)
                if match:
                    state["progress"] = dict(zip(("percent", "done", "total"), map(int, match.groups())))
                    recent.append(line[:280])
                elif line.startswith("FAILED:"):
                    state["failure"] = line[:280]
            state["offset"] = stream.tell()
        state["log_updated"] = datetime.fromtimestamp(log.stat().st_mtime, timezone.utc).isoformat()
    if recent:
        state["recent"] = list(recent)
    state_file.write_text(json.dumps(state))

    layouts = []
    for layout in ("non-ab", "ab"):
        release = root / "android/lineage/out/releases/virtio_arm64only" / layout
        built = (release / "release.json").is_file()
        steps = [{"label": "Images", "status": "passed" if built else "running" if state.get("layout") == layout else "pending"}]
        for stage, label in (("policy", "Policy"), ("inspect", "Image checks"), ("upload", "Upload")):
            key = f"{stage}-{layout}"
            status = "passed" if markers.get(key) == 0 else "failed" if key in markers else "running" if (root / f"{key}.log").exists() else "pending"
            steps.append({"label": label, "status": status})
        downloads = []
        if markers.get(f"upload-{layout}") == 0:
            name = None
            for line in (root / f"upload-{layout}.log").read_text().splitlines():
                match = re.match(r'\s*(".*") \((\d+) bytes, MD5 ', line)
                if match:
                    name = json.loads(match.group(1))
                url = re.fullmatch(r"\s*(?:folder: )?(https://gofile\.io/d/[A-Za-z0-9-]+)\s*", line)
                if name and url:
                    if line.strip().startswith("folder:"):
                        downloads[-1]["url"] = url.group(1)
                    else:
                        downloads.append({"name": name, "url": url.group(1)})
        status = "failed" if any(step["status"] == "failed" for step in steps) else "complete" if markers.get(f"upload-{layout}") == 0 else "checking" if built else "building" if state.get("layout") == layout else "queued"
        if markers.get("build", 0) != 0 and state.get("layout") == layout:
            status = "failed"
            steps[0]["status"] = "failed"
        layouts.append({"id": layout, "status": status, "steps": steps, "downloads": downloads,
                        "progress": state.get("progress") if state.get("layout") == layout and not built else None})

    failed = any(code != 0 for code in markers.values())
    stage = "Complete" if markers.get("pipeline") == 0 else "Build needs attention" if failed else "Preparing builder" if "bootstrap" not in markers else "Syncing sources" if not state.get("layout") else "Building A/B" if state["layout"] == "ab" else "Building non-A/B recovery" if state.get("variant") == "userdebug" else "Building non-A/B"
    if markers.get("build") == 0 and not failed and markers.get("pipeline") != 0:
        stage = "Verifying and uploading releases"
    return {"sampledAt": datetime.now(timezone.utc).isoformat(), "stage": stage,
            "status": "failed" if failed else "complete" if markers.get("pipeline") == 0 else "running",
            "layouts": layouts, "recent": state.get("recent", []),
            "failure": state.get("failure") if failed else None, "logUpdatedAt": state.get("log_updated")}


if __name__ == "__main__":
    print(json.dumps(collect(Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home())))
