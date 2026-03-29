#!/usr/bin/env python3
"""Record one PCAP per sensor based on a JSON config."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class SensorCapture:
    name: str
    interface: str
    sensor_ip: str | None
    host_ip: str | None
    ports: list[int]
    protocol: str | None
    filter_override: str | None

    @classmethod
    def from_dict(cls, raw: dict[str, Any]) -> "SensorCapture":
        name = require_str(raw, "name")
        interface = require_str(raw, "interface")
        sensor_ip = optional_str(raw, "sensor_ip", allow_empty=True)
        host_ip = optional_str(raw, "host_ip", allow_empty=True)
        protocol = optional_str(raw, "protocol", allow_empty=True) or "udp"
        filter_override = optional_str(raw, "filter", allow_empty=True)

        raw_ports = raw.get("ports", [])
        if raw_ports is None:
            raw_ports = []
        if not isinstance(raw_ports, list):
            raise ValueError(f"Sensor '{name}' has a non-list 'ports' value")

        ports: list[int] = []
        for value in raw_ports:
            if not isinstance(value, int):
                raise ValueError(f"Sensor '{name}' has a non-integer port: {value!r}")
            ports.append(value)

        if not filter_override and not any([sensor_ip, host_ip, ports]):
            raise ValueError(
                f"Sensor '{name}' must define at least one of sensor_ip, host_ip, ports, or filter"
            )

        return cls(
            name=name,
            interface=interface,
            sensor_ip=sensor_ip,
            host_ip=host_ip,
            ports=ports,
            protocol=protocol,
            filter_override=filter_override,
        )

    def filename(self) -> str:
        ip_label = self.sensor_ip or self.host_ip or "capture"
        safe_name = sanitize_path_component(self.name)
        safe_ip = sanitize_path_component(ip_label)
        return f"{safe_name}_{safe_ip}.pcap"

    def build_filter(self) -> str:
        if self.filter_override:
            return self.filter_override

        terms: list[str] = []
        if self.protocol:
            terms.append(self.protocol.lower())
        if self.sensor_ip:
            terms.append(f"host {self.sensor_ip}")
        if self.host_ip:
            terms.append(f"host {self.host_ip}")
        if self.ports:
            port_terms = " or ".join(f"port {port}" for port in self.ports)
            terms.append(f"({port_terms})")

        return " and ".join(terms)


def require_str(raw: dict[str, Any], key: str) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"Missing or invalid '{key}' field")
    return value.strip()


def optional_str(raw: dict[str, Any], key: str, allow_empty: bool = False) -> str | None:
    value = raw.get(key)
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError(f"Invalid '{key}' field")
    stripped = value.strip()
    if not stripped:
        if allow_empty:
            return None
        raise ValueError(f"Invalid '{key}' field")
    return stripped


def sanitize_path_component(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in value)


def load_config(config_path: Path) -> tuple[str, Path, str, list[str], list[SensorCapture]]:
    with config_path.open("r", encoding="utf-8") as handle:
        raw = json.load(handle)

    if not isinstance(raw, dict):
        raise ValueError("Config root must be a JSON object")

    configuration_name = require_str(raw, "configuration_name")
    output_root = raw.get("output_root", "recordings")
    if not isinstance(output_root, str) or not output_root.strip():
        raise ValueError("Missing or invalid 'output_root' field")

    tcpdump_binary = raw.get("tcpdump_binary", "tcpdump")
    if not isinstance(tcpdump_binary, str) or not tcpdump_binary.strip():
        raise ValueError("Missing or invalid 'tcpdump_binary' field")

    tcpdump_flags = raw.get("tcpdump_flags", ["-n", "-s", "0", "-U"])
    if not isinstance(tcpdump_flags, list) or not all(isinstance(flag, str) for flag in tcpdump_flags):
        raise ValueError("'tcpdump_flags' must be a list of strings")

    raw_sensors = raw.get("sensors")
    if not isinstance(raw_sensors, list) or not raw_sensors:
        raise ValueError("Config must include a non-empty 'sensors' array")

    sensors = [SensorCapture.from_dict(sensor) for sensor in raw_sensors]
    return (
        configuration_name,
        (config_path.parent / output_root).resolve(),
        tcpdump_binary.strip(),
        tcpdump_flags,
        sensors,
    )


def write_manifest(experiment_dir: Path, sensors: list[SensorCapture]) -> None:
    manifest = {
        "created_at_epoch_s": time.time(),
        "sensors": [
            {
                "name": sensor.name,
                "interface": sensor.interface,
                "sensor_ip": sensor.sensor_ip,
                "host_ip": sensor.host_ip,
                "ports": sensor.ports,
                "protocol": sensor.protocol,
                "filter": sensor.build_filter(),
                "pcap": sensor.filename(),
            }
            for sensor in sensors
        ],
    }

    with (experiment_dir / "manifest.json").open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Record one PCAP per configured sensor")
    parser.add_argument("config", type=Path, help="Path to recorder JSON config")
    parser.add_argument(
        "experiment_name",
        help="Experiment name used for the output directory under output_root",
    )
    parser.add_argument(
        "--allow-existing",
        action="store_true",
        help="Allow writing into an existing experiment directory",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        configuration_name, output_root, tcpdump_binary, tcpdump_flags, sensors = load_config(
            args.config.resolve()
        )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Failed to load config: {exc}", file=sys.stderr)
        return 1

    experiment_name = sanitize_path_component(args.experiment_name)
    experiment_dir = output_root / experiment_name
    if experiment_dir.exists() and not args.allow_existing:
        print(
            f"Experiment directory already exists: {experiment_dir}\n"
            "Use --allow-existing if you want to reuse it.",
            file=sys.stderr,
        )
        return 1

    experiment_dir.mkdir(parents=True, exist_ok=True)
    write_manifest(experiment_dir, sensors)
    with (experiment_dir / "capture_config.json").open("w", encoding="utf-8") as handle:
        json.dump(
            {
                "experiment_name": args.experiment_name,
                "configuration_name": configuration_name,
                "config_path": str(args.config.resolve()),
            },
            handle,
            indent=2,
        )
        handle.write("\n")

    processes: list[subprocess.Popen[bytes]] = []
    shutting_down = False

    def stop_all(signum: int, _frame: Any) -> None:
        nonlocal shutting_down
        if shutting_down:
            return

        shutting_down = True
        print(f"\nReceived signal {signum}, stopping tcpdump processes...", file=sys.stderr)

        for process in processes:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)

    signal.signal(signal.SIGINT, stop_all)
    signal.signal(signal.SIGTERM, stop_all)

    try:
        for sensor in sensors:
            output_path = experiment_dir / sensor.filename()
            filter_expr = sensor.build_filter()
            command = [
                tcpdump_binary,
                "-i",
                sensor.interface,
                *tcpdump_flags,
                "-w",
                str(output_path),
                filter_expr,
            ]

            print(f"Starting {sensor.name}: {' '.join(shlex.quote(part) for part in command)}")
            process = subprocess.Popen(command, env=os.environ.copy())
            processes.append(process)

        print(f"Recording into {experiment_dir}")
        print("Press Ctrl-C to stop.")

        while True:
            exited = [process for process in processes if process.poll() is not None]
            if exited and not shutting_down:
                failed = [process.returncode for process in exited]
                print(
                    f"One or more tcpdump processes exited unexpectedly: {failed}",
                    file=sys.stderr,
                )
                stop_all(signal.SIGTERM, None)
            if shutting_down:
                break
            time.sleep(0.5)

        exit_code = 0
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

            if process.returncode not in (0, None):
                if shutting_down and process.returncode in (-signal.SIGINT, -signal.SIGTERM):
                    continue
                exit_code = 1

        return exit_code
    except KeyboardInterrupt:
        stop_all(signal.SIGINT, None)
        return 130
    except OSError as exc:
        print(f"Failed to start tcpdump: {exc}", file=sys.stderr)
        stop_all(signal.SIGTERM, None)
        for process in processes:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
