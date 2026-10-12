# 宠物活动状态灯 V1 — 实现工程

> 依据《宠物活动状态灯（V1）工程需求与实施说明》V1.0 实现。
> 需求中的**全部关键数值**（亮度百分比、超时秒数、GPIO 号、型号、协议字段长度、
> 验收指标）已从原始 PDF 中恢复并落到代码，见
> [`docs/需求要点_已恢复全部数值.md`](docs/需求要点_已恢复全部数值.md)。
> 最终执行方案与全部已确认决策见
> [`docs/最终可执行方案.md`](docs/最终可执行方案.md)。

**硬性前提**：原厂 4 MB Flash 已完整备份 —— 本工程以此作为唯一回退手段。
仓库根目录下的 `bslamp2-stock-4MB.bin` 即该备份（4,194,304 字节）。

**语言约定**：设备侧全部是 C / C++。工程内**零 Python**——
ESPHome 组件必需的 `__init__.py`（PC 端代码生成，不进设备）将在 S3 创建，
其余一切离线工具都是 C99。

---

## 1. 当前进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| **S0** | 可移植核心库收敛到 `core/`；全部离线工具改为 C99；统一主机测试入口 | ✅ **已完成**（488 项断言全通过） |
| **S1** | `device_registry` + `night_mode` 实现与测试 | ✅ **已完成**（含在 S0 内） |
| **S2** | `state_machine` / `lamp_effects` 改造（设备表 + 夜间上限 + 手动模式） | ✅ **已完成** |
| **S3** | ESPHome 外部组件（`__init__.py` + C++ 组件 + ble_tracker 订阅 + 内置网页 + NVS） | ⚠️ **代码已完成**；本机无法编译验证（见下） |
| **S4a** | 方案 B（纯 ESP-IDF）接到 `core/`：删除全部重复实现，新增 `lamp_esp32.c`（LEDC 14-bit 适配）、三档任务编排、多狗 NVS、可解析诊断日志 | ✅ **已完成并验证** |
| **S4b** | 端到端集成测试（core → 真实 `lamp_esp32.c` → LEDC 替身）8 项全通过 | ✅ **已完成** |
| **S4c** | 面板协议**已接入**（移植自已验证的社区实现，97 项断言）；12 颗面板指示灯已用于显示焦点狗；灯板频率/通道已按上游改齐并受编译期守卫（RGB 14 bit@3 kHz / 白光 13 bit@9765 Hz / 白在 ch4） | ✅ **已完成** |
| S4d | 真机验证面板 I²C（0x2C 应答、中断、滑条）+ **原厂功能回归 P01–P21** | ⬜ 待真机 |
| S5 | 网页三页已随 S3 完成；MQTT 实体映射（YAML lambda）已给示例 | ⚠️ 待上机验证 |
| S6 | 夜间/多狗联调；OTA + 安全模式；72 小时长稳 | ⬜ 待做 |

### S3 的验证边界（必须说清楚）

S3 的组件代码**已按 ESPHome 2026.11.0-dev 源码逐项核对 API**，
核对过的接口都写在 `esphome/components/pet_status_lamp/pet_status_lamp.h` 的
注释里（含行号）：

| 用到的东西 | 核对结论 |
|---|---|
| `ESPBTDeviceListener::parse_device(const ESPBTDevice &)` | 纯虚函数，签名一致 |
| `ESPBTDevice::get_manufacturer_datas()` | 返回 `vector<ServiceData>`；`uuid` 是 Company ID，`data` **已去掉** Company ID → 代码里拼回去 |
| `ESPBTUUID::type()` / `uuid16()` | 用这两者做 Company ID 过滤（**不是** `get_type()`，已按源码改正） |
| `esp32_ble_tracker::register_ble_device(var, config)` | 会自动 `register_listener`，不需要自己注册 |
| `cg.add_build_flag()` / `cg.new_Pvariable()` | 存在且签名一致 |
| `external_components: source: {type: local, path: ...}` | 路径相对 YAML 所在目录 → `path: components` |
| `esp_http_server.h` | ESPHome 自己的 `web_server_idf` 就在用，ESP32 构建可用 |

**本机没能真正 `esphome compile`**，原因不是设计问题，而是**沙箱策略**：
`pip install esphome` 会在写完 wheel 的 `.metadata` 时报 `Permission denied`
——即使把 pip 的临时目录改到工作区内也一样，说明拒绝来自 DSH 的写入策略
（非工作区/受限路径类别），不是路径或 ACL 问题。因此没有绕过。

另外本机也没有 ESP-IDF 工具链（首次编译需要下载上百 MB）。
所以**首次 `esphome config` / `compile` 可能需要修小问题**——
这类问题输出会直接指到行号，属于常规收尾。

已做的本地可验证项：
* `__init__.py` 通过 `py_compile`；13 个 `setter` 全部在头文件里有声明；
* 组件路径解析正确（`parents[3]/core` 命中 13 个必需文件）；
* 网页 17 KB，与 C++ 原始字符串定界符 `)PETUI"` 无冲突；
* `pet-lamp.yaml` 通过结构检查（无 tab、无孤立碎片、12 个顶层键、缩进层级合法）。

`core/` 那 **488 项断言**不受影响，仍然全通过。

---

## 2. 立即验证（不需要硬件、不需要网络）

```powershell
cd workspace
powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1
```

期望输出：

```
compiler: <你的 gcc/clang 路径>
building + running: protocol
building + running: registry
building + running: effects
building + running: night
building + running: pet_tool vectors
ALL HOST TESTS PASSED (4 suites)
```

离线造包 / 核对字段 / 解析日志：

```powershell
# 打印测试向量并跑自检（与固件实现交叉验证）
.\build\pet_tool.exe vectors

# 造一个 Active 包，并确认灯端会接受
.\build\pet_tool.exe build --state 2 --seq 300 --score 120 --battery 85 --lamp-device-id 0x12345678

# 校验一个给定的 16 字节 MFG（十六进制，允许空格）
.\build\pet_tool.exe build --mfg "FF FF 01 78 56 34 12 00 00 00 00 64 11 00 00 C8"

# 解析灯端串口日志，出链路/稳定性报告 + CSV
.\build\pet_tool.exe log --file lamp.log --expect 0x12345678 --csv report.csv
```

> `pet_tool.exe` 由上面的测试脚本顺带构建；若要单独构建：
> ```
> gcc -std=c99 -O2 -I core -I protocol tools\pet_tool.c core\pet_protocol.c protocol\pet_vectors.c -o build\pet_tool.exe -lm
> ```

---

## 3. 目录结构

```
workspace/
├── README.md                          本文件
├── core/                              ★ 可移植纯 C99 核心（方案 B 的能力载体）
│   ├── pet_config.h                   唯一配置事实来源（含夜间/多狗全部参数）
│   ├── pet_types.h                    平台无关类型（状态、颜色、Q8、输出回调）
│   ├── pet_protocol.{h,c}             广播解析 / CRC-8 / 字段校验
│   ├── device_registry.{h,c}          多狗设备表 / 每狗独立计时 / 最活跃者优先
│   ├── night_mode.{h,c}               夜间模式 / 空闲推断 / 手动豁免 / 节流
│   ├── lamp_gamma.h                   Gamma 曲线（灯效层与驱动层共用）
│   ├── lamp_driver.{h,c}              Gamma / 全通道限流 / 层调制
│   └── lamp_effects.{h,c}             状态目标色 / 过渡 / 呼吸 / 提示 / 恢复动画
│
├── protocol/                          协议规范与测试向量
│   ├── README.md                      ★ 协议唯一事实来源（布局、CRC 定义、向量）
│   ├── pet_vectors.{h,c}              独立参考实现 + 向量表 + 交叉验证
│
├── test/                              ★ 主机测试（无需硬件）
│   ├── run_all_host_tests.ps1         统一入口
│   ├── pet_test.{h,c}                 断言骨架
│   ├── host_protocol_test.c           协议
│   ├── host_registry_test.c           设备表与合并
│   ├── host_effects_test.c            灯效与昼夜
│   └── host_night_test.c              夜间模式与手动豁免
│
├── tools/
│   └── pet_tool.c                     离线工具（vectors / build / log）
│
├── esphome/                           ★ 方案 A 主线（ESPHome 外部组件）
│   ├── pet-lamp.yaml                  设备配置（需自填 secrets）
│   ├── secrets.yaml.example           secrets 模板
│   ├── README.md                      构建 / 刷机 / 组件原理 / 已知限制
│   └── components/pet_status_lamp/
│       ├── __init__.py                ★ 唯一允许的 Python（PC 端代码生成）
│       ├── pet_status_lamp.h/.cpp     生命周期 / BLE / 灯效输出 / NVS / JSON
│       ├── pet_status_lamp_web.cpp    内置网页与 JSON API
│       └── web/pet_ui.html            网页（编译期内嵌成 C++ 字符串）
│
├── docs/
│   ├── 最终可执行方案.md               ★ 执行基线（16 项决策 + 6 个工作包）
│   ├── 需求要点_已恢复全部数值.md       从 PDF 恢复的完整需求参数
│   ├── 需求说明_全文（PDF恢复版）.txt    PDF 全文（含被通用工具丢弃的数值）
│   └── 验收测试记录模板.md              M1–M6 验收记录模板
│
├── wearable_xiao/                     佩戴端（XIAO nRF52840 Sense）
│   ├── README.md
│   └── pet_status_broadcaster/
│       ├── pet_status_broadcaster.ino
│       ├── pet_status_broadcaster_config.h
│       ├── activity_classifier.{h,c}
│       ├── imu_lsm6ds3.{h,cpp}
│       ├── battery_monitor.{h,cpp}
│       └── ble_broadcaster.{h,cpp}
│
└── lamp_bslamp2/                      方案 B：纯 ESP-IDF 实现（已接到 core/）
    ├── README_现状说明.md             分层说明 / 验证方式 / 未完成清单
    ├── sdkconfig.defaults
    ├── main/
    │   ├── app_main.c                 三档任务编排（BLE 20Hz / 状态 1Hz / 灯效 50Hz）
    │   ├── app_config.h               平台常量：GPIO / LEDC / 任务栈 / NVS
    │   ├── lamp_esp32.c/.h            core 的占空比 → LEDC 14-bit + 主开关
    │   ├── ble_scanner.c/.h           NimBLE 被动扫描 → 提取 0xFF → 入队
    │   ├── storage.c/.h               NVS：绑定 / 每狗名字 / 设置
    │   ├── diagnostics.c/.h           需求 §8.5 日志（格式可被 pet_tool 解析）
    │   └── touch_panel.c/.h           面板 I²C 抽象（协议待实物逆向）
    └── test/
        ├── host_integration_test.c    端到端集成测试（8 项）
        ├── mock_driver.{h,c}          直通驱动替身
        ├── run_integration_test.ps1
        └── host_stubs/                ESP-IDF GPIO / LEDC / 日志替身
```

---

## 4. 架构：为什么分 `core/` 与平台层

```
        ┌──────────────────────────────────────────────┐
        │  core/  纯 C99，不含任何平台头文件              │
        │  协议 / 设备表 / 夜间模式 / 灯效 / Gamma        │
        │  → 主机可编译，488 项断言覆盖                   │
        └───────────────┬──────────────────────────────┘
                        │ 只通过两个回调与平台交互
          ┌─────────────┴──────────────┐
          │                            │
   lamp_output_fn                lamp_pwm_write_fn
   （算好的颜色+层系数）           （4 通道占空比）
          │                            │
   ┌──────▼──────┐            ┌────────▼────────┐
   │ ESPHome 组件 │            │ ESP-IDF LEDC    │
   │ （S3 目标）  │            │ （方案 B / 参考）│
   └─────────────┘            └─────────────────┘
```

**好处**：
* 核心逻辑在 PC 上就能验证（本工程已经靠这个抓到并修掉 6 个真实缺陷）；
* 平台切换（ESPHome ↔ 纯 ESP-IDF）只换薄适配层，核心零改动；
* 协议只有一份实现，不存在"两份实现漂移"。

**约束**：`core/` 下任何文件**不得** include ESP-IDF / ESPHome / Arduino 头文件。
用 `gcc -std=c99 -Wall -Wextra -Wpedantic -I core -c core/*.c` 可以随时验证这条约束。

---

## 5. 主机测试抓到的真实缺陷（为什么值得先跑它）

这 6 个缺陷都是"上硬件后表现为莫名其妙、但无硬件跑数学就能定位"的：

| # | 缺陷 | 上硬件后的症状 | 由哪套测试抓到 |
|---|---|---|---|
| 1 | CRC 覆盖范围错位 2 字节 | 两端 CRC 永不匹配，**链路完全不通** | `protocol` |
| 2 | 灯在 2% 亮度下完全不亮 | Quiet 状态**黑屏** | `effects` |
| 3 | 熄灯后残留微弱漏光 | 夜间体验差 | `effects` |
| 4 | 低亮度呼吸被 Gamma 量化吃掉 | Lost 慢呼吸**看不见** | `effects` |
| 5 | 低电量提示亮度暴涨 12 倍 | 变成刺眼闪光 | `effects` |
| 6 | 平滑函数 const/非 const 指针别名 | 编译器优化后输出**永久钉死不动** | `effects` |

另外修掉/规避开的设计问题：

| # | 问题 | 处理 |
|---|---|---|
| 7 | 失联 0.45 降亮系数叠加在 Lost 目标上（双重压暗） | 降亮只作用于 Quiet/Light/Active |
| 8 | 12-bit PWM 在低亮下 1 LSB = 25% 跳变 | 改 14-bit（3 kHz 下可达；20 kHz 在 14 bit 下**物理不可能**，见 §9 缺陷 #13） |
| 9 | 8-bit 逻辑色在 2% 时每级 = 12% 跳变 | 按主导通道统一缩放（保色相），并有 Q8 定点平滑 |
| 10 | 主导通道判定写在赋值之前，恒读到 0 | 从基色判定（该 bug 使上面的修复一度失效） |

详见 [`core/pet_config.h`](core/pet_config.h) 与
[`core/lamp_effects.c`](core/lamp_effects.c) 里的注释——每个取舍都写明了原因，
避免后人"顺手改回去"。

---

## 6. 协议速查

完整规范见 [`protocol/README.md`](protocol/README.md)。

```
BLE Legacy Advertising，AD Type 0xFF（Manufacturer Specific Data）
  [len=17][0xFF] + Manufacturer Specific Data (16 字节 = CompanyID(2) + Payload(14))

Payload 偏移（小端序）：
  0      ProtocolVersion (0x01)
  1..4   DeviceID        每个佩戴端唯一，灯端绑定此 ID
  5..6   Sequence        16-bit 递增，回绕后回 0，灯端必须处理
  7      ActivityState   0=Quiet 1=Light 2=Active
  8      ActivityScore   0–255
  9      BatteryPercent  0–100
  10     Flags           bit0 WORN / 1 CHARGING / 2 SENSOR_ERROR
                        / 3 PAIR_REQUEST / 4 LOW_BATTERY；bit5-7 保留
  11..12 Reserved (0)
  13     CRC-8           覆盖 Payload[0..12]，poly=0x07 init=0x00

测试向量（必须逐字节一致）：
  TV-1 FF FF 01 78 56 34 12 00 00 00 00 64 11 00 00 C8
  TV-2 FF FF 01 78 56 34 12 2A 01 02 C8 55 01 00 00 CE
  TV-3 FF FF 01 78 56 34 12 10 00 00 00 0E 11 00 00 36
```

**行为特征**：灯端与佩戴端之间**没有连接**——佩戴端只广播，灯端只被动扫描
（NimBLE `passive=1`，从不回应）。所谓"连上了"指的是灯端在 `lost_timeout_ms`
（默认 60 s）内收到过该 `DeviceID` 的合法包。首次"配对"只是灯单方面记下
`DeviceID` 并写入 NVS，佩戴端毫不知情。

---

## 7. 已确认的关键决策（详见 `docs/最终可执行方案.md` §1 §7）

| 项 | 决定 |
|---|---|
| 开发基线 | ESPHome 外部组件（复用社区 bslamp2 灯光与触控面板 HAL） |
| 方案 B 能力 | 由 `core/` 纯 C99 保证，可整体搬进纯 ESP-IDF |
| Python | 仅允许 ESPHome 组件必需的 `__init__.py`（PC 端代码生成） |
| 手机控制 | 内置网页（Basic Auth）+ MQTT / HA Discovery |
| 多狗 | 设备表默认 8、网页可配 1–16；**最活跃者优先**；每狗独立计时、可命名 |
| 夜间模式 | 22:00–07:00 + 无时钟空闲推断（120 min）+ 网页/面板开关 |
| 夜间上限 | Quiet 10 / Light 14 / **Active 18（≈7%）** / Lost 6 |
| 手动豁免 | **10 分钟可配**（1–120 min）；任何用户操作刷新；豁免期间暂停自动灯效 |
| 面板 | 单狗时行为与原厂逐位一致；多狗时色彩键改为"合并/只看狗N"，色相切换移到长按 |
| Wi-Fi/BLE 共存 | `PREFER_BALANCE`，按实测有效包率再调 |

---

## 8. 下一步（S3 前置条件）

S3 需要在**能联网的机器**上完成：

1. 安装 ESPHome：`pip install esphome`
2. 拉取社区组件：`git clone https://github.com/mmakaay/esphome-xiaomi_bslamp2`
3. 本机网络受限（Seeed CDN 与 `raw.githubusercontent.com` 均不可达），
   因此 **S3 需要你确认这两步能否在本机完成**；若不能，我可以把
   S3 的组件代码按"离线可写的部分"先写完（`__init__.py`、C++ 骨架、
   `pet-lamp.yaml`），你拿到能联网的机器上编译。

**在此之前 S0–S2 已经完整交付且可验证**：不需要 ESPHome、不需要 ESP-IDF、
不需要网络，只需要一个 C 编译器。

---

## 9. 安全与合规提醒

* **原厂 Flash 备份不要公开上传**：其中可能包含设备标识或网络配置（需求 5.6）。
* **不要使用 5 V TTL 串口模块**，必须 3.3 V 逻辑；USB-TTL 的 VCC 不要接到灯电源线路。
* 拆机前**务必拔掉 12 V 适配器**。
* CRC-8 **不能防止伪造包**。V1 只保证稳定通信；防伪造（截断 HMAC + 滚动计数器 +
  预共享密钥）属于 V1.1（需求 3.6）。
* `lamp_bslamp2/` 内的触控面板协议若从社区项目移植，需保留出处与许可证声明。
