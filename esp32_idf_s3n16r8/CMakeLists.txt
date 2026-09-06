# Top-level CMake for the ESP-IDF port of esp32_wifi_alarm.
# Use with ESP-IDF v5.3 LTS:
#   . $IDF_PATH/export.ps1
#   idf.py set-target esp32s3
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_wifi_alarm_idf)
