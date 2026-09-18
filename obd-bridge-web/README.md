# OBD Bridge web container

This dependency-light ASP.NET Core service serves the private car dashboard, authenticates the ESP32's outbound WebSocket, relays raw OBD bytes, and streams verified firmware images through the same device connection.

## Deployment with Dockhand

Use the Git repository root as the Compose stack directory. The root `compose.yaml` builds `obd-bridge-web/Dockerfile`, exposes container port `8080`, and persists login cookie encryption keys in a named volume mounted at `/data`.

Set these environment variables as Dockhand secrets or configuration:

| Variable | Required | Default / rule |
|---|---|---|
| `WEB_PASSWORD` | Yes | At least 12 characters. Used only by the website. |
| `DEVICE_TOKEN` | Yes | At least 32 characters. Must match the firmware token. |
| `PUBLIC_ORIGIN` | Yes for public deployment | `https://car.gary.systems`; HTTPS origin only, no path. |
| `DEVICE_ID` | No | `obd-bridge-01`; must match firmware. |
| `OTA_MAX_BYTES` | No | `1800000`; maximum accepted firmware body, hard cap `1800000`. |
| `AUTH_COOKIE_DAYS` | No | `30`; clamped to 1–90. |
| `DEVICE_OFFLINE_SECONDS` | No | `30`; clamped to 10–300. |
| `ASPNETCORE_HTTP_PORTS` | No | `8080`. |
| `DATA_PROTECTION_PATH` | No | Docker sets `/data/keys`. |

Startup fails if the website password, device token, or HTTPS public origin is missing or invalid. Generate two different random secrets; do not reuse the website password as the device token. A PowerShell example for generating a token is:

```powershell
[Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(32)).ToLowerInvariant()
```

## Reverse proxy

Terminate TLS at the reverse proxy and forward HTTP to container port `8080`. Configure it to:

- pass WebSocket upgrades for `/ws/browser` and `/ws/device`;
- allow request bodies through 2 MiB for `/api/device/ota`;
- set a long WebSocket idle timeout (at least 2 minutes);
- disable caching for `/api/*` and `/ws/*`;
- preserve the browser's `Origin` header.

The public origin must be exactly `https://car.gary.systems` (or your actual HTTPS hostname). The app compares browser mutation and WebSocket origins against it. The app always marks its authentication cookie `Secure`, even though TLS ends at the proxy. Login redirects are path-relative so the browser stays on HTTPS when the proxy forwards plain HTTP to the container.

The proxy must route `/login`, `/`, `/api/*`, and both WebSocket paths to this container. Do not add an additional proxy login layer that blocks the ESP32 device path; the container authenticates devices with its own bearer token. No public raw TCP port is needed.

The Compose file binds host port `8080` to loopback (`127.0.0.1`) by default, suitable when the reverse proxy runs directly on the Dockhand host. Set `OBD_BRIDGE_BIND` to the Dockhand host's private interface address if the proxy reaches it over the host network, and restrict that port to the proxy with the host firewall. If the proxy is another container, connect both services to the same existing Docker network and route to `obd-bridge:8080`; do not expose port `8080` publicly.

## Local build and run

Install the .NET 10 SDK. From the repository root:

```powershell
$env:WEB_PASSWORD = Read-Host "Dashboard password"
$env:DEVICE_TOKEN = Read-Host "Device token"
$env:PUBLIC_ORIGIN = "https://car.gary.systems"
$env:ASPNETCORE_HTTP_PORTS = "8080"
dotnet run --project .\obd-bridge-web\ObdBridge.Web.csproj
```

For browser tests, use the configured HTTPS origin through a reverse proxy or a local HTTPS development host. The cookie is `Secure` by design. The container Dockerfile uses a multi-stage build, runs as the non-root ASP.NET `app` user, persists data-protection keys under `/data/keys`, and probes `/healthz` through the built-in .NET healthcheck mode.

Run the container and protocol integration tests from the repository root with `dotnet test .\obd-bridge-web.Tests\ObdBridge.Web.Tests.csproj`.

## Browser behavior

- A single password creates a secure, HTTP-only, same-site cookie; there is no user database.
- One browser session at a time owns OBD commands. The lease renews while the browser remains active and the ESP32 is told to release control when the lease expires or the socket closes.
- Other authenticated browsers can watch state and raw OBD output but cannot send commands.
- Reboot and OTA require the dashboard password to be entered again.
- Only recent status and diagnostic events are held in memory. Raw OBD payloads are not logged or persisted.

## Device connection and remote Wi-Fi

The ESP32 makes the TLS WebSocket connection outbound to `wss://car.gary.systems/ws/device`, so home, work, and hotspot networks need no inbound firewall rule. The server checks `Authorization: Bearer ...`, `X-Device-Id`, and protocol version 1. A new valid socket replaces an older connection for that device ID.

In `include/secrets.h`, set the same device token and ID, the WebSocket URL, a PEM CA certificate for the reverse proxy's server certificate, and up to three Wi-Fi SSID/password pairs. Express PEM line breaks as `\n` inside the C string literal. Keep `include/secrets.h` untracked. The firmware waits for the system clock before TLS certificate validation and rotates through configured networks after a disconnect. Certificate verification is mandatory.

## Remote OTA

Select the `.bin` firmware image, acquire command control, re-enter the website password, and start the upload. The container streams the request to a bounded temporary file, verifies its size, computes SHA-256, and then transfers 8 KiB offset-tagged frames over the device WebSocket. The ESP32 requires the exact byte offset, writes to the inactive partition, verifies SHA-256 and the framework image structure, and switches slots only after verification. The dashboard waits up to 75 seconds for the ESP32 to return on the other OTA slot.

The maximum firmware image defaults to 1,800,000 bytes. The partition slot is 1,966,080 bytes. A USB flash is required once to install the enlarged partition table; afterwards remote OTA can update application firmware. The existing LAN `/api/firmware` endpoint remains a recovery path.

If the update is interrupted before commit, the ESP32 aborts the inactive-slot write and continues running the current slot. If it accepts the image but does not reconnect, use the ESP32's LAN or USB recovery path. See [`Protocol.md`](Protocol.md) for message framing and update states.

## Health

- `GET /healthz` reports process health.
- `GET /readyz` reports that the web service can accept requests.
- Device availability is shown separately; an offline car does not make the container unhealthy.
