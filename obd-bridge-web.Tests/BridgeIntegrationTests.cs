using System.Net;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.AspNetCore.Hosting;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace ObdBridge.Web.Tests;

public sealed class BridgeIntegrationTests : IClassFixture<WebApplicationFactory<Program>>
{
    private const string Password = "test-dashboard-password-2026";
    private static readonly string DeviceToken = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant();
    private readonly WebApplicationFactory<Program> _factory;

    public BridgeIntegrationTests(WebApplicationFactory<Program> factory)
    {
        Environment.SetEnvironmentVariable("WEB_PASSWORD", Password);
        Environment.SetEnvironmentVariable("DEVICE_TOKEN", DeviceToken);
        Environment.SetEnvironmentVariable("DEVICE_ID", "test-car");
        Environment.SetEnvironmentVariable("PUBLIC_ORIGIN", "https://car.gary.systems");
        _factory = factory.WithWebHostBuilder(builder => builder.UseEnvironment("Testing"));
    }

    [Fact]
    public async Task LoginCookieProtectsDashboardAndMutationsCheckOrigin()
    {
        using var anonymous = CreateHttpClient();
        var loginPage = await anonymous.GetStringAsync("/login");
        Assert.Contains("Build ", loginPage);

        var health = await anonymous.GetAsync("/healthz");
        Assert.Equal(HttpStatusCode.OK, health.StatusCode);
        var privateStatus = await anonymous.GetAsync("/api/status");
        Assert.Equal(HttpStatusCode.Unauthorized, privateStatus.StatusCode);

        var rejected = await LoginAsync(anonymous, "incorrect-password");
        Assert.Equal(HttpStatusCode.Redirect, rejected.Response.StatusCode);
        Assert.Contains("invalid=1", rejected.Response.Headers.Location!.ToString());

        var loggedIn = await LoginAsync(anonymous, Password);
        Assert.Equal(HttpStatusCode.Redirect, loggedIn.Response.StatusCode);
        Assert.Contains("Secure", loggedIn.CookieHeader, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("HttpOnly", loggedIn.CookieHeader, StringComparison.OrdinalIgnoreCase);

        using var statusRequest = AuthenticatedRequest(HttpMethod.Get, "/api/status", loggedIn.Cookie);
        var status = await anonymous.SendAsync(statusRequest);
        Assert.Equal(HttpStatusCode.OK, status.StatusCode);
        using var body = JsonDocument.Parse(await status.Content.ReadAsStringAsync());
        Assert.False(body.RootElement.GetProperty("deviceOnline").GetBoolean());

        using var badOrigin = AuthenticatedRequest(HttpMethod.Post, "/api/control/acquire", loggedIn.Cookie, "https://attacker.example");
        var rejectedOrigin = await anonymous.SendAsync(badOrigin);
        Assert.Equal(HttpStatusCode.Forbidden, rejectedOrigin.StatusCode);
        using var originError = JsonDocument.Parse(await rejectedOrigin.Content.ReadAsStringAsync());
        Assert.Equal("https://car.gary.systems", originError.RootElement.GetProperty("expectedOrigin").GetString());
        Assert.Equal("https://attacker.example", originError.RootElement.GetProperty("receivedOrigin").GetString());
    }

    [Fact]
    public async Task AnonymousLoginRedirectStaysRelativeBehindTlsTerminatingProxy()
    {
        using var client = _factory.CreateClient(new WebApplicationFactoryClientOptions
        {
            BaseAddress = new Uri("http://car.gary.systems"),
            AllowAutoRedirect = false,
            HandleCookies = false
        });

        using var response = await client.GetAsync("/");
        Assert.Equal(HttpStatusCode.Redirect, response.StatusCode);
        Assert.NotNull(response.Headers.Location);
        Assert.False(response.Headers.Location!.IsAbsoluteUri);
        Assert.StartsWith("/login", response.Headers.Location.OriginalString);
    }

    [Fact]
    public async Task DeviceAndBrowserSocketsRelayBytesAndEnforceOneController()
    {
        using var firstHttp = CreateHttpClient();
        var firstLogin = await LoginAsync(firstHttp, Password);

        using var device = await ConnectDeviceAsync();
        using var firstBrowser = await ConnectBrowserAsync(firstLogin.Cookie);
        await device.SendAsync(Encoding.UTF8.GetBytes("{\"version\":1,\"type\":\"hello\",\"deviceId\":\"test-car\",\"buildId\":\"test-build\",\"runningSlot\":\"app0\"}"), WebSocketMessageType.Text, true, CancellationToken.None);
        _ = await ReceiveTextContainingAsync(firstBrowser, "test-build");

        await SendTextAsync(firstBrowser, "{\"type\":\"acquire\"}");
        var controlRequest = await ReceiveJsonAsync(device);
        Assert.Equal("control", controlRequest.RootElement.GetProperty("command").GetString());
        Assert.True(controlRequest.RootElement.GetProperty("parameters").GetProperty("enabled").GetBoolean());
        var requestId = controlRequest.RootElement.GetProperty("requestId").GetString();
        await device.SendAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { version = 1, type = "result", requestId, command = "control", success = true })), WebSocketMessageType.Text, true, CancellationToken.None);
        _ = await ReceiveTextContainingAsync(firstBrowser, "controlResult");

        using var secondHttp = CreateHttpClient();
        var secondLogin = await LoginAsync(secondHttp, Password);
        using var secondBrowser = await ConnectBrowserAsync(secondLogin.Cookie);
        await SendTextAsync(secondBrowser, "{\"type\":\"acquire\"}");
        using (var result = await ReceiveJsonContainingAsync(secondBrowser, "controlResult"))
            Assert.False(result.RootElement.GetProperty("success").GetBoolean());

        await SendTextAsync(firstBrowser, "{\"type\":\"send\",\"encoding\":\"text\",\"data\":\"ATI\",\"appendCr\":true}");
        var commandFrame = await ReceiveBinaryAsync(device);
        Assert.Equal(new byte[] { 1, (byte)'A', (byte)'T', (byte)'I', 0x0d }, commandFrame);

        await device.SendAsync(new byte[] { 2, (byte)'O', (byte)'K', (byte)'>' }, WebSocketMessageType.Binary, true, CancellationToken.None);
        var responseFrame = await ReceiveBinaryAsync(firstBrowser);
        Assert.Equal(Encoding.ASCII.GetBytes("OK>"), responseFrame);

        await SendTextAsync(firstBrowser, "{\"type\":\"release\"}");
        var releaseRequest = await ReceiveJsonContainingAsync(device, "control");
        Assert.False(releaseRequest.RootElement.GetProperty("parameters").GetProperty("enabled").GetBoolean());
    }

    [Fact]
    public async Task FirmwareUploadStreamsOffsetsAndWaitsForChangedOtaSlot()
    {
        using var http = CreateHttpClient();
        var login = await LoginAsync(http, Password);
        using var device = await ConnectDeviceAsync();
        using var browser = await ConnectBrowserAsync(login.Cookie);
        await device.SendAsync(Encoding.UTF8.GetBytes("{\"version\":1,\"type\":\"hello\",\"deviceId\":\"test-car\",\"buildId\":\"old-build\",\"runningSlot\":\"app0\"}"), WebSocketMessageType.Text, true, CancellationToken.None);
        _ = await ReceiveTextContainingAsync(browser, "old-build");
        await SendTextAsync(browser, "{\"type\":\"acquire\"}");
        var control = await ReceiveJsonAsync(device);
        await device.SendAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
        {
            version = 1,
            type = "result",
            requestId = control.RootElement.GetProperty("requestId").GetString(),
            command = "control",
            success = true
        })), WebSocketMessageType.Text, true, CancellationToken.None);
        _ = await ReceiveTextContainingAsync(browser, "controlResult");

        var firmware = RandomNumberGenerator.GetBytes(512);
        var deviceUpdate = Task.Run(async () =>
        {
            using (var begin = await ReceiveJsonContainingAsync(device, "ota.begin"))
            {
                var transferId = begin.RootElement.GetProperty("transferId").GetString();
                await device.SendAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { version = 1, type = "otaReady", transferId, success = true })), WebSocketMessageType.Text, true, CancellationToken.None);
                var frame = await ReceiveBinaryAsync(device);
                Assert.Equal(3, frame[0]);
                Assert.Equal(0u, System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(frame.AsSpan(1, 4)));
                Assert.Equal(firmware, frame.AsSpan(5).ToArray());
                await device.SendAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { version = 1, type = "otaAck", transferId, offset = firmware.Length })), WebSocketMessageType.Text, true, CancellationToken.None);
                using var commit = await ReceiveJsonContainingAsync(device, "ota.commit");
                await device.SendAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { version = 1, type = "otaResult", transferId, success = true, bytes = firmware.Length })), WebSocketMessageType.Text, true, CancellationToken.None);
                await device.SendAsync(Encoding.UTF8.GetBytes("{\"version\":1,\"type\":\"hello\",\"deviceId\":\"test-car\",\"buildId\":\"new-build\",\"runningSlot\":\"app1\"}"), WebSocketMessageType.Text, true, CancellationToken.None);
            }
        });

        using var upload = new HttpRequestMessage(HttpMethod.Post, "/api/device/ota")
        {
            Content = new ByteArrayContent(firmware)
        };
        upload.Content.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/octet-stream");
        upload.Headers.TryAddWithoutValidation("Cookie", login.Cookie);
        upload.Headers.TryAddWithoutValidation("Origin", "https://car.gary.systems");
        upload.Headers.TryAddWithoutValidation("X-Password-Confirm", Password);
        using var response = await http.SendAsync(upload);
        await deviceUpdate;
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var result = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        Assert.True(result.RootElement.GetProperty("updated").GetBoolean());
        Assert.Equal("app0", result.RootElement.GetProperty("oldSlot").GetString());
        Assert.Equal("app1", result.RootElement.GetProperty("newSlot").GetString());
    }

    private HttpClient CreateHttpClient() => _factory.CreateClient(new WebApplicationFactoryClientOptions
    {
        BaseAddress = new Uri("https://car.gary.systems"),
        AllowAutoRedirect = false,
        HandleCookies = false
    });

    private static async Task<(HttpResponseMessage Response, string CookieHeader, string Cookie)> LoginAsync(HttpClient client, string password)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, "/login")
        {
            Content = new FormUrlEncodedContent(new Dictionary<string, string> { ["password"] = password })
        };
        request.Headers.TryAddWithoutValidation("Origin", "https://car.gary.systems");
        var response = await client.SendAsync(request);
        if (!response.Headers.TryGetValues("Set-Cookie", out var values)) return (response, "", "");
        var cookieHeader = values.Single();
        return (response, cookieHeader, cookieHeader.Split(';', 2)[0]);
    }

    private async Task<WebSocket> ConnectDeviceAsync()
    {
        var client = _factory.Server.CreateWebSocketClient();
        client.ConfigureRequest = request =>
        {
            request.Headers["Authorization"] = $"Bearer {DeviceToken}";
            request.Headers["X-Device-Id"] = "test-car";
            request.Headers["X-Protocol-Version"] = "1";
        };
        return await client.ConnectAsync(new Uri("ws://localhost/ws/device"), CancellationToken.None);
    }

    private Task<WebSocket> ConnectBrowserAsync(string cookie)
    {
        var client = _factory.Server.CreateWebSocketClient();
        client.ConfigureRequest = request =>
        {
            request.Headers["Origin"] = "https://car.gary.systems";
            request.Headers["Cookie"] = cookie;
        };
        return client.ConnectAsync(new Uri("ws://localhost/ws/browser"), CancellationToken.None);
    }

    private static HttpRequestMessage AuthenticatedRequest(HttpMethod method, string path, string cookie, string origin = "https://car.gary.systems")
    {
        var request = new HttpRequestMessage(method, path);
        request.Headers.TryAddWithoutValidation("Cookie", cookie);
        request.Headers.TryAddWithoutValidation("Origin", origin);
        return request;
    }

    private static async Task SendTextAsync(WebSocket socket, string text) =>
        await socket.SendAsync(Encoding.UTF8.GetBytes(text), WebSocketMessageType.Text, true, CancellationToken.None);

    private static async Task<string> ReceiveTextContainingAsync(WebSocket socket, string expected)
    {
        for (var i = 0; i < 20; i++)
        {
            var message = await ReceiveAsync(socket);
            if (message.Type == WebSocketMessageType.Text)
            {
                var text = Encoding.UTF8.GetString(message.Data);
                if (text.Contains(expected, StringComparison.Ordinal)) return text;
            }
        }
        throw new TimeoutException($"WebSocket text message containing '{expected}' was not received.");
    }

    private static async Task<JsonDocument> ReceiveJsonAsync(WebSocket socket)
    {
        var message = await ReceiveAsync(socket);
        Assert.Equal(WebSocketMessageType.Text, message.Type);
        return JsonDocument.Parse(message.Data);
    }

    private static async Task<JsonDocument> ReceiveJsonContainingAsync(WebSocket socket, string expected)
    {
        var text = await ReceiveTextContainingAsync(socket, expected);
        return JsonDocument.Parse(text);
    }

    private static async Task<byte[]> ReceiveBinaryAsync(WebSocket socket)
    {
        for (var i = 0; i < 20; i++)
        {
            var message = await ReceiveAsync(socket);
            if (message.Type == WebSocketMessageType.Binary) return message.Data;
        }
        throw new TimeoutException("WebSocket binary frame was not received.");
    }

    private static async Task<(WebSocketMessageType Type, byte[] Data)> ReceiveAsync(WebSocket socket)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var stream = new MemoryStream();
        var buffer = new byte[8192];
        WebSocketReceiveResult result;
        do
        {
            result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer), timeout.Token);
            stream.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);
        return (result.MessageType, stream.ToArray());
    }
}
