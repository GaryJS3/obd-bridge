# OBD Wi-Fi Bridge

This project connects an OBDLink MX+ through an original Bluetooth Classic-capable ESP32 to a private remote dashboard. The ESP32 keeps an outbound TLS WebSocket to the container, so it remains reachable while the car moves between known Wi-Fi networks. OBD bytes remain transparent; the bridge does not parse ELM, STN, OBD-II, CAN, prompts, line endings, or payload bytes.

## Architecture

```text
Browser -- HTTPS / WebSocket --> ASP.NET Core container -- authenticated WSS --> ESP32
                                                                         |
                                                               Bluetooth Classic SPP
                                                                         |
                                                                   OBDLink MX+
                                                                         |
                                                                      Vehicle

Local diagnostic client -- TCP :35000 --> ESP32 (only while cloud control is idle)
```

Only one command owner is accepted at a time. The container keeps the device connection open; a browser may own commands while other logged-in browsers observe. The local TCP bridge remains available when there is no cloud owner. Neither local TCP port 35000 nor the ESP32 HTTP endpoints should be exposed directly to the Internet.

## Remote dashboard and Docker service

The C# ASP.NET Core service, device protocol, and dashboard live in [`obd-bridge-web`](obd-bridge-web/README.md). The root `compose.yaml` builds `obd-bridge-web/Dockerfile`, persists cookie keys in a named `/data` volume, and binds HTTP to `127.0.0.1:8080` by default. In Dockhand, create a Git Compose stack from the repository root and configure `WEB_PASSWORD` and `DEVICE_TOKEN` as stack environment secrets. See the service README for reverse-proxy binding details.

Put `car.gary.systems` behind the HTTPS reverse proxy. Enable WebSocket upgrades and permit request bodies up to 2 MiB for firmware uploads. Configure long-lived WebSocket idle timeouts. The web app still performs its own password and device-token checks.

The ESP32 makes only outbound `wss://car.gary.systems/ws/device` connections. Its device token is separate from the dashboard password. The TLS root certificate must be configured in ignored `include/secrets.h` as `CLOUD_CA_CERT`; certificate validation is required and there is no insecure mode.

### ESP32 setup for remote access

1. Copy `include/secrets.example.h` to ignored `include/secrets.h`.
2. Set the Wi-Fi profiles, device ID, device token, and CA certificate. The token must match Dockhand's `DEVICE_TOKEN`; the device ID must match `DEVICE_ID`.
3. Set `CLOUD_WS_URL` to the public WebSocket path if it differs from `wss://car.gary.systems/ws/device`.
4. Build with `pio run`.
5. Flash the new partition map over USB once, then reboot and check the serial log for the cloud connection. After this migration, firmware can be uploaded through the dashboard.

The OTA slots were enlarged to 1.875 MiB by reducing the unused SPIFFS partition to 128 KiB. This changes partition offsets, so the current OTA image cannot safely install the new layout by itself. A USB flash is required once to install both the new partition table and firmware. Normal later images are streamed through the container, hash-verified by the ESP32, and written to the inactive slot. A failed or interrupted transfer does not select the new slot.

The complete frame and OTA contract is in [`Protocol.md`](obd-bridge-web/Protocol.md). Deployment and reverse-proxy settings are in [`obd-bridge-web/README.md`](obd-bridge-web/README.md).

## Hardware and initial configuration

- Board: original ESP32 (for example, ESP32 DevKit); ESP32-S3/C3/C6/P4 do not provide the required Classic SPP support.
- OBD device: `OBDLink MX+ 80685`, MAC `00:04:3E:5F:5E:9C`.
- Wi-Fi SSID: `GJNET-HA`, using DHCP.
- TCP bridge: port `35000`.
- HTTP status: port `80`.
- USB serial monitor: `115200` baud.

The project uses a custom 4 MiB dual-OTA partition table. Each application slot is `0x1E0000` bytes (1.875 MiB), with separate NVS, OTA metadata, core-dump, and SPIFFS partitions.

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
