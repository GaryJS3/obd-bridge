# Remote bridge protocol, version 1

The ESP32 initiates a TLS WebSocket to `wss://car.garyjs.com/ws/device`. The reverse proxy terminates TLS and forwards WebSocket traffic to the ASP.NET Core container. The device sends `Authorization: Bearer <DEVICE_TOKEN>`, `X-Device-Id: obd-bridge-01`, and `X-Protocol-Version: 1` during the WebSocket handshake.

The server accepts one current connection for the configured device ID. A newly authenticated connection replaces the old socket. WebSocket messages are bounded; text control messages may not exceed 64 KiB. The ESP32 sends a heartbeat/status message every 10 seconds. An OBD command is never replayed after reconnect.

## Text messages

Device to server:

```json
{"version":1,"type":"hello","deviceId":"obd-bridge-01","firmwareVersion":"1.0","buildId":"Sep 18 2026 10:22:03","runningSlot":"ota_0","capabilities":["obd","status","diagnostics","ota"]}
{"version":1,"type":"status","uptimeMs":123456,"wifi":{"connected":true,"ssid":"home","ip":"192.168.1.25","rssi":-53},"bluetooth":{"connected":true,"targetName":"OBDLink MX+ 80685"},"bridge":{"owner":"cloud","pendingToObd":0,"pendingFromObd":0},"stats":{"cloudTxBytes":24,"cloudRxBytes":68,"wifiReconnects":1,"bluetoothReconnectAttempts":2}}
{"version":1,"type":"result","requestId":"request-guid","command":"control","success":true}
```

Server to device:

```json
{"version":1,"type":"command","requestId":"request-guid","command":"control","parameters":{"enabled":true}}
{"version":1,"type":"command","requestId":"request-guid","command":"bluetooth.reconnect","parameters":{}}
{"version":1,"type":"command","requestId":"request-guid","command":"diagnostics.get","parameters":{}}
{"version":1,"type":"ota.begin","transferId":"transfer-guid","totalBytes":123456,"sha256":"lowercase-sha256","expectedBuild":"optional build id"}
{"version":1,"type":"ota.commit","transferId":"transfer-guid"}
{"version":1,"type":"ota.abort","transferId":"transfer-guid"}
```

The device answers control/management messages with one `result` containing the matching request ID. Diagnostics are sent as a `diagnostics` event. OTA uses `otaReady`, `otaAck`, and `otaResult` messages with the same transfer ID. A failed request includes a short `error` string and never includes credentials.

## Binary messages

Binary frames are one complete protocol message; WebSocket continuation frames are reassembled by the WebSocket library before protocol parsing.

| First byte | Direction | Remaining bytes |
|---:|---|---|
| `0x01` | Server to ESP32 | Raw OBD bytes to the OBDLink |
| `0x02` | ESP32 to server | Raw OBD bytes from the OBDLink |
| `0x03` | Server to ESP32 | 4-byte unsigned big-endian firmware offset, then firmware bytes |

OBD data is unchanged after the one-byte transport channel. CR/LF, binary bytes, prompts, echo, and continuous monitor output are preserved.

## OTA state flow

1. Browser uploads the complete image over HTTPS. The server bounds and spools it to a temporary file, then calculates SHA-256.
2. Server sends `ota.begin`; the ESP32 validates size and available OTA slot and answers `otaReady`.
3. Server streams `0x03` frames in 8192-byte chunks. The ESP32 requires the exact next offset and acknowledges each 64 KiB boundary and the final byte count.
4. Server sends `ota.commit`. The ESP32 verifies the full byte count and SHA-256, calls the framework image verifier, selects the inactive slot, replies with `otaResult`, and reboots.
5. Server waits up to 75 seconds for a new device connection and verifies that the running OTA slot changed.

Before commit, any error triggers `ota.abort` where the connection remains available. The device also times out and aborts a stalled transfer. The current running slot stays selected on incomplete transfer or hash/image verification failure.
