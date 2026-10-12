# 方案 A：ESPHome 外部组件（灯端主线）

> 本目录是 `docs/最终可执行方案.md` 里 **方案 A** 的落地位置：
> 以 ESPHome 为基线，复用社区 `xiaomi_bslamp2` 的灯板与触控面板 HAL，
> 把宠物状态灯做成一个外部组件。
>
> **业务逻辑不在这里** —— 全部在 `../../core/` 的纯 C99 实现里
> （协议、设备表、夜间模式、Gamma/限流/灯效），并且有 290 项主机断言覆盖。
> 本目录只做"BLE 取包 → 喂 core → 输出 PWM → 提供网页"这件薄胶水。

---

## 1. 目录结构

```
esphome/
├── pet-lamp.yaml                     设备配置（要你自己填 secrets）
├── secrets.yaml.example              secrets 模板
├── README.md                         本文件
├── components/pet_status_lamp/       外部组件
│   ├── __init__.py                   ★ 唯一允许的 Python（PC 端代码生成）
│   ├── pet_status_lamp.h             C++ 组件声明
│   ├── pet_status_lamp.cpp           生命周期 / BLE / 灯效输出 / NVS / JSON
│   ├── pet_status_lamp_web.cpp       内置网页与 JSON API
│   └── web/pet_ui.html               网页源文件（编译期内嵌成 C++ 字符串）
└── _ref/                             仅用于查阅上游 API 的临时克隆（可删）
```

---

## 2. 组件怎么工作（三层）

```
esp32_ble_tracker（ESPHome 内置，被动扫描 active:false）
        │  parse_device(ESPBTDevice&)  ← 只做"复制 + 入队"
        ▼
pet_status_lamp.cpp::drain_adv_()      ← 20 Hz
        │  拼回 Company ID，调 core 的 pet_parse_mfg_data()
        ▼
core/device_registry.c                 ← 每狗独立计时、最活跃者优先
core/night_mode.c                      ← 时间窗 / 空闲推断 / 手动豁免 / 节流
        │
        ▼
core/lamp_effects.c                    ← 状态色 / 过渡 / 呼吸 / 低亮提示
        │  lamp_output_fn(color, layer, floor)
        ▼
core/lamp_driver.c                     ← Gamma 2.2 + 全通道限流 + 层调制
        │  lamp_pwm_write_fn(duty[4])
        ▼
ESP32 LEDC → RGB(13/14/5) + 白光(12)
```

**为什么 `core/` 要单独抽出来**：它不含任何 ESP-IDF / ESPHome 头文件，
可以在 PC 上编译。本工程靠这一点在**无硬件**的情况下抓到并修掉了 10 个真实缺陷
（CRC 覆盖错位、2% 亮度黑屏、熄灯漏光、低亮度呼吸不可见、
低电量提示暴涨 12 倍、const 别名导致输出钉死……详见顶层 `README.md` §5）。

改行为的正确姿势：**改 `core/`，跑 `test/run_all_host_tests.ps1`**，不要改这里。

---

## 3. 构建与刷机

### 3.1 环境

```bash
# ESPHome（本工程在 2026.11.0-dev 上核对过 API；建议同代版本）
pip install esphome
esphome version

# 核对 ESPHome 的 Python 版本要求（新版要求 3.12+）
python --version
```

### 3.2 secrets

复制 `secrets.yaml.example` 为 `secrets.yaml`，填上：
Wi-Fi、API 加密密钥、OTA 密码、**宠物网页密码**。
`pet-lamp.yaml` 里的 `pet_web_password` 就是内置网页的 Basic Auth 密码，
**不要留默认值**——同 WiFi 下默认任何人都能打开这个网页。

生成 API key：`openssl rand -base64 32`

### 3.3 编译

```bash
cd workspace/esphome
esphome config pet-lamp.yaml     # 先校验配置与组件
esphome compile pet-lamp.yaml
```

首次编译会下载 ESP-IDF 工具链，几分钟到十几分钟。

### 3.4 刷机

**先确认原厂 4MB Flash 已备份到至少两个位置。**

```bash
# 首次：串口
esphome run pet-lamp.yaml --device COM3

# 之后：OTA（需要保留 ota: 段与安全模式）
esphome run pet-lamp.yaml
```

刷机与恢复原厂的完整流程（含 GPIO0 进下载模式、esptool 命令、
恢复 `bslamp2-stock-4MB.bin`）见顶层
`../docs/需求要点_已恢复全部数值.md` §5。

---

## 4. 灯板与触控面板（必须自己接一次）

`pet-lamp.yaml` 的「灯板与触控面板」段是**注释占位**，原因：
灯板/触控面板的配置键名属于社区组件 `xiaomi_bslamp2`，
我无法在本机联网核对该仓库的最终键名，**不臆造**。

你要做的：打开
<https://github.com/mmakaay/esphome-xiaomi_bslamp2> 的 `example.yaml`，
把它里面 `xiaomi_bslamp2:` 与 `light:` / `front_panel:` 段落整段搬过来。

**引脚取值不用查**，本工程已从需求文档 §4.2 恢复：

| 功能 | GPIO | 组件配置键（社区组件） |
|---|---|---|
| 红 / 绿 / 蓝 PWM | **13 / 14 / 5** | `red` / `green` / `blue` |
| 白光 PWM | **12** | `white` |
| 主开关 1 / 2 | **33 / 4** | `master1` / `master2` |
| 面板 I²C SDA / SCL | **21 / 19** | `i2c_sda` / `i2c_scl` |
| 面板中断 | **16** | `i2c_interrupt_pin` |

> ⚠️ 需求文档特别强调：**不要在未验证社区组件的前提下直接写最小 PWM 程序**，
> 灯板实际电路涉及主开关与初始化顺序。所以这里**不要**跳过社区组件自己接灯。

如果你确实不想用社区组件：`pet_status_lamp` 自带 `pet_status_lamp.cpp`
里的 `PetLampPwm`，会直接驱动 4 路 PWM 与两路主开关（默认引脚就是上表），
但**触控面板与 12 颗指示灯就没了** —— 那等于放弃"原厂用户可见功能等价"，
不建议。

---

## 5. 使用

### 5.1 首次绑定（需求 §7.2）

1. 打开内置网页 `http://<设备IP>:8080/`（或用 YAML 里映射的「进入配对模式」按钮）
2. 点 **进入配对模式** → 60 秒窗口
3. 让佩戴端上电（它在 `PAIR_REQUEST_ON_BOOT` 窗口内持续置 `PAIR_REQUEST` 位）
4. 灯识别到合法包后绑定并**暖白闪一下**确认；60 秒内可以绑多只
5. 之后只接受已绑定 `DeviceID` 的包

### 5.2 内置网页（档 1：App 控制）

三个页签：

| 页签 | 内容 |
|---|---|
| **宠物状态** | 合并状态、来源狗、活动分数、夜间模式、豁免剩余、距最后心跳；**狗狗列表**（名字可改、状态、分数、电量、信号、最近心跳、只看/解绑）；进入配对、合并显示、清空绑定 |
| **灯光控制** | 开关、模式（日光/彩光）、亮度 1–100、色温 1700–6500 K、颜色 — **属性模型与原厂米家一致** |
| **设置** | 夜间开关与时间窗、四档夜间上限、手动豁免时长（1–120 分钟）、夜间节流、失联超时、设备表容量（1–16） |

所有设置写入 NVS，断电保留。所有用户操作都会进入"手动模式"
（暂停自动灯效、夜间压制不生效），无操作满豁免时长后自动回落。

### 5.3 MQTT / Home Assistant（档 2）

`pet-lamp.yaml` 末尾有一段**实体映射示例**（`text_sensor` / `sensor` /
`binary_sensor` / `button`），用 `lambda:` 读本组件的 getter。
解除注释即可；配合 ESPHome 的 `mqtt:` 段，HA 会自动发现这些实体。

**为什么不在 C++ 里直接创建实体**：ESPHome 各版本的实体基类头文件路径会变，
硬编码会带来版本脆性，而我们无法在本机编译验证。用 lambda 映射既能拿到
HA/MQTT 的完整视图，又让实体定义留在你手里。

---

## 6. 与需求/决策的对应

| 需求/决策 | 实现位置 |
|---|---|
| §4.1 被动扫描、不连接 | `esp32_ble_tracker` `active:false` + `parse_device` 只入队 |
| §4.6 六层校验、序号回绕/重启重同步 | `core/pet_protocol.c` + `core/device_registry.c` |
| §4.3 失联分级 0/10/30/60 s | `core/state_machine` 的等价逻辑 `pet_reg_merge` + `night_mode` |
| §4.4 状态灯效区间 | `core/lamp_effects.c` |
| §4.5 Gamma 2.2 / 最小稳定亮度 / 全通道限流 | `core/lamp_driver.c` |
| §6.3 任务频率（20–100 Hz 灯效、1 Hz 状态、10–60 s 日志） | `loop()` 三档节奏 + `TASK_*` 常量 |
| §7.2 配对流程（长按 5 s、60 s 窗口、白闪确认） | `cmd_enter_pairing` / `bind_new_device_` |
| §7.3 手动优先级（短按熄灯、滑条亮度上限、自动优先被手动覆盖） | `cmd_set_power` / `night_mode` 的手动模式 |
| §8.5 可观测性日志字段 | `loop()` 的 30 秒诊断行 |
| D6 设备表 8/可配 1–16 | `max_devices` + `cmd_set_max_devices` + NVS |
| D7 最活跃者优先 | `pet_reg_merge` |
| D8/D14 夜间模式与 Active 上限 | `core/night_mode.c` |
| D9/D10 手动豁免 10 分钟可配、暂停自动灯效 | `core/night_mode.c` |
| D12 网页 Basic Auth | `pet_status_lamp_web.cpp` |
| D13 默认名 Pet-N | `pet_reg_display_name` |
| D16 Wi-Fi/BLE 共存 PREFER_BALANCE | `pet-lamp.yaml` 的 `platformio_options` |

---

## 7. 已知限制与待办

| 项 | 说明 |
|---|---|
| **未在本机编译验证** | 本机网络能装 ESPHome（正在安装中）但 `github.com` 的 git 传输不稳定，且缺少 ESP-IDF 工具链。组件按 ESPHome **2026.11.0-dev 源码**逐项核对过 API（`ESPBTDeviceListener`、`ESPBTDevice::get_manufacturer_datas`、`register_ble_device`、`add_build_flag`、`external_components` 的 `path:` 机制），但**首次 `esphome compile` 可能需要修小问题**。 |
| **PWM 分辨率** | `PetLampPwm` 用 Arduino 的 `analogWrite`（8 bit），缩放的边界保护在 `core/lamp_duty_to_bits()` 里且有主机测试。但**实测 8 bit 会把 Lost 的低亮呼吸波形压成一个常数**：14 bit 下 Lost 谷底占空比 `[4, 1, 9, 0]`，换算到 8 bit 后变成 `[1, 1, 1, 0]` —— 调制幅度几乎全丢。所以 **S4 第一件事就是把 `analogWrite` 换成直接调 LEDC 设 14 bit**（ESP-IDF framework 下可直接用 `driver/ledc.h`，代码示例已写在 `pet_status_lamp.cpp` 的 `write_duty()` 注释里）。 |
| **触控面板事件未接** | 社区组件提供的电源键/色彩键/滑条还没接到 `pet_status_lamp`。S4 要做的：短按→熄灯、长按→配对、色彩键→（单狗）色相/（多狗）切换焦点、滑条→全局亮度上限。 |
| **12 颗面板指示灯未用** | 计划用于显示"当前显示第几只狗"，S4 接。 |
| **Wi-Fi/BLE 共存掉包** | ESP32 单射频，必须实测有效包率（需求 §8.3）。若掉包明显，把 `PREFER_BALANCE` 改成 `PREFER_BLE`（YAML 里改一个数字）。 |
| **默认网页密码** | `pet_web_password` 必须改成自己的，否则同 WiFi 下谁都能控制灯。 |
| **`_ref/` 目录** | 我为了核对 API 克隆的上游仓库，可以整个删掉。 |

---

## 8. 下一步（S4）

1. 把 `PetLampPwm` 从 `analogWrite` 换成直接 LEDC（14 bit，20 kHz）
2. 接入社区组件的 `front_panel` 事件（短按/长按/色彩键/滑条）
3. 用 12 颗面板指示灯显示当前来源狗序号
4. **跑一遍原厂功能回归清单**（`../docs/最终可执行方案.md` §9 的 P01–P21），
   把结果填进 `../docs/验收测试记录模板.md`
5. 实测 Wi-Fi/BLE 共存下的有效包率，决定 coex 偏好
