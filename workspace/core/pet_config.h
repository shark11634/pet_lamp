/*
 * pet_config.h — 宠物活动状态灯 V1：**平台无关**的集中配置
 *
 * 本文件是唯一的配置事实来源。平台特有的配置（GPIO 号、PWM 频率、
 * ESPHome YAML）放在各自的平台层，不要写进这里。
 *
 * 所有取值来自：
 *   - 《宠物活动状态灯（V1）工程需求与实施说明》V1.0（数值恢复稿见
 *     docs/需求要点_已恢复全部数值.md）
 *   - 已确认决策 D1–D16 / O1–O6（见 docs/最终可执行方案.md §1 §7）
 *
 * ⚠️ 本文件不得 include 任何平台头文件。
 */
#ifndef PET_CONFIG_H
#define PET_CONFIG_H

/* ================================================================== */
/* 1. 协议常量（与 protocol/README.md 必须完全一致）                    */
/* ================================================================== */

/** 开发阶段占位 Company ID；量产前申请或替换为自有分配值 */
#define PET_COMPANY_ID            0xFFFFu
/** V1 协议版本 */
#define PET_PROTOCOL_VERSION      0x01u
/** 活动状态变化时佩戴端补发的冗余广播数（1–3） */
#define PET_REDUNDANT_ADV_COUNT   3u
/** 低电量阈值（百分比），与 Flags.LOW_BATTERY 联动 */
#define PET_LOW_BATTERY_THRESHOLD 20u

/** Flags 位（协议 §3） */
#define PET_FLAG_WORN         (1u << 0)
#define PET_FLAG_CHARGING     (1u << 1)
#define PET_FLAG_SENSOR_ERROR (1u << 2)
#define PET_FLAG_PAIR_REQUEST (1u << 3)
#define PET_FLAG_LOW_BATTERY  (1u << 4)

/* ================================================================== */
/* 2. 失联判断（需求 4.3）                                             */
/* ================================================================== */

/** 0–10 s：保留最后一个状态，容忍短暂丢包 */
#define LOST_HOLD_MS              10000u
/** 10–30 s：可逐步降低当前状态亮度，不立即报警 */
#define LOST_DIM_START_MS         10000u
#define LOST_DIM_END_MS           30000u
/** 30–60 s 区间的下界（进入 Lost 的过渡窗口起点） */
#define LOST_ENTER_MS             30000u
/** V1 默认 lost_timeout_ms = 60,000 */
#define LOST_TIMEOUT_MS           60000u
/** 超过 5 分钟：保持 1%–3% Lost 灯效，或进入熄灯 */
#define LOST_DEEP_MS             (5u * 60u * 1000u)
#define LOST_DEEP_BRIGHTNESS_PCT   2u
/** 上电暖白提示时长（需求 4.3 BOOT） */
#define BOOT_EFFECT_MS            1000u
/** 降亮系数：10–30 s 区间末端保留的比例 */
#define LOST_DIM_END_FACTOR       0.45f

/* ================================================================== */
/* 3. 灯效参数（需求 4.4）                                             */
/* ================================================================== */

/* 状态亮度区间（百分比） */
#define FX_QUIET_BRIGHTNESS_MIN    2u
#define FX_QUIET_BRIGHTNESS_PCT    8u
#define FX_LIGHT_BRIGHTNESS_MIN    8u
#define FX_LIGHT_BRIGHTNESS_PCT   20u
#define FX_ACTIVE_BRIGHTNESS_MIN  20u
#define FX_ACTIVE_BRIGHTNESS_PCT  40u
#define FX_LOST_BRIGHTNESS_MIN     2u
#define FX_LOST_BRIGHTNESS_PCT     6u
#define FX_BOOT_BRIGHTNESS_MIN     2u
#define FX_BOOT_BRIGHTNESS_PCT     6u

/* 过渡时间（毫秒） */
#define FX_QUIET_TRANSITION_MS    6000u   /* 4–8 s */
#define FX_LIGHT_TRANSITION_MS    3000u   /* 2–4 s */
#define FX_ACTIVE_TRANSITION_MS   1400u   /* 0.8–2 s */
#define FX_LOST_TRANSITION_MS     4000u   /* 3–5 s */
#define FX_LOWBAT_TRANSITION_MS    750u   /* 0.5–1 s */
#define FX_BOOT_TRANSITION_MS     1000u   /* 约 1 秒柔和暖白渐亮 */

/* 呼吸 / 浮动周期 */
#define FX_QUIET_BREATH_MS       12000u   /* 常亮或约 12 秒极慢呼吸 */
#define FX_LIGHT_FLOAT_MS         8000u   /* 缓慢浮动，可选 */
#define FX_ACTIVE_FLOAT_MS        2000u   /* 仅柔和变化，禁止频闪 */
#define FX_LOST_BREATH_MIN_MS    20000u   /* 每 20–60 秒一次慢呼吸 */
#define FX_LOST_BREATH_MAX_MS    60000u

/* 呼吸幅度（相对当前输出的比例，正负对称） */
#define FX_QUIET_BREATH_AMP       0.06f
#define FX_LIGHT_BREATH_AMP       0.08f
#define FX_ACTIVE_BREATH_AMP      0.04f
#define FX_LOST_BREATH_AMP        0.175f

/* 低电量提示（需求 4.4：每 5–10 分钟一次低亮黄白提示） */
#define FX_LOWBAT_HINT_MIN_MS    (5u * 60u * 1000u)
#define FX_LOWBAT_HINT_MAX_MS   (10u * 60u * 1000u)
#define FX_LOWBAT_HINT_DURATION_MS  1500u
/** 提示叠加权重 */
#define FX_LOWBAT_HINT_BLEND       0.35f
/** 提示亮度上限（逻辑 0–255）；防止在 Quiet 上暴涨成闪光 */
#define FX_LOWBAT_HINT_MAX_LEVEL   36u

/* 恢复连接确认动画 */
#define FX_RECOVER_EXTRA_MS        600u
#define FX_RECOVER_PEAK            0.60f

/* 不做 3 Hz 以上的可见闪烁（需求 4.4） */
#define FX_MAX_FLICKER_HZ          3u

/* ================================================================== */
/* 4. 调光与限流（需求 4.5）                                           */
/* ================================================================== */

/** Gamma 初始建议值 gamma = 2.2 */
#define LAMP_GAMMA                 2.2f
/** 全通道限流系数（0.0–1.0） */
#define LAMP_GLOBAL_CURRENT_LIMIT  0.85f
/** 单通道最大系数：RGB 与白光可分别采用不同系数 */
#define LAMP_RGB_CHANNEL_MAX       1.00f
#define LAMP_WHITE_CHANNEL_MAX     0.90f
/**
 * 最小稳定亮度（逻辑 0–255 的下限）。低亮不稳定时提高此值，
 * 而不是无限压低 PWM（需求 4.5）。
 */
#define LAMP_MIN_STABLE_LEVEL      4u

/**
 * 目标色的最小"主导通道占空比档位"。
 *
 * `lamp_effects_target_color()` 把 8-bit 基色按亮度百分比缩放。在
 * Quiet/Lost 的 2%–6% 区间，缩放因子只有 5 左右，通道值被截断成个位数，
 * 每个 1 单位的量化步就相当于该状态亮度的 ~12% —— 慢呼吸会退化成一格
 * 一格跳（实测峰谷比 1.00）。
 *
 * 这里**不**抬每个通道的色阶（那会破坏色相），而是要求**主导通道**在
 * Gamma 之后至少有这么多个占空比档位；不足时把整个颜色按同一系数放大，
 * 色相不变，只把极暗状态下限抬到能平滑呼吸的水平。
 *
 * 代价：极暗状态的实际亮度会比"纯 2%"略亮。这是需求 4.5 明确的取舍
 * （"提高最小稳定亮度，而不是无限压低 PWM"）。
 */
#define LAMP_MIN_DUTY_LEVELS       16u

/**
 * PWM 分辨率与满量程（平台层必须与此一致）。
 *
 * 用 14 bit（16383）而不是 12 bit（4095）：Quiet/Lost 的逻辑亮度只有
 * 2%–6%，Gamma 2.2 之后占空比只有个位数～几十 LSB。12 bit 时 1 LSB 相当于
 * 该亮度的 ~25% 相对跳变，低亮慢呼吸会退化成一格一格地跳
 * （实测峰谷比 1.00）。
 * 14 bit 在 20 kHz 载波下对应分频系数 80/2^14 = 0.00488，ESP32 LEDC 可精确
 * 实现（这也是 20 kHz 下的可用上限）。若平台不支持，需同步下调并接受
 * 低亮呼吸变粗。
 */
#define LAMP_PWM_RES_BITS          14u
#define LAMP_PWM_MAX_DUTY          ((1u << LAMP_PWM_RES_BITS) - 1u)

/**
 * PWM 载波频率。灯效插值 20–100 Hz，载波远高于此以避免可见闪烁，
 * 同时避免可听噪声（>20 kHz）。
 */
#define LAMP_PWM_FREQ_HZ           20000u

/**
 * 呼吸调制的基准（相对该状态的目标亮度）。
 * 结果层系数 = 1 + breath，其中 breath ∈ [-amp, +amp]，
 * 经正弦中间层映射后保证 layer ∈ [FLOOR, 1]：
 *   Quiet/Light/Active 用 0.85（轻微起伏，永不熄灭）
 *   Lost 用 0.55（"慢呼吸"的明暗对比更明显，但谷底不熄灯）
 */
#define FX_BREATH_FLOOR_DEFAULT    0.85f
#define FX_BREATH_FLOOR_LOST       0.55f

/* ================================================================== */
/* 5. 多宠物 / 设备表（决策 D6、D7；需求 1.3 协议预留扩展空间）         */
/* ================================================================== */

/** 编译期硬上限（slot 数组大小）。网页运行期上限不得超过它。 */
#define PET_DEV_MAX_LIMIT          16u
/** 运行期默认容量（决策 D6：默认 8，网页可配 1–16） */
#define PET_DEV_MAX_DEFAULT        8u
/** 设备名字节数（含结尾 '\0'） */
#define PET_DEV_NAME_LEN           17u

/* ================================================================== */
/* 6. 夜间模式（决策 D8、D14；需求 4.4「不在夜间把 Active 设为高亮」）   */
/* ================================================================== */

#define NIGHT_MODE_ENABLE_DEFAULT  1        /* 编译期默认是否开 */
#define NIGHT_START_HOUR           22       /* 时间窗起点 */
#define NIGHT_END_HOUR             7        /* 时间窗终点 */

/**
 * 无时钟时的退化判据：连续 N 毫秒无任何用户交互且无宠物状态变化，
 * 认为进入"夜间"。默认 120 分钟。
 */
#define NIGHT_IDLE_INFER_MS        (120u * 60u * 1000u)

/** 夜间各状态的亮度上限（逻辑 0–255；决策 D14 定 Active = 18 ≈ 7%） */
#define NIGHT_CAP_QUIET            10u   /* ≈ 4%   */
#define NIGHT_CAP_LIGHT            14u   /* ≈ 5.5% */
#define NIGHT_CAP_ACTIVE           18u   /* ≈ 7%   */
#define NIGHT_CAP_LOST              6u   /* ≈ 2.4% */

/** 夜间 Active 响应节流：每 N 毫秒最多渲染一次状态变化 */
#define NIGHT_ACTIVE_THROTTLE_MS   60000u
#define NIGHT_ACTIVE_THROTTLE_MIN  10000u
#define NIGHT_ACTIVE_THROTTLE_MAX 600000u

/** 夜间是否抑制低电量提示闪烁 */
#define NIGHT_SUPPRESS_LOWBAT_HINT 1

/** 夜间 Lost 呼吸周期放大倍数（20–60 s → 拉到上限附近） */
#define NIGHT_LOST_BREATH_SCALE    3u

/* ================================================================== */
/* 7. 手动豁免（决策 D9、D10）                                         */
/* ================================================================== */

/** 默认豁免时长 10 分钟（网页可配 1–120 分钟，决策 D9） */
#define MANUAL_OVERRIDE_MS_DEFAULT (10u * 60u * 1000u)
#define MANUAL_OVERRIDE_MS_MIN     (1u  * 60u * 1000u)
#define MANUAL_OVERRIDE_MS_MAX     (120u * 60u * 1000u)

/** 短按电源键的"轻触"判据（超过则视为长按配对） */
#define PANEL_SHORT_PRESS_MAX_MS    800u
/** 长按电源键进入配对模式的时间（需求 7.2：约 5 秒） */
#define PAIR_HOLD_MS               5000u
/** 配对模式持续时间（需求 7.2：60 秒） */
#define PAIR_MODE_DURATION_MS     60000u
/** 绑定确认白闪时长 */
#define PAIR_CONFIRM_MS            2000u

/** 手动熄灯渐隐时长 */
#define MANUAL_OFF_FADE_MS          800u

/**
 * 豁免期间是否仍用极暗色温偏移提示宠物状态变化。
 * 决策 O6：默认关闭（"不打扰"优先）。
 */
#define MANUAL_KEEP_AUTO_HINT      0

/* ================================================================== */
/* 8. 运行频率（需求 6.3）                                             */
/* ================================================================== */

/** 灯效插值 / PWM 更新 20–100 Hz */
#define TASK_FX_INTERVAL_MS          20u     /* 50 Hz */
/** 失联检查 1 Hz */
#define TASK_STATE_INTERVAL_MS     1000u
/** BLE 取包轮询 */
#define TASK_RX_INTERVAL_MS          50u     /* 20 Hz */
/** 日志汇总 10–60 秒一次，避免过多 UART 输出 */
#define TASK_DIAG_INTERVAL_MS     30000u
/** 串口波特率 */
#define DIAG_UART_BAUD           115200

/* ================================================================== */
/* 9. 序列号重新同步（需求 4.6）                                       */
/* ================================================================== */

/** 连续 N 个合法包后放宽一次序号判断（佩戴端重启后重新同步） */
#define SEQ_RESYNC_PACKETS            3u

/* ================================================================== */
/* 10. 原厂属性范围（决策 D4/D5；来源 MIoT Spec yeelink.light.bslamp2） */
/* ================================================================== */

#define PET_BRIGHTNESS_MIN           1u
#define PET_BRIGHTNESS_MAX         100u
#define PET_COLOR_TEMP_MIN        1700u
#define PET_COLOR_TEMP_MAX        6500u
#define PET_RGB_MIN                  1u
#define PET_RGB_MAX           16777215u
#define PET_LIGHT_MODE_COLOR         1u
#define PET_LIGHT_MODE_DAY           2u

#endif /* PET_CONFIG_H */
