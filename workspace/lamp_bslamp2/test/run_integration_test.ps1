# ==================================================================
# run_integration_test.ps1 - 端到端集成测试（core -> ESP-IDF 适配层 -> LEDC）
#
# 与 workspace/test/run_all_host_tests.ps1 的分工：
#   那个脚本测 core 的每个模块（4 套单元测试）。
#   本脚本测**接线**：把真实的 lamp_bslamp2/main/lamp_esp32.c 编到主机上，
#   跑完整链路 BLE字节流 -> core -> 适配层 -> LEDC 寄存器替身。
#
# 接线错误（通道顺序反了、占空比被换算了两次、主开关没拉高、限流没生效）
# 单元测试抓不到，只有集成测试能抓到。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File lamp_bslamp2\test\run_integration_test.ps1
# ==================================================================
[CmdletBinding()]
param([switch]$KeepExe, [switch]$VerboseLog)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path      # lamp_bslamp2/test
$lamp = Split-Path -Parent $here                             # lamp_bslamp2
$root = Split-Path -Parent $lamp                             # workspace
$core = Join-Path $root 'core'
$main = Join-Path $lamp 'main'
$stub = Join-Path $here 'host_stubs'
$out  = Join-Path $root 'build'
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
    exit 1
}
Write-Host "compiler: $cc" -ForegroundColor DarkGray

$exe = Join-Path $out 'host_integration_test.exe'

$sources = @(
    (Join-Path $here 'host_integration_test.c')
    (Join-Path $root 'test\pet_test.c')
    # 真实适配层
    (Join-Path $main 'lamp_esp32.c')
    # 真实 core（注意：**不含** lamp_driver.c —— 由 mock_driver.c 代替，
    # 同名符号重定义）
    (Join-Path $core 'pet_protocol.c')
    (Join-Path $core 'device_registry.c')
    (Join-Path $core 'night_mode.c')
    (Join-Path $core 'lamp_effects.c')
    (Join-Path $core 'lamp_duty.c')
    (Join-Path $core 'lamp_gamma.c')
    # 测试替身
    (Join-Path $here 'mock_driver.c')
    (Join-Path $stub 'esp_host_stub.c')
)

$flags = @(
    '-std=c99', '-Wall', '-Wextra', '-O1',
    '-I', $core,
    '-I', $main,
    '-I', (Join-Path $root 'test'),
    '-I', (Join-Path $stub 'stub_headers')
)
if ($VerboseLog) { $flags += '-DHOST_TEST_VERBOSE_LOG' }

Write-Host ''
Write-Host ("=" * 72) -ForegroundColor Cyan
Write-Host 'building + running: integration (core -> lamp_esp32.c -> LEDC stub)' -ForegroundColor Cyan
Write-Host ("=" * 72) -ForegroundColor Cyan

& $cc @flags @sources '-o' $exe '-lm'
if ($LASTEXITCODE -ne 0) {
    Write-Host 'compile FAILED' -ForegroundColor Red
    exit 1
}

& $exe
$rc = $LASTEXITCODE

if (-not $KeepExe) {
    Remove-Item $exe -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($rc -eq 0) {
    Write-Host 'INTEGRATION TEST PASSED' -ForegroundColor Green
} else {
    Write-Host 'INTEGRATION TEST FAILED' -ForegroundColor Red
}
exit $rc
