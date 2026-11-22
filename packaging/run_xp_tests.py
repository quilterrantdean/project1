#!/usr/bin/env python
"""Run the XP-targeted BiblioLED native test bundle safely and repeatably.

Compatible with Python 3.4+ and uses only the standard library.
"""
from __future__ import print_function

import argparse
import ctypes
import hashlib
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import time

TESTS = (
    "application_paths_tests.exe",
    "settings_store_tests.exe",
    "loan_csv_export_tests.exe",
    "loan_rules_tests.exe",
    "loan_statistics_tests.exe",
    "loan_store_tests.exe",
    "backup_compression_tests.exe",
    "backup_retention_tests.exe",
    "audit_log_tests.exe",
)
MAX_REPEAT = 25


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def read_manifest(bundle_dir):
    path = os.path.join(bundle_dir, "manifest.json")
    with open(path, "rb") as source:
        raw = source.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    manifest = json.loads(raw.decode("utf-8"))
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise ValueError("manifest.json has no files hash map")
    for relative, expected in sorted(files.items()):
        if os.path.isabs(relative) or ".." in relative.replace("\\", "/").split("/"):
            raise ValueError("Unsafe manifest path: " + relative)
        full_path = os.path.realpath(os.path.join(bundle_dir, relative))
        if not full_path.startswith(os.path.realpath(bundle_dir) + os.sep):
            raise ValueError("Manifest path escapes bundle: " + relative)
        if not os.path.isfile(full_path):
            raise ValueError("Missing bundle file: " + relative)
        actual = sha256_file(full_path)
        if actual.lower() != str(expected).lower():
            raise ValueError("SHA-256 mismatch for " + relative)
    for name in ("BiblioLED.exe",) + TESTS:
        if name not in files:
            raise ValueError("Manifest does not cover required executable: " + name)
    return manifest


def machine_info():
    info = {
        "python": sys.version,
        "python_executable": sys.executable,
        "python_bits": struct.calcsize("P") * 8,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "windows_version": platform.win32_ver(),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "display": None,
    }
    if os.name == "nt":
        try:
            user32 = ctypes.windll.user32
            info["display"] = {
                "width": int(user32.GetSystemMetrics(0)),
                "height": int(user32.GetSystemMetrics(1)),
            }
        except Exception as error:
            info["display_error"] = str(error)
        try:
            class MemoryStatusEx(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]
            memory = MemoryStatusEx()
            memory.dwLength = ctypes.sizeof(memory)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)):
                info["memory_bytes"] = {
                    "total_physical": int(memory.ullTotalPhys),
                    "available_physical": int(memory.ullAvailPhys),
                }
        except Exception as error:
            info["memory_error"] = str(error)
    return info


def tail_text(path, limit=4096):
    try:
        with open(path, "rb") as source:
            source.seek(0, os.SEEK_END)
            size = source.tell()
            source.seek(max(0, size - limit), os.SEEK_SET)
            data = source.read()
        return data.decode("utf-8", "replace")
    except Exception as error:
        return "(could not read log: %s)" % error


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the BiblioLED XP native tests in isolated temporary folders.")
    parser.add_argument(
        "--repeat", type=int, default=1,
        help="repeat the complete test suite (1-%d, default 1)" % MAX_REPEAT)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.repeat < 1 or args.repeat > MAX_REPEAT:
        print("--repeat must be between 1 and %d" % MAX_REPEAT, file=sys.stderr)
        return 2
    if os.name != "nt":
        print("This runner is intended for Windows XP/Win32.", file=sys.stderr)
        return 2
    if sys.version_info[0] != 3 or struct.calcsize("P") * 8 != 32:
        print("Use a 32-bit Python 3 interpreter on Windows XP.", file=sys.stderr)
        return 2

    bundle_dir = os.path.realpath(os.path.dirname(os.path.abspath(__file__)))
    try:
        manifest = read_manifest(bundle_dir)
    except Exception as error:
        print("Bundle verification failed: %s" % error, file=sys.stderr)
        return 2

    run_root = tempfile.mkdtemp(prefix="BiblioLED-XP-test-")
    info = machine_info()
    info["source_commit"] = manifest.get("source_commit")
    info["workflow_run"] = manifest.get("workflow_run")
    info["application_sha256"] = manifest.get("files", {}).get("BiblioLED.exe")
    info["repeat"] = args.repeat
    print("BiblioLED XP automated tests")
    print("Source commit: %s" % (info["source_commit"] or "unknown"))
    print("Python: %s (%d-bit)" % (platform.python_version(), info["python_bits"]))
    print("Windows: %s" % (info["windows_version"],))
    if info["display"]:
        print("Display: %s x %s" % (
            info["display"]["width"], info["display"]["height"]))
    print("Run folder: %s" % run_root)

    results = []
    interrupted = False
    try:
        for iteration in range(1, args.repeat + 1):
            for executable in TESTS:
                label = "run-%02d-%s" % (
                    iteration, os.path.splitext(executable)[0])
                test_root = os.path.join(run_root, label)
                work_dir = os.path.join(test_root, "work")
                temp_dir = os.path.join(test_root, "temp")
                if not os.path.isdir(work_dir):
                    os.makedirs(work_dir)
                if not os.path.isdir(temp_dir):
                    os.makedirs(temp_dir)
                log_path = os.path.join(run_root, label + ".log")
                environment = os.environ.copy()
                environment["TEMP"] = temp_dir
                environment["TMP"] = temp_dir
                environment["TMPDIR"] = temp_dir
                command = os.path.join(bundle_dir, executable)
                started = time.time()
                print("RUN  %s" % label)
                with open(log_path, "wb") as log_file:
                    try:
                        process = subprocess.Popen(
                            [command], cwd=work_dir, env=environment,
                            stdout=log_file, stderr=subprocess.STDOUT)
                    except OSError as error:
                        log_file.write(("Could not start test: %s\n" % error).encode("utf-8"))
                        exit_code = 127
                    else:
                        try:
                            exit_code = process.wait()
                        except KeyboardInterrupt:
                            interrupted = True
                            try:
                                process.terminate()
                                process.wait()
                            except OSError:
                                pass
                            exit_code = 130
                elapsed = time.time() - started
                result = {
                    "name": label,
                    "executable": executable,
                    "exit_code": exit_code,
                    "elapsed_seconds": round(elapsed, 3),
                    "log": os.path.basename(log_path),
                }
                results.append(result)
                if exit_code == 0:
                    print("PASS %s (%.3f s)" % (label, elapsed))
                else:
                    print("FAIL %s (exit %d; %.3f s)" % (
                        label, exit_code, elapsed))
                    print(tail_text(log_path))
                if interrupted:
                    break
            if interrupted:
                break
    except Exception as error:
        interrupted = True
        print("Runner error: %s" % error, file=sys.stderr)

    passed = sum(1 for item in results if item["exit_code"] == 0)
    failed = len(results) - passed
    summary = {
        "machine": info,
        "results": results,
        "passed": passed,
        "failed": failed,
        "interrupted": interrupted,
        "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    summary_path = os.path.join(run_root, "summary.json")
    try:
        with open(summary_path, "wb") as output:
            output.write(json.dumps(summary, indent=2, sort_keys=True).encode("utf-8"))
            output.write(b"\n")
    except Exception as error:
        print("Could not write summary: %s" % error, file=sys.stderr)
        failed += 1

    print("RESULT: %d passed, %d failed; %s" % (
        passed, failed, "interrupted" if interrupted else "complete"))
    if failed == 0 and not interrupted:
        try:
            shutil.rmtree(run_root)
            print("Temporary test files removed after successful run.")
        except Exception as error:
            print("WARNING: could not remove successful run folder: %s" % error)
            print("Review and remove it manually if appropriate.")
    else:
        print("Failure logs and test folders preserved at: %s" % run_root)
    return 1 if failed or interrupted else 0


if __name__ == "__main__":
    sys.exit(main())
