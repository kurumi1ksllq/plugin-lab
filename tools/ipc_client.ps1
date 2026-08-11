param(
    [Parameter(Mandatory=$true)][string]$Command,
    [int]$TimeoutSeconds = 120,
    [int]$CancelAfterMs = 0
)

$ErrorActionPreference = "Stop"

# Native named-pipe client (kernel32 P/Invoke), byte-identical to the
# PipeServerTests connectClient() helper. .NET NamedPipeClientStream wraps
# the handle with buffering and does not reliably release the OS pipe handle
# on Dispose (D4: getScanStatus then loadPlugin timed out — the server's
# blocking ReadFile in PIPE_WAIT message mode never saw EOF, so the
# single-connection accept loop never returned). CreateFileA + WriteFile +
# ReadFile + CloseHandle is the proven-disconnect path (PipeServerTests R1).
$script:Kernel32Def = @'
using System;
using System.Runtime.InteropServices;
public static class Kernel32
{
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
    public static extern IntPtr CreateFileA(string lpFileName, uint dwDesiredAccess,
        uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool WriteFile(IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToWrite,
        out uint lpNumberOfBytesWritten, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadFile(IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToRead,
        out uint lpNumberOfBytesRead, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool PeekNamedPipe(IntPtr hNamedPipe, byte[] lpBuffer, uint nBufferSize,
        out uint lpBytesRead, out uint lpTotalBytesAvail, out uint lpBytesLeftThisMessage);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr hObject);
}
'@

$pipe = [IntPtr]::Zero
$finalResponse = $null
try {
    Add-Type -TypeDefinition $script:Kernel32Def -ErrorAction Stop

    # Connect with retries: the server thread needs time to create the pipe
    # instance; OPEN_EXISTING fails with FILE_NOT_FOUND until it exists.
    for ($attempt = 0; $attempt -lt 200 -and $pipe -eq [IntPtr]::Zero; ++$attempt) {
        $pipe = [Kernel32]::CreateFileA(
            '\\.\pipe\PluginLab',
            3221225472, # GENERIC_READ | GENERIC_WRITE
            0, [IntPtr]::Zero,
            3,               # OPEN_EXISTING
            0, [IntPtr]::Zero)
        if ($pipe -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 50
        }
    }
    if ($pipe -eq [IntPtr]::Zero) {
        throw "Connect failed (last error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }

    # Send the command as one message (newline-terminated, JSON line).
    $bytes = [Text.Encoding]::UTF8.GetBytes($Command + "`n")
    $written = [uint32]0
    if (-not [Kernel32]::WriteFile($pipe, $bytes, [uint32]$bytes.Length, [ref]$written, [IntPtr]::Zero)) {
        throw "WriteFile failed (last error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }

    # Issue #3: optionally send {"cmd":"stop"} on the SAME connection after
    # $CancelAfterMs ms — the server serves control commands inline while the
    # long command runs on its worker, so stop is reachable mid-measure.
    $sendTime = [DateTime]::UtcNow
    $cancelSent = $false
    $stopBytes = [Text.Encoding]::UTF8.GetBytes('{"cmd":"stop"}' + "`n")

    # Read response LINES until the FINAL response. The server writes one
    # message per line; progress lines ({"ok":true,"progress":...}) and
    # control acks ({"ok":true}) are intermediate and go to stderr — the final
    # response carries "samples" / "export_path" / "error" and goes to stdout.
    # PeekNamedPipe (non-blocking) instead of a blocking ReadFile, so the
    # cancel can fire while the server is busy (mirrors the server's poll).
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $readBuffer = New-Object byte[] 65536
    $pending = ""
    while ([DateTime]::UtcNow -lt $deadline) {
        $avail = [uint32]0; $total = [uint32]0; $left = [uint32]0
        $peeked = [Kernel32]::PeekNamedPipe($pipe, $null, 0, [ref]$avail, [ref]$total, [ref]$left)
        if ($peeked -and $total -gt 0) {
            $read = [uint32]0
            if ([Kernel32]::ReadFile($pipe, $readBuffer, 65536, [ref]$read, [IntPtr]::Zero) -and $read -gt 0) {
                $pending += [Text.Encoding]::UTF8.GetString($readBuffer, 0, [int]$read)
                while ($pending.Contains("`n")) {
                    $nl = $pending.IndexOf("`n")
                    $line = $pending.Substring(0, $nl).TrimEnd("`r")
                    $pending = $pending.Substring($nl + 1)
                    if ($line -match '"samples"|"export_path"|"error"') {
                        $finalResponse = $line
                        break
                    }
                    # Intermediate line (progress / control ack) — stderr.
                    if ($line) { [Console]::Error.WriteLine($line) }
                }
                if ($null -ne $finalResponse) { break }
            }
        } elseif (-not $peeked) {
            # Pipe broken — the server closed the connection.
            break
        }

        # Fire the cancel after the requested delay (issue #3).
        if ($CancelAfterMs -gt 0 -and -not $cancelSent -and
            ([DateTime]::UtcNow - $sendTime).TotalMilliseconds -ge $CancelAfterMs) {
            $written2 = [uint32]0
            [Kernel32]::WriteFile($pipe, $stopBytes, [uint32]$stopBytes.Length, [ref]$written2, [IntPtr]::Zero) | Out-Null
            $cancelSent = $true
            [Console]::Error.WriteLine("-- stop sent after ${CancelAfterMs}ms --")
        }

        Start-Sleep -Milliseconds 50
    }
    if ($null -eq $finalResponse) {
        throw "IPC response timed out after ${TimeoutSeconds}s"
    }

    # Close the raw handle: guarantees the server's blocking ReadFile sees
    # ERROR_BROKEN_PIPE and returns to its accept loop (proven path — the
    # PipeServerTests R1 helper does exactly this).
    [Kernel32]::CloseHandle($pipe) | Out-Null
    Write-Output $finalResponse
    exit 0
} catch {
    Write-Error "IPC error: $_"
    if ($pipe -ne [IntPtr]::Zero) { [Kernel32]::CloseHandle($pipe) | Out-Null }
    exit 1
}
