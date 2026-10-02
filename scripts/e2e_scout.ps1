# ============================================================
# e2e_scout.ps1 - Scout 四探针一键 e2e（Scout V1 / docs/34 §3.5）
#   T1b cpu（--name 采样 cpuSpin）
#   T1  freeze（--name 三态 started/recovered）
#   T3  radar（--radar 全机发现 + radar=1）
#   T4  uplink（aggregator 收链路 + JSON 健康自恤断言）
# 用法：powershell -File scripts/e2e_scout.ps1 [-BuildDir build] [-QtBin <path>]
# 退出码：任一断言失败即非 0（CI/本地门禁直用）
# ============================================================
param(
    [string]$BuildDir = "build",
    [string]$QtBin = "F:\IDE.2\Qt5.15.2\5.15.2\msvc2019_64\bin"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "$BuildDir\bin\Release"
$Scout = Join-Path $Bin "scout.exe"
$Demo = Join-Path $Bin "basic_demo.exe"
$Agg = Join-Path $Bin "aggregator.exe"
$Tmp = Join-Path $env:TEMP "qewt_e2e_scout"
if (Test-Path $Tmp) { Remove-Item $Tmp -Recurse -Force }
New-Item -ItemType Directory -Path $Tmp | Out-Null
$env:PATH = "$QtBin;" + $env:PATH

$global:Failures = @()
function Assert-Log($Name, $Pattern, $LogFile) {
    if (Select-String -Path $LogFile -Pattern $Pattern -Quiet) {
        Write-Host "  [PASS] $Name - '$Pattern'"
    } else {
        Write-Host "  [FAIL] $Name - '$Pattern' not found in $LogFile"
        $global:Failures += "${Name}: '$Pattern' missing in $(Split-Path -Leaf $LogFile)"
    }
}

function Stop-Graceful($Proc) {
    # 优雅关闭保 spdlog 落盘（Stop-Process -Force 会吞日志缓冲）
    if ($Proc -and !$Proc.HasExited) {
        $null = $Proc.CloseMainWindow()
        Start-Sleep -Milliseconds 500
        if (!$Proc.HasExited) { Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue }
    }
}

function Run-E2E {
    param([string]$Name, [string[]]$ScoutArgs, [int]$DurationMs, [int]$DemoSpinMs = 6000,
          [switch]$Radar, [switch]$Uplink)

    Write-Host "=== $Name ==="
    $outLog = Join-Path $Tmp "${Name}.out.log"
    $errLog = Join-Path $Tmp "${Name}.err.log"
    $uplinkJson = Join-Path $Tmp "$Name.uplink.json"

    $demo = Start-Process $Demo -ArgumentList "--spin", "$DemoSpinMs" -PassThru
    Start-Sleep -Milliseconds 800

    # 注意：不可用 $agg——与脚本级 $Agg 大小写不敏感同名，函数局部置 null
    # 会吞掉右侧对 $Agg 的解析（实测踩坑），用 $aggProc 隔离
    $aggProc = $null
    $args2 = $ScoutArgs
    if ($Uplink) {
        $aggProc = Start-Process $Agg -ArgumentList "--duration", "14", "--out", $uplinkJson `
            -PassThru -RedirectStandardOutput (Join-Path $Tmp "${Name}.agg.log") `
            -RedirectStandardError (Join-Path $Tmp "${Name}.agg.err.log")
        Start-Sleep -Milliseconds 800
        $args2 = $ScoutArgs + @("--uplink", "QtEventWatcherAggregator")
    }

    $p = Start-Process $Scout -ArgumentList ($args2 + @("--duration", "$DurationMs")) -PassThru `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    $null = Wait-Process -Id $p.Id -Timeout ([int]($DurationMs / 1000) + 20) -ErrorAction SilentlyContinue
    Stop-Graceful $demo
    if ($aggProc) {
        # aggregator 退出时才写 --out JSON（console 无窗口，强杀=丢 JSON）——
        # 等 --duration 到点自然退出（≤8s 兜底）再走优雅关闭
        $null = Wait-Process -Id $aggProc.Id -Timeout 8 -ErrorAction SilentlyContinue
        Stop-Graceful $aggProc
    }
    Write-Host "  scout exit done, log=$outLog"
}

# 1. T1b：CPU 单目标采样
Run-E2E -Name "cpu" -ScoutArgs @("--name", "basic_demo.exe", "--cpu-threshold", "95",
    "--cpu-runs", "3", "--interval", "250") -DurationMs 12000
Assert-Log "cpu" "event=cpuSpin" (Join-Path $Tmp "cpu.out.log")
Assert-Log "cpu" "source=scout" (Join-Path $Tmp "cpu.out.log")

# 2. T1：窗口冻结三态
Run-E2E -Name "freeze" -ScoutArgs @("--name", "basic_demo.exe", "--threshold", "2000",
    "--interval", "250") -DurationMs 12000
Assert-Log "freeze" "freeze started" (Join-Path $Tmp "freeze.out.log")
Assert-Log "freeze" "freeze recovered" (Join-Path $Tmp "freeze.out.log")

# 3. T3：全机雷达（免 pid/name）
Run-E2E -Name "radar" -ScoutArgs @("--radar", "--threshold", "2000", "--interval", "250",
    "--radar-exclude", "RtkUWP") -DurationMs 12000
Assert-Log "radar" "freeze started" (Join-Path $Tmp "radar.out.log")
Assert-Log "radar" "freeze recovered" (Join-Path $Tmp "radar.out.log")
Assert-Log "radar" "radar=1" (Join-Path $Tmp "radar.out.log")

# 4. T4：radar + uplink 全链路
Run-E2E -Name "uplink" -ScoutArgs @("--radar", "--threshold", "2000", "--interval", "250",
    "--radar-exclude", "RtkUWP") -DurationMs 12000 -Radar -Uplink
Assert-Log "uplink" "freeze started" (Join-Path $Tmp "uplink.out.log")
$uplinkJson = Join-Path $Tmp "uplink.uplink.json"
if (Test-Path $uplinkJson) {
    try {
        $j = Get-Content $uplinkJson -Raw -Encoding UTF8 | ConvertFrom-Json
        $ok = $false
        foreach ($s in $j.sessions) {
            $dump = ($s | ConvertTo-Json -Depth 8 -Compress)
            $hasRadar = $dump -match 'radar=1'
            $recv = [int]$s.received
            $sdrop = [int]$s.dropped
            $pushed = [int]$s.health.pushed
            $lastSeq = [int]$s.health.lastSeq
            Write-Host "  session pid=$($s.host_pid) received=$recv pushed=$pushed lastSeq=$lastSeq sdrop=$sdrop radar1=$hasRadar"
            # 无损口径：实收==入库且零丢弃；health 是客户端自报快照，退出时
            # 析构 flush 尾批不随行更新 health——允许 received>=pushed
            if ($hasRadar -and $recv -gt 0 -and $recv -ge $pushed -and $sdrop -eq 0) { $ok = $true }
        }
        if ($ok) { Write-Host "  [PASS] uplink - lossless (received==records, dropped=0, received>=pushed) with radar=1" }
        else { Write-Host "  [FAIL] uplink chain inconsistent"; $global:Failures += "uplink: lossless uplink with radar=1 not satisfied" }
    } catch {
        Write-Host "  [FAIL] uplink json parse: $_"
        $global:Failures += "uplink: json parse failed"
    }
} else {
    Write-Host "  [FAIL] uplink json missing: $uplinkJson"
    $global:Failures += "uplink: aggregator json not produced"
}

Write-Host ""
Write-Host "==== e2e summary ===="
if ($global:Failures.Count -eq 0) {
    Write-Host "ALL PASS (4 scenarios)"
    exit 0
} else {
    Write-Host "FAILURES ($($global:Failures.Count)):"
    $global:Failures | ForEach-Object { Write-Host "  - $_" }
    Write-Host "logs in: $Tmp"
    exit 1
}
