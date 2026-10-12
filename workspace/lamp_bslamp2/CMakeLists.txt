# 宠物活动状态灯 V1 — 灯端独立固件（ESP-IDF v5.x）
#
# 目标：米家床头灯 2 / bslamp2 / MUE4093GL（ESP32-WROOM-32D, 4MB Flash）
#
# ⚠️ 首次覆盖固件前必须完整备份原厂 Flash（见 README.md 的恢复流程）。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pet_status_lamp)
