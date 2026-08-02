param(
    [Parameter(Mandatory=$true)][string]$Command,
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Stop"

try {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'PluginLab', [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(30000)
    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($pipe)

    $writer.WriteLine($Command)
    $pipe.WaitForPipeDrain()

    # Read response — the server may write one JSON line per response
    $task = $reader.ReadLineAsync()
    if (-not $task.Wait([TimeSpan]::FromSeconds($TimeoutSeconds))) {
        throw "IPC response timed out after ${TimeoutSeconds}s"
    }
    $response = $task.Result
    Write-Output $response

    $pipe.Dispose()
} catch {
    Write-Error "IPC error: $_"
    exit 1
}
