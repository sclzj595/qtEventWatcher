# V4 A1 一键四矩阵回归脚本
# 用法（仓库根执行）：
#   powershell -File scripts\run_regression.ps1                    # 快速档（off+slow）+ 漂移检查
#   powershell -File scripts\run_regression.ps1 -Full              # benchmark 全 5 模式
#   powershell -File scripts\run_regression.ps1 -SkipBuild         # 跳过编译（复用现有产物）
#   powershell -File scripts\run_regression.ps1 -UpdateBaselines   # 采集/更新基线（不做漂移判定）
#   powershell -File scripts\run_regression.ps1 -Strict            # DRIFT 阻断（默认仅提示）
#   powershell -File scripts\run_regression.ps1 -Matrices qt5152-msvc,qt653-mingw
[CmdletBinding()]
param(
    [switch]$Full,
    [switch]$SkipBuild,
    [switch]$UpdateBaselines,
    [switch]$Strict,
    [string[]]$Matrices
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$matrixFile = Join-Path $PSScriptRoot 'regression\qt-matrix.json'
$baselineFile = Join-Path $PSScriptRoot 'regression\baselines-v3.json'
$reportFile = Join-Path $PSScriptRoot 'regression\last-report.md'

$cfg = Get-Content $matrixFile -Raw | ConvertFrom-Json
$benCfg = $cfg.benchmark
$tolerance = [double]$benCfg.driftTolerance
$modes = if ($Full) { @($benCfg.fullModes) } else { @($benCfg.quickModes) }

# 基线：hashtable["<matrix>|<scenario>|<field>"] = double
$baselines = @{}
if (Test-Path $baselineFile) {
    $raw = Get-Content $baselineFile -Raw | ConvertFrom-Json
    foreach ($p in $raw.PSObject.Properties) { $baselines[$p.Name] = [double]$p.Value }
}

function Parse-BenchmarkOutput([string[]]$lines) {
    # scenario=NAME k=v k=v ... -> hashtable["NAME|k"] = v（数值字段转 double）
    $out = @{}
    foreach ($line in $lines) {
        if ($line -notmatch '^scenario=([A-Za-z0-9_-]+)\s+(.*)$') { continue }
        $name = $Matches[1]
        foreach ($tok in ($Matches[2] -split '\s+')) {
            $eq = $tok.IndexOf('=')
            if ($eq -le 0) { continue }
            $k = $tok.Substring(0, $eq); $v = $tok.Substring($eq + 1)
            $d = 0.0
            if ([double]::TryParse($v, [Globalization.NumberStyles]::Float,
                                   [Globalization.CultureInfo]::InvariantCulture, [ref]$d)) {
                $out["$name|$k"] = $d
            }
        }
    }
    return $out
}

# 漂移检查关注字段：pair 场景看 delta_avg，单值场景看 avg
$watchFields = @('delta_avg', 'avg')

$results = New-Object System.Collections.Generic.List[object]
foreach ($m in $cfg.matrices) {
    if ($Matrices -and ($Matrices -notcontains $m.name)) { continue }

    # ---- PATH 前置（仅当前进程）----
    $env:PATH = ($m.envPath -join ';') + ';' + $env:PATH

    # ---- 构建 ----
    $buildStatus = 'SKIP'
    if (-not $SkipBuild) {
        $buildArgs = @('--build', (Join-Path $repoRoot $m.buildDir))
        if ($m.config) { $buildArgs += @('--config', $m.config) }
        $buildLog = & cmake @buildArgs 2>&1
        $buildStatus = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $results.Add([pscustomobject]@{ matrix = $m.name; step = 'build'; status = $buildStatus; detail = '' })

    if ($buildStatus -eq 'FAIL') {
        $results.Add([pscustomobject]@{ matrix = $m.name; step = 'smoke'; status = 'SKIP'; detail = 'build failed' })
        $results.Add([pscustomobject]@{ matrix = $m.name; step = 'unit'; status = 'SKIP'; detail = 'build failed' })
        $results.Add([pscustomobject]@{ matrix = $m.name; step = 'benchmark'; status = 'SKIP'; detail = 'build failed' })
        continue
    }

    $exeDir = Join-Path $repoRoot $m.exeDir

    # ---- 冒烟 ----
    $smokeOut = & (Join-Path $exeDir $cfg.smoke.exe) 2>&1
    $smokeOk = ($LASTEXITCODE -eq 0) -and (-not ($smokeOut | Where-Object { $_ -match 'FAIL' }))
    $failLines = @($smokeOut | Where-Object { $_ -match 'FAIL' } | Select-Object -First 2)
    $results.Add([pscustomobject]@{
        matrix = $m.name; step = 'smoke'
        status = if ($smokeOk) { 'PASS' } else { 'FAIL' }
        detail = ($failLines -join ' | ')
    })

    # ---- 单元测试（V6 Q1：纯逻辑断言，exit=0 即全过）----
    $unitExe = Join-Path $exeDir $cfg.smoke.unitExe
    $unitOut = & $unitExe 2>&1
    $unitFailLines = @($unitOut | Where-Object { $_ -match '\[FAIL\]' } | Select-Object -First 2)
    $unitOk = ($LASTEXITCODE -eq 0)
    $unitDetail = if ($unitOk) {
        ($unitOut | Where-Object { $_ -match '^checks=' } | Select-Object -First 1)
    } else { $unitFailLines -join ' | ' }
    $results.Add([pscustomobject]@{
        matrix = $m.name; step = 'unit'
        status = if ($unitOk) { 'PASS' } else { 'FAIL' }
        detail = [string]$unitDetail
    })

    # ---- benchmark ----
    $measured = @{}
    foreach ($mode in $modes) {
        $out = & (Join-Path $exeDir $cfg.smoke.benchmarkExe) $mode 2>&1
        if ($LASTEXITCODE -ne 0) {
            $results.Add([pscustomobject]@{ matrix = $m.name; step = "benchmark:$mode"; status = 'FAIL'; detail = "exit=$LASTEXITCODE" })
            continue
        }
        $parsed = Parse-BenchmarkOutput $out
        foreach ($k in $parsed.Keys) { $measured["$($m.name)|$mode|$k"] = $parsed[$k] }
    }

    # ---- 漂移对比 / 基线更新 ----
    $drifts = New-Object System.Collections.Generic.List[string]
    if ($UpdateBaselines) {
        foreach ($kv in $measured.GetEnumerator()) { $baselines[$kv.Key] = $kv.Value }
        $results.Add([pscustomobject]@{ matrix = $m.name; step = 'baseline'; status = 'UPDATED'; detail = "$($measured.Count) keys" })
    } elseif ($baselines.Count -gt 0) {
        foreach ($kv in $measured.GetEnumerator()) {
            $scenario = ($kv.Key -split '\|')[-2]
            if ($benCfg.excludeScenarios -and ($benCfg.excludeScenarios -contains $scenario)) { continue }
            foreach ($f in $watchFields) {
                if (-not $kv.Key.EndsWith("|$f")) { continue }
                if (-not $baselines.ContainsKey($kv.Key)) { continue }   # 新场景无基线，静默
                $old = $baselines[$kv.Key]; $new = $kv.Value
                if ($old -le 0) { continue }                             # 除零防护
                $rel = [math]::Abs($new - $old) / $old
                if ($rel -gt $tolerance) {
                    $drifts.Add("$($kv.Key) : $old -> $new (+$([math]::Round($rel*100,1))%)")
                }
            }
        }
        $bStatus = if ($drifts.Count -gt 0) { 'DRIFT' } else { 'PASS' }
        $results.Add([pscustomobject]@{
            matrix = $m.name; step = 'benchmark'
            status = $bStatus
            detail = "$(($measured.Keys | Where-Object { $_ -match '\|delta_avg$|\|avg$' }).Count) metrics; $($drifts -join ' | ')"
        })
    } else {
        $results.Add([pscustomobject]@{ matrix = $m.name; step = 'benchmark'; status = 'NOTE'; detail = 'no baseline; run -UpdateBaselines first' })
    }
}

# ---- 基线写盘 ----
if ($UpdateBaselines) {
    $ordered = [ordered]@{}
    foreach ($k in ($baselines.Keys | Sort-Object)) { $ordered[$k] = [math]::Round($baselines[$k], 1) }
    $ordered | ConvertTo-Json | Set-Content $baselineFile -Encoding UTF8
    Write-Host "[baseline] written: $baselineFile"
}

# ---- 控制台汇总 ----
Write-Host ''
Write-Host '==== Regression Summary ===='
foreach ($r in $results) {
    Write-Host ("[{0}] {1,-12} {2,-22} {3}" -f $r.status, $r.matrix, $r.step, $r.detail)
}
$hasFail = ($results | Where-Object { $_.status -in @('FAIL') }).Count -gt 0
$hasDrift = ($results | Where-Object { $_.status -eq 'DRIFT' }).Count -gt 0

# ---- Markdown 报告 ----
$lines = @('# Last Regression Report', '', "- Time: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')", "- Modes: $($modes -join ', ')", '', '| Status | Matrix | Step | Detail |', '|---|---|---|---|')
foreach ($r in $results) { $lines += ('| {0} | {1} | {2} | {3} |' -f $r.status, $r.matrix, $r.step, ($r.detail -replace '\|', '\|')) }
$lines += ''
Set-Content $reportFile $lines -Encoding UTF8
Write-Host "[report] $reportFile"

if ($hasFail -or ($hasDrift -and $Strict)) { exit 1 }
exit 0
