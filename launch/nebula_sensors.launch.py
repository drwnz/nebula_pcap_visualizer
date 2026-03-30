#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.actions import LogInfo
from launch.actions import OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from experiment_config import first_port
from experiment_config import infer_model
from experiment_config import load_config
from experiment_config import optional_str
from experiment_config import resolve_calibration_path
from experiment_config import second_port
from experiment_config import vendor_for_model


def _base_param_file(vendor: str, model: str) -> str:
    if vendor == "hesai":
        package = "nebula_hesai"
    elif vendor == "robosense":
        package = "nebula_robosense"
    elif vendor == "seyond":
        package = "nebula_seyond"
    else:
        raise ValueError(f"Unsupported vendor: {vendor}")
    return str(Path(get_package_share_directory(package)) / "config" / f"{model}.param.yaml")


def _packet_remappings(vendor: str) -> list[tuple[str, str]]:
    if vendor == "robosense":
        return [
            ("/robosense_packets", "robosense_packets"),
            ("/robosense_info_packets", "robosense_info_packets"),
        ]
    return []


def _node_spec(vendor: str) -> tuple[str, str]:
    if vendor == "hesai":
        return "nebula_hesai", "hesai_ros_wrapper_node"
    if vendor == "robosense":
        return "nebula_robosense", "robosense_ros_wrapper_node"
    if vendor == "seyond":
        return "nebula_seyond", "seyond_ros_wrapper_node"
    raise ValueError(f"Unsupported vendor: {vendor}")


def _overrides(config_path: Path, sensor: dict, model: str, vendor: str) -> dict:
    name = sensor["name"]
    overrides = {
        "sensor_model": model,
        "launch_hw": True,
        "frame_id": name,
    }

    sensor_ip = optional_str(sensor, "sensor_ip")
    if sensor_ip:
        overrides["sensor_ip"] = sensor_ip

    host_ip = optional_str(sensor, "host_ip")
    if host_ip:
        overrides["host_ip"] = host_ip

    if vendor == "hesai":
        data_port = first_port(sensor)
        if data_port is not None:
            overrides["data_port"] = data_port
        gnss_port = second_port(sensor)
        if gnss_port is not None:
            overrides["gnss_port"] = gnss_port
        overrides["setup_sensor"] = False

        calib = optional_str(sensor, "calib")
        if calib:
            calib_path = str(resolve_calibration_path(config_path, calib))
            overrides["correction_file"] = calib_path
            overrides["calibration_download_enabled"] = False

    elif vendor == "robosense":
        data_port = first_port(sensor)
        if data_port is not None:
            overrides["data_port"] = data_port
        difop_port = second_port(sensor)
        if difop_port is not None:
            overrides["gnss_port"] = difop_port
        overrides["setup_sensor"] = False

    elif vendor == "seyond":
        udp_port = first_port(sensor)
        if udp_port is not None:
            overrides["udp_port"] = udp_port
            overrides["udp_message_port"] = udp_port
            overrides["udp_status_port"] = udp_port
        overrides["setup_sensor"] = False

    return overrides


def launch_setup(context, *args, **kwargs):
    config_path = Path(LaunchConfiguration("config").perform(context)).resolve()
    config = load_config(config_path)

    actions = []
    for sensor in config["sensors"]:
        if not isinstance(sensor, dict):
            actions.append(LogInfo(msg="Skipping invalid non-object sensor entry in config"))
            continue

        model = infer_model(sensor)
        if not model:
            actions.append(
                LogInfo(msg=f"Skipping {sensor.get('name', '<unnamed>')}: unsupported Nebula model")
            )
            continue

        vendor = vendor_for_model(model)
        if vendor is None:
            actions.append(
                LogInfo(msg=f"Skipping {sensor.get('name', '<unnamed>')}: unsupported vendor")
            )
            continue

        package, executable = _node_spec(vendor)
        parameters = [
            _base_param_file(vendor, model),
            _overrides(config_path, sensor, model, vendor),
        ]

        actions.append(
            Node(
                package=package,
                executable=executable,
                namespace=sensor["name"],
                name=executable,
                output="screen",
                parameters=parameters,
                remappings=_packet_remappings(vendor),
            )
        )

    return actions


def generate_launch_description():
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "config",
                default_value=str(REPO_ROOT / "jari_experiment.json"),
                description="Path to the experiment config JSON",
            ),
            OpaqueFunction(function=launch_setup),
        ]
    )
