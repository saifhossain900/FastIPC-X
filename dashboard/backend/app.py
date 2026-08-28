from flask import Flask, jsonify, request
from flask_cors import CORS

from pathlib import Path
import csv
import subprocess


app = Flask(__name__)
CORS(app)


PROJECT_ROOT = Path(__file__).resolve().parents[2]
FASTIPC = PROJECT_ROOT / "fastipc"
RESULTS = PROJECT_ROOT / "results"


COMMAND_SPECS = {
    "benchmark": ["int", "int"],
    "optimize-chunk": ["int", "int"],
    "optimize-shm": ["int", "int", "int"],
    "optimize-ring-slots": ["int", "int", "int"],
    "analyze-affinity": ["int", "int", "int"],
    "optimize-memory": ["int", "int", "int"],
    "recommend": ["int"],
    "auto": ["int"],
    "environment": [],
    "verify": ["method", "int", "int"],
    "profile": ["method", "int", "int"],
    "compare-syscalls": ["method", "method", "int", "int"],
    "final-summary": [],
}

ALLOWED_METHODS = {
    "pipe",
    "fifo",
    "socket",
    "shm",
    "shm-opt",
}


DEMO_PRESETS = {
    "quick-benchmark": {
        "label": "Quick IPC Benchmark",
        "command": "benchmark",
        "args": ["37", "3"],
        "description": "37 MB payload, 3 trials",
    },
    "shm-optimization": {
        "label": "SHM Optimization",
        "command": "optimize-shm",
        "args": ["37", "72", "3"],
        "description": "37 MB, 72 KB chunks, 3 trials",
    },
    "integrity": {
        "label": "Integrity Check",
        "command": "verify",
        "args": ["shm-opt", "17", "72"],
        "description": "17 MB SHM-RING checksum verification",
    },
    "scheduler": {
        "label": "CPU Scheduler Test",
        "command": "analyze-affinity",
        "args": ["37", "72", "3"],
        "description": "37 MB, 72 KB chunks, 3 trials",
    },
    "memory": {
        "label": "Page-Fault Test",
        "command": "optimize-memory",
        "args": ["37", "72", "3"],
        "description": "37 MB, 72 KB chunks, 3 trials",
    },
}


def read_csv_file(filename):
    path = RESULTS / filename

    if not path.exists():
        return []

    with path.open("r", encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file))


def read_first_csv(filename):
    rows = read_csv_file(filename)
    return rows[0] if rows else {}


def load_integrity_results():
    rows = []

    for path in sorted(RESULTS.glob("integrity_*.csv")):
        try:
            with path.open("r", encoding="utf-8", newline="") as file:
                data = list(csv.DictReader(file))

            for row in data:
                row["_file"] = path.name
                rows.append(row)

        except OSError:
            pass

    return rows


def parse_manifest(path):
    wanted = {
        "Run ID": "run_id",
        "Timestamp": "timestamp",
        "Category": "category",
        "Command": "command",
        "Command status": "status",
        "Git commit": "commit",
        "Git branch": "branch",
        "Source-tree state": "source_state",
    }

    result = {
        "file": path.name
    }

    try:
        for line in path.read_text(
            encoding="utf-8",
            errors="replace"
        ).splitlines():

            if ":" not in line:
                continue

            left, right = line.split(":", 1)

            key = left.strip()
            value = right.strip()

            if key in wanted:
                result[wanted[key]] = value

    except OSError:
        pass

    return result


def load_manifests():
    paths = sorted(
        RESULTS.glob("run_manifest_*.txt"),
        key=lambda item: item.stat().st_mtime,
        reverse=True,
    )

    return [
        parse_manifest(path)
        for path in paths[:12]
    ]


def validate_args(command, args):
    spec = COMMAND_SPECS.get(command)

    if spec is None:
        return None, "Command not allowed."

    if not isinstance(args, list):
        return None, "Arguments must be a list."

    if len(args) != len(spec):
        return None, (
            f"{command} expects {len(spec)} "
            f"argument(s), got {len(args)}."
        )

    safe = []

    for value, kind in zip(args, spec):
        text = str(value).strip()

        if kind == "int":
            if not text.isdigit() or int(text) <= 0:
                return None, (
                    f"Invalid positive integer: {text}"
                )

        elif kind == "method":
            if text not in ALLOWED_METHODS:
                return None, (
                    f"Invalid IPC method: {text}"
                )

        safe.append(text)

    return safe, None


@app.get("/api/health")
def health():
    return jsonify({
        "status": "ready",
        "fastipc_exists": FASTIPC.exists(),
        "results_exists": RESULTS.exists(),
        "project_root": str(PROJECT_ROOT),
    })


@app.get("/api/dashboard")
def dashboard():
    return jsonify({
        "benchmark": read_csv_file(
            "benchmark_100MB.csv"
        ),

        "shm_optimization": read_csv_file(
            "shm_sync_optimization_100MB_64KB.csv"
        ),

        "syscalls": read_csv_file(
            "syscall_comparison_shm_vs_shm-opt_100MB_64KB.csv"
        ),

        "affinity": read_csv_file(
            "cpu_affinity_summary_100MB_64KB.csv"
        ),

        "memory": read_csv_file(
            "memory_summary_100MB_64KB.csv"
        ),

        "workloads": read_csv_file(
            "workload_adaptive_summary.csv"
        ),

        "ring_slots": read_csv_file(
            "shm_ring_slot_summary_100MB_64KB.csv"
        ),

        "integrity": load_integrity_results(),

        "environment": read_first_csv(
            "system_environment.csv"
        ),

        "manifests": load_manifests(),

        "final_summary": read_csv_file(
            "final_summary.csv"
        ),
    })


@app.get("/api/final-summary")
def final_summary():
    return jsonify({
        "rows": read_csv_file(
            "final_summary.csv"
        )
    })


@app.post("/api/run")
def run_fastipc():
    payload = request.get_json(
        silent=True
    ) or {}

    command = str(
        payload.get("command", "")
    ).strip()

    args = payload.get(
        "args",
        []
    )

    safe_args, error = validate_args(
        command,
        args
    )

    if error:
        return jsonify({
            "error": error
        }), 400

    if not FASTIPC.exists():
        return jsonify({
            "error": "fastipc executable was not found. Run make first."
        }), 500

    command_line = [
        str(FASTIPC),
        command,
        *safe_args,
    ]

    try:
        result = subprocess.run(
            command_line,
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=300,
        )

        return jsonify({
            "command": " ".join(
                [
                    "./fastipc",
                    command,
                    *safe_args,
                ]
            ),
            "exit_code": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        })

    except subprocess.TimeoutExpired:
        return jsonify({
            "error": "Experiment timed out after 300 seconds."
        }), 408

    except OSError as exc:
        return jsonify({
            "error": str(exc)
        }), 500



@app.get("/api/demo/presets")
def demo_presets():
    return jsonify({
        "presets": {
            key: {
                "label": value["label"],
                "description": value["description"],
            }
            for key, value in DEMO_PRESETS.items()
        }
    })


@app.post("/api/demo/run/<preset_id>")
def run_demo_preset(preset_id):
    preset = DEMO_PRESETS.get(preset_id)

    if not preset:
        return jsonify({
            "error": "Unknown demo preset."
        }), 404

    if not FASTIPC.exists():
        return jsonify({
            "error": "fastipc executable was not found. Run make first."
        }), 500

    command = preset["command"]
    args = preset["args"]

    command_line = [
        str(FASTIPC),
        command,
        *args,
    ]

    try:
        result = subprocess.run(
            command_line,
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=300,
        )

        return jsonify({
            "preset": preset_id,
            "label": preset["label"],
            "command": " ".join(
                [
                    "./fastipc",
                    command,
                    *args,
                ]
            ),
            "exit_code": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "safe_demo": True,
        })

    except subprocess.TimeoutExpired:
        return jsonify({
            "error": "Demo experiment timed out after 300 seconds."
        }), 408

    except OSError as exc:
        return jsonify({
            "error": str(exc)
        }), 500


if __name__ == "__main__":
    app.run(
        host="127.0.0.1",
        port=5050,
        debug=True,
    )
