<#
    FileLocksmith 本机自测脚本
    与 CI（.github/workflows/ci.yml）里跑的是同一套用例，只是把 exe 路径参数化了。

    用法：
        pwsh -File tools\run-local-tests.ps1 -Exe .\FileLocksmith-x64.exe
        powershell -ExecutionPolicy Bypass -File tools\run-local-tests.ps1

    说明：
      * 会在 %TEMP% 下建一个 fl-local-test 目录放测试文件；
      * 会临时占用/结束自己启动的 powershell 测试进程，不会碰其它程序；
      * 会临时注册再移除右键菜单、创建再删除「发送到」快捷方式；
      * 失败会用非 0 退出码返回，方便接到别的脚本里。
#>
param(
    [string]$Exe = ".\FileLocksmith-x64.exe"
)

$ErrorActionPreference = 'Stop'
$script:Passed = 0
$script:Failed = 0

function Write-Case([string]$Text) { Write-Host "`n=== $Text" -ForegroundColor Cyan }
function Write-Ok([string]$Text) { $script:Passed++; Write-Host "  [通过] $Text" -ForegroundColor Green }
function Write-Bad([string]$Text) { $script:Failed++; Write-Host "  [失败] $Text" -ForegroundColor Red }

function Invoke-FL {
    param([string[]]$Arguments)
    $outFile = New-TemporaryFile
    $errFile = New-TemporaryFile
    $p = Start-Process -FilePath $ExePath -ArgumentList $Arguments -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput $outFile.FullName -RedirectStandardError $errFile.FullName
    $stdout = Get-Content $outFile.FullName -Raw -Encoding UTF8
    $stderr = Get-Content $errFile.FullName -Raw -Encoding UTF8
    Remove-Item $outFile.FullName, $errFile.FullName -Force -ErrorAction SilentlyContinue
    if ($null -eq $stdout) { $stdout = '' }
    if ($null -eq $stderr) { $stderr = '' }
    [pscustomobject]@{ ExitCode = $p.ExitCode; StdOut = $stdout; StdErr = $stderr }
}

# 让后台 powershell 独占占用一个文件（FileShare::None），返回进程对象
function Start-Locker([string]$Path) {
    Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-NonInteractive', '-Command',
        "`$f=[System.IO.File]::Open('$Path','Open','ReadWrite','None'); Start-Sleep -Seconds 300"
    )
}

function Test-FileIsFree([string]$Path) {
    try {
        $probe = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $probe.Close(); $probe.Dispose()
        return $true
    } catch {
        return $false
    }
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到 exe：$Exe" }
$ExePath = (Resolve-Path -LiteralPath $Exe).Path
Write-Host "被测程序: $ExePath"
$version = (Invoke-FL @('--version')).StdOut.Trim()
Write-Host "版本信息:`n$version"

$dir = Join-Path $env:TEMP 'fl-local-test'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$file = Join-Path $dir 'locked.txt'
Set-Content -Path $file -Value 'hello'

# --- 用例 1：检出占用者 ---
Write-Case "用例 1 / 检出占用者"
Write-Host "  测试文件: $file"
$stream = [System.IO.File]::Open($file, 'Open', 'ReadWrite', 'None')
Write-Host "  本进程 PID = $PID 独占占用该文件"
$r = Invoke-FL @($file, '--json', '--cli')
$stream.Close(); $stream.Dispose()
Write-Host "  stdout: $($r.StdOut.Trim())"
if ($r.ExitCode -ne 0) { Write-Bad "退出码 $($r.ExitCode)" }
elseif ($r.StdOut -notmatch '"pid"\s*:\s*' + $PID + '\b') { Write-Bad "没检出占用者 PID $PID" }
else { Write-Ok "检出占用进程 $PID（进程名/用户名/路径都对的话，可以在输出里核对）" }

# --- 用例 2：无效路径要被明确报出 ---
Write-Case "用例 2 / 打不开的路径不能当成`"没有占用`""
$ghost = Join-Path $dir 'does-not-exist.txt'
$r2 = Invoke-FL @($ghost, '--json', '--cli')
Write-Host "  stdout: $($r2.StdOut.Trim())"
if ($r2.StdOut -notmatch '"unresolved"' -or $r2.StdOut -notmatch 'does-not-exist') {
    Write-Bad "无效路径没有被报出来"
} else { Write-Ok "无效路径出现在 unresolved 里" }

# --- 用例 3：结束占用进程 ---
Write-Case "用例 3 / --kill 结束占用进程"
$locker = Start-Locker $file
Start-Sleep -Seconds 5
$r3 = Invoke-FL @($file, '--kill', '--cli')
Write-Host "  stdout: $($r3.StdOut.Trim())"
Start-Sleep -Seconds 3
if (-not $locker.HasExited) {
    Stop-Process -Id $locker.Id -Force -ErrorAction SilentlyContinue
    Write-Bad "占用进程 $($locker.Id) 没被结束"
} else { Write-Ok "占用进程 $($locker.Id) 已被结束" }

# --- 用例 4：强制解除占用（关句柄，不结束进程）---
Write-Case "用例 4 / --unlock 强制解除占用（进程要活着）"
$locker2 = Start-Locker $file
Start-Sleep -Seconds 5
$r4 = Invoke-FL @($file, '--unlock', '--cli')
Write-Host "  stdout: $($r4.StdOut.Trim())"
Start-Sleep -Seconds 2
$alive = -not $locker2.HasExited
$free = Test-FileIsFree $file
Stop-Process -Id $locker2.Id -Force -ErrorAction SilentlyContinue
if (-not $alive) { Write-Bad "占用进程被结束了，强制解锁不应该结束进程" }
elseif (-not $free) { Write-Bad "句柄关掉了但文件仍打不开" }
else { Write-Ok "文件已解锁，进程 $($locker2.Id) 仍在运行" }

# --- 用例 5：--json 的 stdout 必须只有 JSON ---
Write-Case "用例 5 / --json 输出纯净性（诊断走 stderr）"
$locker3 = Start-Locker $file
Start-Sleep -Seconds 5
$r5 = Invoke-FL @($file, '--json', '--unlock', '--cli')
Stop-Process -Id $locker3.Id -Force -ErrorAction SilentlyContinue
Write-Host "  stdout: $($r5.StdOut.Trim())"
Write-Host "  stderr: $($r5.StdErr.Trim())"
try { $null = $r5.StdOut.Trim() | ConvertFrom-Json } catch { Write-Bad "stdout 不是合法 JSON：$_" }
if ($script:Failed -eq 0) {
    if ($r5.StdErr -notmatch '已强制关闭') { Write-Bad "诊断信息没走 stderr" }
    else { Write-Ok "stdout 只有一行 JSON，说明信息在 stderr" }
}

# --- 用例 6：右键菜单开启 / 关闭 ---
Write-Case "用例 6 / 资源管理器右键菜单开关（写入 HKCU，不需要管理员）"
$menuKey = 'HKCU:\Software\Classes\*\shell\FileLocksmith'
$r6 = Invoke-FL @('--register', '--cli')
Write-Host "  stdout: $($r6.StdOut.Trim())"
if (-not (Test-Path -LiteralPath $menuKey)) { Write-Bad "注册后 $menuKey 不存在" }
else {
    $command = (Get-Item -LiteralPath "$menuKey\command").GetValue('')
    Write-Host "  注册的命令: $command"
    $r7 = Invoke-FL @('--unregister', '--cli')
    if (Test-Path -LiteralPath $menuKey) { Write-Bad "--unregister 之后注册表项还在" }
    elseif ($command -notmatch [regex]::Escape('"%1"')) { Write-Bad "命令里没有 %1 参数" }
    else { Write-Ok "可以注册也可以移除，命令指向 $([IO.Path]::GetFileName($ExePath)) `"%1`"" }
}

# --- 用例 7：「发送到」快捷方式 ---
Write-Case "用例 7 / 添加到「发送到」菜单"
$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\SendTo\FileLocksmith.lnk'
$r8 = Invoke-FL @('--sendto', '--cli')
Write-Host "  stdout: $($r8.StdOut.Trim())"
if ($r8.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $lnk)) { Write-Bad "快捷方式没创建出来" }
else {
    Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue
    Write-Ok "已创建并清理「发送到」快捷方式（测试后已删除）"
}

# --- 用例 8：无占用时不误报 ---
Write-Case "用例 8 / 没有占用时不误报"
$r9 = Invoke-FL @($file, '--json', '--cli')
Write-Host "  stdout: $($r9.StdOut.Trim())"
if ($r9.StdOut -match '"pid"') { Write-Bad "文件已经没人占用了，却还报告占用" }
else { Write-Ok "正确地报告没有占用" }

# --- 汇总 ---
Write-Host "`n================ 结果 ================" -ForegroundColor Cyan
Write-Host "通过 $script:Passed 项，失败 $script:Failed 项"
if ($script:Failed -gt 0) {
    Write-Host "把上面的完整输出（含 --version 那段）贴出来即可定位问题。" -ForegroundColor Yellow
    exit 1
}
Write-Host "全部通过。" -ForegroundColor Green
exit 0
