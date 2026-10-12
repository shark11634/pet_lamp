# `lamp_bslamp2/` — 方案 B（纯 ESP-IDF）参考实现

> 定位见 `docs/最终可执行方案.md`：主线是方案 A（ESPHome 外部组件），
> 本目录是**方案 B 的骨架**，也是把 `core/` 接到 ESP-IDF 的示例。
>
> **S4 起它已重新可构建**：所有业务逻辑改从 `../core/` 引入，
> 自己不再保留任何 Gamma / 限流 / 灯效 / 状态机副本。

---

## 1. 现在的分层

```
lamp_bslamp2/main/                     ESP-IDF 绑定层（薄）
├── app_main.c        编排：三个任务（BLE 20Hz / 状态 1Hz / 灯效 50Hz）
├── app_config.h      只放平台常量：GPIO、LEDC、任务栈、NVS 命名空间
├── ble_scanner.c     NimBLE 被动扫描 → 提取 0xFF 广播 → 入队（不解析）
├── lamp_esp32.c      core 的占空比 → LEDC 14-bit + 主开关 GPIO
├── storage.c         NVS：绑定列表 / 每狗名字 / 设置（只在变化时写）
├── diagnostics.c     需求 §8.5 的日志字段（一行一个字段组，可被工具解析）
└── touch_panel.c     面板 I²C 抽象（协议待 §5.6 逆向，当前不产生事件）

../core/                               业务逻辑（纯 C99，300+ 项主机断言）
├── pet_protocol.c       广播解析 / CRC-8 / 六层校验
├── device_registry.c    多狗设备表 / 每狗独立计时 / 最活跃者优先
├── night_mode.c         夜间模式 / 空闲推断 / 手动豁免 / Active 节流
├── lamp_effects.c       状态目标色 / 过渡 / 呼吸 / 提示 / 恢复动画
├── lamp_duty.c          全通道限流 + 占空比换算（纯函数）
├── lamp_gamma.c         Gamma 2.2 曲线 + 平台位数换算（纯函数）
└── lamp_driver.c        有状态封装（持有平台回调与最近占空比）
```

**为什么这样切**：`core/` 不含任何 ESP-IDF 头文件，因此能在 PC 上编译并跑
断言。本轮就是靠这一点，在**没有 ESP32、没有 ESP-IDF 工具链**的情况下
完成了方案 B 的移植并验证了接线。

---

## 2. 验证方式（不需要硬件）

```powershell
# core 的四套单元测试 + 端到端集成测试
cd workspace
powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1
# → ALL HOST TESTS PASSED (5 suites)

# 只跑集成测试（core → 真实 lamp_esp32.c → LEDC 寄存器替身）
powershell -ExecutionPolicy Bypass -File lamp_bslamp2\test\run_integration_test.ps1
```

集成测试用 `test/host_stubs/` 里的 ESP-IDF 外设替身（GPIO / LEDC / 日志），
被测代码是**真实的** `lamp_esp32.c`，检查的是"接线"：

| 检查项 | 实测结果 |
|---|---|
| 通道映射真值表（逐通道点亮 R/G/B/W） | 4/4，且无串扰 |
| Active 输出 | `[12, 675, 1459, 0]` 峰值 1459/16383 → 偏蓝，≈39% |
| 夜间 Active（上限 18/255） | 日间 39% → **夜间 7%** |
| Lost 输出 | `[3, 1, 7, 0]` ≈ **3%**，不熄灯 |
| 多狗：A 超时 63 s、B 在册 15 s | 合并 = **Active，来源 B**（不误判 Lost） |
| 占空比二次换算 | 否（原样透传） |
| 超量程占空比 | 钳到 14-bit 满量程，不回绕 |

---

## 3. 未完成 / 已知限制（诚实清单）

| 项 | 说明 |
|---|---|
| **未在真实 ESP-IDF 上编译** | 本机没有 ESP-IDF 工具链（首次 `idf.py build` 要下载数百 MB）。组件结构、CMakeLists 的 SRCS/INCLUDE_DIRS/REQUIRES、NimBLE 与 legacy I²C 驱动调用都按 ESP-IDF v5.x 的稳定 API 写，但**首次编译可能需要修小问题**。 |
| **触摸面板协议未接入** | `touch_panel.c` 已就绪（I²C 总线、中断脚、事件队列、回调分发、总线扫描），但**寄存器语义必须按实物确认**，因此 `PANEL_PROTOCOL_CONFIRMED=0`，当前不产生事件。见该文件头的四步流程。瞎猜时序最坏会把面板 MCU 写进异常状态。 |
| **12 颗面板指示灯未用** | 计划用于显示"当前显示第几只狗"，依赖上面的面板协议。 |
| **原厂手动灯光未接管** | 短按熄灯、滑条亮度上限的**事件处理已写好**（`on_panel_event`），但手动灯光的持久化与优先级还需要面板协议确认后一起接。 |
| **未接 SNTP** | `clock_hour()` 恒返回 false，因此夜间模式走"空闲推断"兜底路径。这恰好验证了设计目标（没有时钟也能夜间免打扰）；要按真实时间窗工作，接上 SNTP 后填充该函数即可。 |
| **原厂功能回归 P01–P21 未执行** | 需要真机。清单与记录模板在 `docs/最终可执行方案.md` §9 与 `docs/验收测试记录模板.md`。 |

---

## 4. 下一步

1. 在能联网的机器上 `idf.py set-target esp32 && idf.py build`，按报错修小问题
2. 按 `touch_panel.c` 文件头的四步流程确认面板寄存器协议，打开
   `PANEL_PROTOCOL_CONFIRMED`，接上短按 / 长按 / 色彩键 / 滑条
3. 用 12 颗面板指示灯显示当前焦点狗序号
4. 跑原厂功能回归清单 P01–P21，结果填进验收模板
5. 实测 Wi-Fi/BLE 共存下的有效包率，决定需要的 coex 偏好
