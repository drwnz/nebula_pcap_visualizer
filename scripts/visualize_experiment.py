#!/usr/bin/env python3
"""Create and run the shared Nebula visualizer project for an experiment."""

from __future__ import annotations

import argparse
import json
import os
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


def optional_int_list(raw: dict[str, Any], key: str) -> list[int] | None:
    value = raw.get(key)
    if value is None:
        return None
    if not isinstance(value, list) or not all(isinstance(item, int) for item in value):
        raise ValueError(f"Invalid '{key}' field")
    return value


def optional_int(raw: dict[str, Any], key: str) -> int | None:
    value = raw.get(key)
    if value is None:
        return None
    if not isinstance(value, int):
        raise ValueError(f"Invalid '{key}' field")
    return value


def sanitize_path_component(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in value)


def resolve_calibration_path(config_path: Path, experiment_dir: Path, calib: str) -> Path:
    calib_ref = Path(calib)
    if calib_ref.is_absolute():
        return calib_ref.resolve()

    if calib_ref.parts and calib_ref.parts[0] == "calibration":
        return (experiment_dir / calib_ref).resolve()

    return (config_path.resolve().parent / calib_ref).resolve()


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
    target_x = float(index) * 5.0
    return {
        "pos": [target_x - 3.0, 0.0, 1.5],
        "target": [target_x, 0.0, 0.0],
        "up": [0.0, 0.0, 1.0],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the shared visualizer project and run pcap_visualizer for an experiment"
    )
    parser.add_argument("config", type=Path, help="Path to the shared experiment config JSON")
    parser.add_argument("experiment_name", help="Experiment directory name under output_root")
    return parser.parse_args()


def path_for_project(target_path: Path, project_dir: Path) -> str:
    return os.path.relpath(target_path.resolve(), project_dir.resolve())


def main() -> int:
    args = parse_args()
    config_path = args.config.resolve()
    web_port = int(os.environ.get("WEB_PORT", "8000"))

    try:
        with config_path.open("r", encoding="utf-8") as handle:
            config = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"Failed to load config: {exc}", file=sys.stderr)
        return 1

    if not isinstance(config, dict):
        print("Config root must be a JSON object", file=sys.stderr)
        return 1

    output_root = config.get("output_root", "../experiments")
    if not isinstance(output_root, str) or not output_root.strip():
        print("Missing or invalid 'output_root' field", file=sys.stderr)
        return 1

    experiment_dir = (config_path.parent / output_root / sanitize_path_component(args.experiment_name)).resolve()
    if not experiment_dir.is_dir():
        print(
            "Experiment recording not found.\n"
            f"Expected directory: {experiment_dir}\n"
            f"Run './pcap_recorder {config_path} {args.experiment_name}' first.",
            file=sys.stderr,
        )
        return 1

    capture_config_path = experiment_dir / "capture_config.json"
    manifest_path = experiment_dir / "manifest.json"
    if not capture_config_path.is_file() or not manifest_path.is_file():
        print(
            "Experiment directory is incomplete.\n"
            f"Expected files:\n"
            f"  - {capture_config_path}\n"
            f"  - {manifest_path}\n"
            f"Run './pcap_recorder {config_path} {args.experiment_name}' to create the recording structure.",
            file=sys.stderr,
        )
        return 1

    project_out = config_path.parent / "visualizer_project.json"
    project_dir = project_out.parent.resolve()

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
            "pcap": path_for_project(pcap_path, project_dir),
            "transform": sensor.get("transform", default_transform(len(project_sensors))),
            "camera": sensor.get("camera", default_camera(len(project_sensors))),
        }

        sensor_ip = optional_str(sensor, "sensor_ip")
        if sensor_ip:
            project_sensor["sensor_ip"] = sensor_ip

        host_ip = optional_str(sensor, "host_ip")
        if host_ip:
            project_sensor["host_ip"] = host_ip

        ports = optional_int_list(sensor, "ports")
        if ports:
            project_sensor["ports"] = ports

        data_port = optional_int(sensor, "data_port")
        if data_port is not None:
            project_sensor["data_port"] = data_port

        difop_port = optional_int(sensor, "difop_port")
        if difop_port is not None:
            project_sensor["difop_port"] = difop_port

        protocol = optional_str(sensor, "protocol")
        if protocol:
            project_sensor["protocol"] = protocol

        calib = optional_str(sensor, "calib")
        if calib:
            calib_path = resolve_calibration_path(config_path, experiment_dir, calib)
            project_sensor["calib"] = path_for_project(calib_path, project_dir)

        project_sensors.append(project_sensor)

    if not project_sensors:
        print("No visualizable sensors found in the experiment", file=sys.stderr)
        for line in skipped:
            print(f"  - {line}", file=sys.stderr)
        return 1

    project = {
        "project_name": args.experiment_name,
        "export_defaults": {
            "frame_stride": 10,
        },
        "sensors": project_sensors,
    }

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

    visualizer_binary = config_path.parent / "build" / "pcap_visualizer"
    if not visualizer_binary.is_file():
        print(
            "pcap_visualizer binary not found.\n"
            f"Expected binary: {visualizer_binary}\n"
            "Run './build.sh' first.",
            file=sys.stderr,
        )
        return 1

    command = [str(visualizer_binary.resolve()), str(project_out)]
    print(f"Running: {' '.join(command)}")
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        return completed.returncode

    web_dir = config_path.parent / "web"
    if not web_dir.is_dir():
        print(
            "Web frontend directory not found.\n"
            f"Expected directory: {web_dir}",
            file=sys.stderr,
        )
        return 1

    print(f"Serving {web_dir} at http://localhost:{web_port}")
    server_command = [sys.executable, "-m", "http.server", str(web_port)]
    return subprocess.run(server_command, cwd=web_dir, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
