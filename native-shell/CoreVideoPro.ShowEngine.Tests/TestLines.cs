namespace CoreVideoPro.ShowEngine.Tests;

/// <summary>Wire lines shaped exactly like `show-engine/src/host/protocol.ts` emits them.</summary>
internal static class TestLines
{
    /// <summary>The unsolicited handshake EVENT (no id) the host writes at startup. Two actions, for speed.</summary>
    public static string HandshakeEvent(int generation, int protocolVersion = 1) =>
        "{\"event\":\"handshake\",\"protocolVersion\":" + protocolVersion +
        ",\"engineVersion\":\"0.1.0\",\"generation\":" + generation +
        ",\"actions\":[" +
        "{\"id\":\"ohg.program.cut\",\"title\":\"Cut\",\"description\":\"Cut to preview\",\"params\":[]}," +
        "{\"id\":\"ohg.look.set\",\"title\":\"Set look\",\"description\":\"Select a look\"," +
        "\"params\":[{\"name\":\"lookId\",\"type\":\"string\",\"required\":true,\"description\":\"Look id\"}]}" +
        "],\"fieldTemplates\":[\"ohg/look\",\"ohg/slot/*/name\"]," +
        "\"snapshot\":{\"revision\":3,\"capacity\":10},\"fields\":{\"ohg/look\":\"wide\",\"ohg/onAir\":true}}";

    /// <summary>The handshake payload delivered as a RESPONSE to an explicit `handshake` request.</summary>
    public static string HandshakeResponse(string id, int generation, int protocolVersion = 1)
    {
        var evt = HandshakeEvent(generation, protocolVersion);
        // swap the event discriminator for the response envelope
        return "{\"id\":\"" + id + "\",\"ok\":true," + evt["{\"event\":\"handshake\",".Length..];
    }

    public static string SnapshotEvent(int generation, long revision) =>
        "{\"event\":\"snapshot\",\"generation\":" + generation + ",\"revision\":" + revision +
        ",\"snapshot\":{\"revision\":" + revision + "},\"fields\":{\"ohg/look\":\"tight\"}}";

    public static string HostCommandEvent(int generation, long seq, string name, string argsJson = "[]") =>
        "{\"event\":\"hostCommand\",\"generation\":" + generation + ",\"seq\":" + seq +
        ",\"name\":\"" + name + "\",\"args\":" + argsJson + "}";

    public static string LogEvent(string level, string message) =>
        "{\"event\":\"log\",\"level\":\"" + level + "\",\"message\":\"" + message + "\"}";

    public static string OkResponse(string id, string extraJson = "") =>
        "{\"id\":\"" + id + "\",\"ok\":true" + (extraJson.Length == 0 ? "" : "," + extraJson) + "}";

    public static string ErrorResponse(string id, string message) =>
        "{\"id\":\"" + id + "\",\"ok\":false,\"error\":{\"message\":\"" + message + "\"}}";
}
