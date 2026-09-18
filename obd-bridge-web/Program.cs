using System.Security.Claims;
using System.Text;
using System.Threading.RateLimiting;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authentication.Cookies;
using Microsoft.AspNetCore.DataProtection;
using Microsoft.AspNetCore.RateLimiting;
using ObdBridge.Web;

if (args.Contains("--healthcheck", StringComparer.Ordinal))
{
    try
    {
        using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
        using var response = await client.GetAsync(Environment.GetEnvironmentVariable("HEALTHCHECK_URL") ?? "http://127.0.0.1:8080/healthz");
        Environment.ExitCode = response.IsSuccessStatusCode ? 0 : 1;
    }
    catch { Environment.ExitCode = 1; }
    return;
}

var builder = WebApplication.CreateBuilder(args);
var settings = AppOptions.Load(builder.Configuration);
builder.Services.AddSingleton(settings);
builder.Services.AddSingleton<BridgeCoordinator>();
builder.Services.AddHostedService(provider => provider.GetRequiredService<BridgeCoordinator>());
builder.Services.AddDataProtection();
if (!string.IsNullOrWhiteSpace(builder.Configuration["DATA_PROTECTION_PATH"]))
{
    var keyPath = builder.Configuration["DATA_PROTECTION_PATH"]!;
    Directory.CreateDirectory(keyPath);
    builder.Services.AddDataProtection().PersistKeysToFileSystem(new DirectoryInfo(keyPath));
}
builder.Services.AddAuthentication(CookieAuthenticationDefaults.AuthenticationScheme).AddCookie(cookie =>
{
    cookie.Cookie.Name = "__Host-obd_session";
    cookie.Cookie.Path = "/";
    cookie.Cookie.HttpOnly = true;
    cookie.Cookie.SecurePolicy = CookieSecurePolicy.Always;
    cookie.Cookie.SameSite = SameSiteMode.Strict;
    cookie.ExpireTimeSpan = TimeSpan.FromDays(settings.AuthCookieDays);
    cookie.SlidingExpiration = true;
    cookie.LoginPath = "/login";
    cookie.Events.OnRedirectToLogin = context =>
    {
        if (context.Request.Path.StartsWithSegments("/api") || context.Request.Path.StartsWithSegments("/ws"))
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            return Task.CompletedTask;
        }
        context.Response.Redirect(context.RedirectUri);
        return Task.CompletedTask;
    };
    cookie.Events.OnValidatePrincipal = async context =>
    {
        var stamp = context.Principal?.FindFirstValue("password_stamp") ?? "";
        if (!settings.PasswordStampMatches(stamp))
        {
            context.RejectPrincipal();
            await context.HttpContext.SignOutAsync(CookieAuthenticationDefaults.AuthenticationScheme);
        }
    };
});
builder.Services.AddAuthorization(authorization =>
    authorization.FallbackPolicy = authorization.DefaultPolicy);
builder.Services.AddRateLimiter(limiter => limiter.AddPolicy("login", context =>
    RateLimitPartition.GetFixedWindowLimiter(
        context.Connection.RemoteIpAddress?.ToString() ?? "unknown",
        _ => new FixedWindowRateLimiterOptions
        {
            PermitLimit = 5,
            Window = TimeSpan.FromMinutes(1),
            QueueLimit = 0,
            AutoReplenishment = true
        })));

var app = builder.Build();
var loginHtml = """
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark"><title>Car Console · Sign in</title><style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;background:#111511;color:#e5e3d8;font:16px/1.5 Georgia,serif}.panel{width:min(420px,calc(100vw - 32px));padding:34px;background:#1c211d;border:1px solid #394139;box-shadow:12px 12px 0 #090b09}.mark{color:#d8a449;font:700 11px/1.2 Consolas,monospace;letter-spacing:.18em;text-transform:uppercase}.rule{height:1px;background:#394139;margin:22px 0}h1{font-size:32px;font-weight:400;margin:0 0 8px}.muted{color:#9ca399;font:13px/1.5 Consolas,monospace}label{display:block;margin:24px 0 7px;font:11px Consolas,monospace;letter-spacing:.1em;text-transform:uppercase}input{width:100%;padding:12px;background:#101410;border:1px solid #454e45;color:#fff;font:16px Consolas,monospace}button{width:100%;margin-top:14px;padding:12px;border:0;background:#dda94f;color:#1c1b15;font:700 12px Consolas,monospace;letter-spacing:.08em;text-transform:uppercase;cursor:pointer}#error{min-height:20px;color:#e88b6a;font:12px Consolas,monospace;margin:12px 0 0}</style></head>
<body><main class="panel"><div class="mark">Gary's garage · remote link</div><div class="rule"></div><h1>Car console</h1><div class="muted">Private vehicle bridge</div><form method="post" action="/login"><label for="password">Console password</label><input id="password" name="password" type="password" required autocomplete="current-password" autofocus><button type="submit">Open console</button><div id="error"></div></form></main><script>if(new URLSearchParams(location.search).has('invalid'))document.getElementById('error').textContent='Password not accepted.';</script></body></html>
""";

app.Use(async (context, next) =>
{
    context.Response.Headers.CacheControl = "no-store";
    context.Response.Headers.XContentTypeOptions = "nosniff";
    context.Response.Headers.XFrameOptions = "DENY";
    context.Response.Headers["Referrer-Policy"] = "no-referrer";
    context.Response.Headers.ContentSecurityPolicy = $"default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self' wss://{new Uri(settings.PublicOrigin).Authority}; img-src 'self' data:; form-action 'self'; base-uri 'none'; frame-ancestors 'none'";

    var isMutation = HttpMethods.IsPost(context.Request.Method) || HttpMethods.IsPut(context.Request.Method) || HttpMethods.IsPatch(context.Request.Method) || HttpMethods.IsDelete(context.Request.Method);
    var isBrowserSocket = context.Request.Path == "/ws/browser";
    if ((isMutation || isBrowserSocket) && context.Request.Path != "/ws/device")
    {
        var origin = context.Request.Headers.Origin.ToString();
        if (!string.Equals(origin, settings.PublicOrigin, StringComparison.OrdinalIgnoreCase))
        {
            context.Response.StatusCode = StatusCodes.Status403Forbidden;
            await context.Response.WriteAsJsonAsync(new { error = "origin_rejected" });
            return;
        }
    }
    await next();
});

app.UseWebSockets(new WebSocketOptions { KeepAliveInterval = TimeSpan.FromSeconds(20) });
app.UseAuthentication();
app.UseAuthorization();
app.UseRateLimiter();

app.MapGet("/healthz", () => Results.Text("ok")).AllowAnonymous();
app.MapGet("/readyz", () => Results.Json(new { ready = true })).AllowAnonymous();
app.MapGet("/login", () => Results.Content(loginHtml, "text/html; charset=utf-8")).AllowAnonymous();
app.MapPost("/login", async (HttpContext context, AppOptions options) =>
{
    if (!context.Request.HasFormContentType) return Results.BadRequest(new { error = "form_required" });
    var form = await context.Request.ReadFormAsync(context.RequestAborted);
    var supplied = form["password"].ToString();
    if (!options.PasswordMatches(supplied))
    {
        await Task.Delay(Random.Shared.Next(120, 260), context.RequestAborted);
        return Results.Redirect("/login?invalid=1");
    }
    var identity = new ClaimsIdentity([
        new Claim(ClaimTypes.NameIdentifier, Guid.NewGuid().ToString("N")),
        new Claim("password_stamp", options.PasswordStamp)
    ], CookieAuthenticationDefaults.AuthenticationScheme);
    await context.SignInAsync(CookieAuthenticationDefaults.AuthenticationScheme, new ClaimsPrincipal(identity), new AuthenticationProperties
    {
        IsPersistent = true,
        ExpiresUtc = DateTimeOffset.UtcNow.AddDays(options.AuthCookieDays)
    });
    return Results.Redirect("/");
}).AllowAnonymous().RequireRateLimiting("login");
app.MapPost("/logout", async (HttpContext context) =>
{
    await context.SignOutAsync(CookieAuthenticationDefaults.AuthenticationScheme);
    return Results.Redirect("/login");
});

app.MapGet("/", (IWebHostEnvironment environment) =>
    Results.File(Path.Combine(environment.ContentRootPath, "wwwroot", "index.html"), "text/html; charset=utf-8"));
app.MapGet("/api/status", (BridgeCoordinator bridge, ClaimsPrincipal user) => Results.Json(bridge.Snapshot(user.FindFirstValue(ClaimTypes.NameIdentifier))));
app.MapPost("/api/control/acquire", async (BridgeCoordinator bridge, ClaimsPrincipal user, CancellationToken cancellationToken) =>
{
    var id = user.FindFirstValue(ClaimTypes.NameIdentifier)!;
    var acquired = await bridge.AcquireControlAsync(id, cancellationToken);
    return acquired ? Results.Ok(new { acquired = true }) : Results.Conflict(new { error = "control_unavailable" });
});
app.MapPost("/api/control/release", async (BridgeCoordinator bridge, ClaimsPrincipal user, CancellationToken cancellationToken) =>
{
    await bridge.ReleaseControlAsync(user.FindFirstValue(ClaimTypes.NameIdentifier)!, cancellationToken);
    return Results.Ok(new { released = true });
});
app.MapPost("/api/device/reboot", async (HttpContext context, AppOptions options, BridgeCoordinator bridge, ClaimsPrincipal user, CancellationToken cancellationToken) =>
{
    if (!options.PasswordMatches(context.Request.Headers["X-Password-Confirm"].ToString())) return Results.Unauthorized();
    try
    {
        var result = await bridge.SendManagementAsync(user.FindFirstValue(ClaimTypes.NameIdentifier)!, "device.reboot", cancellationToken);
        return Results.Ok(result);
    }
    catch (InvalidOperationException ex) { return Results.Conflict(new { error = ex.Message }); }
});
app.MapPost("/api/device/ota", async (HttpContext context, AppOptions options, BridgeCoordinator bridge, ClaimsPrincipal user, IHostApplicationLifetime lifetime) =>
{
    var confirmation = context.Request.Headers["X-Password-Confirm"].ToString();
    if (!options.PasswordMatches(confirmation)) return Results.Unauthorized();
    if (context.Request.ContentLength is not long size) return Results.StatusCode(StatusCodes.Status411LengthRequired);
    if (size is < 1 || size > options.OtaMaxBytes) return Results.BadRequest(new { error = $"Firmware must be at most {options.OtaMaxBytes} bytes." });
    if (!context.Request.ContentType?.StartsWith("application/octet-stream", StringComparison.OrdinalIgnoreCase) ?? true)
        return Results.BadRequest(new { error = "application_octet_stream_required" });
    var expectedBuild = context.Request.Headers["X-Firmware-Build"].ToString();
    try
    {
        var result = await bridge.UploadFirmwareAsync(user.FindFirstValue(ClaimTypes.NameIdentifier)!, context.Request.Body, size, expectedBuild,
            lifetime.ApplicationStopping);
        return Results.Ok(result);
    }
    catch (InvalidOperationException ex) { return Results.Conflict(new { error = ex.Message }); }
    catch (TimeoutException ex) { return Results.Conflict(new { error = ex.Message }); }
    catch (OperationCanceledException) { return Results.StatusCode(503); }
});

app.MapGet("/ws/device", async (HttpContext context, AppOptions options, BridgeCoordinator bridge) =>
{
    var auth = context.Request.Headers.Authorization.ToString();
    var supplied = auth.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase) ? auth[7..].Trim() : "";
    var deviceId = context.Request.Headers["X-Device-Id"].ToString();
    if (!options.DeviceTokenMatches(supplied) || deviceId != options.DeviceId || context.Request.Headers["X-Protocol-Version"] != "1")
    {
        context.Response.StatusCode = StatusCodes.Status401Unauthorized;
        return;
    }
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }
    using var socket = await context.WebSockets.AcceptWebSocketAsync();
    await bridge.HandleDeviceAsync(socket, context.RequestAborted);
}).AllowAnonymous();

app.MapGet("/ws/browser", async (HttpContext context, BridgeCoordinator bridge) =>
{
    var origin = context.Request.Headers.Origin.ToString();
    if (!string.Equals(origin, settings.PublicOrigin, StringComparison.OrdinalIgnoreCase))
    {
        context.Response.StatusCode = StatusCodes.Status403Forbidden;
        return;
    }
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }
    using var socket = await context.WebSockets.AcceptWebSocketAsync();
    await bridge.HandleBrowserAsync(socket, context.User.FindFirstValue(ClaimTypes.NameIdentifier)!, context.RequestAborted);
});

app.Run();

public partial class Program { }
