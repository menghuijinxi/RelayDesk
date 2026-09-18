# 把 RelayDesk 的崩溃转储符号化成可读的调用栈。
#
# 背景：分发版本崩溃后，用户回传的是 data/crashes/crash-<时间>/ 目录。那个目录里
# 的 report.txt 只有模块名和偏移量，因为 pdb 有几十 MB，不适合随诊断包回传。
# 本脚本用符号归档（out/symbols/v<版本>-<配置>/）把偏移量还原成函数名与行号。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\symbolize-crash.ps1 `
#       -CrashDirectory "D:\...\data\crashes\crash-20260615-101530"
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\symbolize-crash.ps1 `
#       -CrashDirectory <崩溃目录> -SymbolDirectory "D:\...\out\symbols\v61-Release"
#
# 说明：
#   * Windows 默认禁止运行 .ps1，所以上面显式加了 -ExecutionPolicy Bypass；
#     只对本次调用生效，不改变本机设置。
#   * 需要 Windows SDK 的 "Debugging Tools for Windows"（提供 cdb.exe）。
#     未安装时脚本不会失败，而是打印用 Visual Studio 手工分析的步骤。
#   * 结果写到崩溃目录下的 symbolized.txt。

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CrashDirectory,

    # pdb 所在目录；默认自动在 out/symbols 下查找与崩溃报告版本匹配的归档。
    [string]$SymbolDirectory,

    # 显式指定 cdb.exe；默认在常见安装位置和 PATH 中查找。
    [string]$DebuggerPath,

    # 传给 cdb 的额外搜索路径，例如微软公共符号服务器。
    [string]$ExtraSymbolPath = 'srv*https://msdl.microsoft.com/download/symbols'
)

$ErrorActionPreference = 'Stop'

function Write-Step
{
    param([string]$Message)
    Write-Host "[symbolize] $Message"
}

function Resolve-CrashDirectory
{
    param([string]$Path)
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $resolved)
    {
        throw "崩溃目录不存在：$Path"
    }
    return $resolved.Path
}

function Find-DumpFile
{
    param([string]$Directory)
    $dump = Join-Path $Directory 'crash.dmp'
    if (Test-Path -LiteralPath $dump)
    {
        return $dump
    }
    # 兼容 Windows 错误报告兜底生成的命名方式。
    $candidates = Get-ChildItem -LiteralPath $Directory -Filter '*.dmp' -File -ErrorAction SilentlyContinue
    if ($candidates.Count -gt 0)
    {
        return $candidates[0].FullName
    }
    throw "在 $Directory 下找不到 crash.dmp。请确认这是崩溃目录，而不是 data/crashes 根目录。"
}

function Get-ReportField
{
    param(
        [string]$ReportPath,
        [string]$FieldName
    )
    if (-not (Test-Path -LiteralPath $ReportPath))
    {
        return $null
    }
    $pattern = '^' + [regex]::Escape($FieldName) + '=(.*)$'
    foreach ($line in Get-Content -LiteralPath $ReportPath -Encoding UTF8)
    {
        if ($line -match $pattern)
        {
            return $Matches[1].Trim()
        }
    }
    return $null
}

function Find-CdbExecutable
{
    param([string]$ExplicitPath)

    if ($ExplicitPath)
    {
        if (Test-Path -LiteralPath $ExplicitPath)
        {
            return (Resolve-Path -LiteralPath $ExplicitPath).Path
        }
        throw "指定的调试器不存在：$ExplicitPath"
    }

    $onPath = Get-Command 'cdb.exe' -ErrorAction SilentlyContinue
    if ($onPath)
    {
        return $onPath.Source
    }

    $kitRoots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\Debuggers')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($root in $kitRoots)
    {
        $found = Get-ChildItem -LiteralPath $root -Recurse -Filter 'cdb.exe' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found)
        {
            return $found.FullName
        }
    }
    return $null
}

function Resolve-SymbolDirectory
{
    param(
        [string]$ExplicitDirectory,
        [string]$RequestedVersion,
        [string]$CrashFolder
    )

    if ($ExplicitDirectory)
    {
        if (-not (Test-Path -LiteralPath $ExplicitDirectory))
        {
            throw "指定的符号目录不存在：$ExplicitDirectory"
        }
        return (Resolve-Path -LiteralPath $ExplicitDirectory).Path
    }

    # 崩溃目录旁边如果放了 pdb，优先使用它，避免版本串台。
    $localPdb = Get-ChildItem -LiteralPath $CrashFolder -Filter '*.pdb' -File -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($localPdb)
    {
        return $CrashFolder
    }

    $archiveRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'out\symbols'
    if (-not (Test-Path -LiteralPath $archiveRoot))
    {
        return $null
    }
    if (-not $RequestedVersion)
    {
        return $null
    }

    $match = Get-ChildItem -LiteralPath $archiveRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "v$RequestedVersion-*" } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($match)
    {
        return $match.FullName
    }
    return $null
}

function Write-FallbackGuidance
{
    param(
        [string]$CrashFolder,
        [string]$DumpPath,
        [string]$Version,
        [string]$Configuration
    )

    Write-Host ''
    Write-Host '未找到 cdb.exe，无法自动符号化。' -ForegroundColor Yellow
    Write-Host '请选择以下任一方式：'
    Write-Host ''
    Write-Host '  方式一：安装 Windows SDK 的 "Debugging Tools for Windows"，然后重跑本脚本。'
    Write-Host ''
    Write-Host '  方式二：用 Visual Studio 手工分析（无需额外安装）：'
    Write-Host "    1. 找到本次构建的符号归档：out\symbols\v$Version-$Configuration\"
    Write-Host "    2. 把该目录下的 relaydesk_skiaui.exe 和 relaydesk_skiaui.pdb 复制到 $CrashFolder"
    Write-Host "    3. 用 Visual Studio 打开 $DumpPath"
    Write-Host '    4. 在“并行堆栈”窗口查看全部线程，当前线程停在异常点'
    Write-Host ''
    Write-Host '提示：pdb 必须与崩溃的 exe 严格来自同一次构建，否则行号会指向错误位置。'
}

$crashFolder = Resolve-CrashDirectory -Path $CrashDirectory
$dumpPath = Find-DumpFile -Directory $crashFolder
$reportPath = Join-Path $crashFolder 'report.txt'

$version = Get-ReportField -ReportPath $reportPath -FieldName 'version'
$configuration = Get-ReportField -ReportPath $reportPath -FieldName 'build_configuration'
$buildTimestamp = Get-ReportField -ReportPath $reportPath -FieldName 'build_timestamp'
$failingModule = Get-ReportField -ReportPath $reportPath -FieldName 'failing_module'
$failingOffset = Get-ReportField -ReportPath $reportPath -FieldName 'failing_module_offset'

Write-Step "崩溃目录：$crashFolder"
Write-Step "转储文件：$dumpPath"
Write-Step "版本：v$version ($configuration, $buildTimestamp)"
Write-Step "出错模块：$failingModule + $failingOffset"

$symbols = Resolve-SymbolDirectory -ExplicitDirectory $SymbolDirectory `
    -RequestedVersion $version -CrashFolder $crashFolder
if ($symbols)
{
    Write-Step "符号目录：$symbols"
}
else
{
    Write-Step '未找到匹配的符号目录，将只解析系统模块（RelayDesk 自身栈会缺少行号）。'
}

$cdb = Find-CdbExecutable -ExplicitPath $DebuggerPath
if (-not $cdb)
{
    Write-FallbackGuidance -CrashFolder $crashFolder -DumpPath $dumpPath `
        -Version $version -Configuration $configuration
    exit 2
}

Write-Step "调试器：$cdb"

$symbolPath = $ExtraSymbolPath
if ($symbols)
{
    $symbolPath = "$symbols;$symbolPath"
}

# cdb 的命令文件：先定位异常上下文，再看当前线程与全部线程的栈。
$commands = @(
    '.symfix',
    '.reload /f',
    '!analyze -v',
    '.ecxr',
    'kv',
    '~*kv',
    'lm',
    'q'
) -join '; '

$commandFile = Join-Path ([System.IO.Path]::GetTempPath()) ("relaydesk-symbolize-" + [guid]::NewGuid().ToString('N') + '.txt')
Set-Content -LiteralPath $commandFile -Value $commands -Encoding ASCII

$outputPath = Join-Path $crashFolder 'symbolized.txt'
try
{
    Write-Step '正在运行 cdb，完整内存转储可能需要几分钟……'
    & $cdb -z $dumpPath -y $symbolPath -cf $commandFile 2>&1 |
        Out-File -LiteralPath $outputPath -Encoding UTF8
    Write-Step "符号化结果：$outputPath"
}
finally
{
    Remove-Item -LiteralPath $commandFile -Force -ErrorAction SilentlyContinue
}

# 把崩溃摘要附在符号化结果前面，阅读时不必再来回切换文件。
$header = @(
    "=== 符号化结果 ===",
    "crash_directory=$crashFolder",
    "dump=$dumpPath",
    "app_version=$version",
    "build_configuration=$configuration",
    "build_timestamp=$buildTimestamp",
    "failing_module=$failingModule",
    "failing_module_offset=$failingOffset",
    "symbol_directory=$symbols",
    "debugger=$cdb",
    ""
) -join "`n"

$body = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
Set-Content -LiteralPath $outputPath -Value ($header + $body) -Encoding UTF8

exit 0
