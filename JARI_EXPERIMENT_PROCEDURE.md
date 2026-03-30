# JARI Experiment Procedure

This document summarizes the current JARI experiment workflow for:

- running live sensors through Nebula
- recording Nebula packet topics to rosbag
- recording raw network traffic to PCAP
- visualizing recorded experiments

## Repositories

Required repositories:

- Nebula PCAP visualizer: `git@github.com:drwnz/nebula_pcap_visualizer.git`
- Nebula: `git@github.com:drwnz/nebula.git`

Clone them into the same parent directory:

```bash
mkdir -p ~/Projects/JARI
cd ~/Projects/JARI

git clone git@github.com:drwnz/nebula.git
git clone git@github.com:drwnz/nebula_pcap_visualizer.git
```

Expected layout:

```text
~/Projects/JARI/
  nebula/
  nebula_pcap_visualizer/
```

## Build and Environment

Build Nebula first:

```bash
cd ~/Projects/JARI/nebula
colcon build --symlink-install
```

Then build the visualizer:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
CCACHE_DISABLE=1 ./build.sh
```

For all runtime commands below, source Nebula first:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
```

## Shared Experiment Config

The shared JARI experiment config is:

- [jari_experiment.json](/home/davidwong/Projects/JARI/nebula_pcap_visualizer/jari_experiment.json)

It is used for:

- live Nebula launch
- rosbag packet recording
- raw PCAP recording
- recorded-experiment visualization

## Workflow A: Live Nebula Launch

Launch all supported sensors in their own namespaces:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
./run_nebula_launch jari_experiment.json
```

What this does:

- launches one Nebula wrapper per supported sensor
- uses the sensor `name` as the ROS namespace
- uses `launch_hw=true`
- uses `setup_sensor=false` where supported

Launch files:

- dynamic launch: [launch/nebula_sensors.launch.py](/home/davidwong/Projects/JARI/nebula_pcap_visualizer/launch/nebula_sensors.launch.py)
- static backup launch: [launch/nebula_sensors_backup.launch.xml](/home/davidwong/Projects/JARI/nebula_pcap_visualizer/launch/nebula_sensors_backup.launch.xml)

The backup XML launch is useful when you want to edit each sensor’s launch parameters directly instead of deriving them from the JSON config.

## Workflow B: Record Nebula Packet Topics to Rosbag

Start the Nebula launch first, then in another shell:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
./record_nebula_bag jari_experiment.json
```

To preview the derived topic list without recording:

```bash
./record_nebula_bag jari_experiment.json --dry-run
```

Current packet topics recorded:

- Hesai: `/<sensor_name>/pandar_packets`
- Robosense: `/<sensor_name>/robosense_packets`
- Robosense: `/<sensor_name>/robosense_info_packets`

Current limitation:

- Seyond hardware wrappers in the current Nebula branch do not publish raw packet topics, so those sensors are skipped by the rosbag helper.

## Workflow C: Record Raw PCAP Experiments

To record an experiment directly from the network:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
./pcap_recorder jari_experiment.json experiment_name
```

Example:

```bash
./pcap_recorder jari_experiment.json yard_test_01
```

This creates an experiment directory under `../experiments`:

```text
../experiments/
  yard_test_01/
    calibration/
    capture_config.json
    manifest.json
    <sensor_name>_<sensor_ip>.pcap
```

Notes:

- calibration subdirectories are created only for sensors that need external calibration files
- each sensor gets its own recorder-style PCAP file
- stop recording with `Ctrl-C`

## Workflow D: Visualize a Recorded Experiment

After a PCAP experiment has been recorded:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
./visualize_experiment jari_experiment.json yard_test_01
```

What this does:

- reads the shared experiment config
- finds the matching experiment under `../experiments`
- writes `visualizer_project.json` in the repo root
- runs `build/pcap_visualizer`
- starts the web server

Then open:

```text
http://localhost:8000
```

If you want a different port:

```bash
WEB_PORT=8080 ./visualize_experiment jari_experiment.json yard_test_01
```

## PCAP Visualization Notes

- traffic isolation uses `sensor_ip`, `host_ip`, `ports`, and `protocol` from the shared config
- for Robosense, `data_port` and `difop_port` can be specified explicitly
- EM4 visualization depends on loading calibration from the DIFOP/info stream
- if a sample PCAP does not match the configured IP/port filter, the visualizer warns and falls back to relaxed matching for that file

## Typical End-to-End Session

### Option 1: Live Nebula plus rosbag

Shell 1:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
./run_nebula_launch jari_experiment.json
```

Shell 2:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
./record_nebula_bag jari_experiment.json
```

### Option 2: Raw PCAP plus offline visualization

Shell 1:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
./pcap_recorder jari_experiment.json experiment_name
```

After recording:

```bash
cd ~/Projects/JARI/nebula_pcap_visualizer
source ../nebula/install/setup.bash
./visualize_experiment jari_experiment.json experiment_name
```

## Troubleshooting

If `pcap_visualizer` fails with missing `librclcpp.so`, the Nebula environment was not sourced:

```bash
source ../nebula/install/setup.bash
```

If the visualizer build fails because Nebula packages are not found, build Nebula first and then rebuild the visualizer.

If a sensor is missing from a recorded experiment, the visualizer will skip it and continue with the remaining sensors.
