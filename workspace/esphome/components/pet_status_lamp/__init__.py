"""ESPHome 外部组件 pet_status_lamp 的 schema 定义与代码生成。

⚠️ 这是本工程**唯一**允许存在的 Python 文件。
   它只在 PC 上运行（ESPHome 编译期代码生成），**不会进入设备固件**。
   设备侧全部逻辑是 C / C++。

它做四件事：
  1. 声明 YAML 可配置项（默认值与 core/pet_config.h 保持一致）
  2. 把 core/ 的纯 C 源文件与头文件复制进 ESPHome 的 src/pet_core/，
     这样平台构建能直接编译它们（无需在工程里放重复副本）
  3. 把 web/pet_ui.html 生成为 C++ 头文件 pet_ui_html.h
     —— 自定义网页因此不依赖任何 ESPHome web_server 的内部 API，
        也就不会随 ESPHome 版本升级而失效
  4. 生成 C++ 组件实例并注册 BLE 监听
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@pet-status-lamp"]

# 依赖：需要在 ESP32 上跑，且要 BLE 被动扫描
DEPENDENCIES = ["esp32", "esp32_ble_tracker"]
AUTO_LOAD = []

pet_status_lamp_ns = cg.esphome_ns.namespace("pet_status_lamp")
PetStatusLamp = pet_status_lamp_ns.class_(
    "PetStatusLamp", cg.Component, esp32_ble_tracker.ESPBTDeviceListener
)

# ------------------------------------------------------------------ keys
CONF_MAX_DEVICES = "max_devices"
CONF_LOST_TIMEOUT = "lost_timeout"
CONF_MANUAL_OVERRIDE = "manual_override"
CONF_PAIR_WINDOW = "pair_window"

CONF_NIGHT_ENABLED = "night_enabled"
CONF_NIGHT_START = "night_start_hour"
CONF_NIGHT_END = "night_end_hour"
CONF_NIGHT_IDLE_INFER = "night_idle_infer"
CONF_NIGHT_CAP_QUIET = "night_cap_quiet"
CONF_NIGHT_CAP_LIGHT = "night_cap_light"
CONF_NIGHT_CAP_ACTIVE = "night_cap_active"
CONF_NIGHT_CAP_LOST = "night_cap_lost"
CONF_NIGHT_THROTTLE = "night_active_throttle"
CONF_NIGHT_SUPPRESS_LOWBAT = "night_suppress_lowbattery"

CONF_WEB_PORT = "web_port"
CONF_WEB_USERNAME = "web_username"
CONF_WEB_PASSWORD = "web_password"

CONF_PIN_RED = "pin_red"
CONF_PIN_GREEN = "pin_green"
CONF_PIN_BLUE = "pin_blue"
CONF_PIN_WHITE = "pin_white"
CONF_PIN_SWITCH1 = "pin_switch1"
CONF_PIN_SWITCH2 = "pin_switch2"

# core/ 里参与编译的源文件（顺序无关，但必须齐全）
CORE_SOURCES = (
    "pet_protocol.c",
    "device_registry.c",
    "night_mode.c",
    "lamp_driver.c",
    "lamp_duty.c",
    "lamp_gamma.c",
    "lamp_effects.c",
)
# core/ 里需要一起复制的头文件
CORE_HEADERS = (
    "pet_config.h",
    "pet_types.h",
    "pet_protocol.h",
    "device_registry.h",
    "night_mode.h",
    "lamp_driver.h",
    "lamp_gamma.h",
    "lamp_effects.h",
)

# ------------------------------------------------------------------ schema
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PetStatusLamp),
        # ---- 多狗（决策 D6：默认 8，可配 1–16） ----
        cv.Optional(CONF_MAX_DEVICES, default=8): cv.int_range(min=1, max=16),
        # ---- 失联与豁免（需求 4.3 / 决策 D9） ----
        cv.Optional(CONF_LOST_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MANUAL_OVERRIDE, default="10min"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PAIR_WINDOW, default="60s"): cv.positive_time_period_milliseconds,
        # ---- 夜间模式（决策 D8 / D14） ----
        cv.Optional(CONF_NIGHT_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_NIGHT_START, default=22): cv.int_range(min=0, max=23),
        cv.Optional(CONF_NIGHT_END, default=7): cv.int_range(min=0, max=23),
        cv.Optional(CONF_NIGHT_IDLE_INFER, default="120min"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_NIGHT_CAP_QUIET, default=10): cv.int_range(min=0, max=255),
        cv.Optional(CONF_NIGHT_CAP_LIGHT, default=14): cv.int_range(min=0, max=255),
        cv.Optional(CONF_NIGHT_CAP_ACTIVE, default=18): cv.int_range(min=0, max=255),
        cv.Optional(CONF_NIGHT_CAP_LOST, default=6): cv.int_range(min=0, max=255),
        cv.Optional(CONF_NIGHT_THROTTLE, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_NIGHT_SUPPRESS_LOWBAT, default=True): cv.boolean,
        # ---- 内置网页（档 1 App 控制） ----
        cv.Optional(CONF_WEB_PORT, default=8080): cv.port,
        cv.Optional(CONF_WEB_USERNAME, default="admin"): cv.string_strict,
        cv.Optional(CONF_WEB_PASSWORD, default="petlamp"): cv.string_strict,
        # ---- 灯板 PWM 引脚（默认值 = bslamp2 实际接线，需求 4.2） ----
        cv.Optional(CONF_PIN_RED, default=13): cv.int_range(min=0, max=48),
        cv.Optional(CONF_PIN_GREEN, default=14): cv.int_range(min=0, max=48),
        cv.Optional(CONF_PIN_BLUE, default=5): cv.int_range(min=0, max=48),
        cv.Optional(CONF_PIN_WHITE, default=12): cv.int_range(min=0, max=48),
        cv.Optional(CONF_PIN_SWITCH1, default=33): cv.int_range(min=0, max=48),
        cv.Optional(CONF_PIN_SWITCH2, default=4): cv.int_range(min=0, max=48),
    }
).extend(cv.COMPONENT_SCHEMA)


def _clamp(value: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, int(value)))


def _core_dir() -> Path:
    """core/ 的绝对路径：本组件在 ws/esphome/components/pet_status_lamp/。"""
    return Path(__file__).resolve().parents[3] / "core"


def _stage_core() -> Path:
    """把 core/ 复制进 ESPHome 的 src/pet_core/，返回该目录。

    为什么复制而不是引用原目录：
      ESPHome 的构建只编译 src/ 下的源文件；外部目录需要额外配置
      build_src_filter，且容易出现 include 路径不一致。
      复制是可控且幂等的（copy2 每次都刷新），代价只是构建目录里多一份，
      源工程仍然只有一份权威实现（core/）。
    """
    src = _core_dir()
    if not src.is_dir():
        raise ValueError(
            f"找不到 core/ 目录：{src}\n"
            "pet_status_lamp 组件要求 workspace/core/ 与本组件一起存在。"
        )
    dst = Path(CORE.relative_src_path("pet_core"))
    dst.mkdir(parents=True, exist_ok=True)

    for name in CORE_SOURCES + CORE_HEADERS:
        f = src / name
        if not f.is_file():
            raise ValueError(f"core/ 缺少必需文件：{f}")
        shutil.copy2(f, dst / name)

    # 生成网页头文件（原始字符串包裹，HTML 里的括号不会影响 C++）
    html_path = Path(__file__).resolve().parent / "web" / "pet_ui.html"
    html = html_path.read_text(encoding="utf-8")
    if ")PETUI\"" in html:
        raise ValueError("pet_ui.html 含有与原始字符串定界符冲突的序列 )PETUI\"")
    header = (
        "// 由 components/pet_status_lamp/__init__.py 自动生成，请勿手改。\n"
        "// 源文件：components/pet_status_lamp/web/pet_ui.html\n"
        "#pragma once\n"
        "static const char PET_UI_HTML[] = R\"PETUI(\n"
        + html
        + "\n)PETUI\";\n"
    )
    (dst / "pet_ui_html.h").write_text(header, encoding="utf-8")
    return dst


async def to_code(config):
    """生成 C++ 代码。"""
    core_dir = _stage_core()

    var = cg.new_Pvariable(config[CONF_ID])

    # ---- 编译 core/：加入 include 路径 + 参与构建 ----
    cg.add_build_flag(f"-I{CORE.relative_src_path('pet_core')}")

    # ---- 时序参数（按需求/决策钳位，避免 YAML 写出离谱值） ----
    cg.add(var.set_max_devices(config[CONF_MAX_DEVICES]))
    cg.add(
        var.set_lost_timeout_ms(
            _clamp(config[CONF_LOST_TIMEOUT], 10_000, 600_000)
        )
    )
    cg.add(
        var.set_manual_override_ms(
            _clamp(config[CONF_MANUAL_OVERRIDE], 60_000, 120 * 60_000)
        )
    )
    cg.add(
        var.set_pair_window_ms(_clamp(config[CONF_PAIR_WINDOW], 5_000, 300_000))
    )

    # ---- 夜间模式 ----
    cg.add(var.set_night_enabled(config[CONF_NIGHT_ENABLED]))
    cg.add(var.set_night_window(config[CONF_NIGHT_START], config[CONF_NIGHT_END]))
    cg.add(
        var.set_night_idle_infer_ms(
            _clamp(config[CONF_NIGHT_IDLE_INFER], 10 * 60_000, 10 * 3600_000)
        )
    )
    cg.add(
        var.set_night_caps(
            config[CONF_NIGHT_CAP_QUIET],
            config[CONF_NIGHT_CAP_LIGHT],
            config[CONF_NIGHT_CAP_ACTIVE],
            config[CONF_NIGHT_CAP_LOST],
        )
    )
    cg.add(
        var.set_night_active_throttle_ms(
            _clamp(config[CONF_NIGHT_THROTTLE], 10_000, 600_000)
        )
    )
    cg.add(var.set_night_suppress_lowbattery(config[CONF_NIGHT_SUPPRESS_LOWBAT]))

    # ---- 内置网页 ----
    cg.add(var.set_web_port(config[CONF_WEB_PORT]))
    cg.add(
        var.set_web_credentials(
            config[CONF_WEB_USERNAME], config[CONF_WEB_PASSWORD]
        )
    )

    # ---- 灯板引脚 ----
    cg.add(
        var.set_pwm_pins(
            config[CONF_PIN_RED],
            config[CONF_PIN_GREEN],
            config[CONF_PIN_BLUE],
            config[CONF_PIN_WHITE],
            config[CONF_PIN_SWITCH1],
            config[CONF_PIN_SWITCH2],
        )
    )

    # ---- 组件注册 + BLE 监听注册 ----
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)

    # 提示：core_dir 已复制到构建目录
    cg.add_define("USE_PET_STATUS_LAMP")
