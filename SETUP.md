# 使用说明

> 目标仓库：<https://github.com/shark11634/pet_lamp.git>
>
> **快速上手**：先看 [`README.md`](README.md)（进度与验证命令），
> 再按需深入 [`docs/开发文档.md`](docs/开发文档.md)（架构、缺陷全表、开发约定）。

---

## 1. 仓库状态

本地已完成 `git init` 与首次提交，内容干净可直接推送：

| 项 | 值 |
|---|---|
| 分支 | `main` |
| 首次提交 | `1ff419d` feat: 宠物活动状态灯 V1 首个可用版本 |
| 文件数 | 87 |
| 工作树 | 干净（无未提交改动） |
| 体积 | `.git` 0.93 MB，工作树 1.70 MB |

---

## 2. 推送

### 为什么没能在这里推完（已定位到根因）

不是 GitHub 不可达。同一台机器、同一时刻的实测：

| 探测方式 | 结果 |
|---|---|
| Python 自带 TLS 栈 → `https://github.com/shark11634/pet_lamp` | **HTTP 200** |
| `git -c http.sslBackend=openssl` → `https://gitee.com/...` | **成功列出 refs** |
| `git` → `https://api.github.com/...` | 连接建立（返回 403，属正常匿名拒绝） |
| `git` → `https://github.com/shark11634/pet_lamp.git` | `Recv failure: Connection was reset` |

结论：**出网是通的，是开发环境的沙箱拦住了 git.exe 的出网**。
`curl.exe` / schannel 报的 `SEC_E_NO_CREDENTIALS` 也是沙箱不让读
Windows 证书库所致，同样是沙箱现象，不是网络问题。

也就是说，只要在**不受该沙箱限制的环境**里执行推送即可 —— 这正是下面的方式 1。

三种方式，**推荐方式 1**。

### 方式 1（推荐）：用 bundle 一键推送

`pet_lamp.bundle` 是一个自包含的 git 归档（0.89 MB，含完整提交历史）。
把它拷到能上网的机器，然后：

**Windows**：双击 `push-to-github.cmd`。

**命令行**：

```bash
git clone pet_lamp.bundle pet_lamp
cd pet_lamp
git remote set-url origin https://github.com/shark11634/pet_lamp.git
git push -u origin main
```

`bundle` 方式的好处：**历史与提交信息完整保留**，不需要重新提交。

### 方式 2：直接在这台机器的仓库里推

```bash
cd <本目录>
git remote set-url origin https://github.com/shark11634/pet_lamp.git
git push -u origin main
```

`origin` 已经配好了，只差网络与凭据。

### 方式 3：手动重做一次提交

如果只想推源码、不要 git 历史，把 `workspace/` 目录拷到一个新仓库即可。
**但要记得先删掉** `bslamp2-stock-4MB.bin`（见下节）。

### 关于凭据

GitHub 已不接受账号密码，需用 **Personal Access Token（classic，勾 `repo`）**
或 SSH key。HTTPS 首次推送会提示输入用户名与 token（密码位置填 token）。

---

## 3. ★ 提交前必须确认的事

`.gitignore` 已经挡住下面这些，但**如果你手工拷贝目录**，请再确认一遍：

| 绝不能提交 | 原因 |
|---|---|
| `bslamp2-stock-4MB.bin` | 原厂 4 MB Flash 备份。它是唯一的设备恢复手段，且可能含设备标识（MAC / eFuse 派生值 / 出厂配置）。公开等于把设备的出厂身份一起公布。需求 §5.6 明确要求不要公开上传。 |
| `secrets.yaml` | Wi-Fi 密码、API 加密密钥、OTA 密码、网页密码。仓库里只有 `.example` 模板。 |

**原厂备份请另外存到至少两个物理位置**（U 盘 + 网盘 / 另一台机器）。
它一旦丢失，设备就回不去了。

推送前可以自查：

```bash
git ls-files | findstr /i "\.bin$ secrets.yaml"     # 应为空
```

---

## 4. 拿到代码后怎么验证（无需硬件）

只需要一个 C 编译器，不需要 ESP-IDF、不需要 ESPHome、不需要网络：

```powershell
cd workspace
powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1
```

期望输出：

```
building + running: protocol
building + running: registry
building + running: effects
building + running: night
building + running: pet_tool vectors
building + running: integration (core -> lamp_esp32.c -> LEDC stub)
building + running: panel (触控面板 I2C 协议)
ALL HOST TESTS PASSED (6 suites)
```

**488 项断言**。这套测试在从 bundle 克隆出的干净副本上验证过，可重现。

---

## 5. 关键约定（动手前务必知道）

这四条写在这里是因为**违反它们会真的出错**，不是风格偏好：

1. **业务逻辑改 `workspace/core/`，不要改平台层。**
   `core/` 是纯 C99、无平台依赖，488 项断言覆盖它。
   在平台层"顺手"补一份实现会导致两份漂移——本项目为此返工过两次。

2. **`core/` 不得 include 平台头文件。** 随时可验证：
   ```powershell
   gcc -std=c99 -Wall -Wextra -Wpedantic -I core -c core/*.c
   ```

3. **行为参数只写在 `core/pet_config.h`。**
   平台层（`lamp_bslamp2/main/app_config.h`）只放 GPIO / LEDC / 任务栈这类
   与板子绑死的常量。

4. **Windows 上不要用 PowerShell 的 `Get-Content` / `Set-Content`
   改写含中文的源文件**——会变乱码或插入字面量 `` `n ``。
   用编辑器或 `[System.IO.File]::WriteAllText()`。

---

## 6. 已知限制（不是待办，是当前状态）

| 项 | 说明 |
|---|---|
| ESPHome 组件未编译验证 | API 已按 ESPHome 2026.11.0-dev 源码逐项核对，但本机构建环境受限。首次 `esphome config/compile` 可能需修小问题。 |
| 方案 B 未在真实 ESP-IDF 上编译 | 适配层已用真实替身头做 `-Wall -Wextra -Wpedantic` 严格编译，零警告；`app_main.c` 引用的符号都核实过有声明。 |
| 白光 13 bit @ 9765 Hz（非原厂 10 kHz） | ESP32 LEDC 在 14 bit 下上限只有 4882 Hz。取舍与推导见 `docs/开发文档.md` §5.2，由 `app_config.h` 的编译期断言守护。 |
| 面板 I²C 时序、原厂功能回归 P01–P21、72 小时长稳 | 均需真机验证。 |

细节见 [`docs/开发文档.md`](docs/开发文档.md) 第 14 章。

---

## 7. 目录速查

```
README.md                    进度、验证命令、目录总览
SETUP.md                     本文件
docs/开发文档.md             ★ 架构 / 缺陷全表 / 测试体系 / 开发约定
docs/最终可执行方案.md        16 项决策（D1–D16）与工作包
docs/需求要点_已恢复全部数值.md 从需求 PDF 恢复的全部参数
workspace/core/              ★ 可移植纯 C99 核心（被测代码）
workspace/protocol/          协议规范 + 独立参考实现 + 测试向量
workspace/test/              四套主机测试 + 统一入口
workspace/tools/pet_tool.c   离线 CLI：vectors / build / log
workspace/esphome/           方案 A：ESPHome 外部组件 + 内置网页
workspace/lamp_bslamp2/      方案 B：纯 ESP-IDF + 集测 + 面板协议
workspace/wearable_xiao/     佩戴端固件（XIAO nRF52840 Sense）
```
