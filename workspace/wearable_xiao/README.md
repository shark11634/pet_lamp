# 佩戴端固件 — Seeed XIAO nRF52840 (Sense)

> 依据《宠物活动状态灯（V1）工程需求与实施说明》3.1–3.8 节。
> 板载 **LSM6DS3TR-C** 六轴 IMU（Sense 版），只使用加速度三轴。

---

## 1. 目录结构

```
wearable_xiao/pet_status_broadcaster/
├── pet_status_broadcaster.ino          主程序（严格对应需求 3.7 伪代码）
├── pet_status_broadcaster_config.h     全部编译期配置（DeviceID / 广播间隔 / 阈值）
├── activity_classifier.h/.c            活动分类（需求 3.4）
├── imu_lsm6ds3.h/.cpp                  板载 LSM6DS3TR-C 采集
├── battery_monitor.h/.cpp              电压 / 电量 / 充电状态（需求 3.2）
└── ble_broadcaster.h/.cpp              Bluefruit 广播（需求 3.5 / 3.6）
```

> **协议实现不在这里**。S0 之后协议只有一份实现，位于 `workspace/core/`：
> `pet_protocol.{h,c}`、`pet_types.h`、`pet_config.h`。
> 佩戴端通过编译期 include 路径引用它们（见 §2.3），
> **不再有"同步副本"**，因此不存在两份实现漂移的风险。
>
> 离线造包/核对字段用 `workspace/tools/pet_tool.c` 构建出的 `pet_tool.exe`
> （`vectors` / `build` / `log` 三个子命令），工程内已无脚本语言工具。

---

## 2. 环境准备

### 2.1 板级包

Arduino IDE → 文件 → 首选项 → 附加开发板管理器网址，加入：

```
https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
```

然后 工具 → 开发板 → 开发板管理器，安装 **Seeed nRF52 Boards**
（提供 `Seeed XIAO nRF52840` 与 `Seeed XIAO nRF52840 Sense`；本工程用 **Sense**）。

> ⚠️ 不要装 "Seeed nRF52 mbed-enabled Boards"：
> mbed 版没有 Adafruit Bluefruit 库，本工程依赖 Bluefruit 做 Manufacturer Data 广播。

### 2.2 库

库管理器安装：

| 库 | 用途 | 是否必需 |
|---|---|---|
| **Adafruit Bluefruit nRF52 Libraries** | `Bluefruit.h`、BLEAdvertising | 必需 |
| **Seeed Arduino LSM6DS3** | 板载 IMU | 必需（或改用直读路径，见 §4） |

### 2.3 用 arduino-cli（推荐，可脚本化）

```powershell
# 一次性：注册板级包地址
arduino-cli config init --overwrite
arduino-cli config add board_manager.additional_urls https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:Seeed_nRF52

# 确认开发板 FQBN（不同版本板级包的 board id 可能不同，务必以本命令输出为准）
arduino-cli board listall | Select-String "XIAO nRF52840"

# 安装库
arduino-cli lib install "Adafruit Bluefruit nRF52 Libraries"
arduino-cli lib install "Seeed Arduino LSM6DS3"

# 编译（把 <FQBN> 换成上一命令查到的实际值，例如 Seeeduino:Seeed_nRF52:xiaonRF52840）
# 关键：把 core/ 加进 include 路径，让佩戴端直接用唯一那份协议实现
arduino-cli compile --fqbn <FQBN> `
  --build-property "compiler.c.extra_flags=-I..\..\core" `
  --build-property "compiler.cpp.extra_flags=-I..\..\core" `
  .\pet_status_broadcaster

# 烧录（XIAO 双击 RESET 进入 UF2 bootloader 后会出现串口/U盘）
arduino-cli upload --fqbn <FQBN> -p COM5 .\pet_status_broadcaster
```

Arduino IDE 里等价做法：**文件 → 首选项 → 更多编译器选项**，
在 `compiler.c.extra_flags` 与 `compiler.cpp.extra_flags` 里都加上
`-I<绝对路径>\workspace\core`。

> 注意：`core/pet_protocol.c` 需要参与编译。arduino-cli 不会自动编译
> sketch 目录外的 `.c` 文件，因此要么把 `core/pet_protocol.c` 复制到 sketch
> 目录（会重新引入"两份副本"问题，不推荐），要么在 sketch 里放一个
> 只做 `#include "../../core/pet_protocol.c"` 的转发文件。
> **推荐做法**：见仓库 `wearable_xiao/pet_status_broadcaster/pet_protocol_link.c`
> （S0 已提供，内容就是一行 include，不复制代码）。

---

## 3. 配置（`pet_status_broadcaster_config.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `DEVICE_ID` | `0x12345678` | **每个佩戴端必须唯一**。置 0 时自动从芯片 FICR 派生稳定值 |
| `ADV_INTERVAL_UNITS` | `1600` | 1600 × 0.625 ms = **1 s**（需求 3.5：V1 固定 1 秒） |
| `ADV_REDUNDANT_BURST` | `3` | 状态变化时立即补发的冗余广播数（1–3） |
| `LOW_BATTERY_THRESHOLD_PCT` | `20` | 低于此值置 `LOW_BATTERY`（需求 3.6 示例值） |
| `PAIR_REQUEST_ON_BOOT` | `1` | 上电 60 秒内持续置 `PAIR_REQUEST`，便于灯端首次绑定 |
| `PET_IMU_USE_LIBRARY` | `1` | 置 0 则不依赖任何 IMU 库，直接读寄存器 |
| `IMU_I2C_ADDRESS` | `0x6A` | LSM6DS3TR-C（SA0 接地） |
| `PET_BATTERY_ADC_PIN` | `31` | 电池分压 ADC 引脚（P0.31/AIN7）；按板级包实际编号调整 |

### 活动分类阈值（`activity_classifier.h`）

| 宏 | 默认 | 含义 |
|---|---|---|
| `CLS_BASELINE_ALPHA` | `0.02` | 重力基线低通系数（100 ms 采样） |
| `CLS_T_QUIET_MG` | `45` | 均值低于此值视为几乎不动 |
| `CLS_T_QUIET_PEAK_MG` | `120` | Quiet 允许的峰值上限 |
| `CLS_T_ACTIVE_MG` | `260` | 均值超过此值视为活跃 |
| `CLS_T_ACTIVE_PEAK_MG` | `700` | 峰值超过此值也可判活跃 |
| `CLS_T_ACTIVE_RATIO` | `0.25` | 超阈值样本比例阈值 |
| `CLS_WINDOW_SAMPLES` | `30` | 窗口 3 秒 @100 ms（需求 2–5 秒） |
| `CLS_QUIET_ENTER_MS` | `15000` | 连续 10–20 s 才进入 Quiet |
| `CLS_LIGHT_ENTER_MS` | `4000` | 连续 3–5 s 才进入 Light |
| `CLS_ACTIVE_ENTER_MS` | `2500` | 连续 2–3 s 即可进入 Active |
| `CLS_EXIT_FROM_ACTIVE_MS` | `6000` | 从 Active 降级用更长退出时间（防抖） |

> **这些阈值必须用真实宠物测试校准**（需求 3.4 明确要求）。
> V1 的目标是"观感上合理"，不是精确测量卡路里。

---

## 4. IMU 两条实现路径

| 路径 | 条件 | 说明 |
|---|---|---|
| 库路径（默认） | `PET_IMU_USE_LIBRARY 1` + `PET_USE_SEEED_LSM6DS3 1` | 用 `LSM6DS3Class`，量程 ±2 g、ODR 104 Hz |
| 库路径（回退） | `PET_USE_ADAFRUIT_LSM6DS 1` | 用 `Adafruit_LSM6DS3TRC`（输出 m/s²，代码内换算） |
| 直读路径 | `PET_IMU_USE_LIBRARY 0`，或库 `begin()` 失败时自动回退 | 直接读 `WHO_AM_I(0x0F)` / `CTRL1_XL(0x10)=0x60` / `OUTX_L_XL(0x28)` 共 6 字节 |

**量程 / 采样率 / 安装方向**（需求 10.1 要求交付）：

* 量程 **±2 g**（1 LSB ≈ 0.061 mg），宠物活动不会持续超过 2 g；
* IMU ODR **104 Hz**，上层 **100 ms** 轮询；
* 算法只用加速度向量模长 `|a| = sqrt(ax²+ay²+az²)`，**对安装方向不敏感**，
  V1 不要求标定方向。若后续要做姿态功能，需在佩戴端外壳标注 +X/+Y/+Z。

---

## 5. 运行日志判读

上电后串口（115200）会输出：

```
=== 宠物活动状态灯 V1 佩戴端 ===
pet-broadcaster/1.0.0
DeviceID = 0x12345678
[ OK] LSM6DS3TR-C 就绪：±2 g / 104 Hz
[ OK] 电池 100% / 4100 mV
[ OK] BLE 广播已启动，间隔 1000 ms
开始运行：每 100 ms 采样，每 1 s 广播一次心跳
[10s] state=Quiet score=3 mean=12.4mg peak=88.1mg ratio=0.00 bat=100% (4100mV) seq=9 adv=on flags=0x11
状态变化 -> Active
[20s] state=Active score=210 mean=402.3mg peak=1180.5mg ratio=0.53 bat=100% (4088mV) seq=19 adv=on flags=0x11
```

---

## 6. 验收对照（需求 3.8）

| 验收项 | 如何验证 |
|---|---|
| 静止时仍每 1–5 秒一包心跳 | 串口 `seq` 每秒 +1；nRF Connect 观察广播间隔 |
| 1–10 米室内可稳定收到 | 用灯端 `RSSI` / `有效包率` 日志（灯端 diagnostics 输出） |
| `Quiet→Light→Active` 更新 ≤ 5 秒 | 晃动开发板，观察串口 `状态变化 ->` 与灯端状态切换时延 |
| 停止广播后灯端进入 Lost | 给佩戴端断电，灯端 30–60 秒内进入 Lost |
| 电量变化正确显示 | 串口 `bat=` 与灯端 `battery` 字段一致 |
| 连续运行 ≥ 24 小时 | 挂机跑 `seq`，确认不死机、序号持续更新 |

---

## 7. 低功耗说明（V1 范围外）

V1 优先保证可靠性，**未做深度睡眠**：

* `loop()` 每 1 ms 轮询一次，CPU 常开；
* 广播由 SoftDevice 自动按 1 s 周期发送；
* 需求 3.5 已明确"电池优化放在后续阶段"。

后续优化方向（不在 V1 必做范围）：

* 安静状态把广播间隔放宽到 3–5 s（`ADV_INTERVAL_UNITS` 提升到 4800–8000）；
* 用 `suspendLoop()` + RTC 定时唤醒替代忙轮询；
* 用 `Bluefruit.autoConnLed(false)` 关掉连接指示灯。

---

## 8. 已知限制

1. **"状态变化立即广播"的语义**：Bluefruit 只暴露单个广播集，没有"立刻触发一次广播"
   的 API。实现方式是内容更新后 `stop()`/`start()`，SoftDevice 在下一次广播事件
   （≤1 s）发送新内容，再补发冗余广播。这会推迟该周期的自动广播。
   若要严格 1 s 周期 + 独立冗余广播，需改用 nRF5 SDK 多广播集或 NCS/Zephyr 扩展广播。
2. **DeviceID 写入流程**未实现 NVS 版本（需求 10.1 要求交付该流程）。
   V1 采用编译期常量，或用 `DEVICE_ID 0` 从 FICR 派生稳定值。
   后续应改为 FDS/NVS 存储 + 配对时写入。
3. **电池电压读取**依赖板级包宏或 `PET_BATTERY_ADC_PIN`；
   若板级包宏名不同，请按 §3 表格调整，或临时把 `battery_read_percent()` 固定为 100
   以先跑通 BLE 链路。
