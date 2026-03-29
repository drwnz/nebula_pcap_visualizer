// Copyright 2026 drwnz
#include <nebula_core_common/loggers/logger.hpp>
#include <nebula_core_common/nebula_common.hpp>
#include <nebula_hesai_common/hesai_common.hpp>
#include <nebula_hesai_decoders/hesai_driver.hpp>
#include <nebula_robosense_common/robosense_common.hpp>
#include <nebula_robosense_decoders/offline/em4_pcap_calibration.hpp>
#include <nebula_robosense_decoders/robosense_driver.hpp>
#include <nebula_seyond_common/seyond_common.hpp>
#include <nebula_seyond_decoders/seyond_decoder.hpp>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <pcap.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
std::mutex g_log_mutex;

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
  size_t source_index;
  std::string path;
};

struct UdpPacket
{
  std::string src_ip;
  std::string dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  std::vector<uint8_t> payload;
};

struct ReassemblyKey
{
  uint32_t src;
  uint32_t dst;
  uint16_t id;
  uint8_t protocol;

  bool operator<(const ReassemblyKey & other) const
  {
    return std::tie(src, dst, id, protocol) <
           std::tie(other.src, other.dst, other.id, other.protocol);
  }
};

struct FragmentAssembly
{
  bool saw_first_fragment{false};
  bool saw_last_fragment{false};
  std::string src_ip{};
  std::string dst_ip{};
  uint16_t source_port{0};
  uint16_t dest_port{0};
  size_t expected_udp_payload_size{0};
  std::vector<uint8_t> udp_payload{};
  std::vector<bool> received{};

  void ensure_capacity(size_t size)
  {
    if (udp_payload.size() < size) {
      udp_payload.resize(size);
      received.resize(size, false);
    }
  }

  bool is_complete() const
  {
    return saw_first_fragment && saw_last_fragment && expected_udp_payload_size > 0 &&
           udp_payload.size() >= expected_udp_payload_size &&
           std::all_of(
             received.begin(),
             received.begin() + static_cast<std::ptrdiff_t>(expected_udp_payload_size),
             [](bool v) { return v; });
  }
};

struct SensorTrafficFilter
{
  std::string sensor_ip{};
  std::string host_ip{};
  std::string protocol{"udp"};
  std::set<uint16_t> ports{};
};

SensorTrafficFilter traffic_filter_from_json(const json & sensor_cfg)
{
  SensorTrafficFilter filter;
  filter.sensor_ip = sensor_cfg.value("sensor_ip", "");
  filter.host_ip = sensor_cfg.value("host_ip", "");
  filter.protocol = sensor_cfg.value("protocol", "udp");

  if (sensor_cfg.contains("ports") && sensor_cfg["ports"].is_array()) {
    for (const auto & port : sensor_cfg["ports"]) {
      if (port.is_number_unsigned()) {
        filter.ports.insert(port.get<uint16_t>());
      }
    }
  }

  return filter;
}

SensorTrafficFilter relaxed_traffic_filter(const SensorTrafficFilter & filter)
{
  SensorTrafficFilter relaxed = filter;
  relaxed.sensor_ip.clear();
  relaxed.host_ip.clear();
  relaxed.ports.clear();
  return relaxed;
}

bool matches_traffic_filter(
  const SensorTrafficFilter & filter, const std::string & src_ip, const std::string & dst_ip,
  uint16_t src_port, uint16_t dst_port)
{
  if (!filter.protocol.empty() && filter.protocol != "udp") {
    return false;
  }

  if (!filter.sensor_ip.empty() && !filter.host_ip.empty()) {
    const bool direct = src_ip == filter.sensor_ip && dst_ip == filter.host_ip;
    const bool reverse = src_ip == filter.host_ip && dst_ip == filter.sensor_ip;
    if (!direct && !reverse) {
      return false;
    }
  } else if (!filter.sensor_ip.empty()) {
    if (src_ip != filter.sensor_ip && dst_ip != filter.sensor_ip) {
      return false;
    }
  } else if (!filter.host_ip.empty()) {
    if (src_ip != filter.host_ip && dst_ip != filter.host_ip) {
      return false;
    }
  }

  if (!filter.ports.empty()) {
    if (filter.ports.count(src_port) == 0 && filter.ports.count(dst_port) == 0) {
      return false;
    }
  }

  return true;
}

bool matches_traffic_filter(const SensorTrafficFilter & filter, const UdpPacket & udp_packet)
{
  return matches_traffic_filter(
    filter, udp_packet.src_ip, udp_packet.dst_ip, udp_packet.src_port, udp_packet.dst_port);
}

std::optional<uint16_t> optional_uint16_from_json(const json & obj, const char * key)
{
  if (!obj.contains(key) || !obj[key].is_number_unsigned()) {
    return std::nullopt;
  }
  return obj[key].get<uint16_t>();
}

std::optional<UdpPacket> find_first_matching_udp_packet(
  const std::string & pcap_file, const std::function<bool(const UdpPacket &)> & predicate);

bool is_candidate_packet_for_model(const std::string & model, const UdpPacket & udp_packet)
{
  if (model == "RobinW" || model == "RobinE1X" || model == "HummingbirdD1" || model == "FalconK") {
    return true;
  }
  if (model == "FTX140" || model == "FTX180") {
    return udp_packet.payload.size() == 554;
  }
  if (model == "PandarAT128") {
    return udp_packet.payload.size() >= 1000;
  }
  if (model == "EMX") {
    return udp_packet.payload.size() == 812;
  }
  if (model == "E1" || model == "EM4") {
    return udp_packet.payload.size() >= 1000;
  }
  return false;
}

bool has_matching_udp_packet(
  const std::string & pcap_file, const std::function<bool(const UdpPacket &)> & predicate)
{
  return find_first_matching_udp_packet(pcap_file, predicate).has_value();
}

bool process_seyond_ipv4_packet(
  const uint8_t * data, size_t size, std::map<ReassemblyKey, FragmentAssembly> & assemblies,
  nebula::drivers::SeyondDecoder & decoder, const SensorTrafficFilter & traffic_filter)
{
  constexpr size_t kEthernetHeaderSize = sizeof(ether_header);
  if (size <= kEthernetHeaderSize) {
    return false;
  }

  const auto * ip_header = reinterpret_cast<const ip *>(data + kEthernetHeaderSize);
  if (ip_header->ip_v != 4 || ip_header->ip_p != IPPROTO_UDP) {
    return false;
  }

  const size_t ip_header_size = static_cast<size_t>(ip_header->ip_hl) * 4;
  const size_t total_ip_size = ntohs(ip_header->ip_len);
  if (
    ip_header_size < sizeof(ip) || kEthernetHeaderSize + total_ip_size > size ||
    total_ip_size < ip_header_size) {
    return false;
  }

  const size_t ip_payload_size = total_ip_size - ip_header_size;
  const uint8_t * ip_payload = reinterpret_cast<const uint8_t *>(ip_header) + ip_header_size;
  const uint16_t ip_off = ntohs(ip_header->ip_off);
  const bool more_fragments = (ip_off & IP_MF) != 0;
  const size_t fragment_offset = static_cast<size_t>(ip_off & IP_OFFMASK) * 8;

  ReassemblyKey key{
    ip_header->ip_src.s_addr, ip_header->ip_dst.s_addr, ntohs(ip_header->ip_id), ip_header->ip_p};
  auto & assembly = assemblies[key];

  if (fragment_offset == 0) {
    if (ip_payload_size < sizeof(udphdr)) {
      return false;
    }

    const auto * udp_header = reinterpret_cast<const udphdr *>(ip_payload);
    assembly.saw_first_fragment = true;
    char src_ip[INET_ADDRSTRLEN]{};
    char dst_ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &ip_header->ip_src, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &ip_header->ip_dst, dst_ip, sizeof(dst_ip));
    assembly.src_ip = src_ip;
    assembly.dst_ip = dst_ip;
    assembly.source_port = ntohs(udp_header->uh_sport);
    assembly.dest_port = ntohs(udp_header->uh_dport);
    assembly.expected_udp_payload_size = ntohs(udp_header->uh_ulen) - sizeof(udphdr);
    assembly.ensure_capacity(assembly.expected_udp_payload_size);

    const size_t fragment_udp_data_size = ip_payload_size - sizeof(udphdr);
    const uint8_t * fragment_udp_data = ip_payload + sizeof(udphdr);
    std::copy(
      fragment_udp_data, fragment_udp_data + fragment_udp_data_size, assembly.udp_payload.begin());
    std::fill(
      assembly.received.begin(),
      assembly.received.begin() + static_cast<std::ptrdiff_t>(fragment_udp_data_size), true);
  } else {
    if (fragment_offset < sizeof(udphdr)) {
      return false;
    }
    const size_t udp_data_offset = fragment_offset - sizeof(udphdr);
    const size_t fragment_udp_data_size = ip_payload_size;
    assembly.ensure_capacity(udp_data_offset + fragment_udp_data_size);
    std::copy(
      ip_payload, ip_payload + fragment_udp_data_size,
      assembly.udp_payload.begin() + static_cast<std::ptrdiff_t>(udp_data_offset));
    std::fill(
      assembly.received.begin() + static_cast<std::ptrdiff_t>(udp_data_offset),
      assembly.received.begin() +
        static_cast<std::ptrdiff_t>(udp_data_offset + fragment_udp_data_size),
      true);
  }

  if (!more_fragments) {
    assembly.saw_last_fragment = true;
  }

  if (!assembly.is_complete()) {
    return false;
  }

  if (
    matches_traffic_filter(
      traffic_filter, assembly.src_ip, assembly.dst_ip, assembly.source_port, assembly.dest_port)) {
    decoder.unpack(assembly.udp_payload);
  }

  assemblies.erase(key);
  return true;
}

bool extract_udp_payload(const uint8_t * packet_bytes, size_t caplen, UdpPacket & udp_packet)
{
  const auto * eth_header = reinterpret_cast<const ether_header *>(packet_bytes);
  if (caplen < sizeof(ether_header) || ntohs(eth_header->ether_type) != ETHERTYPE_IP) {
    return false;
  }

  const auto * ip_header = reinterpret_cast<const ip *>(packet_bytes + sizeof(ether_header));
  if (ip_header->ip_p != IPPROTO_UDP) {
    return false;
  }

  const int payload_offset = sizeof(ether_header) + ip_header->ip_hl * 4 + sizeof(udphdr);
  const auto * udp_header =
    reinterpret_cast<const udphdr *>(packet_bytes + sizeof(ether_header) + ip_header->ip_hl * 4);
  const int payload_len = ntohs(udp_header->uh_ulen) - sizeof(udphdr);
  if (payload_len <= 0 || static_cast<size_t>(payload_offset + payload_len) > caplen) {
    return false;
  }

  char src_ip[INET_ADDRSTRLEN]{};
  char dst_ip[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &ip_header->ip_src, src_ip, sizeof(src_ip));
  inet_ntop(AF_INET, &ip_header->ip_dst, dst_ip, sizeof(dst_ip));

  udp_packet.src_ip = src_ip;
  udp_packet.dst_ip = dst_ip;
  udp_packet.src_port = ntohs(udp_header->uh_sport);
  udp_packet.dst_port = ntohs(udp_header->uh_dport);
  udp_packet.payload.assign(
    packet_bytes + payload_offset, packet_bytes + payload_offset + payload_len);
  return true;
}

std::optional<UdpPacket> find_first_matching_udp_packet(
  const std::string & pcap_file, const std::function<bool(const UdpPacket &)> & predicate)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t * handle = pcap_open_offline(pcap_file.c_str(), errbuf);
  if (handle == nullptr) {
    std::cerr << "Could not open pcap file: " << pcap_file << " | error: " << errbuf << std::endl;
    return std::nullopt;
  }

  struct pcap_pkthdr * header;
  const u_char * packet_bytes;
  while (pcap_next_ex(handle, &header, &packet_bytes) >= 0) {
    UdpPacket udp_packet{};
    if (!extract_udp_payload(packet_bytes, header->caplen, udp_packet)) {
      continue;
    }
    if (predicate(udp_packet)) {
      pcap_close(handle);
      return udp_packet;
    }
  }

  pcap_close(handle);
  return std::nullopt;
}

std::string normalize_model_name(std::string model)
{
  std::string compact;
  compact.reserve(model.size());
  for (char c : model) {
    if (c != ' ' && c != '_' && c != '-') {
      compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }

  if (compact == "falconk") return "FalconK";
  if (compact == "robinw") return "RobinW";
  if (compact == "robine1x") return "RobinE1X";
  if (compact == "hummingbirdd1") return "HummingbirdD1";
  if (compact == "ftx140") return "FTX140";
  if (compact == "ftx180") return "FTX180";
  if (compact == "at128" || compact == "pandarat128") return "PandarAT128";
  if (compact == "e1") return "E1";
  if (compact == "em4") return "EM4";
  if (compact == "emx") return "EMX";
  return model;
}

bool is_seyond_model(const std::string & model)
{
  return model == "FalconK" || model == "RobinW" || model == "RobinE1X" || model == "HummingbirdD1";
}

bool is_hesai_model(const std::string & model)
{
  return model == "FTX140" || model == "FTX180" || model == "PandarAT128";
}

bool is_robosense_model(const std::string & model)
{
  return model == "EMX" || model == "E1" || model == "EM4";
}

bool should_process_hesai_packet(
  const std::string & model, const UdpPacket & udp_packet,
  const SensorTrafficFilter & traffic_filter)
{
  if (!matches_traffic_filter(traffic_filter, udp_packet)) {
    return false;
  }
  if (model == "FTX140" || model == "FTX180") {
    return udp_packet.payload.size() == 554;
  }
  if (model == "PandarAT128") {
    return udp_packet.payload.size() >= 1000;
  }
  return false;
}

bool should_process_robosense_packet(
  const std::string & model, const UdpPacket & udp_packet,
  const SensorTrafficFilter & traffic_filter)
{
  if (!matches_traffic_filter(traffic_filter, udp_packet)) {
    return false;
  }
  if (model == "EMX") {
    return udp_packet.payload.size() == 812;
  }
  if (model == "E1" || model == "EM4") {
    return udp_packet.payload.size() >= 1000;
  }
  return false;
}

class CloudExporter
{
public:
  CloudExporter(const std::string & sensor_name, const std::string & base_dir, size_t frame_stride)
  : sensor_name_(sensor_name),
    base_dir_(base_dir),
    frame_stride_(std::max<size_t>(1, frame_stride)),
    seen_frame_count_(0),
    written_frame_count_(0)
  {
    output_dir_ = base_dir_ + "/" + sensor_name_;
    fs::create_directories(output_dir_);
  }

  void callback(nebula::drivers::NebulaPointCloudPtr cloud, uint64_t timestamp)
  {
    if (!cloud || cloud->empty()) {
      return;
    }

    const size_t frame_index = seen_frame_count_++;
    if ((frame_index % frame_stride_) != 0) {
      return;
    }

    std::string rel_path = sensor_name_ + "/frame_" + std::to_string(written_frame_count_) + ".bin";
    std::string full_path = base_dir_ + "/" + rel_path;

    std::ofstream ofs(full_path, std::ios::binary | std::ios::trunc);
    std::vector<float> packed_points;
    packed_points.reserve(cloud->size() * 4);
    for (size_t i = 0; i < cloud->size(); ++i) {
      const auto & pt = cloud->at(i);
      packed_points.push_back(pt.x);
      packed_points.push_back(pt.y);
      packed_points.push_back(pt.z);
      packed_points.push_back(static_cast<float>(pt.intensity));
    }
    if (!packed_points.empty()) {
      ofs.write(
        reinterpret_cast<const char *>(packed_points.data()),
        static_cast<std::streamsize>(packed_points.size() * sizeof(float)));
    }

    frames_.push_back({timestamp, frame_index, rel_path});
    written_frame_count_++;
  }

  const std::vector<SensorFrame> & get_frames() const { return frames_; }
  const std::string & get_name() const { return sensor_name_; }
  size_t get_seen_frame_count() const { return seen_frame_count_; }

private:
  std::string sensor_name_;
  std::string base_dir_;
  std::string output_dir_;
  size_t frame_stride_;
  size_t seen_frame_count_;
  size_t written_frame_count_;
  std::vector<SensorFrame> frames_;
};

struct SensorResult
{
  bool processed{false};
  json sensor_cfg{};
  std::shared_ptr<CloudExporter> exporter{};
};

fs::path resolve_project_path(const fs::path & project_dir, const std::string & raw_path)
{
  const fs::path path(raw_path);
  if (path.is_absolute()) {
    return path;
  }
  return project_dir / path;
}

SensorResult process_sensor(
  const json & sensor_cfg, const json & export_defaults, const fs::path & project_dir,
  const std::shared_ptr<ConsoleLogger> & logger)
{
  SensorResult result;
  result.sensor_cfg = sensor_cfg;

  const std::string name = sensor_cfg["name"];
  const std::string model_str = normalize_model_name(sensor_cfg["model"]);
  try {
    const SensorTrafficFilter configured_traffic_filter = traffic_filter_from_json(sensor_cfg);
    const fs::path pcap_file = resolve_project_path(project_dir, sensor_cfg["pcap"]);
    const std::string calib_file =
      sensor_cfg.contains("calib")
        ? resolve_project_path(project_dir, sensor_cfg["calib"]).lexically_normal().string()
        : "";
    const size_t frame_stride = sensor_cfg.value(
      "frame_stride", export_defaults.value("frame_stride", static_cast<size_t>(1)));

    {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cout << "Processing sensor: " << name << " (" << model_str << ")" << std::endl;
    }

    SensorTrafficFilter active_traffic_filter = configured_traffic_filter;
    const bool has_strict_matches = has_matching_udp_packet(
      pcap_file.string(), [model_str, configured_traffic_filter](const UdpPacket & udp_packet) {
        return is_candidate_packet_for_model(model_str, udp_packet) &&
               matches_traffic_filter(configured_traffic_filter, udp_packet);
      });
    if (!has_strict_matches) {
      active_traffic_filter = relaxed_traffic_filter(configured_traffic_filter);
      if (
        !configured_traffic_filter.sensor_ip.empty() ||
        !configured_traffic_filter.host_ip.empty() || !configured_traffic_filter.ports.empty()) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        std::cerr << "Warning: no packets matched configured IP/port filter for sensor " << name
                  << " in " << pcap_file << ". Falling back to relaxed matching for this file."
                  << std::endl;
      }
    }

    auto exporter = std::make_shared<CloudExporter>(name, "web/data", frame_stride);
    auto callback =
      std::bind(&CloudExporter::callback, exporter, std::placeholders::_1, std::placeholders::_2);

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t * handle = pcap_open_offline(pcap_file.c_str(), errbuf);
    if (handle == nullptr) {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cerr << "Could not open pcap file: " << pcap_file << " | error: " << errbuf << std::endl;
      return result;
    }

    std::unique_ptr<nebula::drivers::SeyondDecoder> seyond_decoder;
    std::unique_ptr<nebula::drivers::HesaiDriver> hesai_driver;
    std::unique_ptr<nebula::drivers::RobosenseDriver> robosense_driver;
    if (is_seyond_model(model_str)) {
      auto config = std::make_shared<nebula::drivers::SeyondSensorConfiguration>();
      config->sensor_model = nebula::drivers::seyond_sensor_model_from_string(model_str);
      nebula::drivers::SeyondCalibrationData calib;
      if (!calib_file.empty()) {
        auto res = nebula::drivers::SeyondCalibrationData::load_from_file(calib_file);
        if (res.has_value()) calib = res.value();
      }
      seyond_decoder = std::make_unique<nebula::drivers::SeyondDecoder>(*config, callback, calib);
    } else if (is_hesai_model(model_str)) {
      auto config = std::make_shared<nebula::drivers::HesaiSensorConfiguration>();
      config->sensor_model = nebula::drivers::sensor_model_from_string(model_str);
      config->frame_id = "hesai";
      config->min_range = 0.05;
      config->max_range = model_str == "PandarAT128" ? 200.0 : 25.0;
      config->rotation_speed = 600;
      config->return_mode =
        model_str == "PandarAT128"
          ? nebula::drivers::ReturnMode::DUAL_ONLY
          : nebula::drivers::return_mode_from_string_hesai("First", config->sensor_model);
      config->dual_return_distance_threshold = 0.1;
      config->calibration_download_enabled = !calib_file.empty();

      if (model_str == "FTX140") {
        config->cloud_min_angle = 20;
        config->cloud_max_angle = 160;
        config->sync_angle = 20;
        config->cut_angle = 160.0;
      } else if (model_str == "FTX180") {
        config->cloud_min_angle = 0;
        config->cloud_max_angle = 180;
        config->sync_angle = 90;
        config->cut_angle = 180.0;
      } else {
        config->cloud_min_angle = 0;
        config->cloud_max_angle = 360;
        config->sync_angle = 0;
        config->cut_angle = 0.0;
      }

      auto first_data_packet = find_first_matching_udp_packet(
        pcap_file.string(), [model_str, active_traffic_filter](const UdpPacket & udp_packet) {
          if (!matches_traffic_filter(active_traffic_filter, udp_packet)) {
            return false;
          }
          if (model_str == "PandarAT128") {
            return udp_packet.payload.size() >= 1000;
          }
          return udp_packet.payload.size() == 554;
        });
      if (first_data_packet) {
        config->sensor_ip = first_data_packet->src_ip;
        config->host_ip = first_data_packet->dst_ip;
        config->data_port = first_data_packet->dst_port;
      }

      std::shared_ptr<nebula::drivers::HesaiCalibrationConfigurationBase> calib;
      if (config->sensor_model == nebula::drivers::SensorModel::HESAI_PANDARAT128) {
        auto at_calib = std::make_shared<nebula::drivers::HesaiCorrection>();
        if (!calib_file.empty()) {
          at_calib->load_from_file(calib_file);
        }
        calib = at_calib;
      } else {
        auto ftx_calib = std::make_shared<nebula::drivers::HesaiCorrectionFTX>();
        if (!calib_file.empty()) {
          ftx_calib->load_from_file(calib_file);
        }
        calib = ftx_calib;
      }

      hesai_driver =
        std::make_unique<nebula::drivers::HesaiDriver>(config, calib, logger, callback);
    } else if (is_robosense_model(model_str)) {
      auto config = std::make_shared<nebula::drivers::RobosenseSensorConfiguration>();
      config->sensor_model = nebula::drivers::sensor_model_from_string(model_str);
      config->return_mode = nebula::drivers::ReturnMode::SINGLE_STRONGEST;
      config->sensor_ip = sensor_cfg.value("sensor_ip", "");
      config->host_ip = sensor_cfg.value("host_ip", "");
      if (const auto data_port = optional_uint16_from_json(sensor_cfg, "data_port")) {
        config->data_port = *data_port;
      }
      if (const auto difop_port = optional_uint16_from_json(sensor_cfg, "difop_port")) {
        config->gnss_port = *difop_port;
      }

      std::shared_ptr<nebula::drivers::RobosenseCalibrationConfiguration> calib;
      if (
        config->sensor_model == nebula::drivers::SensorModel::ROBOSENSE_EM4 && calib_file.empty()) {
        calib = nebula::drivers::load_em4_calibration_from_pcap(pcap_file.string(), *config);
      } else {
        calib = std::make_shared<nebula::drivers::RobosenseCalibrationConfiguration>();
      }
      if (config->sensor_model == nebula::drivers::SensorModel::ROBOSENSE_EMX) {
        calib->set_channel_size(192);
        calib->calibration.resize(192);
        calib->pixel_pitch.assign(192, 0);
        calib->half_vcsel_yaw_offset.assign(24, 0);
        calib->surface_pitch_offset.assign(2, 0);
      } else if (config->sensor_model == nebula::drivers::SensorModel::ROBOSENSE_EM4) {
        if (calib->calibration.empty()) {
          calib->set_channel_size(520);
        }
      } else {
        calib->set_channel_size(128);
      }
      if (!calib_file.empty()) {
        calib->load_from_file(calib_file);
      }
      robosense_driver = std::make_unique<nebula::drivers::RobosenseDriver>(config, calib);
    } else {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cerr << "Unsupported sensor model in current build: " << model_str << std::endl;
      pcap_close(handle);
      return result;
    }

    struct pcap_pkthdr * header;
    const u_char * packet_bytes;
    std::map<ReassemblyKey, FragmentAssembly> seyond_assemblies;
    while (pcap_next_ex(handle, &header, &packet_bytes) >= 0) {
      if (seyond_decoder) {
        process_seyond_ipv4_packet(
          packet_bytes, header->caplen, seyond_assemblies, *seyond_decoder, active_traffic_filter);
      } else if (hesai_driver || robosense_driver) {
        UdpPacket udp_packet{};
        if (!extract_udp_payload(packet_bytes, header->caplen, udp_packet)) {
          continue;
        }

        if (hesai_driver) {
          if (!should_process_hesai_packet(model_str, udp_packet, active_traffic_filter)) {
            continue;
          }
          hesai_driver->parse_cloud_packet(udp_packet.payload);
        } else if (robosense_driver) {
          if (!should_process_robosense_packet(model_str, udp_packet, active_traffic_filter)) {
            continue;
          }
          auto [cloud, timestamp] = robosense_driver->parse_cloud_packet(udp_packet.payload);
          if (cloud && !cloud->empty()) {
            exporter->callback(cloud, static_cast<uint64_t>(timestamp * 1e9));
          }
        }
      }
    }
    pcap_close(handle);

    {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cout << "Done sensor: " << name << " | frames: " << exporter->get_frames().size()
                << std::endl;
    }

    result.processed = true;
    result.exporter = exporter;
  } catch (const std::exception & ex) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cerr << "Sensor processing failed: " << name << " (" << model_str
              << ") | error: " << ex.what() << std::endl;
  }
  return result;
}

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
  const fs::path project_dir = fs::absolute(fs::path(project_file)).parent_path();
  if (!project.contains("sensors") || !project["sensors"].is_array()) {
    std::cerr << "Project file must contain a 'sensors' array" << std::endl;
    return 1;
  }

  auto logger = std::make_shared<ConsoleLogger>();
  std::vector<std::shared_ptr<CloudExporter>> exporters;
  json processed_sensors = json::array();
  json skipped_sensors = json::array();

  fs::remove_all("web/data");  // Clean old data
  fs::create_directories("web/data");

  const json export_defaults = project.value("export_defaults", json::object());

  std::vector<std::future<SensorResult>> futures;
  futures.reserve(project["sensors"].size());
  for (const auto & sensor_cfg : project["sensors"]) {
    if (!sensor_cfg.is_object()) {
      skipped_sensors.push_back({{"reason", "sensor entry is not a JSON object"}});
      continue;
    }

    const std::string name = sensor_cfg.value("name", "<unnamed>");
    const std::string pcap_value = sensor_cfg.value("pcap", "");
    if (pcap_value.empty()) {
      skipped_sensors.push_back({{"name", name}, {"reason", "missing pcap path"}});
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cerr << "Skipping sensor " << name << ": missing pcap path" << std::endl;
      continue;
    }

    const fs::path pcap_file = resolve_project_path(project_dir, pcap_value);
    std::error_code ec;
    const bool exists = fs::exists(pcap_file, ec);
    if (ec || !exists) {
      skipped_sensors.push_back(
        {{"name", name}, {"pcap", pcap_file.string()}, {"reason", "pcap file not found"}});
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cerr << "Skipping sensor " << name << ": pcap file not found: " << pcap_file
                << std::endl;
      continue;
    }

    futures.push_back(
      std::async(
        std::launch::async, process_sensor, sensor_cfg, export_defaults, project_dir, logger));
  }

  for (auto & future : futures) {
    auto result = future.get();
    if (!result.processed) {
      skipped_sensors.push_back(
        {{"name", result.sensor_cfg.value("name", "<unnamed>")},
         {"pcap", result.sensor_cfg.value("pcap", "")},
         {"reason", "failed during processing"}});
      continue;
    }
    exporters.push_back(result.exporter);
    processed_sensors.push_back(result.sensor_cfg);
  }

  // Save metadata for web visualizer
  json meta;
  meta["project_name"] = project.value("project_name", "Multi-Sensor Visualization");
  meta["skipped_sensors"] = skipped_sensors;

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
    s["model"] = normalize_model_name(processed_sensors[i]["model"]);
    s["source_frame_count"] = exporters[i]->get_seen_frame_count();
    s["transform"] = processed_sensors[i].value(
      "transform", json::object({{"pos", {0, 0, 0}}, {"rot", {0, 0, 0}}}));
    s["camera"] = processed_sensors[i].value(
      "camera", json::object({{"pos", {0, 0, 50}}, {"target", {0, 0, 0}}}));

    json frames_list = json::array();
    for (auto & f : exporters[i]->get_frames()) {
      frames_list.push_back(
        {{"t", f.timestamp}, {"path", f.path}, {"source_index", f.source_index}});
    }
    s["frames"] = frames_list;
    sensors_meta.push_back(s);
  }
  meta["sensors"] = sensors_meta;

  std::ofstream meta_file("web/data/metadata.json");
  meta_file << meta.dump(2);
  meta_file.close();

  std::cout << "Finished! Processed " << exporters.size() << " sensors";
  if (!skipped_sensors.empty()) {
    std::cout << ", skipped " << skipped_sensors.size() << " sensors";
  }
  std::cout << ", and exported " << global_timeline.size() << " global timeline points."
            << std::endl;

  if (exporters.empty()) {
    std::cerr << "No sensors were processed successfully." << std::endl;
    return 1;
  }

  return 0;
}
