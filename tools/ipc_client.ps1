param(
    [Parameter(Mandatory=$true)][string]$Command,
    [int]$TimeoutSeconds = 120
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
    public static extern bool CloseHandle(IntPtr hObject);
}
'@

$pipe = [IntPtr]::Zero
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

    # Read the response line. The server writes one JSON line per response.
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $readBuffer = New-Object byte[] 65536
    $response = ""
    while ([DateTime]::UtcNow -lt $deadline) {
        $read = [uint32]0
        if ([Kernel32]::ReadFile($pipe, $readBuffer, 65536, [ref]$read, [IntPtr]::Zero)) {
            if ($read -gt 0) {
                $response += [Text.Encoding]::UTF8.GetString($readBuffer, 0, [int]$read)
                if ($response.Contains("`n")) { break }
                continue
            }
        }
        # No data yet — poll (server may be processing a blocking command).
        Start-Sleep -Milliseconds 50
    }
    if ($response.Length -eq 0) {
        throw "IPC response timed out after ${TimeoutSeconds}s"
    }

    # Close the raw handle: guarantees the server's blocking ReadFile sees
    # ERROR_BROKEN_PIPE and returns to its accept loop (proven path — the
    # PipeServerTests R1 helper does exactly this).
    [Kernel32]::CloseHandle($pipe) | Out-Null
    Write-Output $response.TrimEnd("`r", "`n")
    exit 0
} catch {
    Write-Error "IPC error: $_"
    if ($pipe -ne [IntPtr]::Zero) { [Kernel32]::CloseHandle($pipe) | Out-Null }
    exit 1
}
