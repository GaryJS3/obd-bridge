using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace ObdBridge.Web;

public sealed class BridgeCoordinator(AppOptions options, ILogger<BridgeCoordinator> logger) : BackgroundService
{
    private const int MaxMessageBytes = 64 * 1024;
    private const int OtaChunkBytes = 8192;
    private readonly object _sync = new();
    private readonly HashSet<SocketPeer> _browsers = [];
    private readonly ConcurrentDictionary<string, TaskCompletionSource<JsonElement>> _requests = new();
    private readonly ConcurrentDictionary<int, TaskCompletionSource<bool>> _otaAcks = new();
    private SocketPeer? _device;
    private string? _controlOwner;
    private DateTimeOffset _controlExpires = DateTimeOffset.MinValue;
    private JsonElement _hello;
    private JsonElement _deviceStatus;
    private DateTimeOffset? _lastSeen;
    private bool _otaActive;
    private volatile bool _awaitingOtaReconnect;
    private readonly SemaphoreSlim _otaLock = new(1, 1);
    private TaskCompletionSource<JsonElement>? _otaReady;
    private TaskCompletionSource<JsonElement>? _otaResult;
    private TaskCompletionSource<JsonElement>? _reconnected;
    private string? _activeTransferId;

    public object Snapshot(string? browserId = null)
    {
        lock (_sync)
        {
            var deviceOnline = IsDeviceOnlineLocked();
            return new
            {
                deviceOnline,
                deviceId = options.DeviceId,
                lastSeen = _lastSeen,
                hello = _hello.ValueKind == JsonValueKind.Undefined ? (JsonElement?)null : _hello,
                status = _deviceStatus.ValueKind == JsonValueKind.Undefined ? (JsonElement?)null : _deviceStatus,
                control = new { owned = _controlOwner is not null && _controlExpires > DateTimeOffset.UtcNow, mine = browserId is not null && browserId == _controlOwner && _controlExpires > DateTimeOffset.UtcNow, expiresAt = _controlOwner is null || _controlExpires <= DateTimeOffset.UtcNow ? (DateTimeOffset?)null : _controlExpires },
                ota = new { active = _otaActive }
            };
        }
    }

    public async Task HandleBrowserAsync(WebSocket socket, string browserId, CancellationToken cancellationToken)
    {
        var peer = new SocketPeer(socket, browserId);
        lock (_sync) _browsers.Add(peer);
        await SendStateToBrowserAsync(peer, browserId, cancellationToken);
        await BroadcastNoticeAsync("Browser connected", cancellationToken);
        try
        {
            while (socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
            {
                var message = await ReceiveAsync(socket, cancellationToken);
                if (message is null) break;
                if (message.Value.Type == WebSocketMessageType.Binary)
                {
                    await SendObdAsync(browserId, message.Value.Data, cancellationToken);
                    continue;
                }
                if (message.Value.Type == WebSocketMessageType.Close) break;
                try { await HandleBrowserTextAsync(peer, browserId, message.Value.Data, cancellationToken); }
                catch (Exception ex) when (ex is JsonException or InvalidOperationException or FormatException or TimeoutException)
                {
                    await peer.SendTextAsync(JsonSerializer.Serialize(new { type = "error", message = ex.Message }), cancellationToken);
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (WebSocketException ex) { logger.LogInformation("Browser socket ended: {Message}", ex.Message); }
        finally
        {
            bool released;
            lock (_sync)
            {
                _browsers.Remove(peer);
                released = _controlOwner == browserId;
            }
            if (released) await ReleaseControlAsync(browserId, CancellationToken.None);
            await peer.DisposeAsync();
            await BroadcastStateAsync(CancellationToken.None);
        }
    }

    public async Task HandleDeviceAsync(WebSocket socket, CancellationToken cancellationToken)
    {
        var peer = new SocketPeer(socket);
        SocketPeer? previous;
        lock (_sync)
        {
            previous = _device;
            _device = peer;
            _lastSeen = DateTimeOffset.UtcNow;
            _hello = default;
            _deviceStatus = default;
        }
        if (previous is not null)
        {
            try { await previous.DisposeAsync(); }
            catch (Exception ex) { logger.LogDebug("Old device socket close failed: {Message}", ex.Message); }
        }
        logger.LogInformation("Device {DeviceId} connected", options.DeviceId);
        await BroadcastStateAsync(cancellationToken);
        try
        {
            while (socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
            {
                var message = await ReceiveAsync(socket, cancellationToken);
                if (message is null || message.Value.Type == WebSocketMessageType.Close) break;
                lock (_sync) _lastSeen = DateTimeOffset.UtcNow;
                if (message.Value.Type == WebSocketMessageType.Binary)
                    await HandleDeviceBinaryAsync(peer, message.Value.Data, cancellationToken);
                else
                    await HandleDeviceTextAsync(peer, message.Value.Data, cancellationToken);
            }
        }
        catch (OperationCanceledException) { }
        catch (WebSocketException ex) { logger.LogInformation("Device socket ended: {Message}", ex.Message); }
        finally
        {
            lock (_sync)
            {
                if (_device == peer) _device = null;
                if (_controlOwner is not null)
                {
                    _controlOwner = null;
                    _controlExpires = DateTimeOffset.MinValue;
                }
            }
            logger.LogWarning("Device {DeviceId} disconnected", options.DeviceId);
            await peer.DisposeAsync();
            await BroadcastStateAsync(CancellationToken.None);
        }
    }

    public async Task<bool> AcquireControlAsync(string browserId, CancellationToken cancellationToken)
    {
        lock (_sync)
        {
            if (_controlOwner is not null && _controlExpires <= DateTimeOffset.UtcNow) _controlOwner = null;
            if (!IsDeviceOnlineLocked() || _otaActive || (_controlOwner is not null && _controlOwner != browserId))
                return false;
            _controlOwner = browserId;
            _controlExpires = DateTimeOffset.UtcNow.AddSeconds(60);
        }

        JsonElement result;
        try { result = await SendRequestAsync("control", new { enabled = true }, TimeSpan.FromSeconds(8), cancellationToken); }
        catch
        {
            lock (_sync) if (_controlOwner == browserId) _controlOwner = null;
            await BroadcastStateAsync(CancellationToken.None);
            throw;
        }
        var accepted = result.TryGetProperty("success", out var success) && success.GetBoolean();
        if (!accepted)
        {
            lock (_sync)
            {
                if (_controlOwner == browserId) _controlOwner = null;
            }
        }
        else
        {
            logger.LogInformation("OBD control acquired by browser session");
            await BroadcastNoticeAsync("Browser control acquired", cancellationToken);
        }
        await BroadcastStateAsync(cancellationToken);
        return accepted;
    }

    public async Task ReleaseControlAsync(string browserId, CancellationToken cancellationToken)
    {
        bool owns;
        lock (_sync)
        {
            owns = _controlOwner == browserId;
            if (owns)
            {
                _controlOwner = null;
                _controlExpires = DateTimeOffset.MinValue;
            }
        }
        if (!owns) return;
        try { await SendRequestAsync("control", new { enabled = false }, TimeSpan.FromSeconds(4), cancellationToken); }
        catch (Exception ex) { logger.LogDebug("Control release notification failed: {Message}", ex.Message); }
        await BroadcastNoticeAsync("Browser control released", cancellationToken);
        await BroadcastStateAsync(cancellationToken);
    }

    public async Task<bool> SendCommandAsync(string browserId, byte[] data, CancellationToken cancellationToken)
    {
        if (data.Length is 0 or > 4096) return false;
        SocketPeer? device;
        lock (_sync)
        {
            if (_controlOwner != browserId || _controlExpires <= DateTimeOffset.UtcNow || _otaActive || !IsDeviceOnlineLocked()) return false;
            _controlExpires = DateTimeOffset.UtcNow.AddSeconds(60);
            device = _device;
        }
        if (device?.Socket.State != WebSocketState.Open) return false;
        var frame = new byte[data.Length + 1];
        frame[0] = 1;
        data.CopyTo(frame, 1);
        await device.SendAsync(WebSocketMessageType.Binary, frame, cancellationToken);
        await BroadcastTextAsync(JsonSerializer.Serialize(new
        {
            type = "tx",
            base64 = Convert.ToBase64String(data),
            length = data.Length,
            timestamp = DateTimeOffset.UtcNow
        }), cancellationToken);
        return true;
    }

    public async Task<JsonElement> SendManagementAsync(string browserId, string command, CancellationToken cancellationToken)
    {
        if (!IsController(browserId)) throw new InvalidOperationException("Acquire OBD control before sending device commands.");
        return await SendRequestAsync(command, new { }, TimeSpan.FromSeconds(8), cancellationToken);
    }

    public async Task<object> UploadFirmwareAsync(string browserId, Stream firmware, long size, string expectedBuild, CancellationToken cancellationToken)
    {
        if (size is < 1 || size > options.OtaMaxBytes) throw new InvalidOperationException($"Firmware must be between 1 byte and {options.OtaMaxBytes} bytes.");
        if (!await _otaLock.WaitAsync(0, cancellationToken)) throw new InvalidOperationException("An OTA update is already active.");
        var temporaryPath = Path.Combine(Path.GetTempPath(), $"obd-fw-{Guid.NewGuid():N}.bin");
        var commitRequested = false;
        try
        {
            lock (_sync)
            {
                if (_controlOwner != browserId || _controlExpires <= DateTimeOffset.UtcNow) throw new InvalidOperationException("Acquire OBD control before updating firmware.");
                if (!IsDeviceOnlineLocked()) throw new InvalidOperationException("The ESP32 is offline.");
                _otaActive = true;
            }
            await BroadcastOtaAsync("upload", 0, size, "Receiving firmware", cancellationToken);
            await using (var output = new FileStream(temporaryPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                var buffer = new byte[65536];
                long total = 0;
                int read;
                while ((read = await firmware.ReadAsync(buffer, cancellationToken)) > 0)
                {
                    total += read;
                    if (total > size || total > options.OtaMaxBytes) throw new InvalidOperationException("Firmware upload exceeded the declared size limit.");
                    await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
                }
                if (total != size) throw new InvalidOperationException("Firmware upload ended before all bytes arrived.");
            }

            string sha256;
            await using (var input = File.OpenRead(temporaryPath))
                sha256 = Convert.ToHexString(await SHA256.HashDataAsync(input, cancellationToken)).ToLowerInvariant();

            JsonElement oldHello;
            lock (_sync) oldHello = _hello.ValueKind == JsonValueKind.Undefined ? default : _hello.Clone();
            var oldSlot = GetString(oldHello, "runningSlot");
            var transferId = Guid.NewGuid().ToString("N");
            _activeTransferId = transferId;
            _ackedOffset = 0;
            _otaReady = NewCompletion();
            _otaResult = NewCompletion();
            _reconnected = NewCompletion();
            _awaitingOtaReconnect = false;
            await BroadcastOtaAsync("prepare", 0, size, "Checking the device OTA slot", cancellationToken);
            await SendDeviceJsonAsync(new { type = "ota.begin", transferId, totalBytes = size, sha256, expectedBuild }, cancellationToken);
            var ready = await _otaReady.Task.WaitAsync(TimeSpan.FromSeconds(15), cancellationToken);
            EnsureDeviceSuccess(ready, "ESP32 rejected the firmware image.");

            await BroadcastOtaAsync("transfer", 0, size, "Writing inactive firmware slot", cancellationToken);
            var block = new byte[OtaChunkBytes];
            long offset = 0;
            await using (var input = File.OpenRead(temporaryPath))
            {
                int read;
                while ((read = await input.ReadAsync(block, cancellationToken)) > 0)
                {
                    var frame = new byte[5 + read];
                    frame[0] = 3;
                    BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(1, 4), checked((uint)offset));
                    block.AsSpan(0, read).CopyTo(frame.AsSpan(5));
                    var device = GetDeviceOrThrow();
                    await device.SendAsync(WebSocketMessageType.Binary, frame, cancellationToken);
                    offset += read;
                    if (offset % 65536 == 0 || offset == size)
                    {
                        var ack = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                        _otaAcks[(int)offset] = ack;
                        // Device acknowledgements may arrive before the waiter is registered, so the receive loop also stores the latest offset.
                        if (_ackedOffset >= offset) ack.TrySetResult(true);
                        if (!await ack.Task.WaitAsync(TimeSpan.FromSeconds(20), cancellationToken))
                            throw new InvalidOperationException("ESP32 aborted the firmware transfer.");
                        await BroadcastOtaAsync("transfer", offset, size, "Writing inactive firmware slot", cancellationToken);
                    }
                }
            }

            await BroadcastOtaAsync("verify", size, size, "Verifying image hash and firmware structure", cancellationToken);
            await SendDeviceJsonAsync(new { type = "ota.commit", transferId }, cancellationToken);
            commitRequested = true;
            var result = await _otaResult.Task.WaitAsync(TimeSpan.FromSeconds(25), cancellationToken);
            EnsureDeviceSuccess(result, GetString(result, "error") ?? "ESP32 could not commit firmware.");
            await BroadcastOtaAsync("reboot", size, size, "Firmware accepted; waiting for ESP32 to return", cancellationToken);

            try
            {
                var hello = await _reconnected.Task.WaitAsync(TimeSpan.FromSeconds(75), cancellationToken);
                var newSlot = GetString(hello, "runningSlot");
                if (oldSlot is not null && newSlot is not null && oldSlot == newSlot)
                    throw new InvalidOperationException("ESP32 reconnected on the previous OTA slot; check boot validation and rollback logs.");
                await BroadcastOtaAsync("complete", size, size, "ESP32 rebooted and reconnected", cancellationToken);
                return new { updated = true, bytes = size, sha256, oldSlot, newSlot, buildId = GetString(hello, "buildId") };
            }
            catch (TimeoutException)
            {
                await BroadcastOtaAsync("reconnect-timeout", size, size, "Firmware was accepted, but ESP32 did not reconnect in time", cancellationToken);
                throw new InvalidOperationException("Firmware was accepted by the ESP32, but it did not reconnect within 75 seconds. Check the LAN or USB recovery path.");
            }
        }
        catch
        {
            if (!commitRequested && _activeTransferId is not null)
            {
                try { await SendDeviceJsonAsync(new { type = "ota.abort", transferId = _activeTransferId }, CancellationToken.None); }
                catch (Exception ex) { logger.LogDebug("Could not notify ESP32 to abort OTA: {Message}", ex.Message); }
            }
            await BroadcastOtaAsync("failed", 0, size, "Firmware update failed", CancellationToken.None);
            throw;
        }
        finally
        {
            lock (_sync) _otaActive = false;
            _activeTransferId = null;
            _otaReady = null;
            _otaResult = null;
            _reconnected = null;
            _awaitingOtaReconnect = false;
            _otaAcks.Clear();
            _otaLock.Release();
            try { File.Delete(temporaryPath); } catch (IOException) { }
            await BroadcastStateAsync(CancellationToken.None);
        }
    }

    private async Task HandleBrowserTextAsync(SocketPeer peer, string browserId, byte[] data, CancellationToken cancellationToken)
    {
        using var document = JsonDocument.Parse(data);
        var root = document.RootElement;
        var type = GetString(root, "type");
        switch (type)
        {
            case "acquire":
                await peer.SendTextAsync(JsonSerializer.Serialize(new { type = "controlResult", success = await AcquireControlAsync(browserId, cancellationToken) }), cancellationToken);
                break;
            case "release":
                await ReleaseControlAsync(browserId, cancellationToken);
                break;
            case "renew":
                lock (_sync)
                    if (_controlOwner == browserId) _controlExpires = DateTimeOffset.UtcNow.AddSeconds(60);
                break;
            case "command":
                var command = GetString(root, "command") ?? "";
                if (command is not ("bluetooth.reconnect" or "status.get" or "diagnostics.get"))
                    throw new InvalidOperationException("Unknown device command.");
                var result = await SendManagementAsync(browserId, command, cancellationToken);
                await peer.SendTextAsync(JsonSerializer.Serialize(new { type = "commandResult", command, result }), cancellationToken);
                break;
            case "send":
                var encoding = GetString(root, "encoding") ?? "text";
                byte[] payload;
                if (encoding == "hex")
                {
                    var hex = (GetString(root, "data") ?? "").Where(Uri.IsHexDigit).ToArray();
                    if (hex.Length == 0 || hex.Length % 2 != 0) throw new InvalidOperationException("Hex input must have an even number of digits.");
                    payload = Convert.FromHexString(new string(hex));
                }
                else payload = Encoding.UTF8.GetBytes(GetString(root, "data") ?? "");
                if (root.TryGetProperty("appendCr", out var appendCr) && appendCr.ValueKind == JsonValueKind.True)
                    payload = [.. payload, 0x0d];
                if (!await SendCommandAsync(browserId, payload, cancellationToken))
                    throw new InvalidOperationException("Command rejected. Confirm control ownership and device availability.");
                break;
            default:
                throw new InvalidOperationException("Unknown browser message.");
        }
    }

    private async Task HandleDeviceTextAsync(SocketPeer peer, byte[] data, CancellationToken cancellationToken)
    {
        using var document = JsonDocument.Parse(data);
        var root = document.RootElement;
        var type = GetString(root, "type");
        lock (_sync)
        {
            if (_device != peer) return;
            _lastSeen = DateTimeOffset.UtcNow;
            if (type == "hello")
            {
                _hello = root.Clone();
                if (_awaitingOtaReconnect && _reconnected is not null)
                {
                    _awaitingOtaReconnect = false;
                    _reconnected.TrySetResult(_hello);
                }
            }
            if (type == "status") _deviceStatus = root.Clone();
        }

        if (type == "result" && GetString(root, "requestId") is { } requestId && _requests.TryRemove(requestId, out var request))
            request.TrySetResult(root.Clone());
        else if (type == "otaReady" && _otaReady is not null && GetString(root, "transferId") == _activeTransferId) _otaReady.TrySetResult(root.Clone());
        else if (type == "otaResult" && _otaResult is not null)
        {
            if (GetString(root, "transferId") == _activeTransferId)
            {
                _otaResult.TrySetResult(root.Clone());
                _awaitingOtaReconnect = root.TryGetProperty("success", out var success) && success.ValueKind == JsonValueKind.True;
                if (!root.TryGetProperty("success", out var otaSuccess) || otaSuccess.ValueKind != JsonValueKind.True)
                    foreach (var ack in _otaAcks.Values) ack.TrySetResult(false);
            }
        }
        else if (type == "otaAck" && GetString(root, "transferId") == _activeTransferId && root.TryGetProperty("offset", out var offsetElement))
        {
            var offset = offsetElement.GetInt32();
            if (offset > _ackedOffset) _ackedOffset = offset;
            if (_otaAcks.TryRemove(offset, out var ack)) ack.TrySetResult(true);
        }

        await BroadcastTextAsync(JsonSerializer.Serialize(new { type = "deviceEvent", message = root }), cancellationToken);
        await BroadcastStateAsync(cancellationToken);
    }

    private volatile int _ackedOffset;

    private async Task HandleDeviceBinaryAsync(SocketPeer device, byte[] data, CancellationToken cancellationToken)
    {
        if (data.Length < 2 || data[0] != 2) return;
        lock (_sync) if (_device != device) return;
        await BroadcastBinaryAsync(data.AsMemory(1), toDevice: false, cancellationToken);
    }

    private async Task SendObdAsync(string browserId, byte[] data, CancellationToken cancellationToken)
    {
        if (!await SendCommandAsync(browserId, data, cancellationToken))
            await BroadcastNoticeAsync("OBD command ignored because this browser does not own control", cancellationToken);
    }

    private async Task<JsonElement> SendRequestAsync(string command, object parameters, TimeSpan timeout, CancellationToken cancellationToken)
    {
        var id = Guid.NewGuid().ToString("N");
        var completion = NewCompletion();
        _requests[id] = completion;
        try
        {
            await SendDeviceJsonAsync(new { type = "command", requestId = id, command, parameters }, cancellationToken);
            return await completion.Task.WaitAsync(timeout, cancellationToken);
        }
        finally { _requests.TryRemove(id, out _); }
    }

    private async Task SendDeviceJsonAsync(object value, CancellationToken cancellationToken)
    {
        var device = GetDeviceOrThrow();
        await device.SendTextAsync(JsonSerializer.Serialize(value), cancellationToken);
    }

    private SocketPeer GetDeviceOrThrow()
    {
        lock (_sync)
            return IsDeviceOnlineLocked() ? _device! : throw new InvalidOperationException("The ESP32 is offline.");
    }

    private bool IsDeviceOnlineLocked() => _device?.Socket.State == WebSocketState.Open &&
        _lastSeen >= DateTimeOffset.UtcNow.AddSeconds(-options.DeviceOfflineSeconds);

    private bool IsController(string browserId)
    {
        lock (_sync) return _controlOwner == browserId && _controlExpires > DateTimeOffset.UtcNow && !_otaActive;
    }

    public async Task ExpireControlAsync(CancellationToken cancellationToken)
    {
        string? expired = null;
        lock (_sync)
        {
            if (_controlOwner is not null && _controlExpires <= DateTimeOffset.UtcNow)
            {
                expired = _controlOwner;
                _controlOwner = null;
                _controlExpires = DateTimeOffset.MinValue;
            }
        }
        if (expired is null) return;
        try { await SendRequestAsync("control", new { enabled = false }, TimeSpan.FromSeconds(4), cancellationToken); }
        catch (Exception ex) { logger.LogInformation("Expired control lease could not be released on device: {Message}", ex.Message); }
        await BroadcastNoticeAsync("OBD control lease expired", cancellationToken);
        await BroadcastStateAsync(cancellationToken);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            try { await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken); }
            catch (OperationCanceledException) { break; }
            await ExpireControlAsync(stoppingToken);
        }
    }

    private async Task SendStateToBrowserAsync(SocketPeer peer, string browserId, CancellationToken cancellationToken) =>
        await peer.SendTextAsync(JsonSerializer.Serialize(new { type = "state", snapshot = Snapshot(browserId) }), cancellationToken);

    private async Task BroadcastStateAsync(CancellationToken cancellationToken)
    {
        SocketPeer[] browsers;
        lock (_sync) browsers = _browsers.ToArray();
        foreach (var browser in browsers)
        {
            var id = browser.Id;
            await browser.SendTextAsync(JsonSerializer.Serialize(new { type = "state", snapshot = Snapshot(id) }), cancellationToken);
        }
    }

    private async Task BroadcastTextAsync(string text, CancellationToken cancellationToken)
    {
        SocketPeer[] browsers;
        lock (_sync) browsers = _browsers.ToArray();
        foreach (var browser in browsers)
            try { await browser.SendTextAsync(text, cancellationToken); }
            catch (Exception ex) when (ex is WebSocketException or ObjectDisposedException) { }
    }

    private Task BroadcastNoticeAsync(string text, CancellationToken cancellationToken) =>
        BroadcastTextAsync(JsonSerializer.Serialize(new { type = "notice", message = text }), cancellationToken);

    private async Task BroadcastBinaryAsync(ReadOnlyMemory<byte> data, bool toDevice, CancellationToken cancellationToken)
    {
        SocketPeer[] targets;
        lock (_sync)
        {
            if (toDevice) targets = _device is null ? [] : [_device];
            else targets = _browsers.ToArray();
        }
        foreach (var target in targets)
            try { await target.SendAsync(WebSocketMessageType.Binary, data, cancellationToken); }
            catch (Exception ex) when (ex is WebSocketException or ObjectDisposedException) { }
    }

    private Task BroadcastOtaAsync(string stage, long complete, long total, string message, CancellationToken cancellationToken) =>
        BroadcastTextAsync(JsonSerializer.Serialize(new { type = "ota", stage, complete, total, message }), cancellationToken);

    private static async Task<(WebSocketMessageType Type, byte[] Data)?> ReceiveAsync(WebSocket socket, CancellationToken cancellationToken)
    {
        using var output = new MemoryStream();
        var buffer = new byte[8192];
        WebSocketReceiveResult result;
        do
        {
            result = await socket.ReceiveAsync(buffer, cancellationToken);
            if (result.MessageType == WebSocketMessageType.Close) return (WebSocketMessageType.Close, []);
            if (output.Length + result.Count > MaxMessageBytes) throw new WebSocketException(WebSocketError.HeaderError, "WebSocket message too large.");
            output.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);
        return (result.MessageType, output.ToArray());
    }

    private static TaskCompletionSource<JsonElement> NewCompletion() => new(TaskCreationOptions.RunContinuationsAsynchronously);
    private static string? GetString(JsonElement value, string name) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(name, out var prop) && prop.ValueKind == JsonValueKind.String ? prop.GetString() : null;
    private static void EnsureDeviceSuccess(JsonElement result, string error)
    {
        if (!result.TryGetProperty("success", out var success) || success.ValueKind != JsonValueKind.True)
            throw new InvalidOperationException(error);
    }
}
