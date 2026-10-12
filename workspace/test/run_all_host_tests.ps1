# ==================================================================
# run_all_host_tests.ps1 - 全部主机自检（无需任何硬件 / 无需网络）
#
# 编译并运行 4 个测试套件。被测代码是真实的 core/ 与 protocol/ 源码，
# 平台相关部分用 test/stub 替身，因此这里验证的是产品代码本身。
#
#   host_protocol_test  协议：测试向量、解析正/负例、CRC、序号回绕、字节序
#   host_registry_test  多狗设备表：容量、绑定、每狗独立超时、最活跃者优先
#   host_effects_test   灯效：Gamma、限流、区间、呼吸、熄灯、夜间压制
#   host_night_test     夜间模式：时间窗、空闲推断、手动豁免、节流
#
# 另外顺带构建并自检 tools/pet_tool.c（离线造包 / 解析日志）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1
#   powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1 -Suite effects
# ==================================================================
[CmdletBinding()]
param(
    [ValidateSet('all', 'protocol', 'registry', 'effects', 'night')]
    [string]$Suite = 'all',
    [switch]$KeepExe
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path      # workspace/test
$root = Split-Path -Parent $here                             # workspace
$core = Join-Path $root 'core'
$proto = Join-Path $root 'protocol'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Find-Tool {
    param([string[]]$Names)
    foreach ($n in $Names) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    return $null
}

$cc = Find-Tool @('gcc', 'clang', 'cc')
if (-not $cc) {
    Write-Host 'no C compiler found.' -ForegroundColor Yellow
    Write-Host 'install one of:' -ForegroundColor Yellow
    Write-Host '  winget install --id BrechtSanders.WinLibs.POSIX.UCRT' -ForegroundColor Yellow
    Write-Host '  or MSYS2 / LLVM' -ForegroundColor Yellow
    exit 1
}
Write-Host "compiler: $cc" -ForegroundColor DarkGray

$common = @('-std=c99', '-Wall', '-Wextra', '-O1', '-I', $core, '-I', $proto, '-I', $here)

$suites = @(
    @{ name = 'protocol'; src = @('host_protocol_test.c'); lib = @('pet_protocol.c');        extra = @((Join-Path $proto 'pet_vectors.c')) },
    @{ name = 'registry'; src = @('host_registry_test.c'); lib = @('device_registry.c', 'pet_protocol.c'); extra = @() },
    @{ name = 'effects';  src = @('host_effects_test.c');  lib = @('lamp_effects.c', 'lamp_driver.c', 'lamp_duty.c', 'lamp_gamma.c');     extra = @() },
    @{ name = 'night';    src = @('host_night_test.c');    lib = @('night_mode.c');                        extra = @() }
)

$totalFails = 0
$ran = 0

foreach ($s in $suites) {
    if ($Suite -ne 'all' -and $Suite -ne $s.name) { continue }
    $ran++

    $exe = Join-Path $out ("host_" + $s.name + "_test.exe")
    $sources = @()
    foreach ($f in $s.src)   { $sources += (Join-Path $here $f) }
    $sources += (Join-Path $here 'pet_test.c')
    foreach ($f in $s.lib)   { $sources += (Join-Path $core $f) }
    foreach ($f in $s.extra) { $sources += $f }

    Write-Host ''
    Write-Host ("=" * 72) -ForegroundColor Cyan
    Write-Host ("building + running: " + $s.name) -ForegroundColor Cyan
    Write-Host ("=" * 72) -ForegroundColor Cyan

    & $cc @common @sources '-o' $exe '-lm'
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("compile FAILED: " + $s.name) -ForegroundColor Red
        $totalFails++
        continue
    }

    & $exe
    if ($LASTEXITCODE -ne 0) {
        $totalFails++
    }
    if (-not $KeepExe) {
        Remove-Item $exe -Force -ErrorAction SilentlyContinue
    }
}

# 额外：离线工具自检（vectors 子命令内含交叉验证）
if ($Suite -eq 'all') {
    Write-Host ''
    Write-Host ("=" * 72) -ForegroundColor Cyan
    Write-Host 'building + running: pet_tool vectors' -ForegroundColor Cyan
    Write-Host ("=" * 72) -ForegroundColor Cyan
    $tool = Join-Path $out 'pet_tool.exe'
    & $cc @common (Join-Path $root 'tools\pet_tool.c') `
        (Join-Path $core 'pet_protocol.c') (Join-Path $proto 'pet_vectors.c') `
        '-o' $tool '-lm'
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'pet_tool compile FAILED' -ForegroundColor Red
        $totalFails++
    } else {
        & $tool vectors | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host 'pet_tool vectors FAILED' -ForegroundColor Red
            $totalFails++
        } else {
            Write-Host 'pet_tool vectors: OK (含参考实现与固件实现交叉验证)' -ForegroundColor Green
        }
    }
    if (-not $KeepExe) {
        Remove-Item $tool -Force -ErrorAction SilentlyContinue
    }

    # ---------------------------------------------------------------
    # 端到端集成测试：core -> 真实 lamp_bslamp2/main/lamp_esp32.c -> LEDC 替身
    #
    # 为什么不并进上面四个套件：它的替身集合不同（真实适配层 + 直通驱动），
    # 验证的是"接线"而不是单个模块。接线错误（通道顺序反了、回调没接上、
    # 占空比被换算两次）单元测试抓不到。
    # ---------------------------------------------------------------
    Write-Host ''
    Write-Host ("=" * 72) -ForegroundColor Cyan
    Write-Host 'building + running: integration (core -> lamp_esp32.c -> LEDC stub)' -ForegroundColor Cyan
    Write-Host ("=" * 72) -ForegroundColor Cyan

    $integ = Join-Path $root 'lamp_bslamp2\test\run_integration_test.ps1'
    if (Test-Path $integ) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $integ
        if ($LASTEXITCODE -ne 0) {
            $totalFails++
        }
        $ran++
    } else {
        Write-Host 'integration test script not found, skipped' -ForegroundColor Yellow
    }

    # ---------------------------------------------------------------
    # 触摸面板协议单元测试（纯函数，无硬件依赖）
    # 协议移植自 esphome-xiaomi_bslamp2 / front_panel_hal.h。
    # ---------------------------------------------------------------
    Write-Host ''
    Write-Host ("=" * 72) -ForegroundColor Cyan
    Write-Host 'building + running: panel (触控面板 I2C 协议)' -ForegroundColor Cyan
    Write-Host ("=" * 72) -ForegroundColor Cyan

    $panelExe = Join-Path $out 'host_panel_test.exe'
    # 面板测试的被测代码在 lamp_bslamp2/main，需要额外的 include 路径
    $panelFlags = @('-std=c99', '-Wall', '-Wextra', '-O1',
                    '-I', $core,
                    '-I', (Join-Path $root 'lamp_bslamp2\main'),
                    '-I', $here,
                    '-I', (Join-Path $root 'lamp_bslamp2\test\host_stubs\stub_headers'))
    & $cc @panelFlags `
        (Join-Path $root 'lamp_bslamp2\test\host_panel_test.c') `
        (Join-Path $here 'pet_test.c') `
        (Join-Path $root 'lamp_bslamp2\main\panel_protocol.c') `
        '-o' $panelExe '-lm'
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'panel test compile FAILED' -ForegroundColor Red
        $totalFails++
    } else {
        & $panelExe
        if ($LASTEXITCODE -ne 0) {
            $totalFails++
        }
        $ran++
    }
    if (-not $KeepExe) {
        Remove-Item $panelExe -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host ("=" * 72)
if ($totalFails -eq 0) {
    Write-Host ("ALL HOST TESTS PASSED (" + $ran + " suites)") -ForegroundColor Green
} else {
    Write-Host ("FAILED: " + $totalFails + " suite(s)") -ForegroundColor Red
}
Write-Host ("=" * 72)
exit $totalFails
