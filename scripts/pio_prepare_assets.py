Import("env")

import os
import subprocess
import sys

from SCons.Script import COMMAND_LINE_TARGETS


def _should_prepare_assets(targets):
    names = {str(t).lower() for t in targets}
    return "uploadfs" in names or "buildfs" in names


def _run_optimizer(project_dir):
    script_path = os.path.join(project_dir, "scripts", "optimize_frontend_assets.py")
    if not os.path.exists(script_path):
        print("[ASSETS][WARN] Optimizer script missing, skipping precompression")
        return

    cache_file = os.path.join(project_dir, ".cache", "frontend_assets_cache.json")

    command = [
        sys.executable,
        script_path,
        "--data-dir",
        os.path.join(project_dir, "data"),
        "--cache-file",
        cache_file,
    ]

    print("[ASSETS] Preparing frontend payloads for LittleFS...")
    result = subprocess.run(command, cwd=project_dir)
    if result.returncode != 0:
        raise Exception("Frontend asset optimization failed")


if _should_prepare_assets(COMMAND_LINE_TARGETS):
    _run_optimizer(env["PROJECT_DIR"])
