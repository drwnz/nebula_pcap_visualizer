cmake_minimum_required(VERSION 3.10)
project(nebula_pcap_visualizer)

set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(nebula_core_common REQUIRED)
find_package(nebula_core_decoders REQUIRED)
find_package(nebula_seyond_common REQUIRED)
find_package(nebula_seyond_decoders REQUIRED)
find_package(nebula_hesai_common REQUIRED)
find_package(nebula_hesai_decoders REQUIRED)
find_package(nebula_robosense_common REQUIRED)
find_package(nebula_robosense_decoders REQUIRED)
find_package(nlohmann_json REQUIRED)
find_path(PCAP_INCLUDE_DIR NAMES pcap.h)
find_library(PCAP_LIBRARY NAMES pcap)

include_directories(
  ${PCAP_INCLUDE_DIR}
  ${nebula_core_common_INCLUDE_DIRS}
  ${nebula_core_decoders_INCLUDE_DIRS}
  ${nebula_seyond_common_INCLUDE_DIRS}
  ${nebula_seyond_decoders_INCLUDE_DIRS}
  ${nebula_hesai_common_INCLUDE_DIRS}
  ${nebula_hesai_decoders_INCLUDE_DIRS}
  ${nebula_robosense_common_INCLUDE_DIRS}
  ${nebula_robosense_decoders_INCLUDE_DIRS}
)

add_executable(pcap_converter src/main.cpp)

target_link_libraries(pcap_converter
  nebula_core_common::nebula_core_common
  nebula_core_decoders::nebula_core_decoders
  nebula_seyond_decoders::nebula_seyond_decoders
  nebula_hesai_decoders::nebula_hesai_decoders
  nebula_robosense_decoders::nebula_robosense_decoders
  nlohmann_json::nlohmann_json
  ${PCAP_LIBRARY}
)
