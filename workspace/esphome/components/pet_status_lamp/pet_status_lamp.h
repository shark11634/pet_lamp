/*
 * pet_status_lamp.h — ESPHome 外部组件：宠物活动状态灯（灯端）
 *
 * 本组件是"薄胶水层"：
 *   全部业务逻辑在 workspace/core/ 的纯 C99 实现里（协议解析、设备表、
 *   夜间模式、灯效）。ESPHome 只负责三件事：
 *     1. 用 esp32_ble_tracker 做被动扫描，把原始广播交给 core 解析
 *     2. 把 core 算出的逻辑颜色 + 层系数换算成 4 路 PWM 并输出
 *     3. 提供内置网页（宠物状态 / 灯光控制 / 设置）
 *
 * 为什么这样分层：core 可以在 PC 上编译并跑 290 项断言（不需要 ESP32、
 * 不需要 ESPHome），因此绝大多数缺陷在烧板子之前就能发现。
 *
 * ── 已对着 ESPHome 源码核实的接口 ───────────────────────────────────
 *   esp32_ble_tracker.h:47   using ESPBTDevice = ble_device_base::ESPBTDevice;
 *   ble_device.h             class ESPBTDeviceListener {
 *                              virtual bool parse_device(const ESPBTDevice &) = 0; }
 *   ble_device.h             ESPBTDevice::get_manufacturer_datas()
 *                              -> const std::vector<ServiceData> &
 *                            ServiceData { ESPBTUUID uuid; adv_data_t data; }
 *                            uuid 是 Company ID；data 是 **去掉 Company ID
 *                            之后** 的字节。我们的协议把 Company ID 当作
 *                            16 字节 MFG 的第 0–1 字节，所以要拼回去。
 *   ble_device.h             ESPBTUUID::uuid16() / contains()
 *   esp32_ble_tracker.h:190  void register_listener(ESPBTDeviceListener *);
 *
 * ── 刻意不做的事 ────────────────────────────────────────────────────
 *   不直接持有 ESPHome 实体（text_sensor / sensor / binary_sensor）的指针。
 *   那些类型的头文件路径与基类在 ESPHome 各版本间会变，硬编码会带来
 *   版本脆性；而且我们无法在本机编译验证。
 *   取而代之：所有状态都通过本组件的公开 getter + 网页 API 暴露，
 *   YAML 里用 `lambda:` 把它们映射成标准 ESPHome 实体
 *   ——这样 MQTT / HomeAssistant 自动发现由 ESPHome 自己负责，
 *   我们一行实体代码都不用写，也不会有版本耦合。
 *   见 esphome/pet-lamp.yaml 的 "实体映射" 段。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include "esp_http_server.h"

/* ---- core/ 的纯 C 接口（以 extern "C" 引入，保持 C99 语义） ----
 * 这些头文件由 __init__.py 复制到 src/pet_core/ 并加入 include 路径，
 * 因此这里可以直接按名字包含。 */
extern "C" {
#include "pet_config.h"
#include "pet_types.h"
#include "pet_protocol.h"
#include "device_registry.h"
#include "night_mode.h"
#include "lamp_driver.h"
#include "lamp_effects.h"
}

namespace esphome::pet_status_lamp {

/* ------------------------------------------------------------------ */
/* core → 硬件的 PWM 输出适配                                          */
/* ------------------------------------------------------------------ */

/**
 * 把 core 算好的 4 通道占空比写进 ESP32 LEDC。
 *
 * 本类**不做** Gamma 校正与限流 —— 那些在 core/lamp_driver.c 里，
 * 已经过主机测试。这里只做"占空比 → 寄存器"。
 */
class PetLampPwm {
 public:
  void setup(uint8_t pin_r, uint8_t pin_g, uint8_t pin_b, uint8_t pin_w,
             uint8_t sw1, uint8_t sw2);
  void write_duty(const uint16_t *duty);
  bool ready() const { return this->ready_; }

 private:
  int pin_[LAMP_CH_COUNT]{};
  bool ready_{false};
};

/* ------------------------------------------------------------------ */
/* 主组件                                                             */
/* ------------------------------------------------------------------ */

class PetStatusLamp : public Component,
                      public esp32_ble_tracker::ESPBTDeviceListener {
 public:
  /* ---- 生命周期 ---- */
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  /* ---- BLE：回调里只做入队，解析放到 loop()（不阻塞 BLE 任务） ---- */
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;
  void on_scan_end() override {}

  /* ---- YAML 注入（由 __init__.py 的 to_code 调用） ---- */
  void set_max_devices(uint8_t n) { this->max_devices_ = n; }
  void set_lost_timeout_ms(uint32_t ms) { this->lost_timeout_ms_ = ms; }
  void set_manual_override_ms(uint32_t ms) { this->override_ms_ = ms; }
  void set_pair_window_ms(uint32_t ms) { this->pair_window_ms_ = ms; }
  void set_night_enabled(bool on) { this->night_enabled_ = on; }
  void set_night_window(uint8_t start, uint8_t end) {
    this->night_start_ = start;
    this->night_end_ = end;
  }
  void set_night_idle_infer_ms(uint32_t ms) { this->night_idle_ms_ = ms; }
  void set_night_caps(uint8_t quiet, uint8_t light, uint8_t active, uint8_t lost) {
    this->cap_quiet_ = quiet;
    this->cap_light_ = light;
    this->cap_active_ = active;
    this->cap_lost_ = lost;
  }
  void set_night_active_throttle_ms(uint32_t ms) { this->night_throttle_ms_ = ms; }
  void set_night_suppress_lowbattery(bool on) { this->night_suppress_lowbat_ = on; }
  void set_web_port(uint16_t port) { this->web_port_ = port; }
  void set_web_credentials(const std::string &user, const std::string &pass) {
    this->web_user_ = user;
    this->web_pass_ = pass;
  }
  void set_pwm_pins(uint8_t pin_r, uint8_t pin_g, uint8_t pin_b, uint8_t pin_w,
                    uint8_t sw1, uint8_t sw2) {
    this->pin_r_ = pin_r;
    this->pin_g_ = pin_g;
    this->pin_b_ = pin_b;
    this->pin_w_ = pin_w;
    this->sw1_ = sw1;
    this->sw2_ = sw2;
  }

  /* ---- 供网页 / YAML lambda 调用的命令（全部刷新手动豁免计时） ---- */
  void cmd_set_power(bool on);
  void cmd_set_brightness(uint8_t pct);       /* 1–100，对齐米家属性 */
  void cmd_set_color_temp(uint32_t kelvin);   /* 1700–6500 */
  void cmd_set_rgb(uint32_t rgb);             /* 1–16777215 */
  void cmd_set_mode(uint8_t mode);            /* 1 = Color, 2 = Day */
  void cmd_set_night_enabled(bool on);
  void cmd_set_night_window(uint8_t start, uint8_t end);
  void cmd_set_night_caps(uint8_t quiet, uint8_t light, uint8_t active, uint8_t lost);
  void cmd_set_night_throttle_ms(uint32_t ms);
  void cmd_set_lost_timeout_ms(uint32_t ms);
  void cmd_set_override_minutes(uint16_t minutes);
  void cmd_exit_manual();
  void cmd_enter_pairing();
  void cmd_clear_bindings();
  bool cmd_set_max_devices(uint8_t n);
  bool cmd_set_focus(int index);              /* -1 = 合并显示 */
  bool cmd_rename(uint32_t device_id, const std::string &name);
  bool cmd_remove(uint32_t device_id);

  /** HTTP 处理器需要读认证凭据；不对外暴露为可写 */
  const std::string &web_user() const { return this->web_user_; }
  const std::string &web_pass() const { return this->web_pass_; }

  /* ---- 状态读取（HTTP API / YAML lambda / 日志） ---- */
  const pet_registry_t &registry() const { return this->reg_; }
  const pet_night_t &night() const { return this->night_; }
  pet_lamp_state_t merged_state() const { return this->merged_; }
  uint8_t merged_source() const { return this->merged_src_; }
  uint8_t merged_score() const { return this->merged_score_; }
  const char *merged_state_name() const { return lamp_state_name(this->merged_); }
  const char *night_mode_name() const {
    return pet_night_mode_name(&this->night_, this->now_ms_());
  }
  uint8_t device_count() const { return pet_reg_count(&this->reg_); }
  uint8_t max_devices() const { return this->reg_.cap; }
  uint32_t lost_timeout_ms() const { return this->lost_timeout_ms_; }
  uint32_t override_ms() const { return this->override_ms_; }
  uint32_t override_remaining_ms() const {
    return pet_night_override_remaining_ms(&this->night_, this->now_ms_());
  }
  uint64_t lost_age_ms() const { return pet_reg_lost_age_ms(&this->reg_, this->now_ms_()); }
  bool is_pairing() const { return this->pairing_; }
  bool manual_off() const { return this->manual_off_; }
  bool low_battery() const {
    return pet_reg_any_low_battery(&this->reg_, this->now_ms_(), this->lost_timeout_ms_);
  }
  bool sensor_error() const {
    return pet_reg_any_sensor_error(&this->reg_, this->now_ms_(), this->lost_timeout_ms_);
  }
  /** 某只狗的名字；未命名时返回 "Pet-N"（决策 O3） */
  void device_name(uint8_t index, char *out, size_t out_len) const {
    pet_reg_display_name(&this->reg_, index, out, out_len);
  }
  const pet_device_t *device_at(uint8_t index) const {
    return (index < this->reg_.count) ? &this->reg_.dev[index] : nullptr;
  }

  /* ---- 供 HTTP 处理器生成 JSON ---- */
  std::string json_state();
  std::string json_devices();
  std::string json_settings();

 private:
  /* ---- BLE 原始广播环形缓冲（回调 → loop 传递） ---- */
  struct RawAdv {
    uint8_t mfg[PET_MFG_DATA_LEN];
    uint8_t mfg_len;
    int8_t rssi;
    uint32_t rx_ms;
  };
  static constexpr size_t ADV_QUEUE_LEN = 16;
  RawAdv adv_queue_[ADV_QUEUE_LEN]{};
  volatile size_t adv_head_{0};
  volatile size_t adv_tail_{0};
  void push_adv_(const RawAdv &raw);

  /* ---- 三档节奏（对应需求 6.3 的任务频率） ---- */
  void drain_adv_();       /* BLE 取包 + 协议解析 + 设备表更新 */
  void tick_state_();      /* 1 Hz：合并、失联、夜间 tick */
  void tick_effects_();    /* 50 Hz：灯效插值 + 输出 */

  void bind_new_device_(uint32_t device_id);
  void refresh_manual_flags_();
  void load_prefs_();
  void save_bindings_();
  void save_settings_();
  bool start_web_();
  uint32_t now_ms_() const;
  bool clock_hour_(int *hour) const;

  /** core 的输出回调：把逻辑颜色+层系数转成占空比并写硬件 */
  static void output_trampoline_(const lamp_color_t *color, float layer,
                                 uint16_t floor_duty, void *user);
  lamp_color_t apply_pwm_limit_(const lamp_color_t *c, float layer) const;

  /* ---- 配置（YAML 可覆盖） ---- */
  uint8_t max_devices_{PET_DEV_MAX_DEFAULT};
  uint32_t lost_timeout_ms_{LOST_TIMEOUT_MS};
  uint32_t override_ms_{MANUAL_OVERRIDE_MS_DEFAULT};
  uint32_t pair_window_ms_{PAIR_MODE_DURATION_MS};
  bool night_enabled_{NIGHT_MODE_ENABLE_DEFAULT != 0};
  uint8_t night_start_{NIGHT_START_HOUR};
  uint8_t night_end_{NIGHT_END_HOUR};
  uint32_t night_idle_ms_{NIGHT_IDLE_INFER_MS};
  uint32_t night_throttle_ms_{NIGHT_ACTIVE_THROTTLE_MS};
  bool night_suppress_lowbat_{NIGHT_SUPPRESS_LOWBAT_HINT != 0};
  uint8_t cap_quiet_{NIGHT_CAP_QUIET};
  uint8_t cap_light_{NIGHT_CAP_LIGHT};
  uint8_t cap_active_{NIGHT_CAP_ACTIVE};
  uint8_t cap_lost_{NIGHT_CAP_LOST};
  uint16_t web_port_{80};
  std::string web_user_{"admin"};
  std::string web_pass_{"petlamp"};
  uint8_t pin_r_{13};
  uint8_t pin_g_{14};
  uint8_t pin_b_{5};
  uint8_t pin_w_{12};
  uint8_t sw1_{33};
  uint8_t sw2_{4};

  /* ---- 运行状态 ---- */
  pet_registry_t reg_{};
  pet_night_t night_{};
  lamp_driver_t driver_{};
  lamp_fx_state_t fx_{};
  PetLampPwm pwm_{};
  pet_lamp_state_t merged_{PET_LAMP_BOOT};
  uint8_t merged_src_{PET_FOCUS_MERGE};
  uint8_t merged_score_{0};
  bool pairing_{false};
  uint32_t pairing_until_{0};
  uint32_t last_state_tick_{0};
  uint32_t last_effect_tick_{0};
  uint32_t last_diag_{0};
  uint32_t boot_ms_{0};
  bool setup_done_{false};

  /* ---- 手动灯光（对齐米家属性模型） ---- */
  bool manual_off_{false};
  pet_manual_t manual_{};

  /* ---- 持久化 ---- */
  ESPPreferenceObject pref_bindings_{};
  ESPPreferenceObject pref_settings_{};
  ESPPreferenceObject pref_names_{};
  bool prefs_ready_{false};

  /* ---- 网页 ---- */
  httpd_handle_t httpd_{nullptr};
};

/* ------------------------------------------------------------------ */
/* NVS 布局（与 docs/最终可执行方案.md §4 WP3 的键规划一致）           */
/* ------------------------------------------------------------------ */

struct PetBindingsBlob {
  uint32_t magic;                                 /* 变更布局时递增，读到时清零 */
  uint8_t  count;
  uint8_t  focus;                                 /* 0xFF = 合并显示 */
  uint8_t  max_devices;                           /* 运行期容量 1–16 */
  uint8_t  reserved;
  uint32_t device_id[PET_DEV_MAX_LIMIT];
};

struct PetNamesBlob {
  uint32_t magic;
  uint8_t  count;
  uint8_t  reserved[3];
  uint32_t device_id[PET_DEV_MAX_LIMIT];
  char     name[PET_DEV_MAX_LIMIT][PET_DEV_NAME_LEN];
};

struct PetSettingsBlob {
  uint32_t magic;
  uint32_t lost_timeout_ms;
  uint32_t override_ms;
  uint32_t night_idle_ms;
  uint32_t night_throttle_ms;
  uint8_t  night_enabled;
  uint8_t  night_start;
  uint8_t  night_end;
  uint8_t  cap_quiet;
  uint8_t  cap_light;
  uint8_t  cap_active;
  uint8_t  cap_lost;
  uint8_t  night_suppress_lowbat;
  uint8_t  manual_off;
  uint8_t  manual_on;
  uint8_t  manual_brightness;
  uint8_t  manual_light_mode;                     /* 1 = Color, 2 = Day */
  uint32_t manual_color_temp;
  uint32_t manual_rgb;
};

static constexpr uint32_t PET_NVS_MAGIC = 0x50455431u;  /* 'PET1' */

}  // namespace esphome::pet_status_lamp
