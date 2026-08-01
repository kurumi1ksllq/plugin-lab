param([Parameter(Mandatory=$true)][string]$Command)

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
    $response = $reader.ReadLine()
    Write-Output $response

    $pipe.Dispose()
} catch {
    Write-Error "IPC error: $_"
    exit 1
}
