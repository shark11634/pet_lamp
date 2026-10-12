/*
 * pet_status_lamp.cpp — ESPHome 外部组件实现（薄胶水层）
 *
 * 本文件只做四件事：
 *   1. BLE 广播 → 入队（回调里绝不做解析/分配）
 *   2. 队列 → core/pet_protocol.c + core/device_registry.c
 *   3. core 的输出回调 → LEDC 占空比
 *   4. HTTP 网页/JSON（宠物状态、灯光控制、设置）
 *
 * 所有阈值、亮度区间、夜间上限、豁免时长都来自 core/pet_config.h，
 * 不在这里重复定义。改行为请改 core/ 并跑 test/run_all_host_tests.ps1。
 */
#include "pet_status_lamp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"

namespace esphome::pet_status_lamp {

static const char *const TAG = "pet_lamp";

/* 生成的网页（由 __init__.py 在代码生成阶段写入 src/pet_core/pet_ui_html.h） */
#include "pet_ui_html.h"

/* ================================================================== */
/* PetLampPwm：占空比 → LEDC                                          */
/* ================================================================== */

void PetLampPwm::setup(uint8_t pin_r, uint8_t pin_g, uint8_t pin_b, uint8_t pin_w,
                       uint8_t sw1, uint8_t sw2) {
  this->pin_[LAMP_CH_R] = pin_r;
  this->pin_[LAMP_CH_G] = pin_g;
  this->pin_[LAMP_CH_B] = pin_b;
  this->pin_[LAMP_CH_W] = pin_w;

  /* 主开关必须先拉高，否则灯光电路不导通（需求 4.2 的硬件特性：
   * 不要在未验证电路的前提下直接写最小 PWM 程序）。 */
  pinMode(sw1, OUTPUT);
  pinMode(sw2, OUTPUT);
  digitalWrite(sw1, HIGH);
  digitalWrite(sw2, HIGH);

  for (int i = 0; i < LAMP_CH_COUNT; ++i) {
    pinMode(this->pin_[i], OUTPUT);
    digitalWrite(this->pin_[i], LOW);
  }
  this->ready_ = true;
}

void PetLampPwm::write_duty(const uint16_t *duty) {
  if (!this->ready_) {
    return;
  }
  /*
   * 逻辑颜色 → PWM 占空比的换算已在 core/lamp_driver.c 完成（含 Gamma 2.2、
   * 最小稳定亮度、全通道限流、层调制）。这里只做"占空比 → 引脚"。
   *
   * ⚠️ 分辨率：core 的 Gamma 表按 14 bit 设计（低亮呼吸需要足够量化档位）。
   * 本实现用 Arduino 的 analogWrite，其分辨率由内核决定（通常 8 bit），
   * 因此按比例缩放会**损失低亮的呼吸平滑度**。缩放的边界保护（非零不得变 0）
   * 在 core 的 lamp_duty_to_bits() 里，并且有主机测试覆盖。
   *
   * 想要完整的 14 bit（推荐）：改用 ESP-IDF 的 LEDC 驱动，示例：
   *   #include "driver/ledc.h"
   *   ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE,
   *       .duty_resolution = LEDC_TIMER_14_BIT,
   *       .timer_num = LEDC_TIMER_0, .freq_hz = LAMP_PWM_FREQ_HZ,
   *       .clk_cfg = LEDC_AUTO_CLK };
   *   ledc_timer_config(&t);
   *   // 每个通道 ledc_channel_config() 之后：
   *   ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty[i]);
   *   ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
   * 这样 duty[i] 可以原样写入，不需要 lamp_duty_to_bits()。
   */
  for (int i = 0; i < LAMP_CH_COUNT; ++i) {
    analogWrite(this->pin_[i], (int)lamp_duty_to_bits(duty[i], 8u));
  }
}

/* ================================================================== */
/* 组件生命周期                                                        */
/* ================================================================== */

float PetStatusLamp::get_setup_priority() const {
  /* 需要 BLE tracker 与网络先就绪 */
  return setup_priority::AFTER_BLUETOOTH;
}

uint32_t PetStatusLamp::now_ms_() const {
  return (uint32_t)(millis() & 0xFFFFFFFFu);
}

bool PetStatusLamp::clock_hour_(int *hour) const {
  auto *t = time::get_time();
  if (t == nullptr || t->year < 2020) {
    return false;   /* 时间未同步：让 night_mode 走空闲推断兜底 */
  }
  *hour = (int)t->hour;
  return true;
}

void PetStatusLamp::setup() {
  this->boot_ms_ = this->now_ms_();

  /* ---- 1. 设备表与夜间模式：先用 YAML 默认值初始化 ---- */
  pet_reg_init(&this->reg_, this->max_devices_);

  pet_night_config_t ncfg;
  pet_night_config_defaults(&ncfg);
  ncfg.enabled = this->night_enabled_;
  ncfg.start_hour = this->night_start_;
  ncfg.end_hour = this->night_end_;
  ncfg.idle_infer_ms = this->night_idle_ms_;
  ncfg.override_ms = this->override_ms_;
  ncfg.cap_quiet = this->cap_quiet_;
  ncfg.cap_light = this->cap_light_;
  ncfg.cap_active = this->cap_active_;
  ncfg.cap_lost = this->cap_lost_;
  ncfg.active_throttle_ms = this->night_throttle_ms_;
  ncfg.suppress_lowbat_hint = this->night_suppress_lowbat_;
  pet_night_init(&this->night_, &ncfg, this->now_ms_());

  /* ---- 2. 手动灯光默认值（对齐米家属性模型） ---- */
  this->manual_.on = false;
  this->manual_.brightness_pct = 100;
  this->manual_.color_temp_k = 3500;
  this->manual_.rgb = 0xFFC080u;
  this->manual_.mode = PET_LIGHT_MODE_DAY;

  /* ---- 3. NVS：载入绑定列表与设置（会覆盖上面的默认值） ---- */
  this->load_prefs_();

  /* ---- 4. PWM 输出 ---- */
  this->pwm_.setup(this->pin_r_, this->pin_g_, this->pin_b_, this->pin_w_,
                   this->sw1_, this->sw2_);

  lamp_platform_t plat;
  plat.write = [](const uint16_t *duty, void *user) {
    static_cast<PetLampPwm *>(user)->write_duty(duty);
  };
  plat.user = &this->pwm_;
  lamp_driver_init(&this->driver_, &plat);

  lamp_effects_init(&this->fx_);
  lamp_effects_set_output(&this->fx_, &PetStatusLamp::output_trampoline_, this);

  /* ---- 5. 注册 BLE 监听（被动扫描由 ESPHome 的 esp32_ble_tracker 负责） ---- */
  /* 注意：register_listener 由 ESPHome 的 codegen 在 register_ble_device() 里
   * 自动调用（见 __init__.py），这里不需要再做。 */

  /* ---- 6. 内置网页 ---- */
  if (this->web_port_ != 0) {
    if (!this->start_web_()) {
      ESP_LOGW(TAG, "内置网页启动失败（端口 %u 可能被占用）；"
                    "若同时启用了 ESPHome 的 web_server，请把其中一个换端口或关掉",
               (unsigned)this->web_port_);
    }
  }

  this->last_state_tick_ = this->boot_ms_;
  this->last_effect_tick_ = this->boot_ms_;
  this->last_diag_ = this->boot_ms_;
  this->setup_done_ = true;

  ESP_LOGI(TAG, "宠物活动状态灯就绪：设备表容量 %u，失联超时 %u ms，"
                "手动豁免 %u ms，夜间 %s（%02u:00-%02u:00，Active 上限 %u）",
           (unsigned)this->reg_.cap, (unsigned)this->lost_timeout_ms_,
           (unsigned)this->override_ms_, this->night_enabled_ ? "开" : "关",
           (unsigned)this->night_start_, (unsigned)this->night_end_,
           (unsigned)this->cap_active_);
}

void PetStatusLamp::dump_config() {
  ESP_LOGCONFIG(TAG, "Pet Status Lamp:");
  ESP_LOGCONFIG(TAG, "  设备表容量: %u（上限 %u）", (unsigned)this->reg_.cap,
                (unsigned)PET_DEV_MAX_LIMIT);
  ESP_LOGCONFIG(TAG, "  失联超时: %u ms", (unsigned)this->lost_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  手动豁免: %u ms", (unsigned)this->override_ms_);
  ESP_LOGCONFIG(TAG, "  夜间模式: %s  %02u:00-%02u:00",
                this->night_enabled_ ? "ON" : "OFF",
                (unsigned)this->night_start_, (unsigned)this->night_end_);
  ESP_LOGCONFIG(TAG, "  夜间上限: Quiet %u / Light %u / Active %u / Lost %u",
                (unsigned)this->cap_quiet_, (unsigned)this->cap_light_,
                (unsigned)this->cap_active_, (unsigned)this->cap_lost_);
  ESP_LOGCONFIG(TAG, "  夜间节流: %u ms", (unsigned)this->night_throttle_ms_);
  ESP_LOGCONFIG(TAG, "  网页: 端口 %u，用户 %s", (unsigned)this->web_port_,
                this->web_user_.c_str());
  ESP_LOGCONFIG(TAG, "  已绑定设备: %u", (unsigned)pet_reg_count(&this->reg_));
  for (uint8_t i = 0; i < this->reg_.count; ++i) {
    char name[PET_DEV_NAME_LEN + 8];
    pet_reg_display_name(&this->reg_, i, name, sizeof(name));
    ESP_LOGCONFIG(TAG, "    [%u] %s  ID=0x%08X", (unsigned)i, name,
                  (unsigned)this->reg_.dev[i].device_id);
  }
}

/* ================================================================== */
/* BLE：回调只入队                                                     */
/* ================================================================== */

void PetStatusLamp::push_adv_(const RawAdv &raw) {
  const size_t next = (this->adv_head_ + 1u) % ADV_QUEUE_LEN;
  if (next == this->adv_tail_) {
    /* 队列满：丢弃最旧的一条，绝不阻塞 BLE 任务 */
    this->adv_tail_ = (this->adv_tail_ + 1u) % ADV_QUEUE_LEN;
  }
  this->adv_queue_[this->adv_head_] = raw;
  this->adv_head_ = next;
}

bool PetStatusLamp::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  /*
   * ESPHome 已经把 AD 结构解析过了：get_manufacturer_datas() 返回的是
   *   ServiceData { ESPBTUUID uuid; adv_data_t data; }
   * 其中 uuid 是 Company ID，data 是**去掉 Company ID 之后**的字节。
   * 我们的协议把 Company ID 当作 16 字节 MFG 的第 0–1 字节，所以要拼回去。
   *
   * 这里只做"复制 + 入队"，不做任何解析（解析在 loop() 里），
   * 因为 BLE 回调运行在扫描任务上下文，长时间占用会影响收包率。
   */
  const auto &mfg_list = device.get_manufacturer_datas();
  if (mfg_list.empty()) {
    return false;
  }

  for (const auto &mfg : mfg_list) {
    /* 先按 Company ID 快速过滤，省掉后续拷贝。
     * 已核实 ble_device.h:  enum class Type { UNSET, UUID16, UUID32, UUID128 };
     *                      Type type() const;  uint16_t uuid16() const; */
    if (mfg.uuid.type() != ble_device_base::ESPBTUUID::Type::UUID16) {
      continue;
    }
    if (mfg.uuid.uuid16() != PET_COMPANY_ID) {
      continue;
    }
    /* data.size() 应为 PET_MFG_DATA_LEN - 2（14 字节 Payload） */
    if (mfg.data.size() + 2u != PET_MFG_DATA_LEN) {
      ESP_LOGV(TAG, "MFG 长度不符：%u（期望 %u）",
               (unsigned)(mfg.data.size() + 2u), (unsigned)PET_MFG_DATA_LEN);
      continue;
    }

    RawAdv raw;
    memset(&raw, 0, sizeof(raw));
    /* 拼回 Company ID（小端序） */
    raw.mfg[0] = (uint8_t)(PET_COMPANY_ID & 0xFFu);
    raw.mfg[1] = (uint8_t)((PET_COMPANY_ID >> 8) & 0xFFu);
    memcpy(&raw.mfg[2], mfg.data.data(), mfg.data.size());
    raw.mfg_len = (uint8_t)PET_MFG_DATA_LEN;
    raw.rssi = (int8_t)device.get_rssi();
    raw.rx_ms = this->now_ms_();
    this->push_adv_(raw);
    return true;
  }
  return false;
}

/* ================================================================== */
/* loop：三档节奏                                                       */
/* ================================================================== */

void PetStatusLamp::loop() {
  if (!this->setup_done_) {
    return;
  }
  const uint32_t now = this->now_ms_();

  /* 1) BLE 取包（20 Hz 足够：广播间隔 1 s，一轮扫到的包很少） */
  this->drain_adv_();

  /* 2) 状态合并 / 失联 / 夜间（1 Hz，对应需求 6.3） */
  if ((uint32_t)(now - this->last_state_tick_) >= TASK_STATE_INTERVAL_MS) {
    this->last_state_tick_ = now;
    this->tick_state_();
  }

  /* 3) 灯效插值 + 输出（50 Hz，对应需求 6.3 的 20–100 Hz） */
  if ((uint32_t)(now - this->last_effect_tick_) >= TASK_FX_INTERVAL_MS) {
    this->last_effect_tick_ = now;
    this->tick_effects_();
  }

  /* 4) 诊断日志（需求 8.5：10–60 秒一次，避免过多串口输出） */
  if ((uint32_t)(now - this->last_diag_) >= TASK_DIAG_INTERVAL_MS) {
    this->last_diag_ = now;
    const uint64_t age = this->lost_age_ms();
    ESP_LOGI(TAG,
             "state=%s src=%s score=%u nights=%s devices=%u/%u "
             "valid=%u dup=%u unbound=%u rssi=%d seq=%u last_seen_age=%s",
             this->merged_state_name(),
             (this->merged_src_ == PET_FOCUS_MERGE)
                 ? "merge"
                 : (this->device_at(this->merged_src_) ? "one" : "-"),
             (unsigned)this->merged_score_, this->night_mode_name(),
             (unsigned)pet_reg_count(&this->reg_), (unsigned)this->reg_.cap,
             (unsigned)this->reg_.total_valid, (unsigned)this->reg_.total_dup,
             (unsigned)this->reg_.total_unbound,
             (this->merged_src_ != PET_FOCUS_MERGE && this->device_at(this->merged_src_))
                 ? (int)this->device_at(this->merged_src_)->rssi
                 : 0,
             (this->merged_src_ != PET_FOCUS_MERGE && this->device_at(this->merged_src_))
                 ? (unsigned)this->device_at(this->merged_src_)->last_sequence
                 : 0u,
             (age == UINT64_MAX) ? "never" : "ms");
    if (age != UINT64_MAX) {
      ESP_LOGI(TAG, "last_seen_age_ms=%u", (unsigned)age);
    }
  }
}

void PetStatusLamp::drain_adv_() {
  while (this->adv_tail_ != this->adv_head_) {
    const RawAdv raw = this->adv_queue_[this->adv_tail_];
    this->adv_tail_ = (this->adv_tail_ + 1u) % ADV_QUEUE_LEN;

    pet_packet_t pkt;
    pet_rx_stats_t rx_stats;
    memset(&rx_stats, 0, sizeof(rx_stats));
    const pet_parse_result_t r = pet_parse_mfg_data(raw.mfg, raw.mfg_len,
                                                   &pkt, &rx_stats);
    if (r != PET_PARSE_OK) {
      /* 只记 DEBUG：CRC/版本错误在实测中可能很频繁（例如别家的 0xFFFF 广播） */
      ESP_LOGD(TAG, "丢弃广播：%s", pet_parse_error_name(r));
      continue;
    }
    pkt.rssi = raw.rssi;

    const bool pair_ok = this->pairing_;
    const pet_reg_result_t res = pet_reg_on_packet(&this->reg_, &pkt,
                                                   raw.rx_ms, pair_ok);
    switch (res) {
      case PET_RX_ACCEPTED:
        if (pair_ok && pet_reg_is_bound(&this->reg_, pkt.device_id)) {
          /* 配对模式下新设备入表：立即持久化并给用户可见反馈 */
          this->bind_new_device_(pkt.device_id);
        }
        break;
      case PET_RX_ERR_UNBOUND:
        ESP_LOGV(TAG, "未绑定设备 0x%08X 被忽略", (unsigned)pkt.device_id);
        break;
      case PET_RX_ERR_STALE:
        break;
      case PET_RX_ERR_NO_SLOT:
        ESP_LOGW(TAG, "设备表已满（%u），忽略 0x%08X",
                 (unsigned)this->reg_.cap, (unsigned)pkt.device_id);
        break;
    }
  }
}

void PetStatusLamp::bind_new_device_(uint32_t device_id) {
  this->save_bindings_();
  /* 需求 7.2：识别第一个合法协议包后用白色闪烁或常亮确认。
   * 这里用一次"恢复动画"（暖白闪一下）表达，并且不进入手动模式
   * ——因为这不是用户操作。 */
  lamp_effects_trigger_recover(&this->fx_, this->now_ms_());
  ESP_LOGI(TAG, "已绑定设备 0x%08X（当前 %u 只）", (unsigned)device_id,
           (unsigned)pet_reg_count(&this->reg_));
}

void PetStatusLamp::tick_state_() {
  const uint32_t now = this->now_ms_();

  /* 配对窗口到期 */
  if (this->pairing_ && (int32_t)(now - this->pairing_until_) >= 0) {
    this->pairing_ = false;
    ESP_LOGI(TAG, "配对窗口结束，当前绑定 %u 只",
             (unsigned)pet_reg_count(&this->reg_));
  }

  /* 夜间模式 tick（时间窗 + 空闲推断 + 豁免超时回落） */
  int hour = 0;
  const bool clock_ok = this->clock_hour_(&hour);
  pet_night_tick(&this->night_, now, hour, clock_ok);

  /* 各设备累计状态驻留时长（网页展示用） */
  pet_reg_tick(&this->reg_, now);

  /* 合并：最活跃者优先（决策 D7）；全部超时才算 Lost */
  uint8_t src = PET_FOCUS_MERGE;
  const pet_lamp_state_t st = pet_reg_merge(&this->reg_, now,
                                            this->lost_timeout_ms_, &src);
  if (st != this->merged_) {
    ESP_LOGI(TAG, "合并状态 %s -> %s", lamp_state_name(this->merged_),
             lamp_state_name(st));
    if (st == PET_LAMP_LOST) {
      /* Lost 是"状态暂不可用"，不是报警（需求 4.4 的避免事项） */
      ESP_LOGW(TAG, "全部设备失联（超时 %u ms）", (unsigned)this->lost_timeout_ms_);
    }
  }
  this->merged_ = st;
  this->merged_src_ = src;
  this->merged_score_ = pet_reg_merged_score(&this->reg_, src);

  /* 夜间 Active 响应节流（决策 D8：每分钟最多一次，避免狗翻身就亮） */
  if (st == PET_LAMP_ACTIVE && !this->night_.manual_mode) {
    if (!pet_night_allow_active_render(&this->night_, now)) {
      /* 保持上一次的渲染结果：不改 merged_，灯效层继续用旧值 */
      ESP_LOGV(TAG, "夜间 Active 被节流");
    }
  }
}

void PetStatusLamp::tick_effects_() {
  const uint32_t now = this->now_ms_();

  /* 唤醒判定：宠物状态发生显著变化时重置空闲推断（但**不**进入手动模式，
   * 否则狗一动就永久豁免、夜里会一直亮） */
  if (this->merged_ != PET_LAMP_LOST) {
    pet_night_on_state_change(&this->night_, now);
  }

  lamp_fx_input_t in;
  memset(&in, 0, sizeof(in));
  in.state = this->merged_;
  in.activity_score = this->merged_score_;
  /* 全局亮度上限（需求 7.3：滑条只影响上限，不改变状态颜色规则）。
   * 目前固定 100；后续接面板滑条事件时改这里即可。 */
  in.brightness_limit_pct = 100u;
  in.brightness_cap = pet_night_cap_for(&this->night_, this->merged_);
  in.manual_off = this->manual_off_;
  in.manual_mode = this->night_.manual_mode;
  in.low_battery = this->low_battery();
  in.lowbat_hint_allowed = !pet_night_suppress_lowbat(&this->night_);
  const uint64_t age = this->lost_age_ms();
  in.lost_age_ms = (age == UINT64_MAX) ? 0u : age;
  in.night_active = pet_night_is_night(&this->night_);
  in.manual = &this->manual_;

  /* 手动模式真正生效的条件：处于手动模式 **且** 手动灯是开的。
   * 手动灯关闭时回落到自动灯效，这样"熄灯"与"恢复自动显示"是两个独立概念
   * （需求 7.3：短按临时熄灯或恢复自动显示）。 */
  if (in.manual_mode && !this->manual_.on) {
    in.manual_mode = false;
  }

  lamp_effects_tick(&this->fx_, &in, now);
}

/* ------------------------------------------------------------------ */
/* core → 硬件                                                        */
/* ------------------------------------------------------------------ */

void PetStatusLamp::output_trampoline_(const lamp_color_t *color, float layer,
                                       uint16_t floor_duty, void *user) {
  auto *self = static_cast<PetStatusLamp *>(user);
  if (self == nullptr || color == nullptr) {
    return;
  }
  lamp_driver_set_layer(&self->driver_, color, layer, floor_duty);
}

/* ================================================================== */
/* 命令（网页 / YAML lambda 调用）                                     */
/* ================================================================== */

void PetStatusLamp::refresh_manual_flags_() {
  if (!this->manual_.on) {
    this->manual_off_ = true;
  } else {
    this->manual_off_ = false;
  }
}

void PetStatusLamp::cmd_set_power(bool on) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  this->manual_.on = on;
  this->refresh_manual_flags_();
  this->save_settings_();
  ESP_LOGI(TAG, "手动开关 -> %s", on ? "开" : "关");
}

void PetStatusLamp::cmd_set_brightness(uint8_t pct) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  if (pct < PET_BRIGHTNESS_MIN) pct = (uint8_t)PET_BRIGHTNESS_MIN;
  if (pct > PET_BRIGHTNESS_MAX) pct = (uint8_t)PET_BRIGHTNESS_MAX;
  this->manual_.brightness_pct = pct;
  this->save_settings_();
  ESP_LOGI(TAG, "手动亮度 -> %u%%", (unsigned)pct);
}

void PetStatusLamp::cmd_set_color_temp(uint32_t kelvin) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  if (kelvin < PET_COLOR_TEMP_MIN) kelvin = PET_COLOR_TEMP_MIN;
  if (kelvin > PET_COLOR_TEMP_MAX) kelvin = PET_COLOR_TEMP_MAX;
  this->manual_.color_temp_k = kelvin;
  this->manual_.mode = PET_LIGHT_MODE_DAY;
  this->save_settings_();
  ESP_LOGI(TAG, "手动色温 -> %u K", (unsigned)kelvin);
}

void PetStatusLamp::cmd_set_rgb(uint32_t rgb) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  if (rgb < PET_RGB_MIN || rgb > PET_RGB_MAX) {
    return;
  }
  this->manual_.rgb = rgb;
  this->manual_.mode = PET_LIGHT_MODE_COLOR;
  this->save_settings_();
  ESP_LOGI(TAG, "手动颜色 -> #%06X", (unsigned)rgb);
}

void PetStatusLamp::cmd_set_mode(uint8_t mode) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  if (mode != PET_LIGHT_MODE_COLOR && mode != PET_LIGHT_MODE_DAY) {
    return;
  }
  this->manual_.mode = mode;
  this->save_settings_();
}

void PetStatusLamp::cmd_set_night_enabled(bool on) {
  pet_night_on_user_action(&this->night_, this->now_ms_());
  this->night_.cfg.enabled = on;
  this->night_enabled_ = on;
  this->save_settings_();
  ESP_LOGI(TAG, "夜间模式 -> %s", on ? "开" : "关");
}

void PetStatusLamp::cmd_set_override_minutes(uint16_t minutes) {
  uint32_t ms = (uint32_t)minutes * 60000u;
  if (ms < MANUAL_OVERRIDE_MS_MIN) ms = MANUAL_OVERRIDE_MS_MIN;
  if (ms > MANUAL_OVERRIDE_MS_MAX) ms = MANUAL_OVERRIDE_MS_MAX;
  this->override_ms_ = ms;
  this->night_.cfg.override_ms = ms;
  /* 改完立即按新时长重算一次豁免窗口 */
  pet_night_on_user_action(&this->night_, this->now_ms_());
  this->save_settings_();
  ESP_LOGI(TAG, "手动豁免时长 -> %u 分钟", (unsigned)(ms / 60000u));
}

void PetStatusLamp::cmd_exit_manual() {
  pet_night_exit_manual(&this->night_, this->now_ms_());
  this->save_settings_();
  ESP_LOGI(TAG, "退出夜间手动豁免");
}

void PetStatusLamp::cmd_enter_pairing() {
  this->pairing_ = true;
  this->pairing_until_ = this->now_ms_() + this->pair_window_ms_;
  this->cmd_clear_bindings();
  ESP_LOGI(TAG, "进入配对模式 %u ms：等待合法 PAIR_REQUEST 广播",
           (unsigned)this->pair_window_ms_);
}

void PetStatusLamp::cmd_clear_bindings() {
  pet_reg_clear(&this->reg_);
  this->save_bindings_();
}

bool PetStatusLamp::cmd_set_max_devices(uint8_t n) {
  const uint8_t applied = pet_reg_set_cap(&this->reg_, n);
  this->max_devices_ = applied;
  this->save_bindings_();
  ESP_LOGI(TAG, "设备表容量 -> %u（当前绑定 %u）", (unsigned)applied,
           (unsigned)this->reg_.count);
  return applied == n;
}

bool PetStatusLamp::cmd_set_focus(int index) {
  const bool ok = pet_reg_set_focus(&this->reg_,
                                   (index < 0) ? PET_FOCUS_MERGE : (uint8_t)index);
  if (ok) {
    this->save_bindings_();
    ESP_LOGI(TAG, "焦点 -> %s", (index < 0) ? "合并显示" : "单只");
  }
  return ok;
}

bool PetStatusLamp::cmd_rename(uint32_t device_id, const std::string &name) {
  const bool ok = pet_reg_set_name(&this->reg_, device_id, name.c_str());
  if (ok) {
    this->save_bindings_();   /* 绑定点与名字共用一次持久化 */
    ESP_LOGI(TAG, "重命名 0x%08X -> %s", (unsigned)device_id,
             name.empty() ? "(默认名)" : name.c_str());
  }
  return ok;
}

bool PetStatusLamp::cmd_remove(uint32_t device_id) {
  const bool ok = pet_reg_remove(&this->reg_, device_id);
  if (ok) {
    this->save_bindings_();
    ESP_LOGI(TAG, "解绑 0x%08X（剩余 %u）", (unsigned)device_id,
             (unsigned)this->reg_.count);
  }
  return ok;
}

/* ================================================================== */
/* NVS 持久化                                                          */
/* ================================================================== */

void PetStatusLamp::load_prefs_() {
  this->pref_bindings_ = global_preferences->make_preference<PetBindingsBlob>(0x50455401u);
  this->pref_names_    = global_preferences->make_preference<PetNamesBlob>(0x50455402u);
  this->pref_settings_ = global_preferences->make_preference<PetSettingsBlob>(0x50455403u);

  PetBindingsBlob bind;
  memset(&bind, 0, sizeof(bind));
  if (this->pref_bindings_.load(&bind) && bind.magic == PET_NVS_MAGIC) {
    /* 运行期容量（决策 D6） */
    pet_reg_set_cap(&this->reg_, bind.max_devices);
    /* 绑定列表：有名字的先建出来，再套名字（顺序与下标由数组决定） */
    for (uint8_t i = 0; i < bind.count && i < PET_DEV_MAX_LIMIT; ++i) {
      if (bind.device_id[i] == 0u) {
        continue;
      }
      if (pet_reg_add(&this->reg_, bind.device_id[i], 0u) < 0) {
        break;
      }
    }
    pet_reg_set_focus(&this->reg_, bind.focus);
    ESP_LOGI(TAG, "NVS：载入 %u 只设备（容量 %u，焦点 %s）",
             (unsigned)this->reg_.count, (unsigned)this->reg_.cap,
             (bind.focus == PET_FOCUS_MERGE) ? "合并" : "单只");
  } else {
    ESP_LOGI(TAG, "NVS：无绑定记录，等待配对");
  }

  PetNamesBlob names;
  memset(&names, 0, sizeof(names));
  if (this->pref_names_.load(&names) && names.magic == PET_NVS_MAGIC) {
    for (uint8_t i = 0; i < names.count && i < PET_DEV_MAX_LIMIT; ++i) {
      const int8_t idx = pet_reg_find(&this->reg_, names.device_id[i]);
      if (idx < 0) {
        continue;
      }
      pet_reg_set_name(&this->reg_, names.device_id[i], names.name[i]);
    }
  }

  PetSettingsBlob set;
  memset(&set, 0, sizeof(set));
  if (this->pref_settings_.load(&set) && set.magic == PET_NVS_MAGIC) {
    if (set.lost_timeout_ms >= 10000u) this->lost_timeout_ms_ = set.lost_timeout_ms;
    if (set.override_ms >= MANUAL_OVERRIDE_MS_MIN) {
      this->override_ms_ = set.override_ms;
      this->night_.cfg.override_ms = set.override_ms;
    }
    if (set.night_idle_ms >= 10u * 60000u) {
      this->night_idle_ms_ = set.night_idle_ms;
      this->night_.cfg.idle_infer_ms = set.night_idle_ms;
    }
    if (set.night_throttle_ms >= NIGHT_ACTIVE_THROTTLE_MIN) {
      this->night_throttle_ms_ = set.night_throttle_ms;
      this->night_.cfg.active_throttle_ms = set.night_throttle_ms;
    }
    this->night_.cfg.enabled = set.night_enabled != 0;
    this->night_.cfg.start_hour = set.night_start;
    this->night_.cfg.end_hour = set.night_end;
    this->night_.cfg.cap_quiet = set.cap_quiet;
    this->night_.cfg.cap_light = set.cap_light;
    this->night_.cfg.cap_active = set.cap_active;
    this->night_.cfg.cap_lost = set.cap_lost;
    this->night_.cfg.suppress_lowbat_hint = set.night_suppress_lowbat != 0;

    this->night_enabled_ = set.night_enabled != 0;
    this->night_start_ = set.night_start;
    this->night_end_ = set.night_end;
    this->cap_quiet_ = set.cap_quiet;
    this->cap_light_ = set.cap_light;
    this->cap_active_ = set.cap_active;
    this->cap_lost_ = set.cap_lost;
    this->night_suppress_lowbat_ = set.night_suppress_lowbat != 0;

    this->manual_off_ = set.manual_off != 0;
    this->manual_.on = set.manual_on != 0;
    if (set.manual_brightness >= PET_BRIGHTNESS_MIN &&
        set.manual_brightness <= PET_BRIGHTNESS_MAX) {
      this->manual_.brightness_pct = set.manual_brightness;
    }
    if (set.manual_light_mode == PET_LIGHT_MODE_COLOR ||
        set.manual_light_mode == PET_LIGHT_MODE_DAY) {
      this->manual_.mode = set.manual_light_mode;
    }
    if (set.manual_color_temp >= PET_COLOR_TEMP_MIN &&
        set.manual_color_temp <= PET_COLOR_TEMP_MAX) {
      this->manual_.color_temp_k = set.manual_color_temp;
    }
    if (set.manual_rgb >= PET_RGB_MIN && set.manual_rgb <= PET_RGB_MAX) {
      this->manual_.rgb = set.manual_rgb;
    }
    ESP_LOGI(TAG, "NVS：载入设置（夜间 %s，豁免 %u ms）",
             this->night_.cfg.enabled ? "开" : "关",
             (unsigned)this->override_ms_);
  }
  this->prefs_ready_ = true;
}

void PetStatusLamp::save_bindings_() {
  if (!this->prefs_ready_) {
    return;
  }
  /* 绑定列表：仅绑定或设置改变时写入，不每包写 Flash（需求 6.3） */
  PetBindingsBlob bind;
  memset(&bind, 0, sizeof(bind));
  bind.magic = PET_NVS_MAGIC;
  bind.count = this->reg_.count;
  bind.focus = this->reg_.focus_index;
  bind.max_devices = this->reg_.cap;
  for (uint8_t i = 0; i < this->reg_.count && i < PET_DEV_MAX_LIMIT; ++i) {
    bind.device_id[i] = this->reg_.dev[i].device_id;
  }
  this->pref_bindings_.save(&bind);

  PetNamesBlob names;
  memset(&names, 0, sizeof(names));
  names.magic = PET_NVS_MAGIC;
  names.count = this->reg_.count;
  for (uint8_t i = 0; i < this->reg_.count && i < PET_DEV_MAX_LIMIT; ++i) {
    names.device_id[i] = this->reg_.dev[i].device_id;
    memcpy(names.name[i], this->reg_.dev[i].name, PET_DEV_NAME_LEN);
    names.name[i][PET_DEV_NAME_LEN - 1] = '\0';
  }
  this->pref_names_.save(&names);
}

void PetStatusLamp::save_settings_() {
  if (!this->prefs_ready_) {
    return;
  }
  PetSettingsBlob set;
  memset(&set, 0, sizeof(set));
  set.magic = PET_NVS_MAGIC;
  set.lost_timeout_ms = this->lost_timeout_ms_;
  set.override_ms = this->override_ms_;
  set.night_idle_ms = this->night_idle_ms_;
  set.night_throttle_ms = this->night_throttle_ms_;
  set.night_enabled = this->night_.cfg.enabled ? 1u : 0u;
  set.night_start = this->night_.cfg.start_hour;
  set.night_end = this->night_.cfg.end_hour;
  set.cap_quiet = this->night_.cfg.cap_quiet;
  set.cap_light = this->night_.cfg.cap_light;
  set.cap_active = this->night_.cfg.cap_active;
  set.cap_lost = this->night_.cfg.cap_lost;
  set.night_suppress_lowbat = this->night_.cfg.suppress_lowbat_hint ? 1u : 0u;
  set.manual_off = this->manual_off_ ? 1u : 0u;
  set.manual_on = this->manual_.on ? 1u : 0u;
  set.manual_brightness = this->manual_.brightness_pct;
  set.manual_light_mode = this->manual_.mode;
  set.manual_color_temp = this->manual_.color_temp_k;
  set.manual_rgb = this->manual_.rgb;
  this->pref_settings_.save(&set);
}

/* ================================================================== */
/* JSON（网页用；不引入任何 JSON 库，手工拼，字段少且可控）             */
/* ================================================================== */

static void json_escape_append(std::string *out, const char *s) {
  for (const char *p = s; p != nullptr && *p != '\0'; ++p) {
    const char c = *p;
    switch (c) {
      case '"':  out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        /* 只转义控制字符；UTF-8 多字节原样透传 */
        if ((unsigned char)c < 0x20u) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04X", (unsigned)(unsigned char)c);
          out->append(buf);
        } else {
          out->push_back(c);
        }
    }
  }
}

std::string PetStatusLamp::json_state() {
  const uint32_t now = this->now_ms_();
  const uint64_t age = this->lost_age_ms();
  char buf[256];
  std::string out;
  out.reserve(512);

  out.append("{");
  out.append("\"merged_state\":\"");
  out.append(this->merged_state_name());
  out.append("\",");
  snprintf(buf, sizeof(buf), "\"merged_state_id\":%u,", (unsigned)this->merged_);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"source_index\":%u,", (unsigned)this->merged_src_);
  out.append(buf);
  out.append("\"source_name\":\"");
  if (this->merged_src_ != PET_FOCUS_MERGE && this->merged_src_ < this->reg_.count) {
    char name[PET_DEV_NAME_LEN + 8];
    pet_reg_display_name(&this->reg_, this->merged_src_, name, sizeof(name));
    json_escape_append(&out, name);
  } else {
    out.append("merge");
  }
  out.append("\",");
  snprintf(buf, sizeof(buf), "\"activity_score\":%u,", (unsigned)this->merged_score_);
  out.append(buf);
  out.append("\"night_mode\":\"");
  out.append(this->night_mode_name());
  out.append("\",");
  snprintf(buf, sizeof(buf), "\"night_active\":%s,",
           pet_night_is_night(&this->night_) ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_enabled\":%s,",
           this->night_.cfg.enabled ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"manual_mode\":%s,",
           this->night_.manual_mode ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"override_remaining_ms\":%u,",
           (unsigned)pet_night_override_remaining_ms(&this->night_, now));
  out.append(buf);
  if (age == UINT64_MAX) {
    out.append("\"lost_age_ms\":null,");
  } else {
    snprintf(buf, sizeof(buf), "\"lost_age_ms\":%llu,",
             (unsigned long long)age);
    out.append(buf);
  }
  snprintf(buf, sizeof(buf), "\"lost_timeout_ms\":%u,", (unsigned)this->lost_timeout_ms_);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"pairing\":%s,", this->pairing_ ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"device_count\":%u,", (unsigned)this->reg_.count);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"max_devices\":%u,", (unsigned)this->reg_.cap);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"low_battery\":%s,", this->low_battery() ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"sensor_error\":%s,", this->sensor_error() ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"manual_off\":%s,", this->manual_off_ ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"brightness_pct\":%u,", (unsigned)this->manual_.brightness_pct);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"color_temp_k\":%u,", (unsigned)this->manual_.color_temp_k);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"rgb\":%u,", (unsigned)this->manual_.rgb);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"light_mode\":%u,", (unsigned)this->manual_.mode);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"manual_on\":%s", this->manual_.on ? "true" : "false");
  out.append(buf);
  out.append("}");
  return out;
}

std::string PetStatusLamp::json_devices() {
  const uint32_t now = this->now_ms_();
  char buf[512];
  std::string out;
  out.reserve(1024);

  out.append("[");
  for (uint8_t i = 0; i < this->reg_.count; ++i) {
    const pet_device_t *d = &this->reg_.dev[i];
    const bool alive = pet_reg_device_alive(d, now, this->lost_timeout_ms_);
    char name[PET_DEV_NAME_LEN + 8];
    pet_reg_display_name(&this->reg_, i, name, sizeof(name));

    if (i > 0u) {
      out.append(",");
    }
    out.append("{");
    snprintf(buf, sizeof(buf), "\"index\":%u,", (unsigned)i);
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"device_id\":%u,", (unsigned)d->device_id);
    out.append(buf);
    out.append("\"device_id_hex\":\"");
    snprintf(buf, sizeof(buf), "%08X", (unsigned)d->device_id);
    out.append(buf);
    out.append("\",");
    out.append("\"name\":\"");
    json_escape_append(&out, name);
    out.append("\",");
    out.append("\"name_custom\":");
    out.append(d->name[0] != '\0' ? "true" : "false");
    out.append(",");
    out.append("\"alive\":");
    out.append(alive ? "true" : "false");
    out.append(",");
    out.append("\"state\":\"");
    out.append(pet_state_name(d->activity_state));
    out.append("\",");
    snprintf(buf, sizeof(buf), "\"activity_score\":%u,", (unsigned)d->activity_score);
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"battery_percent\":%u,", (unsigned)d->battery_percent);
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"rssi\":%d,", (int)d->rssi);
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"last_seen_age_ms\":%u,",
             (unsigned)(now - d->last_seen_ms));
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"sequence\":%u,", (unsigned)d->last_sequence);
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"packets\":%u,", (unsigned)d->packets_total);
    out.append(buf);
    snprintf(buf, sizeof(buf), "\"flags\":%u,", (unsigned)d->flags);
    out.append(buf);
    /* 累计驻留时长（秒），网页展示"今天活跃了多久" */
    snprintf(buf, sizeof(buf), "\"active_s\":%u,\"light_s\":%u,\"quiet_s\":%u",
             (unsigned)(d->active_ms_total / 1000u),
             (unsigned)(d->light_ms_total / 1000u),
             (unsigned)(d->quiet_ms_total / 1000u));
    out.append(buf);
    out.append("}");
  }
  out.append("]");
  return out;
}

std::string PetStatusLamp::json_settings() {
  char buf[256];
  std::string out;
  out.reserve(512);
  out.append("{");
  snprintf(buf, sizeof(buf), "\"max_devices\":%u,", (unsigned)this->reg_.cap);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"focus_index\":%u,", (unsigned)this->reg_.focus_index);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"lost_timeout_ms\":%u,", (unsigned)this->lost_timeout_ms_);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"override_ms\":%u,", (unsigned)this->override_ms_);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_enabled\":%s,",
           this->night_.cfg.enabled ? "true" : "false");
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_start_hour\":%u,", (unsigned)this->night_.cfg.start_hour);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_end_hour\":%u,", (unsigned)this->night_.cfg.end_hour);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_idle_infer_ms\":%u,", (unsigned)this->night_.cfg.idle_infer_ms);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_throttle_ms\":%u,", (unsigned)this->night_.cfg.active_throttle_ms);
  out.append(buf);
  snprintf(buf, sizeof(buf),
           "\"night_caps\":{\"quiet\":%u,\"light\":%u,\"active\":%u,\"lost\":%u},",
           (unsigned)this->night_.cfg.cap_quiet, (unsigned)this->night_.cfg.cap_light,
           (unsigned)this->night_.cfg.cap_active, (unsigned)this->night_.cfg.cap_lost);
  out.append(buf);
  snprintf(buf, sizeof(buf), "\"night_suppress_lowbattery\":%s",
           this->night_.cfg.suppress_lowbat_hint ? "true" : "false");
  out.append(buf);
  out.append("}");
  return out;
}

}  // namespace esphome::pet_status_lamp
