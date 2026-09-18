param([string]$Address = '10.44.65.174')
$ErrorActionPreference = 'Stop'
$client = [Net.Sockets.TcpClient]::new($Address, 35000)
try
{
    $stream = $client.GetStream()
    $stream.ReadTimeout = 5000
    foreach ($command in @('ATI', 'STI'))
    {
        $request = [Text.Encoding]::ASCII.GetBytes($command + "`r")
        $stream.Write($request, 0, $request.Length)
        $buffer = [byte[]]::new(4096)
        $received = [IO.MemoryStream]::new()
        try
        {
            $stream.ReadTimeout = 5000
            do
            {
                $count = $stream.Read($buffer, 0, $buffer.Length)
                if ($count -eq 0) { break }
                $received.Write($buffer, 0, $count)
                $stream.ReadTimeout = 700
            } while ($received.Length -lt 16384)
        }
        catch [IO.IOException]
        {
            # A bounded idle read ends capture without parsing adapter prompts.
        }
        $bytes = $received.ToArray()
        [pscustomobject]@{
            Command = $command
            RequestHex = [BitConverter]::ToString($request)
            ResponseHex = [BitConverter]::ToString($bytes)
            Text = [Text.Encoding]::ASCII.GetString($bytes).Replace("`r", '<CR>').Replace("`n", '<LF>')
        }
        $received.Dispose()
    }
}
finally
{
    $client.Dispose()
}
