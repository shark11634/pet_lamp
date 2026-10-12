# 灯端主组件
#
# 业务逻辑不再放在这里：全部来自 ../../core 的纯 C99 实现，
# 那部分有 366 项主机断言覆盖（test/run_all_host_tests.ps1，含集成测试）。
# main/ 只保留 ESP-IDF 绑定层：BLE 扫描、LEDC、NVS、面板 I²C、编排。
idf_component_register(
    SRCS
        # ---- ESP-IDF 绑定层（本目录） ----
        "app_main.c"
        "ble_scanner.c"
        "lamp_esp32.c"
        "touch_panel.c"
        "panel_protocol.c"
        "storage.c"
        "diagnostics.c"
        # ---- core/：唯一一份业务实现（主机可测） ----
        "${CMAKE_CURRENT_LIST_DIR}/../../core/pet_protocol.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/device_registry.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/night_mode.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/lamp_driver.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/lamp_duty.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/lamp_gamma.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/lamp_effects.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../core"
    REQUIRES
        nvs_flash
        esp_timer
        driver
        bt
)
