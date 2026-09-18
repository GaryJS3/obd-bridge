using System.Security.Cryptography;
using System.Text;

namespace ObdBridge.Web;

public sealed class AppOptions
{
    public required string WebPassword { get; init; }
    public required string DeviceToken { get; init; }
    public required string PublicOrigin { get; init; }
    public string DeviceId { get; init; } = "obd-bridge-01";
    public int AuthCookieDays { get; init; } = 30;
    public int DeviceOfflineSeconds { get; init; } = 30;
    public long OtaMaxBytes { get; init; } = 1_650_000;

    public static AppOptions Load(IConfiguration configuration)
    {
        var password = configuration["WEB_PASSWORD"];
        var token = configuration["DEVICE_TOKEN"];
        var origin = configuration["PUBLIC_ORIGIN"] ?? "https://car.gary.systems";
        if (string.IsNullOrWhiteSpace(password) || password.Length < 12)
            throw new InvalidOperationException("WEB_PASSWORD must be configured with at least 12 characters.");
        if (string.IsNullOrWhiteSpace(token) || token.Length < 32)
            throw new InvalidOperationException("DEVICE_TOKEN must be configured with at least 32 characters.");
        if (!Uri.TryCreate(origin, UriKind.Absolute, out var originUri) || originUri.Scheme != Uri.UriSchemeHttps || originUri.AbsolutePath != "/")
            throw new InvalidOperationException("PUBLIC_ORIGIN must be an HTTPS origin, for example https://car.gary.systems.");

        var maxBytes = long.TryParse(configuration["OTA_MAX_BYTES"], out var parsed) ? parsed : 1_800_000;
        if (maxBytes is < 1 or > 1_800_000)
            throw new InvalidOperationException("OTA_MAX_BYTES must be between 1 and 1800000 for the configured ESP32 OTA slots.");

        return new AppOptions
        {
            WebPassword = password,
            DeviceToken = token,
            PublicOrigin = originUri.GetLeftPart(UriPartial.Authority),
            DeviceId = configuration["DEVICE_ID"] ?? "obd-bridge-01",
            AuthCookieDays = int.TryParse(configuration["AUTH_COOKIE_DAYS"], out var days) ? Math.Clamp(days, 1, 90) : 30,
            DeviceOfflineSeconds = int.TryParse(configuration["DEVICE_OFFLINE_SECONDS"], out var offline) ? Math.Clamp(offline, 10, 300) : 30,
            OtaMaxBytes = maxBytes
        };
    }

    public bool PasswordMatches(string candidate) => FixedMatches(WebPassword, candidate);
    public bool DeviceTokenMatches(string candidate) => FixedMatches(DeviceToken, candidate);
    public string PasswordStamp => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(WebPassword)));
    public bool PasswordStampMatches(string candidate) => FixedMatches(PasswordStamp, candidate);

    private static bool FixedMatches(string expected, string actual)
    {
        var expectedHash = SHA256.HashData(Encoding.UTF8.GetBytes(expected));
        var actualHash = SHA256.HashData(Encoding.UTF8.GetBytes(actual));
        return CryptographicOperations.FixedTimeEquals(expectedHash, actualHash);
    }
}
