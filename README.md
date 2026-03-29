# PCAP Visualizer

A multi-sensor LiDAR visualization tool designed to quickly inspect and compare point cloud data from Ethernet-based sensors using PCAP recordings.

## Overview

The Nebula PCAP Visualizer simplifies the process of verifying LiDAR sensor data by providing a unified workflow to:

1.  **Parse**: Extract raw UDP packets from PCAP files using Nebula's core decoders.
2.  **Export**: Convert parsed packets into a web-friendly JSON format with synchronized timestamps.
3.  **Visualize**: Interact with 3D point clouds in a multi-viewport web interface powered by Three.js.

## Supported Sensors

The Nebula PCAP Visualizer currently supports the following sensor models:

- **Hesai**: `AT128`, `FTX140`, `FTX180`
- **Robosense**: `EMX`, `E1`, `EM4`
- **Seyond**: `FalconK`, `RobinW`, `RobinE1X`, `HummingbirdD1`

## Architecture

- **Backend (C++)**: A standalone tool that leverages `nebula_core`, `nebula_hesai`, `nebula_robosense`, and `nebula_seyond` to process packets and export frame data.
- **Frontend (HTML/JS)**: A responsive web interface that renders multiple sensor viewports side-by-side, allowing for synchronized timeline scrubbing and spatial comparison.

## Prerequisites

### System Dependencies

- `libpcap-dev`: For reading PCAP files.
- `nlohmann-json3-dev`: For handling JSON configuration and export.

### Internal Dependencies

This tool requires the following Nebula packages to be present in your workspace:

- `nebula_core`
- `nebula_hesai`
- `nebula_robosense`
- `nebula_seyond` (for Seyond sensors)

## Build Instructions

Building is handled via CMake. Ensure you have the required dependencies installed before proceeding:

```bash
./build.sh
```

## Configuration

The visualizer uses a `project.json` file to define which sensors and PCAP files to process.

### Example `project.json`

```json
{
  "project_name": "Multi-Sensor Comparison",
  "sensors": [
    {
      "name": "front_hesai",
      "model": "FTX180",
      "pcap": "/path/to/hesai_capture.pcap",
      "transform": { "pos": [0, 0, 0], "rot": [0, 0, 0] },
      "camera": { "pos": [20, 20, 20], "target": [0, 0, 0] }
    },
    {
      "name": "side_robosense",
      "model": "EMX",
      "pcap": "/path/to/robosense_capture.pcap",
      "transform": { "pos": [2, 0, 0], "rot": [0, 0, 0] },
      "camera": { "pos": [0, 0, 50], "target": [2, 0, 0] }
    }
  ]
}
```

- **`name`**: Unique identifier for the sensor.
- **`model`**: Sensor model string (e.g., `AT128`, `FTX140`, `FTX180`, `EMX`, `E1`, `EM4`, `FalconK`, `RobinW`, `RobinE1X`, `HummingbirdD1`).
- **`pcap`**: Path to the PCAP file.
- **`calib`** (optional): Path to the calibration file (`.csv` or `.bin`).
- **`transform`**: Spatial offset of the sensor in the 3D scene.
- **`camera`**: Default camera position and target for this sensor's viewport.

## Usage

### 1. Process PCAP Data

Run the backend tool with your project configuration:

```bash
./pcap_visualizer project.json
```

This will clear the `web/data` directory and export new frame data.

### 2. Launch the Visualizer

Serve the `web/` directory using any local web server. For example, using Python:

```bash
cd web
python3 -m http.server 8000
```

Open `http://localhost:8000` in your browser to view the results.

## Output Structure

The tool creates the following structure in `web/data`:

- `metadata.json`: Contains global timeline and sensor configurations.
- `[sensor_name]/`: Subdirectories containing individual frame data in JSON format (`frame_0.json`, `frame_1.json`, etc.).

## Recording New PCAPs

This repository also includes a small recorder utility for live packet capture. It starts one `tcpdump` process per configured sensor and writes captures into a dedicated experiment directory.

Each experiment directory gets a `calibration/` subtree only for sensors that need external calibration files, currently FTX, `RobinW`, `RobinE1X`, and `HummingbirdD1`.

### Recorder Config

Use a shared experiment config such as `experiment_config.json`:

```json
{
  "configuration_name": "multi_sensor_setup",
  "output_root": "../experiments",
  "sensors": [
    {
      "name": "front_hesai",
      "interface": "enp6s0",
      "host_ip": "192.168.10.10",
      "sensor_ip": "192.168.10.201",
      "ports": [2368, 10110]
    }
  ]
}
```

Recorder fields:

- **`configuration_name`**: Label for the capture setup stored in the config.
- **`output_root`**: Base directory for recordings. Relative paths are resolved relative to the config file.
- **`tcpdump_binary`** (optional): Override the `tcpdump` executable path.
- **`tcpdump_flags`** (optional): Extra `tcpdump` flags. Defaults to `["-n", "-s", "0", "-U"]`.
- **`sensors`**: List of captures to run in parallel.

Per-sensor fields:

- **`name`**: Sensor label used in the output file name.
- **`interface`**: Network interface passed to `tcpdump -i`.
- **`sensor_ip`** (optional): Sensor endpoint IP.
- **`host_ip`** (optional): Host-side IP on that network.
- **`ports`** (optional): UDP/TCP ports to include in the capture filter.
- **`protocol`** (optional): Defaults to `udp`.
- **`filter`** (optional): Raw BPF filter string. If set, it overrides the generated filter.

If both `sensor_ip` and `host_ip` are set, the generated filter captures traffic between those endpoints. This works for setups where multiple sensors share the same NIC through a switch.

### Recorder Usage

Run the recorder with:

```bash
./pcap_recorder experiment_config.json experiment_name
```

Stop it with `Ctrl-C`. The script will ask each `tcpdump` process to exit cleanly.

### Recorder Output

The recorder creates:

```text
../experiments/
  experiment_name/
    calibration/
      front_hesai/
      roof_seyond/
    capture_config.json
    manifest.json
    front_hesai_192.168.10.201.pcap
    left_robosense_192.168.10.202.pcap
    roof_seyond_192.168.20.210.pcap
```

The `capture_config.json` file stores the experiment name and source config reference. The `manifest.json` file stores the resolved filter and file name for each configured sensor.

## Visualizing Recorded Experiments

After recording, run the visualizer helper for an experiment:

```bash
./visualize_experiment experiment_config.json experiment_name
```

This writes `visualizer_project.json` next to the shared experiment config, runs `build/pcap_visualizer` against it, and then starts a local web server for the `web/` directory.

The helper reads `model` from each sensor entry when present. Supported Nebula model names include `EMX`, `E1`, `EM4`, `RobinW`, `RobinE1X`, `FTX180`, `FTX140`, `PandarAT128`, `HummingbirdD1`, and `FalconK`.

During visualization, the configured `sensor_ip`, `host_ip`, `ports`, and `protocol` are used to isolate traffic for each sensor inside its PCAP. If a test capture does not match the configured endpoints or ports, the tool prints a warning and falls back to relaxed matching for that file so the sample can still be visualized.

For Robosense sensors, you can also provide `data_port` and `difop_port` explicitly. This is especially important for `EM4`, where offline calibration is loaded from the DIFOP/info stream before point decoding. In the current Nebula API that `difop_port` is adapted into Robosense's `gnss_port` field internally.

If a sensor entry includes `calib`, you can point it at an absolute file path or at an experiment-local path under `calibration/`. For example, `"calib": "calibration/seyond_robin_e/anglehv_table.bin"` resolves to `../experiments/experiment_name/calibration/seyond_robin_e/anglehv_table.bin` when the project file is generated.

Sensors with `visualize: false` are skipped. Sensors are also skipped if their output PCAP file is missing.

By default the frontend is served at `http://localhost:8000`. Set `WEB_PORT` before running the script if you want a different port.

If `build/pcap_visualizer` does not exist yet, run `./build.sh` first.
