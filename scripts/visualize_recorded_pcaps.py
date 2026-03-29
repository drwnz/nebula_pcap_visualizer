#!/usr/bin/env python3
"""Create and optionally run a Nebula visualizer project from recorded PCAPs."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


MODEL_ALIASES = {
    "falconk": "FalconK",
    "robinw": "RobinW",
    "robine1x": "RobinE1X",
    "hummingbirdd1": "HummingbirdD1",
    "ftx140": "FTX140",
    "ftx180": "FTX180",
    "at128": "PandarAT128",
    "pandarat128": "PandarAT128",
    "e1": "E1",
    "em4": "EM4",
    "emx": "EMX",
}


def require_str(raw: dict[str, Any], key: str) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"Missing or invalid '{key}' field")
    return value.strip()


def optional_str(raw: dict[str, Any], key: str) -> str | None:
    value = raw.get(key)
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError(f"Invalid '{key}' field")
    stripped = value.strip()
    return stripped or None


def sanitize_path_component(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in value)


def compact_model_name(value: str) -> str:
    return "".join(ch.lower() for ch in value if ch.isalnum())


def normalize_model_name(value: str) -> str | None:
    return MODEL_ALIASES.get(compact_model_name(value))


def infer_model(sensor: dict[str, Any]) -> str | None:
    explicit_model = optional_str(sensor, "model")
    if explicit_model:
        return normalize_model_name(explicit_model) or explicit_model

    candidates = [require_str(sensor, "name")]
    sensor_ip = optional_str(sensor, "sensor_ip")
    if sensor_ip:
        candidates.append(sensor_ip)

    for candidate in candidates:
        lowered = candidate.lower()
        if "robin_e" in lowered or "robine1" in lowered:
            return "RobinE1X"
        if "hummingbird" in lowered:
            return "HummingbirdD1"
        if "robin_w" in lowered or "robinw" in lowered:
            return "RobinW"
        if "falcon" in lowered:
            return "FalconK"
        if "ftx180" in lowered:
            return "FTX180"
        if "ftx140" in lowered:
            return "FTX140"
        if "at128" in lowered:
            return "PandarAT128"
        if "emx" in lowered:
            return "EMX"
        if "em4" in lowered:
            return "EM4"
        if "e1" in lowered:
            return "E1"
    return None


def recorded_pcap_path(experiment_dir: Path, sensor: dict[str, Any]) -> Path:
    name = require_str(sensor, "name")
    sensor_ip = optional_str(sensor, "sensor_ip")
    host_ip = optional_str(sensor, "host_ip")
    ip_label = sensor_ip or host_ip or "capture"
    return experiment_dir / f"{sanitize_path_component(name)}_{sanitize_path_component(ip_label)}.pcap"


def default_transform(index: int) -> dict[str, list[float]]:
    return {"pos": [float(index) * 5.0, 0.0, 0.0], "rot": [0.0, 0.0, 0.0]}


def default_camera(index: int) -> dict[str, list[float]]:
    return {
        "pos": [-12.0 + float(index) * 2.0, 0.0, 6.0],
        "target": [float(index) * 5.0, 0.0, 0.0],
        "up": [0.0, 0.0, 1.0],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a Nebula visualizer project from recorded PCAPs"
    )
    parser.add_argument("config", type=Path, help="Path to pcap recorder config JSON")
    parser.add_argument("experiment_name", help="Experiment directory name under output_root")
    parser.add_argument(
        "--project-out",
        type=Path,
        help="Where to write the generated visualizer project JSON",
    )
    parser.add_argument(
        "--run-binary",
        type=Path,
        help="Optional path to the nebula_pcap_visualizer binary to run after generating the project",
    )
    parser.add_argument(
        "--frame-stride",
        type=int,
        default=1,
        help="Export frame stride written into the generated project",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        with args.config.resolve().open("r", encoding="utf-8") as handle:
            config = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"Failed to load config: {exc}", file=sys.stderr)
        return 1

    if not isinstance(config, dict):
        print("Config root must be a JSON object", file=sys.stderr)
        return 1

    output_root = config.get("output_root", "recordings")
    if not isinstance(output_root, str) or not output_root.strip():
        print("Missing or invalid 'output_root' field", file=sys.stderr)
        return 1

    experiment_dir = (args.config.resolve().parent / output_root / sanitize_path_component(args.experiment_name)).resolve()
    if not experiment_dir.is_dir():
        print(f"Experiment directory not found: {experiment_dir}", file=sys.stderr)
        return 1

    sensors = config.get("sensors")
    if not isinstance(sensors, list) or not sensors:
        print("Config must include a non-empty 'sensors' array", file=sys.stderr)
        return 1

    project_sensors: list[dict[str, Any]] = []
    skipped: list[str] = []
    for index, sensor in enumerate(sensors):
        if not isinstance(sensor, dict):
            print("Each sensor entry must be a JSON object", file=sys.stderr)
            return 1

        if sensor.get("visualize") is False:
            skipped.append(f"{sensor.get('name', '<unnamed>')}: visualize=false")
            continue

        model = infer_model(sensor)
        if not model:
            skipped.append(f"{sensor.get('name', '<unnamed>')}: missing Nebula model")
            continue

        pcap_path = recorded_pcap_path(experiment_dir, sensor)
        if not pcap_path.is_file():
            skipped.append(f"{sensor.get('name', '<unnamed>')}: missing {pcap_path.name}")
            continue

        project_sensor = {
            "name": require_str(sensor, "name"),
            "model": model,
            "pcap": str(pcap_path),
            "transform": sensor.get("transform", default_transform(len(project_sensors))),
            "camera": sensor.get("camera", default_camera(len(project_sensors))),
        }

        calib = optional_str(sensor, "calib")
        if calib:
            calib_path = (args.config.resolve().parent / calib).resolve()
            project_sensor["calib"] = str(calib_path)

        project_sensors.append(project_sensor)

    if not project_sensors:
        print("No visualizable sensors found in the experiment", file=sys.stderr)
        for line in skipped:
            print(f"  - {line}", file=sys.stderr)
        return 1

    project = {
        "project_name": args.experiment_name,
        "export_defaults": {
            "frame_stride": args.frame_stride,
        },
        "sensors": project_sensors,
    }

    project_out = args.project_out.resolve() if args.project_out else experiment_dir / "visualizer_project.json"
    project_out.parent.mkdir(parents=True, exist_ok=True)
    with project_out.open("w", encoding="utf-8") as handle:
        json.dump(project, handle, indent=2)
        handle.write("\n")

    print(f"Wrote visualizer project: {project_out}")
    print(f"Included {len(project_sensors)} sensors")
    if skipped:
        print("Skipped sensors:")
        for line in skipped:
            print(f"  - {line}")

    if args.run_binary:
        command = [str(args.run_binary.resolve()), str(project_out)]
        print(f"Running: {' '.join(command)}")
        completed = subprocess.run(command, check=False)
        return completed.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
