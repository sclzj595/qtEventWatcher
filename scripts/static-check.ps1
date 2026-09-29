# V6 Q2 cppcheck 静态分析一键脚本
# 用法（仓库根执行）：
#   powershell -File scripts\static-check.ps1                 # 默认扫描 src examples tests
#   powershell -File scripts\static-check.ps1 -Paths src     # 自定义扫描路径
# 退出码：0 = 无发现；1 = 有发现；2 = cppcheck 未安装
[CmdletBinding()]
param(
    [string[]]$Paths = @('src', 'examples', 'tests')
)

$repoRoot = Split-Path -Parent $PSScriptRoot

# 定位 cppcheck：PATH → 默认安装目录
$cppcheck = Get-Command cppcheck.exe -ErrorAction SilentlyContinue
if ($cppcheck) {
    $exe = $cppcheck.Source
} elseif (Test-Path "$env:ProgramFiles\Cppcheck\cppcheck.exe") {
    $exe = "$env:ProgramFiles\Cppcheck\cppcheck.exe"
} else {
    Write-Host "[static-check] FAIL: cppcheck not found (winget install --id Cppcheck.Cppcheck)" -ForegroundColor Red
    exit 2
}

# 排除产物目录与三方代码
$excludes = @('build', 'build-qt6', 'build-mingw5', 'build-mingw6', 'build-asan',
              'out', 'docs', '.git', 'third_party') |
            ForEach-Object { "-i$_" }

$template = '{location}: {severity}: {id}: {message}'
$args = @(
    '--enable=warning,performance,portability'
    '--library=qt'                 # Q_OBJECT/slots/signals 等 Qt 宏配置，消 unknownMacro（V6 Q2）
    '--std=c++17'
    '--platform=win64'
    "--template=$template"
    '--error-exitcode=1'           # 有发现即退出码 1（cppcheck 默认恒 0，不可依赖）
    '-DQT_VERSION=0x050f02'        # cppcheck 无 Qt 头，QT_VERSION 未定义会误触发 MetaCallParser.cpp:82 的 #error 版本守卫
) + $excludes + $Paths

# PS5.1：cppcheck 进度走 stderr，EAP=Stop 时 2>&1 合流会把进度行误判为终止错误
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$output = & $exe @args 2>&1
$exit = $LASTEXITCODE
$ErrorActionPreference = $prevEap

$findings = @($output | ForEach-Object { "$_" } |
    Where-Object { $_ -match ': (error|warning|performance|portability):' })
foreach ($f in $findings) {
    Write-Host $f -ForegroundColor Yellow
}
if ($exit -ne 0) {
    Write-Host "[static-check] FAIL: $($findings.Count) finding(s)" -ForegroundColor Red
    exit 1
}
Write-Host "[static-check] PASS: 0 findings (cppcheck $(& $exe --version))" -ForegroundColor Green
exit 0
