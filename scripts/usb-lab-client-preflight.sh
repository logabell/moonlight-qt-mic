#!/usr/bin/env bash
#
# Collect safe Moonlight USB passthrough client preflight artifacts.
#
# Default mode does not launch installers, administrator prompts, polkit prompts,
# real USB exports, or USB attach commands. The exporter probe is opt-in because
# it may start a managed usbipd process on Linux.

set -uo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/usb-lab-client-preflight.sh [options]

Options:
  --moonlight <path>      Moonlight binary or command name (default: moonlight)
  --output-root <path>    Directory for timestamped artifacts
  --test-exporter         Also run usb-lab-list --test-exporter
  --help                  Show this help

Environment:
  MOONLIGHT_BIN           Default Moonlight binary or command name

The script writes command logs, JSON payloads, and manifest.json.
EOF
}

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"

moonlight_bin="${MOONLIGHT_BIN:-moonlight}"
output_root="$repo_root/build/usb-lab-client-preflight"
test_exporter=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --moonlight)
            if [ "$#" -lt 2 ]; then
                echo "Missing value for --moonlight" >&2
                exit 2
            fi
            moonlight_bin="$2"
            shift 2
            ;;
        --output-root)
            if [ "$#" -lt 2 ]; then
                echo "Missing value for --output-root" >&2
                exit 2
            fi
            output_root="$2"
            shift 2
            ;;
        --test-exporter)
            test_exporter=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

resolve_moonlight() {
    local value="$1"
    if [ -f "$value" ]; then
        printf '%s\n' "$value"
        return 0
    fi
    if command -v "$value" >/dev/null 2>&1; then
        command -v "$value"
        return 0
    fi
    return 1
}

python_bin=""
if command -v python3 >/dev/null 2>&1; then
    python_bin="$(command -v python3)"
elif command -v python >/dev/null 2>&1; then
    python_bin="$(command -v python)"
else
    echo "python3 or python is required to write manifest.json" >&2
    exit 2
fi

moonlight_path="$(resolve_moonlight "$moonlight_bin")" || {
    echo "Moonlight binary not found: $moonlight_bin" >&2
    exit 2
}

timestamp="$(date -u +%Y%m%d-%H%M%S)"
run_dir="$output_root/$timestamp"
mkdir -p "$run_dir" || exit 2
commands_tsv="$run_dir/commands.tsv"
: > "$commands_tsv"

iso_now() {
    date -u +%Y-%m-%dT%H:%M:%SZ
}

run_lab_command() {
    local name="$1"
    local json_path="$2"
    shift 2

    local log_path="$run_dir/$name.log"
    local started_at finished_at exit_code

    started_at="$(iso_now)"
    "$@" >"$log_path" 2>&1
    exit_code=$?
    finished_at="$(iso_now)"

    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$exit_code" "$started_at" "$finished_at" "$log_path" "$json_path" >> "$commands_tsv"
}

client_install_json="$run_dir/client-install-dry-run.json"
run_lab_command \
    "client-install-dry-run" \
    "$client_install_json" \
    "$moonlight_path" usb-lab-install --dry-run --json --output "$client_install_json"

client_status_json="$run_dir/client-status.json"
run_lab_command \
    "client-status" \
    "$client_status_json" \
    "$moonlight_path" usb-lab-list --json --output "$client_status_json"

client_exporter_json=""
if [ "$test_exporter" -eq 1 ]; then
    client_exporter_json="$run_dir/client-exporter-test.json"
    run_lab_command \
        "client-exporter-test" \
        "$client_exporter_json" \
        "$moonlight_path" usb-lab-list --json --test-exporter --output "$client_exporter_json"
fi

manifest_path="$run_dir/manifest.json"
"$python_bin" - "$manifest_path" "$run_dir" "$moonlight_path" "$commands_tsv" "$test_exporter" <<'PY'
import csv
import json
import os
import sys
from datetime import datetime, timezone

manifest_path, run_dir, moonlight_path, commands_tsv, test_exporter = sys.argv[1:6]


def read_json(path):
    if not path or not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def normalize_path(path):
    if os.name == "nt" and isinstance(path, str):
        if len(path) >= 3 and path[0] == "/" and path[2] == "/" and path[1].isalpha():
            return f"{path[1].upper()}:{path[2:]}"
    return path


def command_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle, delimiter="\t")
        for row in reader:
            if len(row) != 6:
                continue
            name, exit_code, started_at, finished_at, log_path, json_path = row
            log_path = normalize_path(log_path)
            json_path = normalize_path(json_path)
            json_error = None
            parsed = False
            try:
                parsed = read_json(json_path) is not None
            except Exception as exc:
                json_error = str(exc)
            rows.append({
                "name": name,
                "exitCode": int(exit_code),
                "startedAt": started_at,
                "finishedAt": finished_at,
                "logPath": log_path,
                "jsonPath": json_path,
                "jsonParsed": parsed,
                "jsonError": json_error,
            })
    return rows


def summarize_install(payload):
    if not payload:
        return None
    action = payload.get("installAction") or {}
    return {
        "state": payload.get("state"),
        "supported": payload.get("supported"),
        "backend": payload.get("backend"),
        "dependenciesReady": payload.get("dependenciesReady"),
        "actionAvailable": action.get("available"),
        "action": action.get("action"),
        "message": payload.get("message"),
    }


def summarize_status(payload):
    if not payload:
        return None
    devices = payload.get("devices") or []
    return {
        "supported": payload.get("supported"),
        "backend": payload.get("backend"),
        "dependenciesReady": payload.get("dependenciesReady"),
        "usbipdServiceState": payload.get("usbipdServiceState"),
        "usbipCoreLoaded": payload.get("usbipCoreLoaded"),
        "usbipHostLoaded": payload.get("usbipHostLoaded"),
        "exporterTested": payload.get("exporterTested"),
        "exporterReady": payload.get("exporterReady"),
        "deviceCount": len(devices),
        "statusMessage": payload.get("statusMessage"),
        "lastError": payload.get("lastError"),
    }


install = read_json(os.path.join(run_dir, "client-install-dry-run.json"))
status = read_json(os.path.join(run_dir, "client-status.json"))
exporter = read_json(os.path.join(run_dir, "client-exporter-test.json"))

manifest = {
    "generatedAt": datetime.now(timezone.utc).isoformat(),
    "outputDir": run_dir,
    "moonlightExe": moonlight_path,
    "safeOnly": test_exporter != "1",
    "exporterProbeRequested": test_exporter == "1",
    "commands": command_rows(commands_tsv),
    "client": {
        "installDryRun": summarize_install(install),
        "status": summarize_status(status),
        "exporterProbe": summarize_status(exporter),
    },
}

with open(manifest_path, "w", encoding="utf-8") as handle:
    json.dump(manifest, handle, indent=2)
    handle.write("\n")
PY

echo "Moonlight USB lab preflight artifacts written to:"
echo "  $run_dir"
echo "Manifest:"
echo "  $manifest_path"
echo ""
"$python_bin" - "$manifest_path" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    manifest = json.load(handle)

client = manifest.get("client") or {}
install = client.get("installDryRun") or {}
status = client.get("status") or {}
exporter = client.get("exporterProbe") or {}

print("Client:")
print(f"  install dry-run: {install.get('state')} action={install.get('action')}")
print(f"  status: backend={status.get('backend')} dependenciesReady={status.get('dependenciesReady')} devices={status.get('deviceCount')}")
if exporter:
    print(f"  exporter probe: ready={exporter.get('exporterReady')} status={exporter.get('statusMessage')}")
else:
    print("  exporter probe: skipped (pass --test-exporter to include it)")
PY
