# Prometheus Industrial HMI - ESP-IDF project (ESP32-S3 + 800x480 RGB panel + GT911 touch)
#   idf.py set-target esp32s3
#   idf.py menuconfig        (HMI settings: Wi-Fi, PLC address)
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(industrial_hmi)
