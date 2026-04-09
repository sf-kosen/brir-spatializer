using System.IO;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Windows.Threading;
using BrirSpatializer.Audio;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.Wpf;

namespace BrirPlayer.Next.App.Web;

internal sealed class WebUiBridgeController : IDisposable
{
    private const string LocalWebUiUrl = "http://127.0.0.1:4176/";
    private readonly WebView2 _webView;
    private readonly Dispatcher _dispatcher;
    private readonly DeviceManager _deviceManager = new();
    private readonly AudioDeviceMonitor _deviceMonitor = new();
    private readonly LivePlaybackSession _livePlayback = new();
    private readonly JsonSerializerOptions _jsonOptions = new(JsonSerializerDefaults.Web);
    private BridgeRouteState _routeState;
    private BridgeStatePayload _latestPayload;
    private HttpListener? _httpListener;
    private CancellationTokenSource? _serverCts;
    private bool _isWebReady;

    public WebUiBridgeController(WebView2 webView, Dispatcher dispatcher)
    {
        _webView = webView;
        _dispatcher = dispatcher;
        _routeState = LoadInitialRouteState();
        _latestPayload = new BridgeStatePayload(true, false, "Idle", null, [], [], _routeState);
        _deviceMonitor.DevicesChanged += DeviceMonitor_DevicesChanged;
    }

    public async Task InitializeAsync()
    {
        StartLocalServer();
        await _webView.EnsureCoreWebView2Async();

        _webView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
        _webView.CoreWebView2.Settings.AreBrowserAcceleratorKeysEnabled = true;
        _webView.CoreWebView2.Settings.IsStatusBarEnabled = false;
        _webView.CoreWebView2.WebMessageReceived += CoreWebView2_WebMessageReceived;
        RefreshCachedState();

        _webView.Source = new Uri(LocalWebUiUrl);
    }

    public void Dispose()
    {
        _deviceMonitor.DevicesChanged -= DeviceMonitor_DevicesChanged;
        _deviceMonitor.Dispose();
        _livePlayback.Dispose();
        StopLocalServer();

        if (_webView.CoreWebView2 is not null)
        {
            _webView.CoreWebView2.WebMessageReceived -= CoreWebView2_WebMessageReceived;
        }
    }

    private async void DeviceMonitor_DevicesChanged(object? sender, EventArgs e)
    {
        await _dispatcher.InvokeAsync(() =>
        {
            NormalizeRoute();
            RefreshCachedState();
            PostState("devicesChanged");
        });
    }

    private void CoreWebView2_WebMessageReceived(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        using JsonDocument document = JsonDocument.Parse(e.WebMessageAsJson);
        if (!document.RootElement.TryGetProperty("type", out JsonElement typeElement))
        {
            return;
        }

        string? type = typeElement.GetString();
        JsonElement payload = document.RootElement.TryGetProperty("payload", out JsonElement foundPayload)
            ? foundPayload
            : default;

        if (type == "bridgeReady")
        {
            _isWebReady = true;
            NormalizeRoute();
            RefreshCachedState();
            PostState("bridgeState");
            return;
        }

        HandleCommand(type, payload);
        RefreshCachedState();
        PostState(type == "refreshDevices" ? "devicesChanged" : "routeUpdated");
    }

    private void PostState(string messageType)
    {
        if (!_isWebReady || _webView.CoreWebView2 is null)
        {
            return;
        }

        BridgeStatePayload payload = _latestPayload;

        string json = JsonSerializer.Serialize(new WebEnvelope(messageType, payload), _jsonOptions);
        _webView.CoreWebView2.PostWebMessageAsJson(json);
    }

    private BridgeStatePayload BuildStatePayload()
    {
        NormalizeRoute();
        return new BridgeStatePayload(
            true,
            _livePlayback.IsRunning,
            _livePlayback.SessionStatus,
            _livePlayback.LastError,
            GetInputDevices(),
            GetOutputDevices(),
            _routeState);
    }

    private void RefreshCachedState()
    {
        _latestPayload = BuildStatePayload();
    }

    private void HandleCommand(string? type, JsonElement payload)
    {
        switch (type)
        {
            case "refreshDevices":
                NormalizeRoute();
                break;

            case "setMode":
                if (payload.ValueKind == JsonValueKind.String && payload.GetString() is { } mode)
                {
                    _routeState = _routeState with { Mode = mode };
                    NormalizeRoute();
                    RestartPlaybackIfNeeded();
                }
                break;

            case "setInputDevice":
                if (payload.ValueKind == JsonValueKind.String && payload.GetString() is { } inputId)
                {
                    _routeState = _routeState with { InputId = inputId };
                    NormalizeRoute();
                    RestartPlaybackIfNeeded();
                }
                break;

            case "setOutputDevice":
                if (payload.ValueKind == JsonValueKind.String && payload.GetString() is { } outputId)
                {
                    _routeState = _routeState with
                    {
                        OutputId = outputId,
                        OutputVolume = GetOutputVolume(outputId)
                    };
                    NormalizeRoute();
                    RestartPlaybackIfNeeded();
                }
                break;

            case "setOutputVolume":
                if (TryReadNumber(payload, out double outputVolume))
                {
                    double clamped = Math.Clamp(outputVolume, 0.0, 1.0);
                    SetOutputVolume(_routeState.OutputId, clamped);
                    _routeState = _routeState with { OutputVolume = GetOutputVolume(_routeState.OutputId) };
                }
                break;

            case "setExclusive":
                if (payload.ValueKind is JsonValueKind.True or JsonValueKind.False)
                {
                    _routeState = _routeState with { Exclusive = payload.GetBoolean() };
                    RestartPlaybackIfNeeded();
                }
                break;

            case "setVolumeMatch":
                if (payload.ValueKind is JsonValueKind.True or JsonValueKind.False)
                {
                    _routeState = _routeState with { VolumeMatch = payload.GetBoolean() };
                    _livePlayback.UpdateVolumeMatch(_routeState.VolumeMatch);
                    RestartPlaybackIfNeeded();
                }
                break;

            case "startLive":
                if (TryReadPlaybackRequest(payload, out PlaybackRequest? startRequest) && startRequest is not null)
                {
                    TryRun(() => _livePlayback.StartOrRestart(_routeState, startRequest));
                }
                break;

            case "stopLive":
                _livePlayback.Stop();
                break;

            case "setSeat":
                if (TryReadPlaybackRequest(payload, out PlaybackRequest? seatRequest) && seatRequest is not null)
                {
                    _livePlayback.UpdateSeat(seatRequest.SeatX, seatRequest.SeatY);
                }
                break;

            case "setPreset":
                if (_livePlayback.IsRunning &&
                    TryReadPlaybackRequest(payload, out PlaybackRequest? presetRequest) &&
                    presetRequest is not null)
                {
                    TryRun(() => _livePlayback.StartOrRestart(_routeState, presetRequest));
                }
                break;
        }
    }

    private void NormalizeRoute()
    {
        IReadOnlyList<BridgeDevice> inputDevices = GetInputDevices();
        IReadOnlyList<BridgeDevice> outputDevices = GetOutputDevices();

        string inputId = inputDevices.Any(device => device.Id == _routeState.InputId)
            ? _routeState.InputId
            : inputDevices.FirstOrDefault()?.Id ?? string.Empty;

        string outputId = outputDevices.Any(device => device.Id == _routeState.OutputId)
            ? _routeState.OutputId
            : outputDevices.FirstOrDefault()?.Id ?? string.Empty;

        _routeState = _routeState with
        {
            InputId = inputId,
            OutputId = outputId,
            OutputVolume = GetOutputVolume(outputId)
        };
    }

    private IReadOnlyList<BridgeDevice> GetInputDevices()
    {
        return _deviceManager.GetInputDevices()
            .Select(CreateBridgeDevice)
            .OrderBy(device => device.Label, StringComparer.CurrentCultureIgnoreCase)
            .ToList();
    }

    private IReadOnlyList<BridgeDevice> GetOutputDevices()
    {
        return _deviceManager.GetOutputDevices()
            .Select(CreateBridgeDevice)
            .OrderBy(device => device.Label, StringComparer.CurrentCultureIgnoreCase)
            .ToList();
    }

    private BridgeDevice CreateBridgeDevice(AudioDeviceInfo info)
    {
        var device = _deviceManager.TryGetDevice(info.Id);
        string detail;

        if (device?.AudioClient?.MixFormat is { } format)
        {
            detail = $"{format.SampleRate / 1000.0:F1} kHz / {format.Channels} ch";
        }
        else
        {
            detail = "Unavailable";
        }

        return new BridgeDevice(info.Id, info.Name, detail);
    }

    private double GetOutputVolume(string outputId)
    {
        var device = _deviceManager.TryGetDevice(outputId);
        if (device?.AudioEndpointVolume is null)
        {
            return 1.0;
        }

        return Math.Clamp(device.AudioEndpointVolume.MasterVolumeLevelScalar, 0.0f, 1.0f);
    }

    private void SetOutputVolume(string outputId, double volume)
    {
        var device = _deviceManager.TryGetDevice(outputId);
        if (device?.AudioEndpointVolume is null)
        {
            return;
        }

        device.AudioEndpointVolume.MasterVolumeLevelScalar = (float)Math.Clamp(volume, 0.0, 1.0);
    }

    private static bool TryReadNumber(JsonElement payload, out double value)
    {
        if (payload.ValueKind == JsonValueKind.Number && payload.TryGetDouble(out value))
        {
            return true;
        }

        if (payload.ValueKind == JsonValueKind.String && double.TryParse(payload.GetString(), out value))
        {
            return true;
        }

        value = 0;
        return false;
    }

    private bool TryReadPlaybackRequest(JsonElement payload, out PlaybackRequest? request)
    {
        try
        {
            request = payload.Deserialize<PlaybackRequest>(_jsonOptions);
            return request is not null;
        }
        catch
        {
            request = null;
            return false;
        }
    }

    private void RestartPlaybackIfNeeded()
    {
        PlaybackRequest? request = _livePlayback.GetActiveRequest();
        if (!_livePlayback.IsRunning || request is null)
        {
            return;
        }

        TryRun(() => _livePlayback.StartOrRestart(_routeState, request));
    }

    private void TryRun(Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            _livePlayback.ReportFailure(ex);
        }
    }

    private void StartLocalServer()
    {
        if (_httpListener is not null)
        {
            return;
        }

        _httpListener = new HttpListener();
        _httpListener.Prefixes.Add(LocalWebUiUrl);
        _httpListener.Start();
        _serverCts = new CancellationTokenSource();
        _ = Task.Run(() => RunLocalServerAsync(_serverCts.Token));
    }

    private void StopLocalServer()
    {
        _serverCts?.Cancel();
        _serverCts?.Dispose();
        _serverCts = null;

        if (_httpListener is null)
        {
            return;
        }

        try
        {
            _httpListener.Stop();
            _httpListener.Close();
        }
        catch
        {
        }

        _httpListener = null;
    }

    private async Task RunLocalServerAsync(CancellationToken cancellationToken)
    {
        HttpListener listener = _httpListener
            ?? throw new InvalidOperationException("Local web server is not initialized.");

        while (!cancellationToken.IsCancellationRequested)
        {
            HttpListenerContext context;
            try
            {
                context = await listener.GetContextAsync();
            }
            catch when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (HttpListenerException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }

            _ = Task.Run(() => HandleHttpRequestAsync(context), cancellationToken);
        }
    }

    private async Task HandleHttpRequestAsync(HttpListenerContext context)
    {
        try
        {
            string path = context.Request.Url?.AbsolutePath ?? "/";
            if (path.Equals("/api/state", StringComparison.OrdinalIgnoreCase))
            {
                await WriteJsonAsync(context.Response, _latestPayload);
                return;
            }

            if (path.Equals("/api/command", StringComparison.OrdinalIgnoreCase))
            {
                if (!HttpMethodsMatch(context.Request.HttpMethod, "POST"))
                {
                    context.Response.StatusCode = 405;
                    context.Response.Close();
                    return;
                }

                using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding ?? Encoding.UTF8);
                string body = await reader.ReadToEndAsync();
                using JsonDocument document = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
                string? type = document.RootElement.TryGetProperty("type", out JsonElement typeElement)
                    ? typeElement.GetString()
                    : null;
                JsonElement payloadElement = document.RootElement.TryGetProperty("payload", out JsonElement foundPayload)
                    ? foundPayload
                    : default;

                BridgeStatePayload payload = await _dispatcher.InvokeAsync(() =>
                {
                    HandleCommand(type, payloadElement);
                    RefreshCachedState();
                    return _latestPayload;
                });

                await WriteJsonAsync(context.Response, payload);
                return;
            }

            await ServeStaticAsync(context.Response, path);
        }
        catch (Exception ex)
        {
            try
            {
                context.Response.StatusCode = 500;
                await WriteJsonAsync(context.Response, new { error = ex.Message });
            }
            catch
            {
            }
        }
    }

    private static bool HttpMethodsMatch(string? actual, string expected)
    {
        return string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase);
    }

    private async Task WriteJsonAsync(HttpListenerResponse response, object payload)
    {
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(payload, _jsonOptions);
        response.StatusCode = 200;
        response.ContentType = "application/json; charset=utf-8";
        response.ContentLength64 = bytes.Length;
        await response.OutputStream.WriteAsync(bytes);
        response.Close();
    }

    private static async Task ServeStaticAsync(HttpListenerResponse response, string path)
    {
        string root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "WebUI"));
        string relativePath = path.TrimStart('/').Replace('/', Path.DirectorySeparatorChar);
        string candidate = string.IsNullOrWhiteSpace(relativePath)
            ? Path.Combine(root, "index.html")
            : Path.Combine(root, relativePath);

        if (Directory.Exists(candidate))
        {
            candidate = Path.Combine(candidate, "index.html");
        }

        if (!File.Exists(candidate))
        {
            candidate = Path.Combine(root, "index.html");
        }

        if (!File.Exists(candidate))
        {
            response.StatusCode = 404;
            response.Close();
            return;
        }

        string extension = Path.GetExtension(candidate).ToLowerInvariant();
        response.ContentType = extension switch
        {
            ".html" => "text/html; charset=utf-8",
            ".js" => "text/javascript; charset=utf-8",
            ".css" => "text/css; charset=utf-8",
            ".json" => "application/json; charset=utf-8",
            ".svg" => "image/svg+xml",
            ".png" => "image/png",
            ".jpg" or ".jpeg" => "image/jpeg",
            _ => "application/octet-stream"
        };

        byte[] bytes = await File.ReadAllBytesAsync(candidate);
        response.StatusCode = 200;
        response.ContentLength64 = bytes.Length;
        await response.OutputStream.WriteAsync(bytes);
        response.Close();
    }

    private static BridgeRouteState LoadInitialRouteState()
    {
        string path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "..", "..", "..", "..",
            "BrirSpatializer.App",
            "bin",
            "Debug",
            "net8.0-windows",
            "Settings",
            "last_settings.json"));

        if (!File.Exists(path))
        {
            return new BridgeRouteState("Virtual Cable", string.Empty, string.Empty, 1.0, true, true);
        }

        LegacySettingsEnvelope? envelope = JsonSerializer.Deserialize<LegacySettingsEnvelope>(File.ReadAllText(path));
        return new BridgeRouteState(
            "Virtual Cable",
            envelope?.InputDeviceId ?? string.Empty,
            envelope?.OutputDeviceId ?? string.Empty,
            1.0,
            true,
            true);
    }
}

file sealed class LegacySettingsEnvelope
{
    public string? InputDeviceId { get; set; }
    public string? OutputDeviceId { get; set; }
}
