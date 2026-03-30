#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import os
import subprocess
import sys
from pathlib import Path

from experiment_config import infer_model
from experiment_config import load_config
from experiment_config import vendor_for_model


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Record Nebula packet topics for all supported sensors in an experiment config"
    )
    parser.add_argument("config", type=Path, help="Path to the experiment config JSON")
    parser.add_argument(
        "--output",
        type=Path,
        help="Rosbag output path. Defaults to bags/nebula_packets_<timestamp>",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the ros2 bag command and exit without running it",
    )
    return parser.parse_args()


def packet_topics_for_sensor(sensor_name: str, vendor: str) -> list[str]:
    namespace = f"/{sensor_name}"
    if vendor == "hesai":
        return [f"{namespace}/pandar_packets"]
    if vendor == "robosense":
        return [
            f"{namespace}/robosense_packets",
            f"{namespace}/robosense_info_packets",
        ]
    return []


def main() -> int:
    args = parse_args()
    config = load_config(args.config.resolve())

    topics: list[str] = []
    skipped: list[str] = []

    for sensor in config["sensors"]:
        if not isinstance(sensor, dict):
            skipped.append("<invalid>: not a JSON object")
            continue

        model = infer_model(sensor)
        if not model:
            skipped.append(f"{sensor.get('name', '<unnamed>')}: unsupported Nebula model")
            continue

        vendor = vendor_for_model(model)
        if vendor is None:
            skipped.append(f"{sensor['name']}: unsupported vendor")
            continue

        sensor_topics = packet_topics_for_sensor(sensor["name"], vendor)
        if not sensor_topics:
            skipped.append(
                f"{sensor['name']}: current Nebula {vendor} wrapper does not publish packet topics"
            )
            continue
        topics.extend(sensor_topics)

    if not topics:
        print("No baggable packet topics were derived from the config.", file=sys.stderr)
        for line in skipped:
            print(f"  - {line}", file=sys.stderr)
        return 1

    output = args.output
    if output is None:
        timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        output = Path("bags") / f"nebula_packets_{timestamp}"

    output.parent.mkdir(parents=True, exist_ok=True)

    command = ["ros2", "bag", "record", "-o", str(output), *topics]

    print("Recording topics:")
    for topic in topics:
        print(f"  - {topic}")
    if skipped:
        print("Skipped sensors:")
        for line in skipped:
            print(f"  - {line}")

    if args.dry_run:
        print("Command:")
        print(" ".join(command))
        return 0

    os.execvp(command[0], command)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
