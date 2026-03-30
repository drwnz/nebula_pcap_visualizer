#!/usr/bin/env python3
from __future__ import annotations

import json
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

HESAI_MODELS = {"FTX140", "FTX180", "PandarAT128"}
ROBOSENSE_MODELS = {"E1", "EM4", "EMX"}
SEYOND_MODELS = {"FalconK", "RobinW", "RobinE1X", "HummingbirdD1"}


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


def optional_int(raw: dict[str, Any], key: str) -> int | None:
    value = raw.get(key)
    if value is None:
        return None
    if not isinstance(value, int):
        raise ValueError(f"Invalid '{key}' field")
    return value


def optional_int_list(raw: dict[str, Any], key: str) -> list[int]:
    value = raw.get(key)
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(item, int) for item in value):
        raise ValueError(f"Invalid '{key}' field")
    return value


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


def load_config(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    with resolved.open("r", encoding="utf-8") as handle:
        config = json.load(handle)
    if not isinstance(config, dict):
        raise ValueError("Config root must be a JSON object")
    sensors = config.get("sensors")
    if not isinstance(sensors, list) or not sensors:
        raise ValueError("Config must include a non-empty 'sensors' array")
    config["_config_path"] = resolved
    return config


def resolve_calibration_path(config_path: Path, calib: str) -> Path:
    calib_ref = Path(calib)
    if calib_ref.is_absolute():
        return calib_ref.resolve()
    return (config_path.parent / calib_ref).resolve()


def vendor_for_model(model: str) -> str | None:
    if model in HESAI_MODELS:
        return "hesai"
    if model in ROBOSENSE_MODELS:
        return "robosense"
    if model in SEYOND_MODELS:
        return "seyond"
    return None


def first_port(sensor: dict[str, Any]) -> int | None:
    data_port = optional_int(sensor, "data_port")
    if data_port is not None:
        return data_port
    ports = optional_int_list(sensor, "ports")
    return ports[0] if ports else None


def second_port(sensor: dict[str, Any]) -> int | None:
    difop_port = optional_int(sensor, "difop_port")
    if difop_port is not None:
        return difop_port
    gnss_port = optional_int(sensor, "gnss_port")
    if gnss_port is not None:
        return gnss_port
    ports = optional_int_list(sensor, "ports")
    return ports[1] if len(ports) > 1 else None
