// Copyright 2026 drwnz
#include <nebula_core_common/loggers/logger.hpp>
#include <nebula_core_common/nebula_common.hpp>
#include <nebula_hesai_common/hesai_common.hpp>
#include <nebula_hesai_decoders/hesai_driver.hpp>
#include <nebula_robosense_common/robosense_common.hpp>
#include <nebula_robosense_decoders/robosense_driver.hpp>
#include <nebula_seyond_common/seyond_common.hpp>
#include <nebula_seyond_decoders/seyond_decoder.hpp>
#include <nlohmann/json.hpp>

#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <pcap.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

class ConsoleLogger : public nebula::drivers::loggers::Logger
{
public:
  void debug(const std::string & msg) override { /*std::cout << "[DEBUG] " << msg << std::endl;*/ }
  void info(const std::string & msg) override { std::cout << "[INFO] " << msg << std::endl; }
  void warn(const std::string & msg) override { std::cout << "[WARN] " << msg << std::endl; }
  void error(const std::string & msg) override { std::cerr << "[ERROR] " << msg << std::endl; }
  std::shared_ptr<Logger> child(const std::string & name) override
  {
    return std::make_shared<ConsoleLogger>();
  }
};

struct SensorFrame
{
  uint64_t timestamp;
  std::string path;
};

class CloudExporter
{
public:
  CloudExporter(const std::string & sensor_name, const std::string & base_dir)
  : sensor_name_(sensor_name), base_dir_(base_dir), frame_count_(0)
  {
    output_dir_ = base_dir_ + "/" + sensor_name_;
    fs::create_directories(output_dir_);
  }

  void callback(nebula::drivers::NebulaPointCloudPtr cloud, uint64_t timestamp)
  {
    if (!cloud || cloud->empty()) return;

    std::string rel_path = sensor_name_ + "/frame_" + std::to_string(frame_count_) + ".json";
    std::string full_path = base_dir_ + "/" + rel_path;

    std::ofstream ofs(full_path);
    ofs << "[\n";
    for (size_t i = 0; i < cloud->size(); ++i) {
      const auto & pt = cloud->at(i);
      ofs << "  {\"x\":" << pt.x << ",\"y\":" << pt.y << ",\"z\":" << pt.z
          << ",\"i\":" << (int)pt.intensity << "}";
      if (i < cloud->size() - 1) ofs << ",";
      ofs << "\n";
    }
    ofs << "]";

    frames_.push_back({timestamp, rel_path});
    frame_count_++;
  }

  const std::vector<SensorFrame> & get_frames() const { return frames_; }
  const std::string & get_name() const { return sensor_name_; }

private:
  std::string sensor_name_;
  std::string base_dir_;
  std::string output_dir_;
  size_t frame_count_;
  std::vector<SensorFrame> frames_;
};

int main(int argc, char ** argv)
{
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <project_json_file>" << std::endl;
    return 1;
  }

  std::string project_file = argv[1];
  std::ifstream f(project_file);
  if (!f.is_open()) {
    std::cerr << "Could not open project file: " << project_file << std::endl;
    return 1;
  }
  json project = json::parse(f);

  auto logger = std::make_shared<ConsoleLogger>();
  std::vector<std::shared_ptr<CloudExporter>> exporters;

  fs::remove_all("web/data");  // Clean old data
  fs::create_directories("web/data");

  for (auto & sensor_cfg : project["sensors"]) {
    std::string name = sensor_cfg["name"];
    std::string model_str = sensor_cfg["model"];
    std::string pcap_file = sensor_cfg["pcap"];
    std::string calib_file = sensor_cfg.value("calib", "");

    std::cout << "Processing sensor: " << name << " (" << model_str << ")" << std::endl;

    auto exporter = std::make_shared<CloudExporter>(name, "web/data");
    exporters.push_back(exporter);
    auto callback =
      std::bind(&CloudExporter::callback, exporter, std::placeholders::_1, std::placeholders::_2);

    std::unique_ptr<nebula::drivers::SeyondDecoder> seyond_decoder;
    std::unique_ptr<nebula::drivers::HesaiDriver> hesai_driver;
    std::unique_ptr<nebula::drivers::RobosenseDriver> robosense_driver;

    // Seyond
    if (
      model_str.find("Falcon") != std::string::npos ||
      model_str.find("Robin") != std::string::npos ||
      model_str.find("Hummingbird") != std::string::npos) {
      auto config = std::make_shared<nebula::drivers::SeyondSensorConfiguration>();
      config->sensor_model = nebula::drivers::seyond_sensor_model_from_string(model_str);
      nebula::drivers::SeyondCalibrationData calib;
      if (!calib_file.empty()) {
        auto res = nebula::drivers::SeyondCalibrationData::load_from_file(calib_file);
        if (res.has_value()) calib = res.value();
      }
      seyond_decoder = std::make_unique<nebula::drivers::SeyondDecoder>(*config, callback, calib);
    } else if (model_str == "FTX180" || model_str == "FTX140" || model_str == "AT128") {
      auto config = std::make_shared<nebula::drivers::HesaiSensorConfiguration>();
      std::shared_ptr<nebula::drivers::HesaiCalibrationConfigurationBase> calib;
      if (model_str == "AT128") {
        config->sensor_model = nebula::drivers::SensorModel::HESAI_PANDARAT128;
        auto at_calib = std::make_shared<nebula::drivers::HesaiCorrection>();
        if (!calib_file.empty()) at_calib->load_from_file(calib_file);
        calib = at_calib;
      } else {
        if (model_str == "FTX180") {
          config->sensor_model = nebula::drivers::SensorModel::HESAI_FTX180;
        } else {
          config->sensor_model = nebula::drivers::SensorModel::HESAI_FTX140;
        }
        auto ftx_calib = std::make_shared<nebula::drivers::HesaiCorrectionFTX>();
        if (!calib_file.empty()) ftx_calib->load_from_file(calib_file);
        calib = ftx_calib;
      }
      hesai_driver =
        std::make_unique<nebula::drivers::HesaiDriver>(config, calib, logger, callback);
    } else if (model_str == "EMX" || model_str == "E1" || model_str == "EM4") {
      auto config = std::make_shared<nebula::drivers::RobosenseSensorConfiguration>();
      if (model_str == "EM4") {
        config->sensor_model = nebula::drivers::SensorModel::ROBOSENSE_EM4;
      } else if (model_str == "E1") {
        config->sensor_model = nebula::drivers::SensorModel::ROBOSENSE_E1;
      } else {
        config->sensor_model = nebula::drivers::SensorModel::ROBOSENSE_EMX;
      }
      auto calib = std::make_shared<nebula::drivers::RobosenseCalibrationConfiguration>();
      if (!calib_file.empty()) calib->load_from_file(calib_file);
      robosense_driver = std::make_unique<nebula::drivers::RobosenseDriver>(config, calib);
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t * handle = pcap_open_offline(pcap_file.c_str(), errbuf);
    if (handle == nullptr) {
      std::cerr << "Could not open pcap file: " << pcap_file << " | error: " << errbuf << std::endl;
      continue;
    }

    struct pcap_pkthdr * header;
    const u_char * packet_bytes;
    while (pcap_next_ex(handle, &header, &packet_bytes) >= 0) {
      const struct ether_header * eth_header = (struct ether_header *)packet_bytes;
      if (ntohs(eth_header->ether_type) != ETHERTYPE_IP) continue;
      const struct ip * ip_header = (struct ip *)(packet_bytes + sizeof(struct ether_header));
      if (ip_header->ip_p != IPPROTO_UDP) continue;
      const struct udphdr * udp_header =
        (struct udphdr *)(packet_bytes + sizeof(struct ether_header) + ip_header->ip_hl * 4);
      int payload_offset =
        sizeof(struct ether_header) + ip_header->ip_hl * 4 + sizeof(struct udphdr);
      int payload_len = ntohs(udp_header->uh_ulen) - sizeof(struct udphdr);
      if (payload_len <= 0) continue;
      std::vector<uint8_t> udp_payload(
        packet_bytes + payload_offset, packet_bytes + payload_offset + payload_len);

      if (seyond_decoder) {
        seyond_decoder->unpack(udp_payload);
      } else if (hesai_driver) {
        hesai_driver->parse_cloud_packet(udp_payload);
      } else if (robosense_driver) {
        auto [cloud, timestamp] = robosense_driver->parse_cloud_packet(udp_payload);
        if (cloud && !cloud->empty())
          exporter->callback(cloud, static_cast<uint64_t>(timestamp * 1e9));
      }
    }
    pcap_close(handle);
    std::cout << "Done sensor: " << name << " | frames: " << exporter->get_frames().size()
              << std::endl;
  }

  // Save metadata for web visualizer
  json meta;
  meta["project_name"] = project.value("project_name", "Multi-Sensor Visualization");

  // Calculate global timeline (union of all timestamps)
  std::set<uint64_t> all_timestamps;
  for (auto & exporter : exporters) {
    for (auto & frame : exporter->get_frames()) {
      all_timestamps.insert(frame.timestamp);
    }
  }

  std::vector<uint64_t> global_timeline(all_timestamps.begin(), all_timestamps.end());
  meta["timeline"] = global_timeline;

  // Sensor metadata
  json sensors_meta = json::array();
  for (size_t i = 0; i < exporters.size(); ++i) {
    json s;
    s["name"] = exporters[i]->get_name();
    s["model"] = project["sensors"][i]["model"];
    s["transform"] = project["sensors"][i].value(
      "transform", json::object({{"pos", {0, 0, 0}}, {"rot", {0, 0, 0}}}));
    s["camera"] = project["sensors"][i].value(
      "camera", json::object({{"pos", {0, 0, 50}}, {"target", {0, 0, 0}}}));

    json frames_list = json::array();
    for (auto & f : exporters[i]->get_frames()) {
      frames_list.push_back({{"t", f.timestamp}, {"path", f.path}});
    }
    s["frames"] = frames_list;
    sensors_meta.push_back(s);
  }
  meta["sensors"] = sensors_meta;

  std::ofstream meta_file("web/data/metadata.json");
  meta_file << meta.dump(2);
  meta_file.close();

  std::cout << "Finished! Processed " << exporters.size() << " sensors and "
            << global_timeline.size() << " global timeline points." << std::endl;

  return 0;
}
