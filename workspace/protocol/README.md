# 宠物活动状态灯 V1 — BLE 广播协议规范

> 依据《宠物活动状态灯（V1）工程需求与实施说明》V1.0 第 3.6 / 3.7 / 4.6 节整理。
> 本文件是**佩戴端固件**与**灯端固件**共用的唯一事实来源（single source of truth）。
> 任何字段变更必须同时改 `core/pet_protocol.h`、`core/pet_protocol.c`
> 与本节测试向量（`protocol/pet_vectors.c`）。

---

## 1. 传输方式

| 项目 | 取值 |
|---|---|
| 广播类型 | BLE Legacy Advertising，**可连接**（可连接便于后续维护，不影响被动扫描） |
| 广播数据位置 | `AD Type = 0xFF`（Manufacturer Specific Data） |
| Company ID | `0xFFFF`（开发阶段占位；量产前申请或替换为自有分配值） |
| 总长度 | 2 (Company ID) + 14 (Payload) = **16 字节** |
| 广播间隔 | V1 固定策略：**1 s**；安静优化策略：3–5 s；Light/Active：0.5–1 s |
| 状态变化 | 立即额外发送 **1–3** 个冗余广播 |
| 低电量 | **仍保持心跳广播，不可直接停发** |
| 连接 | 不需要 GATT 长连接，无配对、无手机参与 |
| 地址类型 | 建议使用**静态随机地址**（不依赖 MAC 做绑定，仅用协议内 DeviceID） |

> ⚠️ 长度核算：Legacy 广播 PDU 的 AD 数据上限为 31 字节。
> `0xFF` 头 2 字节 + Company ID 2 字节 + Payload 14 字节 = 18 字节，
> 加上 Flags（3 字节）与 AD 长度字节共 22 字节，**仍在 31 字节内**，无需 Extended Advertising。

---

## 2. Payload 布局（14 字节，小端序）

> **先把长度口径说清楚**（原始需求文档此处表述不自洽，本工程以此处为准）：
>
> | 名称 | 长度 | 说明 |
> |---|---|---|
> | **AD 结构** | `1 + 1 + 16 = 18` 字节 | `[len=17][0xFF][MFG(16)]` |
> | **Manufacturer Specific Data (MFG)** | **16 字节** | `CompanyID(2) + Payload(14)` |
> | **Payload** | **14 字节** | 下表按 **Payload 偏移** 编号 |
>
> 需求文档写"总长度 14 bytes"，同时把 CompanyID 放在"偏移 0–1"，
> 两者口径不同（14 是 Payload 长度，含 CompanyID 的 MFG 为 16 字节）。
> 本工程固定采用 **MFG = 16 字节**，灯端与佩戴端两端一致。

### 2.1 Payload 字段（偏移相对 Payload 起始）

| Payload 偏移 | MFG 偏移 | 字段 | 长度 | 示例 | 说明 |
|---|---|---|---|---|---|
| 0 | 2 | `ProtocolVersion` | 1 | `0x01` | 协议升级兼容；灯端不支持的版本直接丢弃 |
| 1–4 | 3–6 | `DeviceID` | 4 | `0x12345678` | **每个佩戴端唯一**；灯绑定此 ID |
| 5–6 | 7–8 | `Sequence` | 2 | 递增 | 16-bit 无符号递增，最大值后回绕到 0；用于去重、防重放、发现卡死 |
| 7 | 9 | `ActivityState` | 1 | `0/1/2` | `0=Quiet`，`1=Light`，`2=Active` |
| 8 | 10 | `ActivityScore` | 1 | `0–255` | 细粒度活动强度 |
| 9 | 11 | `BatteryPercent` | 1 | `0–100` | 电量百分比 |
| 10 | 12 | `Flags` | 1 | bitfield | 见 §3 |
| 11–12 | 13–14 | `Reserved` | 2 | `0` | 保留，必须写 0 |
| 13 | 15 | `CRC-8` | 1 | 计算值 | **仅检测数据损坏/误解析，不是安全认证** |

### 2.2 AD 结构中的 Company ID

| MFG 偏移 | 字段 | 长度 | 示例 |
|---|---|---|---|
| 0–1 | `CompanyID` | 2 | `0xFFFF`（开发阶段占位；量产前申请或替换为自有分配值） |

> ⚠️ AD Type 为 `0xFF` 时，前 2 字节按 Bluetooth 规范就是 Company ID。
> 需求文档表格把 `CompanyID` 标为"偏移 0–1"也正是这个含义。

### 字段一致性校验（灯端必须全部通过才接受）

1. AD 结构遍历后能找到 `0xFF`（Manufacturer Specific Data）；
2. `CompanyID == 0xFFFF`；
3. 总长度 == `2 + 14 = 16`；
4. `ProtocolVersion == 0x01`；
5. `CRC-8` 匹配（覆盖范围见 §4）；
6. `DeviceID == bound_device_id`；
7. `Sequence` 比 `last_sequence` 更新（或满足 §5 的重启重新同步规则）。

任一条不通过 → 丢弃，**不刷新 `last_seen`**。

---

## 3. Flags 位定义

| Bit | 名称 | 含义 |
|---|---|---|
| 0 | `WORN` | 已检测到处于佩戴状态；V1 无佩戴检测时可固定为 1 |
| 1 | `CHARGING` | 正在充电 |
| 2 | `SENSOR_ERROR` | IMU / 电池检测异常 |
| 3 | `PAIR_REQUEST` | 设备希望请求绑定 |
| 4 | `LOW_BATTERY` | 电量低于阈值，例如 **20%** |
| 5–7 | `Reserved` | 保留，必须写 0 |

```
Flags 编码宏：
#define PET_FLAG_WORN          (1u << 0)
#define PET_FLAG_CHARGING      (1u << 1)
#define PET_FLAG_SENSOR_ERROR  (1u << 2)
#define PET_FLAG_PAIR_REQUEST  (1u << 3)
#define PET_FLAG_LOW_BATTERY   (1u << 4)
```

---

## 4. CRC-8 定义

| 参数 | 取值 |
|---|---|
| 算法 | CRC-8/ATM |
| 多项式 | `0x07` (x⁸ + x² + x + 1) |
| 初值 | `0x00` |
| 反射 | 否（MSB-first） |
| 异或输出 | `0x00` |
| 覆盖范围 | **Payload 偏移 0–12**（13 字节）：`ProtocolVersion` 起，到 `Reserved[1]` 止 |
| 校验字段位置 | **Payload 偏移 13**（MFG 偏移 15） |

> 覆盖范围**不包含** CRC 字节自身，也**不包含** AD 长度、AD Type 与 Company ID。
> CRC 字节位于两个保留字节之后，因此 CRC 无法自校验 —— 这是需求文档给定的布局，
> 属于有意为之的弱校验（需求 3.6 节本身也声明 CRC 不是安全机制）。
> 实现必须逐位计算，不得依赖未定义行为的查表越界。
> 固件实现见 `core/pet_protocol.c`，独立参考实现见 `protocol/pet_vectors.c`
> （两者由主机测试交叉验证）。

### 4.1 测试向量（两端必须通过）

| 用例 | 参数 | 期望的 16 字节 Manufacturer Specific Data |
|---|---|---|
| TV-1 Quiet 心跳 | DeviceID `0x12345678`，seq `0`，Quiet，score `0`，battery `100`，flags `0x11` | `FF FF 01 78 56 34 12 00 00 00 00 64 11 00 00 C8` |
| TV-2 Active | seq `0x012A`，Active，score `200`，battery `85`，flags `0x01` | `FF FF 01 78 56 34 12 2A 01 02 C8 55 01 00 00 CE` |
| TV-3 低电量 | seq `0x0010`，Quiet，score `0`，battery `14`，flags `0x11` | `FF FF 01 78 56 34 12 10 00 00 00 0E 11 00 00 36` |
| TV-4 全零 | Payload 偏移 0–12 全 0 | CRC = `0x00` |

拆分查看 TV-1：

```
MFG          = FF FF 01 78 56 34 12 00 00 00 00 64 11 00 00 C8
CompanyID    = FF FF
Payload      = 01 78 56 34 12 00 00 00 00 64 11 00 00 C8
CRC 覆盖     = 01 78 56 34 12 00 00 00 00 64 11 00 00     (Payload[0..12])
CRC 校验字段 = C8                                          (Payload[13])
```

负例（必须被拒）：

* TV-5 篡改 `ActivityState`（Payload 偏移 7，即 MFG[9] 取反 bit0）→ `verify() == false`；
* TV-6 错误 `CompanyID`（MFG[0] = 0x00）→ `verify() == false`。

序号回绕用例：

| `seq` | `last_sequence` | `seq_is_newer` |
|---|---|---|
| `0x0001` | `0x0000` | true |
| `0x0000` | `0xFFFF` | true（回绕） |
| `0xFFFF` | `0x0000` | false |
| `0x0005` | `0x0005` | false |

### 4.2 自检命令

协议向量的**唯一定义处**是 `protocol/pet_vectors.c`：
它既是一份独立编写的参考实现，也内置了与固件实现（`core/pet_protocol.c`）
的交叉验证。

```powershell
# 跑全部主机测试（协议 + 设备表 + 灯效 + 夜间模式）
powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1

# 只跑协议套件
powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1 -Suite protocol

# 打印向量报告 + 跑自检
.\build\pet_tool.exe vectors
```

当前状态：**全部通过**（协议套件 40 项断言，全套 290 项）。

> **本协议曾经存在过一个真实缺陷**：CRC 覆盖范围错位 2 字节
> （`pet_build_mfg_data()` 把 CRC 写到了 `out[13]`，但覆盖范围算的是
> `out[2..14]`）。这会让两端 CRC 永远不匹配、**链路完全不通**。
> 该缺陷由向量比对捕获并修复。这就是保留测试向量的价值。
>
> 之所以用 C 而不是脚本语言写向量：这样参考实现与固件实现共用同一套
> 类型定义与编译环境，并且**工程内不需要任何脚本语言运行时**。

---

## 5. Sequence 规则（灯端）

* `Sequence` 为 **16-bit 无符号**递增，最大值后回到 `0`；灯端必须处理回绕。
* 灯端只接受比 `last_sequence` **更新**的包。
* 判定"更新"采用带符号窗口比较，避免回绕误判：

```c
static inline bool seq_is_newer(uint16_t s, uint16_t last)
{
    return (int16_t)(s - last) > 0;
}
```

* 发生**佩戴端重启**时，允许通过连续收到相同 `DeviceID` 的新包重新同步：
  V1 在**连续 3 个合法包**后放宽一次序号判断（`seq_resync_counter`）。

---

## 6. 灯端处理流程（4.6 节）

```
收到 BLE 广播
  ├─ 是否包含正确 CompanyID？      否则丢弃
  ├─ 长度是否为预期？              否则丢弃
  ├─ ProtocolVersion 是否支持？    否则丢弃
  ├─ DeviceID 是否等于已绑定 ID？  否则丢弃
  ├─ CRC 是否正确？                否则丢弃
  ├─ Sequence 是否为重复或过旧？   是则丢弃
  └─ 有效：
       更新 last_seen
       更新 battery / flags / activity_score
       根据 activity_state 更新目标灯效
       若此前为 LOST，执行恢复连接动画
```

---

## 7. 佩戴端广播频率建议（3.5 节）

| 场景 | 建议广播间隔 |
|---|---|
| V1 固定策略 | 1 s |
| 安静状态优化 | 3–5 s |
| Light / Active | 0.5–1 s |
| 状态改变时 | 立即发送 1–3 个冗余广播 |
| 低电量 | 仍保持心跳，不可直接停发 |

> V1 建议先固定为 **1 s**，优先验证可靠性；电池优化放到后续阶段。

---

## 8. 佩戴端伪代码（3.7 节）

```
init IMU
init BLE advertising
init battery monitor
load DeviceID and sequence from NVS/flash
loop every 100 ms:
    read IMU
    update rolling activity window
loop every 1 s:
    classify activity: QUIET / LIGHT / ACTIVE
    read battery
    build advertising packet
    increment sequence
    advertise packet
    on activity state change: advertise packet immediately 1–3 times
    on battery below threshold: set LOW_BATTERY flag
    on sensor error: set SENSOR_ERROR flag
    continue heartbeat advertising whenever possible
```

---

## 9. 验收要求（3.8 节）

* 设备静止时仍每 **1–5 秒**发送一包心跳。
* **1–10 米**室内距离内，灯端可稳定收到广播。
* 状态由 `Quiet → Light → Active` 的更新时间**不超过 5 秒**。
* 停止广播后，灯端能按配置进入 `Lost` 状态。
* 电量变化可正确显示到广播包。
* 至少持续运行 **24 小时**，不发生广播完全停止、死机或序号不更新。

---

## 10. 后续扩展（不在 V1 必做范围）

* 防伪造：截断 HMAC + 滚动计数器 + 预共享密钥（V1.1）。
* 一只佩戴端对应多个灯；一盏灯监测多个佩戴端（协议预留 `DeviceID` 为 4 字节）。
* 真实 `CompanyID` 申请替换。
