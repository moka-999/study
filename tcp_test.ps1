$tcp = New-Object System.Net.Sockets.TcpClient
try {
    $tcp.Connect('192.168.1.201', 8888)
    Write-Host 'TCP 8888 CONNECTED'
    $stream = $tcp.GetStream()
    $bytes = [System.Text.Encoding]::ASCII.GetBytes('Hello STM32!')
    $stream.Write($bytes, 0, $bytes.Length)
    Start-Sleep -Milliseconds 500
    $buf = New-Object byte[] 256
    if ($stream.DataAvailable) {
        $n = $stream.Read($buf, 0, $buf.Length)
        $resp = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        Write-Host "RX: $resp"
    } else {
        Write-Host 'No response within 500ms'
    }
    $tcp.Close()
} catch {
    Write-Host "FAIL: $_"
}
