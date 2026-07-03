using System.Text.Json;
using CoreVideoPro.MediaCore.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class CoreProtocolParser
{
    public static CoreResponseEnvelope? TryParseResponse(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            if (!document.RootElement.TryGetProperty("id", out var idElement) ||
                idElement.ValueKind != JsonValueKind.String)
            {
                return null;
            }

            var envelope = JsonSerializer.Deserialize<CoreResponseEnvelope>(trimmed, MediaCoreJson.Options);
            return envelope?.Id is { Length: > 0 } ? envelope : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static CoreZoomVideoFrameEvent? TryParseEvent(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _))
            {
                return null;
            }

            if (!root.TryGetProperty("type", out var typeElement) ||
                typeElement.ValueKind != JsonValueKind.String)
            {
                return null;
            }

            return typeElement.GetString() switch
            {
                "zoom-video-frame" => TryParseZoomVideoFrameEvent(root),
                _ => null
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static CoreProgramFramePreviewEvent? TryParseProgramFramePreviewEvent(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _))
            {
                return null;
            }

            if (!root.TryGetProperty("type", out var typeElement) ||
                typeElement.GetString() != "program-frame-preview" ||
                !root.TryGetProperty("preview", out var previewElement))
            {
                return null;
            }

            var preview = TryParseProgramFramePreview(previewElement);
            if (preview is null)
            {
                return null;
            }

            if (preview.Bgra is not { Length: > 0 } && preview.SharedTexture is null)
            {
                return null;
            }

            return new CoreProgramFramePreviewEvent
            {
                Preview = ToProgramFramePreview(preview)
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static CoreProgramSharedTextureEvent? TryParseProgramSharedTextureEvent(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _))
            {
                return null;
            }

            if (!root.TryGetProperty("type", out var typeElement) ||
                typeElement.GetString() != "program-shared-texture" ||
                !root.TryGetProperty("texture", out var textureElement))
            {
                return null;
            }

            var texture = TryParseProgramSharedTexture(textureElement);
            return texture is null
                ? null
                : new CoreProgramSharedTextureEvent { Texture = texture };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static CorePreviewSharedTextureEvent? TryParsePreviewSharedTextureEvent(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _))
            {
                return null;
            }

            if (!root.TryGetProperty("type", out var typeElement) ||
                typeElement.GetString() != "preview-shared-texture" ||
                !root.TryGetProperty("texture", out var textureElement))
            {
                return null;
            }

            var texture = TryParseProgramSharedTexture(textureElement);
            return texture is null
                ? null
                : new CorePreviewSharedTextureEvent { Texture = texture };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static CoreParticipantSharedTextureEvent? TryParseParticipantSharedTextureEvent(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _))
            {
                return null;
            }

            if (!root.TryGetProperty("type", out var typeElement) ||
                typeElement.GetString() != "participant-shared-texture" ||
                !root.TryGetProperty("participantId", out var pidElement) ||
                pidElement.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty("texture", out var textureElement))
            {
                return null;
            }

            var participantId = pidElement.GetString() ?? string.Empty;
            var texture = TryParseProgramSharedTexture(textureElement);
            if (string.IsNullOrWhiteSpace(participantId) || texture is null)
            {
                return null;
            }

            return new CoreParticipantSharedTextureEvent
            {
                Texture = new ParticipantSharedTexture
                {
                    ParticipantId = participantId,
                    SharedHandleHex = texture.SharedHandleHex,
                    Width = texture.Width,
                    Height = texture.Height,
                    Format = texture.Format,
                    FrameNumber = texture.FrameNumber,
                },
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    public static CoreMultiviewSharedTextureEvent? TryParseMultiviewSharedTextureEvent(string line)
    {
        var trimmed = line.Trim();
        if (trimmed.Length == 0)
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(trimmed);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _))
            {
                return null;
            }

            if (!root.TryGetProperty("type", out var typeElement) ||
                typeElement.GetString() != "multiview-shared-texture" ||
                !root.TryGetProperty("texture", out var textureElement))
            {
                return null;
            }

            var texture = TryParseProgramSharedTexture(textureElement);
            if (texture is null)
            {
                return null;
            }

            var canvasWidth = root.TryGetProperty("canvasWidth", out var cw) && cw.ValueKind == JsonValueKind.Number
                ? cw.GetInt32()
                : texture.Width;
            var canvasHeight = root.TryGetProperty("canvasHeight", out var ch) && ch.ValueKind == JsonValueKind.Number
                ? ch.GetInt32()
                : texture.Height;

            var tiles = new List<MultiviewTile>();
            if (root.TryGetProperty("tiles", out var tilesElement) && tilesElement.ValueKind == JsonValueKind.Array)
            {
                foreach (var tileElement in tilesElement.EnumerateArray())
                {
                    if (tileElement.ValueKind != JsonValueKind.Object)
                    {
                        continue;
                    }

                    tiles.Add(new MultiviewTile
                    {
                        SourceId = ReadString(tileElement, "sourceId"),
                        ParticipantId = ReadString(tileElement, "participantId"),
                        Slot = ReadInt(tileElement, "slot"),
                        Label = ReadString(tileElement, "label"),
                        Role = ReadString(tileElement, "role") is { Length: > 0 } role ? role : "source",
                        Tally = ReadString(tileElement, "tally") is { Length: > 0 } tally ? tally : "none",
                        ActiveSpeaker = tileElement.TryGetProperty("activeSpeaker", out var asElement) &&
                            (asElement.ValueKind == JsonValueKind.True ||
                             (asElement.ValueKind == JsonValueKind.Number && asElement.GetDouble() != 0)),
                        X = ReadDouble(tileElement, "x"),
                        Y = ReadDouble(tileElement, "y"),
                        W = ReadDouble(tileElement, "w"),
                        H = ReadDouble(tileElement, "h")
                    });
                }
            }

            return new CoreMultiviewSharedTextureEvent
            {
                Multiview = new MultiviewSharedTexture
                {
                    Texture = texture,
                    CanvasWidth = canvasWidth > 0 ? canvasWidth : texture.Width,
                    CanvasHeight = canvasHeight > 0 ? canvasHeight : texture.Height,
                    Tiles = tiles
                }
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static string ReadString(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static int ReadInt(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number
            ? value.GetInt32()
            : 0;

    private static double ReadDouble(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number
            ? value.GetDouble()
            : 0;

    public static NativeMediaCoreProgramFramePreview? TryParseProgramFramePreview(JsonElement previewElement)
    {
        if (previewElement.ValueKind != JsonValueKind.Object ||
            !previewElement.TryGetProperty("width", out var widthElement) ||
            widthElement.ValueKind != JsonValueKind.Number ||
            !previewElement.TryGetProperty("height", out var heightElement) ||
            heightElement.ValueKind != JsonValueKind.Number ||
            !previewElement.TryGetProperty("frameNumber", out var frameNumberElement) ||
            frameNumberElement.ValueKind != JsonValueKind.Number)
        {
            return null;
        }

        var width = widthElement.GetInt32();
        var height = heightElement.GetInt32();
        if (width <= 0 || height > 180 || width > 320 || height <= 0)
        {
            return null;
        }

        byte[]? bgra = null;
        string? bgraBase64 = null;
        if (previewElement.TryGetProperty("bgraBase64", out var base64Element) &&
            base64Element.ValueKind == JsonValueKind.String)
        {
            bgraBase64 = base64Element.GetString();
            if (!string.IsNullOrWhiteSpace(bgraBase64))
            {
                try
                {
                    bgra = Convert.FromBase64String(bgraBase64);
                }
                catch (FormatException)
                {
                    return null;
                }

                if (bgra.Length != width * height * 4)
                {
                    return null;
                }
            }
        }

        var sharedTexture = previewElement.TryGetProperty("sharedTexture", out var sharedTextureElement)
            ? TryParseProgramSharedTexture(sharedTextureElement)
            : null;

        if (bgra is not { Length: > 0 } && sharedTexture is null)
        {
            return null;
        }

        return new NativeMediaCoreProgramFramePreview
        {
            FrameNumber = frameNumberElement.GetInt32(),
            Width = width,
            Height = height,
            RenderPlanId = previewElement.TryGetProperty("renderPlanId", out var renderPlanIdElement) &&
                           renderPlanIdElement.ValueKind == JsonValueKind.String
                ? renderPlanIdElement.GetString()
                : null,
            Renderer = previewElement.TryGetProperty("renderer", out var rendererElement) &&
                       rendererElement.ValueKind == JsonValueKind.String
                ? rendererElement.GetString() ?? "software"
                : "software",
            Health = previewElement.TryGetProperty("health", out var healthElement) &&
                     healthElement.ValueKind == JsonValueKind.String
                ? healthElement.GetString() ?? "live"
                : "live",
            PixelFormat = previewElement.TryGetProperty("pixelFormat", out var pixelFormatElement) &&
                          pixelFormatElement.ValueKind == JsonValueKind.String
                ? pixelFormatElement.GetString() ?? "bgra"
                : "bgra",
            Bgra = bgra,
            BgraBase64 = bgraBase64,
            SharedTexture = ToNativeSharedTexture(sharedTexture)
        };
    }

    public static ProgramSharedTexture? TryParseProgramSharedTexture(JsonElement textureElement)
    {
        if (textureElement.ValueKind != JsonValueKind.Object ||
            !textureElement.TryGetProperty("sharedHandleHex", out var handleElement) ||
            handleElement.ValueKind != JsonValueKind.String ||
            !textureElement.TryGetProperty("width", out var widthElement) ||
            widthElement.ValueKind != JsonValueKind.Number ||
            !textureElement.TryGetProperty("height", out var heightElement) ||
            heightElement.ValueKind != JsonValueKind.Number)
        {
            return null;
        }

        var width = widthElement.GetInt32();
        var height = heightElement.GetInt32();
        var handleHex = handleElement.GetString() ?? string.Empty;
        if (string.IsNullOrWhiteSpace(handleHex) || width <= 0 || height <= 0)
        {
            return null;
        }

        var frameNumber = textureElement.TryGetProperty("frameNumber", out var frameNumberElement) &&
                          frameNumberElement.ValueKind == JsonValueKind.Number
            ? frameNumberElement.GetInt32()
            : 0;

        return new ProgramSharedTexture
        {
            SharedHandleHex = handleHex,
            Width = width,
            Height = height,
            Format = textureElement.TryGetProperty("format", out var formatElement) &&
                     formatElement.ValueKind == JsonValueKind.String
                ? formatElement.GetString() ?? "B8G8R8A8_UNORM"
                : "B8G8R8A8_UNORM",
            FrameNumber = frameNumber
        };
    }

    public static ulong ParseSharedHandleHex(string handleHex)
    {
        var trimmed = handleHex.Trim();
        if (trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            trimmed = trimmed[2..];
        }

        return ulong.TryParse(trimmed, System.Globalization.NumberStyles.HexNumber, null, out var value)
            ? value
            : 0;
    }

    public static NativeMediaCoreProfile? TryParseHandshakeProfile(JsonDocument response)
    {
        if (!response.RootElement.TryGetProperty("ok", out var okElement) ||
            !okElement.GetBoolean() ||
            !response.RootElement.TryGetProperty("profile", out var profileElement))
        {
            return null;
        }

        return JsonSerializer.Deserialize<NativeMediaCoreProfile>(profileElement.GetRawText(), MediaCoreJson.Options);
    }

    public static NativeMediaCoreStateSnapshot? TryParseSyncSnapshot(JsonDocument response)
    {
        var root = response.RootElement;
        if (!root.TryGetProperty("ok", out var okElement) || !okElement.GetBoolean())
        {
            return null;
        }

        if (root.TryGetProperty("snapshot", out var snapshotElement))
        {
            return JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>(
                snapshotElement.GetRawText(),
                MediaCoreJson.Options);
        }

        if (root.TryGetProperty("state", out var stateElement))
        {
            return JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>(
                stateElement.GetRawText(),
                MediaCoreJson.Options);
        }

        return null;
    }

    public static NativeMediaCoreWireState? TryParseWireState(JsonDocument response)
    {
        var root = response.RootElement;
        JsonElement wireElement;
        if (root.TryGetProperty("snapshot", out var snapshotElement))
        {
            wireElement = snapshotElement;
        }
        else if (root.TryGetProperty("state", out var stateElement))
        {
            wireElement = stateElement;
        }
        else
        {
            return null;
        }

        if (!NativeMediaCoreStateMapper.IsNativeMediaCoreWireState(wireElement))
        {
            return null;
        }

        return JsonSerializer.Deserialize<NativeMediaCoreWireState>(wireElement.GetRawText(), MediaCoreJson.Options);
    }

    public static ZoomMediaSpineNativeSnapshot? TryParseZoomMediaSpineSnapshot(JsonDocument response)
    {
        var root = response.RootElement;
        if (!root.TryGetProperty("ok", out var okElement) ||
            !okElement.GetBoolean() ||
            !root.TryGetProperty("type", out var typeElement) ||
            typeElement.GetString() != "zoom-media-spine-sync")
        {
            return null;
        }

        if (!root.TryGetProperty("spineSnapshot", out var spineElement))
        {
            return null;
        }

        return JsonSerializer.Deserialize<ZoomMediaSpineNativeSnapshot>(
            spineElement.GetRawText(),
            MediaCoreJson.Options);
    }

    public static RawCaptureSnapshot? TryParseCaptureSnapshot(JsonDocument response, string responseType)
    {
        var root = response.RootElement;
        if (!root.TryGetProperty("ok", out var okElement) ||
            !okElement.GetBoolean() ||
            !root.TryGetProperty("type", out var typeElement) ||
            typeElement.GetString() != responseType ||
            !root.TryGetProperty("snapshot", out var snapshotElement))
        {
            return null;
        }

        var snapshot = JsonSerializer.Deserialize<RawCaptureSnapshot>(snapshotElement.GetRawText(), MediaCoreJson.Options);
        if (snapshot is null)
        {
            return null;
        }

        return new RawCaptureSnapshot
        {
            MeetingState = ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(snapshot.MeetingState),
            Participants = snapshot.Participants,
            ActiveSpeakerId = snapshot.ActiveSpeakerId,
            Caption = snapshot.Caption,
            Tick = snapshot.Tick,
            Warnings = snapshot.Warnings
        };
    }

    /// <summary>
    /// Parses a "capture-devices" response ({"ok":true,"type":"capture-devices",
    /// "devices":[...]}) into typed device statuses. Returns null when the
    /// response is not a successful capture-devices payload.
    /// </summary>
    public static IReadOnlyList<NativeCaptureDeviceStatus>? TryParseCaptureDevices(JsonDocument response)
    {
        var root = response.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("ok", out var okElement) ||
            okElement.ValueKind != JsonValueKind.True ||
            !root.TryGetProperty("type", out var typeElement) ||
            typeElement.GetString() != "capture-devices" ||
            !root.TryGetProperty("devices", out var devicesElement) ||
            devicesElement.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        var devices = new List<NativeCaptureDeviceStatus>();
        foreach (var device in devicesElement.EnumerateArray())
        {
            if (device.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            var id = ReadString(device, "id");
            if (string.IsNullOrEmpty(id))
            {
                continue;
            }

            var width = 0;
            var height = 0;
            if (device.TryGetProperty("resolution", out var resolution) &&
                resolution.ValueKind == JsonValueKind.Object)
            {
                width = ReadInt(resolution, "width");
                height = ReadInt(resolution, "height");
            }

            var warning = ReadString(device, "warning");
            var nativeDeviceId = ReadString(device, "nativeDeviceId");
            devices.Add(new NativeCaptureDeviceStatus
            {
                Id = id,
                Name = ReadString(device, "name"),
                Vendor = ReadString(device, "vendor"),
                ConnectionState = ReadString(device, "connectionState"),
                SignalPresent = device.TryGetProperty("signalPresent", out var signal) &&
                    signal.ValueKind == JsonValueKind.True,
                Width = width,
                Height = height,
                FrameRate = ReadInt(device, "frameRate"),
                Warning = warning.Length > 0 ? warning : null,
                NativeDeviceId = nativeDeviceId.Length > 0 ? nativeDeviceId : null
            });
        }

        return devices;
    }

    public static string? TryParseErrorMessage(JsonDocument response)
    {
        if (response.RootElement.TryGetProperty("ok", out var okElement) &&
            okElement.GetBoolean())
        {
            return null;
        }

        var messages = new List<string>();
        if (response.RootElement.TryGetProperty("error", out var errorElement) &&
            errorElement.ValueKind == JsonValueKind.String &&
            errorElement.GetString() is { Length: > 0 } errorText)
        {
            messages.Add(errorText);
        }
        else if (response.RootElement.TryGetProperty("error", out errorElement) &&
            errorElement.ValueKind == JsonValueKind.Object)
        {
            AddStringProperty(errorElement, "message", messages);
            AddStringProperty(errorElement, "detail", messages);
            AddStringProperty(errorElement, "details", messages);
            AddStringProperty(errorElement, "code", messages);
            AddStringProperty(errorElement, "reason", messages);
            AddStringProperty(errorElement, "hint", messages);
            AddStringProperty(errorElement, "stderr", messages);
            AddStringArray(errorElement, "warnings", messages);
            AddStringArray(errorElement, "events", messages);
            AddStringArray(errorElement, "errors", messages);
        }

        AddStringArray(response.RootElement, "warnings", messages);
        AddStringArray(response.RootElement, "events", messages);

        if (response.RootElement.TryGetProperty("snapshot", out var snapshotElement) &&
            snapshotElement.ValueKind == JsonValueKind.Object)
        {
            AddStringArray(snapshotElement, "warnings", messages);
            AddStringArray(snapshotElement, "events", messages);
            AddOutputSenderSessionMessages(snapshotElement, messages);
        }

        var message = string.Join(" ", messages
            .Where(text => !string.IsNullOrWhiteSpace(text))
            .Select(text => text.Trim())
            .Distinct(StringComparer.Ordinal));

        if (message.Length > 0)
        {
            return message;
        }

        return "Media core request failed.";
    }

    private static void AddStringProperty(JsonElement element, string propertyName, List<string> messages)
    {
        if (element.TryGetProperty(propertyName, out var property) &&
            property.ValueKind == JsonValueKind.String &&
            property.GetString() is { Length: > 0 } value)
        {
            messages.Add(value);
        }
    }

    private static void AddStringArray(JsonElement element, string propertyName, List<string> messages)
    {
        if (!element.TryGetProperty(propertyName, out var property) ||
            property.ValueKind != JsonValueKind.Array)
        {
            return;
        }

        foreach (var item in property.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String &&
                item.GetString() is { Length: > 0 } value)
            {
                messages.Add(value);
            }
        }
    }

    private static void AddOutputSenderSessionMessages(JsonElement snapshotElement, List<string> messages)
    {
        if (!snapshotElement.TryGetProperty("outputSenderSession", out var sessionElement) ||
            sessionElement.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        AddStringArray(sessionElement, "warnings", messages);
        if (!sessionElement.TryGetProperty("senders", out var sendersElement) ||
            sendersElement.ValueKind != JsonValueKind.Array)
        {
            return;
        }

        foreach (var senderElement in sendersElement.EnumerateArray())
        {
            if (senderElement.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            var destination = senderElement.TryGetProperty("destination", out var destinationElement) &&
                              destinationElement.ValueKind == JsonValueKind.String
                ? destinationElement.GetString()
                : null;
            var status = senderElement.TryGetProperty("status", out var statusElement) &&
                         statusElement.ValueKind == JsonValueKind.String
                ? statusElement.GetString()
                : null;
            var label = string.IsNullOrWhiteSpace(destination)
                ? "Output sender"
                : $"{destination!.Trim().ToUpperInvariant()} sender";

            AddSenderDetail(senderElement, "lastError", label, status, messages);
            AddSenderDetail(senderElement, "warning", label, status, messages);
        }
    }

    private static void AddSenderDetail(
        JsonElement senderElement,
        string propertyName,
        string label,
        string? status,
        List<string> messages)
    {
        if (!senderElement.TryGetProperty(propertyName, out var property) ||
            property.ValueKind != JsonValueKind.String ||
            property.GetString() is not { Length: > 0 } detail)
        {
            return;
        }

        var statusText = string.IsNullOrWhiteSpace(status) ? string.Empty : $" {status!.Trim()}";
        messages.Add($"{label}{statusText}: {detail.Trim()}");
    }

    public static string DescribeUnexpectedCaptureResponse(JsonDocument response, string responseType)
    {
        var error = TryParseErrorMessage(response);
        if (!string.IsNullOrWhiteSpace(error))
        {
            return $"{responseType} failed: {error}";
        }

        var root = response.RootElement;
        if (root.TryGetProperty("snapshot", out var snapshotElement) &&
            snapshotElement.TryGetProperty("warnings", out var warningsElement) &&
            warningsElement.ValueKind == JsonValueKind.Array)
        {
            foreach (var warning in warningsElement.EnumerateArray())
            {
                if (warning.ValueKind == JsonValueKind.String)
                {
                    var text = warning.GetString();
                    if (!string.IsNullOrWhiteSpace(text))
                    {
                        return $"{responseType} failed: {text}";
                    }
                }
            }
        }

        if (root.TryGetProperty("snapshot", out var stateElement) &&
            stateElement.TryGetProperty("meetingState", out var meetingStateElement))
        {
            return $"{responseType} failed: meeting state '{meetingStateElement.GetString()}'.";
        }

        return $"{responseType} failed: unexpected media-core response.";
    }

    private static CoreZoomVideoFrameEvent? TryParseZoomVideoFrameEvent(JsonElement root)
    {
        if (!root.TryGetProperty("frame", out var frameElement) ||
            !frameElement.TryGetProperty("participantId", out var participantIdElement) ||
            participantIdElement.ValueKind != JsonValueKind.String ||
            !frameElement.TryGetProperty("width", out var widthElement) ||
            widthElement.ValueKind != JsonValueKind.Number ||
            !frameElement.TryGetProperty("height", out var heightElement) ||
            heightElement.ValueKind != JsonValueKind.Number ||
            !frameElement.TryGetProperty("frameId", out var frameIdElement) ||
            frameIdElement.ValueKind != JsonValueKind.Number ||
            !frameElement.TryGetProperty("bgraBase64", out var base64Element) ||
            base64Element.ValueKind != JsonValueKind.String)
        {
            return null;
        }

        var width = widthElement.GetInt32();
        var height = heightElement.GetInt32();
        if (width <= 0 || height <= 0)
        {
            return null;
        }

        byte[] bgra;
        try
        {
            bgra = Convert.FromBase64String(base64Element.GetString() ?? string.Empty);
        }
        catch (FormatException)
        {
            return null;
        }

        if (bgra.Length != width * height * 4)
        {
            return null;
        }

        return new CoreZoomVideoFrameEvent
        {
            Frame = new ZoomVideoFrame
            {
                ParticipantId = participantIdElement.GetString()!,
                Width = width,
                Height = height,
                FrameId = frameIdElement.GetInt32(),
                Bgra = bgra
            }
        };
    }

    private static NativeMediaCoreProgramSharedTexture? ToNativeSharedTexture(ProgramSharedTexture? texture) =>
        texture is null
            ? null
            : new NativeMediaCoreProgramSharedTexture
            {
                SharedHandleHex = texture.SharedHandleHex,
                Width = texture.Width,
                Height = texture.Height,
                Format = texture.Format,
                FrameNumber = texture.FrameNumber
            };

    private static ProgramFramePreview ToProgramFramePreview(NativeMediaCoreProgramFramePreview preview) =>
        new()
        {
            FrameNumber = preview.FrameNumber,
            Width = preview.Width,
            Height = preview.Height,
            RenderPlanId = preview.RenderPlanId,
            Renderer = preview.Renderer,
            Health = preview.Health,
            PixelFormat = preview.PixelFormat,
            Bgra = preview.Bgra ?? [],
            SharedTexture = preview.SharedTexture is null
                ? null
                : new ProgramSharedTexture
                {
                    SharedHandleHex = preview.SharedTexture.SharedHandleHex,
                    Width = preview.SharedTexture.Width,
                    Height = preview.SharedTexture.Height,
                    Format = preview.SharedTexture.Format,
                    FrameNumber = preview.SharedTexture.FrameNumber > 0
                        ? preview.SharedTexture.FrameNumber
                        : preview.FrameNumber
                }
        };
}
