# OBD Wi-Fi Bridge

This firmware turns an original Bluetooth Classic-capable ESP32 into a transparent TCP-to-Bluetooth-SPP bridge for an OBDLink MX+. It never parses ELM, STN, OBD-II, CAN, prompts, line endings, or payload bytes.

## Architecture

```text
Application
    |
Raw TCP :35000
    |
Original ESP32
    |
Bluetooth Classic SPP
    |
OBDLink MX+
    |
Vehicle
```

Only one raw TCP client is accepted at a time. A small independent HTTP server exposes status at `GET /api/status`; it does not expose OBD commands.

## Hardware and initial configuration

- Board: original ESP32 (for example, ESP32 DevKit); ESP32-S3/C3/C6/P4 do not provide the required Classic SPP support.
- OBD device: `OBDLink MX+ 80685`, MAC `00:04:3E:5F:5E:9C`.
- Wi-Fi SSID: `GJNET-HA`, using DHCP.
- TCP bridge: port `35000`.
- HTTP status: port `80`.
- USB serial monitor: `115200` baud.

The project uses a custom 4 MiB dual-OTA partition table. Each application slot is `0x1A0000` bytes (1.625 MiB), with separate NVS, OTA metadata, core-dump, and SPIFFS partitions.

Copy `include/secrets.example.h` to `include/secrets.h` and enter the Wi-Fi credentials. The real secrets file is ignored by Git. On boot, values stored in the `obd-bridge` Preferences/NVS namespace override compiled defaults (`wifi_ssid`, `wifi_psk`, `obd_mac`, `tcp_port`, and `hostname`). A configuration UI is intentionally outside V1.

## Build, upload, and monitor

Install [PlatformIO](https://platformio.org/), then run:

```powershell
pio run
pio run --target upload
pio device monitor --baud 115200
```

The first installation, and any future partition-table change, requires USB flashing. Normal firmware updates can subsequently be installed over Wi-Fi.

Before upload, confirm the connected chip is an original ESP32 with Bluetooth Classic. The initial MX+ pairing may require pressing its physical **Pair** button while the serial log shows connection attempts. The firmware enables Secure Simple Pairing, logs the numeric confirmation and authentication result, and accepts the headless-device confirmation. It does not assume a legacy PIN. If a platform explicitly requests a PIN during troubleshooting, OBDLink documents `0000` as the fallback.

## Testing the raw socket

Replace `obd-bridge` with the DHCP address printed on the serial console if local name resolution is unavailable:

```powershell
$client = [System.Net.Sockets.TcpClient]::new("obd-bridge", 35000)
$stream = $client.GetStream()
$stream.ReadTimeout = 3000

$request = [Text.Encoding]::ASCII.GetBytes("ATI`r")
$stream.Write($request, 0, $request.Length)

$buffer = [byte[]]::new(4096)
$count = $stream.Read($buffer, 0, $buffer.Length)
[BitConverter]::ToString($buffer, 0, $count)
[Text.Encoding]::ASCII.GetString($buffer, 0, $count)

$request = [Text.Encoding]::ASCII.GetBytes("STI`r")
$stream.Write($request, 0, $request.Length)
$count = $stream.Read($buffer, 0, $buffer.Length)
[Text.Encoding]::ASCII.GetString($buffer, 0, $count)

$client.Dispose()
```

The bridge preserves every received byte, including CR, LF, echo, prompts, binary data, and continuous monitor traffic. It does not wait for `>`; that is solely a client concern. Test reconnects, an MX+ power cycle, Wi-Fi loss, continuous safe monitor output, and rejection of a second simultaneous TCP client before vehicle use.

## Status and operations

`GET /api/logs` returns a bounded, volatile history of the last 48 Bluetooth
discovery, authentication, connection, congestion, and error events, together
with cumulative `spp_rx_bytes` and `spp_write_completed_bytes`. Payloads are not
logged. Event status 0 means success for SPP events. History resets at reboot.
`/api/status` includes the build identifier, running OTA slot, and pending byte
counts in each bridge buffer. `tcp_to_bt_bytes` measures bytes accepted by the
BluetoothSerial queue; completed SPP writes are reported separately. Neither
counter alone proves that the remote command interpreter processed a command.

The TCP return path uses nonblocking socket sends because the Arduino 2.0.17
WiFiClient inherits `Print::availableForWrite()` returning zero. Partial sends
retain the unsent buffer tail for the next loop iteration.

```powershell
Invoke-RestMethod http://obd-bridge/api/status
Invoke-RestMethod -Method Post http://obd-bridge/api/bluetooth/reconnect
Invoke-RestMethod -Method Post http://obd-bridge/api/reboot
```

### OTA firmware update

Set a long password as `OTA_UPDATE_PASSWORD` in the ignored `include/secrets.h`. Build normally, then upload the generated application image with HTTP Basic authentication:

```powershell
pio run
$otaPassword = Read-Host "OTA password"
curl.exe --fail-with-body --user "admin:$otaPassword" `
    --form "firmware=@.pio/build/esp32dev/firmware.bin;type=application/octet-stream" `
    http://10.44.65.174/api/firmware
```

The ESP32 writes the inactive application slot, validates the image, changes the boot slot only after a successful upload, returns JSON, and reboots. An interrupted or invalid upload leaves the running slot intact. Keep this endpoint on the trusted IoT network; it is password protected but does not use TLS.

The TCP-to-Bluetooth outage buffer is bounded at 4096 bytes and Bluetooth-to-TCP buffering at 8192 bytes. Overflow is counted and logged rather than consuming unbounded memory. Bluetooth output received with no TCP owner is drained and counted as dropped so stale monitor data is not delivered to a later client.

## Security

Raw TCP port 35000 grants its network client effectively complete command access to the OBDLink and the vehicle buses it exposes. Use only on a trusted, isolated network. Do not expose port 35000 directly to the Internet.
