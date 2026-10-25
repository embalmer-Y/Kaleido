# Kaleido soul-bench: V-34 x V-35 cross stress bench (headless, Korvo-1 ESP32-S31)
# Independent of board BSP. Flash ONLY the app partition to ota_0 @0x20000.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(soul-bench)
