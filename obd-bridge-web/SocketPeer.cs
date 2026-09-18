using System.Net.WebSockets;

namespace ObdBridge.Web;

public sealed class SocketPeer(WebSocket socket, string? id = null) : IAsyncDisposable
{
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    public WebSocket Socket { get; } = socket;
    public string Id { get; } = id ?? Guid.NewGuid().ToString("N");

    public async Task SendAsync(WebSocketMessageType type, ReadOnlyMemory<byte> data, CancellationToken cancellationToken = default)
    {
        await _sendLock.WaitAsync(cancellationToken);
        try
        {
            if (Socket.State == WebSocketState.Open)
                await Socket.SendAsync(data, type, true, cancellationToken);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    public Task SendTextAsync(string text, CancellationToken cancellationToken = default) =>
        SendAsync(WebSocketMessageType.Text, System.Text.Encoding.UTF8.GetBytes(text), cancellationToken);

    public async ValueTask DisposeAsync()
    {
        try
        {
            if (Socket.State is WebSocketState.Open or WebSocketState.CloseReceived)
                await Socket.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "Session ended", CancellationToken.None);
        }
        catch (WebSocketException) { }
        Socket.Dispose();
    }
}
