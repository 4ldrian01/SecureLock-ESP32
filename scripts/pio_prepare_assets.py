Import("env")

import atexit
import os
import shutil
import subprocess
import sys

from SCons.Script import COMMAND_LINE_TARGETS


RUNTIME_MUTABLE_FILES = ("users.json", "logs.json")
RUNTIME_STASH_DIRNAME = ".cache"
RUNTIME_STASH_BASENAME = "uploadfs_runtime_preserve"


def _runtime_stash_dir(project_dir):
    return os.path.join(project_dir, RUNTIME_STASH_DIRNAME, RUNTIME_STASH_BASENAME)


def _restore_runtime_files(project_dir, moved_pairs=None):
    if moved_pairs is None:
        moved_pairs = []
        stash_dir = _runtime_stash_dir(project_dir)
        data_dir = os.path.join(project_dir, "data")

        for name in RUNTIME_MUTABLE_FILES:
            stashed = os.path.join(stash_dir, name)
            original = os.path.join(data_dir, name)
            if os.path.exists(stashed):
                moved_pairs.append((stashed, original))

    restored = 0
    for stashed, original in moved_pairs:
        if not os.path.exists(stashed):
            continue

        os.makedirs(os.path.dirname(original), exist_ok=True)

        # Never overwrite a file that already exists in the workspace.
        if os.path.exists(original):
            os.remove(stashed)
            continue

        shutil.move(stashed, original)
        restored += 1

    stash_dir = _runtime_stash_dir(project_dir)
    if os.path.isdir(stash_dir) and not os.listdir(stash_dir):
        os.rmdir(stash_dir)

    if restored:
        print(f"[ASSETS] Restored {restored} mutable runtime file(s) after LittleFS preparation")


def _stash_runtime_files(project_dir):
    data_dir = os.path.join(project_dir, "data")
    stash_dir = _runtime_stash_dir(project_dir)

    # Recover from interrupted prior runs before creating a new stash.
    _restore_runtime_files(project_dir)

    moved_pairs = []
    for name in RUNTIME_MUTABLE_FILES:
        original = os.path.join(data_dir, name)
        stashed = os.path.join(stash_dir, name)
        if not os.path.exists(original):
            continue

        os.makedirs(os.path.dirname(stashed), exist_ok=True)
        if os.path.exists(stashed):
            os.remove(stashed)

        shutil.move(original, stashed)
        moved_pairs.append((stashed, original))

    if moved_pairs:
        names = ", ".join(name for name in RUNTIME_MUTABLE_FILES if os.path.exists(os.path.join(stash_dir, name)))
        print(f"[ASSETS] Excluding mutable runtime files from LittleFS image: {names}")

        def _restore_on_exit():
            _restore_runtime_files(project_dir, moved_pairs)

        atexit.register(_restore_on_exit)


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
    _stash_runtime_files(env["PROJECT_DIR"])
    _run_optimizer(env["PROJECT_DIR"])
