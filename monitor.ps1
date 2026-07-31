# Plugin Lab 实时调试监控脚本
# 用法: pwsh -File monitor.ps1
# 功能:
#   1. 启动 Plugin Lab
#   2. 实时 tail 日志文件 (每 0.5 秒读新增内容)
#   3. 持续监控窗口响应性 (Responding 状态)
#   4. 应用退出/崩溃时停止并显示总结
#   5. 检测崩溃转储文件 (.dmp)

$ErrorActionPreference = 'Continue'
$logPath = "$env:TEMP\pluginlab_crashlog.txt"
$dumpPath = "$env:TEMP\pluginlab_crash.dmp"
$appPath = "D:\Documents\PluginLab\build\PluginLab_artefacts\Release\Plugin Lab.exe"

Write-Host "=== Plugin Lab 实时监控 ===" -ForegroundColor Cyan
Write-Host "日志: $logPath"
Write-Host "崩溃转储: $dumpPath"

# 清理旧文件
Remove-Item $logPath -ErrorAction SilentlyContinue
Remove-Item $dumpPath -ErrorAction SilentlyContinue

# 启动应用
Write-Host "`n[启动] Plugin Lab..." -ForegroundColor Yellow
$proc = Start-Process -FilePath $appPath -PassThru
Write-Host "[启动] PID=$($proc.Id)"

# 实时监控循环
$lastSize = 0
$lineCount = 0
$startTime = Get-Date
$lastResponding = $null

while (-not $proc.HasExited) {
    # 检查日志新增内容
    if (Test-Path $logPath) {
        $size = (Get-Item $logPath).Length
        if ($size -gt $lastSize) {
            $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
            $stream.Position = $lastSize
            $reader = [System.IO.StreamReader]::new($stream)
            while (-not $reader.EndOfStream) {
                $line = $reader.ReadLine()
                if ($line) {
                    $lineCount++
                    Write-Host "[$($lineCount.ToString('000'))] $line" -ForegroundColor Gray
                }
            }
            $reader.Close(); $stream.Close()
            $lastSize = $size
        }
    }

    # 检查响应性变化
    $proc.Refresh()
    if ($proc.Responding -ne $lastResponding) {
        $state = if ($proc.Responding) { "响应正常" } else { "!! 未响应 !!" }
        $color = if ($proc.Responding) { 'Green' } else { 'Red' }
        Write-Host "`n[状态] $state (t=$([math]::Round(((Get-Date)-$startTime).TotalSeconds))s)" -ForegroundColor $color
        $lastResponding = $proc.Responding
    }

    Start-Sleep -Milliseconds 500
}

# 应用退出
$elapsed = [math]::Round(((Get-Date)-$startTime).TotalSeconds, 1)

Write-Host "`n=== 应用退出 ===" -ForegroundColor Cyan
Write-Host "运行时长: ${elapsed}s"
Write-Host "日志行数: $lineCount"
Write-Host "退出码: $($proc.ExitCode)"

# 检查崩溃转储
if (Test-Path $dumpPath) {
    $dmpSize = (Get-Item $dumpPath).Length
    Write-Host "`n!! 检测到崩溃转储文件: $dumpPath ($dmpSize bytes)" -ForegroundColor Red
} else {
    Write-Host "`n未生成崩溃转储 (正常退出或未触发崩溃过滤器)"
}

# 显示最后日志
Write-Host "`n=== 最后 5 条日志 ===" -ForegroundColor Cyan
if (Test-Path $logPath) {
    Get-Content $logPath -Tail 5
}
