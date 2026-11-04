using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Xml;
using System.Xml.Linq;

namespace CoreVideoPro.MediaCore.Services;

public sealed class GraphicsFeedTransport(
    Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send, IDisposable? owner = null) : IDisposable
{
    public static GraphicsFeedTransport Create()
    {
        var client = new HttpClient(new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            UseProxy = false,
            UseCookies = false,
            AutomaticDecompression = DecompressionMethods.None,
            ConnectCallback = ConnectAsync,
            PooledConnectionLifetime = TimeSpan.FromMinutes(2),
            MaxConnectionsPerServer = 4
        }) { Timeout = Timeout.InfiniteTimeSpan };
        return new((request, cancellation) => client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellation), client);
    }

    public void Dispose() => owner?.Dispose();

    public async Task<GraphicsLocalData> FetchAsync(GraphicsFeedDefinition feed, CancellationToken cancellation)
    {
        var target = ValidateEndpoint(feed);


        for (int hops = 0; ; hops++)
        {
            ValidateTarget(target);
            if (feed.Format == "sheets" && !IsGoogleSheetHost(target.IdnHost))
                throw new InvalidDataException("The Sheets feed redirected to an unsupported host.");
            using var request = new HttpRequestMessage(HttpMethod.Get, target) { VersionPolicy = HttpVersionPolicy.RequestVersionExact };
            using var response = await send(request, cancellation);
            if ((int)response.StatusCode is 301 or 302 or 303 or 307 or 308)
            {
                if (hops == 3 || response.Headers.Location is not { } location || !Uri.TryCreate(target, location, out var redirect))
                    throw new InvalidDataException("Invalid feed redirect.");
                target = redirect;
                continue;
            }
            response.EnsureSuccessStatusCode();
            var bytes = await ReadBody(response.Content, cancellation);
            return await Task.Run(() => Parse(feed, bytes, cancellation), cancellation);
        }
    }

    internal static Uri ValidateEndpoint(GraphicsFeedDefinition feed)
    {
        if (!feed.Valid() || !Uri.TryCreate(feed.Endpoint, UriKind.Absolute, out var target))
            throw new InvalidDataException("Invalid feed URL.");
        ValidateTarget(target);
        if (feed.Format == "sheets") ValidatePublishedSheet(target);
        else if (target.Query.Length != 0) throw new InvalidDataException("Use a public feed URL without query parameters.");
        return target;
    }

    internal static void ValidateWebSocketEndpoint(GraphicsFeedDefinition feed)
    {
        if (feed.Format != "wss" || !feed.Valid() || !Uri.TryCreate(feed.Endpoint, UriKind.Absolute, out var target) ||
            target.Query.Length != 0)
            throw new InvalidDataException("Use a public WSS feed URL without query parameters.");
        ValidateTarget(target, "wss");
    }

    private static bool IsGoogleSheetHost(string host) => host.Equals("docs.google.com", StringComparison.OrdinalIgnoreCase) ||
        host.EndsWith(".googleusercontent.com", StringComparison.OrdinalIgnoreCase);

    private static void ValidatePublishedSheet(Uri target)
    {
        var path = target.AbsolutePath.Split('/');
        var parameters = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var pair in target.Query.TrimStart('?').Split('&'))
        {
            var parts = pair.Split('=', 2);
            if (parts.Length != 2 || !parameters.TryAdd(parts[0], parts[1]))
                throw new InvalidDataException("Use a published Google Sheets CSV link for one tab.");
        }
        if (!target.IdnHost.Equals("docs.google.com", StringComparison.OrdinalIgnoreCase) ||
            path is not ["", "spreadsheets", "d", "e", var id, "pub"] ||
            id.Length == 0 || !id.All(ch => char.IsAsciiLetterOrDigit(ch) || ch is '-' or '_') ||
            parameters.GetValueOrDefault("output") != "csv" ||
            !uint.TryParse(parameters.GetValueOrDefault("gid"), out _) ||
            parameters.Any(pair => pair.Key is not ("gid" or "output" or "single")) ||
            (parameters.TryGetValue("single", out var single) && single != "true"))
            throw new InvalidDataException("Use a published Google Sheets CSV link for one tab.");
    }

    private static void ValidateTarget(Uri target, string scheme = "https")
    {
        if (!target.IsAbsoluteUri || target.Scheme != scheme || target.Port != 443 ||
            target.UserInfo.Length > 0 || target.Fragment.Length > 0 || target.AbsoluteUri.Length > 4096)
            throw new InvalidDataException("The feed destination is not permitted.");
        var host = target.IdnHost.TrimEnd('.');
        if (IPAddress.TryParse(host, out var address) ? !IsPublic(address) :
            !host.Contains('.') || host.EndsWith(".localhost", StringComparison.OrdinalIgnoreCase) ||
            host.EndsWith(".local", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The feed destination is not public.");
    }

    public static async Task<IPAddress[]> ResolvePublicAsync(string host,
        Func<string, CancellationToken, Task<IPAddress[]>> resolve, CancellationToken cancellation)
    {
        var addresses = IPAddress.TryParse(host, out var literal) ? [literal] : await resolve(host, cancellation);
        cancellation.ThrowIfCancellationRequested();
        if (addresses.Length == 0 || addresses.Any(address => !IsPublic(address)))
            throw new InvalidDataException("The feed destination is not public.");
        return addresses;
    }

    private static async ValueTask<Stream> ConnectAsync(SocketsHttpConnectionContext context, CancellationToken cancellation)
    {
        var endpoint = context.DnsEndPoint;
        var addresses = await ResolvePublicAsync(endpoint.Host, Dns.GetHostAddressesAsync, cancellation);
        var socket = new Socket(SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
        try
        {
            await socket.ConnectAsync(addresses, endpoint.Port, cancellation);
            return new NetworkStream(socket, ownsSocket: true);
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    private static bool IsPublic(IPAddress address)
    {
        if (address.IsIPv4MappedToIPv6) address = address.MapToIPv4();
        var b = address.GetAddressBytes();
        if (address.AddressFamily == AddressFamily.InterNetwork)
            return b[0] is > 0 and < 224 and not (10 or 127) &&
                !(b[0] == 100 && b[1] is >= 64 and <= 127) &&
                !(b[0] == 169 && b[1] == 254) &&
                !(b[0] == 172 && b[1] is >= 16 and <= 31) &&
                !(b[0] == 192 && (b[1] == 168 || b[1] == 0 && b[2] is 0 or 2 || b[1] == 88 && b[2] == 99)) &&
                !(b[0] == 198 && (b[1] is 18 or 19 || b[1] == 51 && b[2] == 100)) &&
                !(b[0] == 203 && b[1] == 0 && b[2] == 113);
        // Global-unicast space only. Exclude special-purpose and transition ranges.
        return address.AddressFamily == AddressFamily.InterNetworkV6 && address.ScopeId == 0 &&
            (b[0] & 0xe0) == 0x20 && !(b[0] == 0x20 && b[1] == 0x02) &&
            !(b[0] == 0x20 && b[1] == 0x01 && (b[2] < 2 || b[2] == 0x0d && b[3] == 0xb8)) &&
            !(b[0] == 0x3f && (b[1] & 0xf0) == 0xf0);
    }

    private static async Task<byte[]> ReadBody(HttpContent content, CancellationToken cancellation)
    {
        if (content.Headers.ContentLength > GraphicsLocalData.ByteLimit)
            throw new InvalidDataException("Feed body exceeds 1 MiB.");
        using var response = await content.ReadAsStreamAsync(cancellation);
        var received = await ReadBounded(response, cancellation);
        if (content.Headers.ContentEncoding.Count == 0) return received;
        if (content.Headers.ContentEncoding.Count != 1 ||
            !string.Equals(content.Headers.ContentEncoding.Single(), "gzip", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Unsupported feed encoding.");
        using var compressed = new MemoryStream(received, writable: false);
        using var decoded = new GZipStream(compressed, CompressionMode.Decompress);
        return await ReadBounded(decoded, cancellation);
    }

    private static async Task<byte[]> ReadBounded(Stream stream, CancellationToken cancellation)
    {
        using var body = new MemoryStream();
        var buffer = new byte[8192];
        int read;
        while ((read = await stream.ReadAsync(buffer, cancellation)) != 0)
        {
            if (body.Length + read > GraphicsLocalData.ByteLimit) throw new InvalidDataException("Feed body exceeds 1 MiB.");
            body.Write(buffer, 0, read);
        }
        return body.ToArray();
    }

    private static GraphicsLocalData Parse(GraphicsFeedDefinition feed, byte[] bytes, CancellationToken cancellation)
    {
        string text;
        try { text = new UTF8Encoding(false, true).GetString(bytes).TrimStart('\uFEFF'); }
        catch (DecoderFallbackException ex) { throw new InvalidDataException("Feed must be UTF-8.", ex); }
        cancellation.ThrowIfCancellationRequested();
        var data = feed.Format switch
        {
            "rss" => ReadRss(text, cancellation),
            "sheets" => GraphicsLocalData.Parse(text, "csv", feed.KeyColumn),
            _ => GraphicsLocalData.Parse(text, "json", feed.KeyColumn)
        };
        cancellation.ThrowIfCancellationRequested();
        return data;
    }

    private static GraphicsLocalData ReadRss(string text, CancellationToken cancellation)
    {
        try
        {
            using var reader = XmlReader.Create(new StringReader(text), new XmlReaderSettings
            {
                DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = GraphicsLocalData.ByteLimit
            });
            var root = XDocument.Load(reader).Root;
            var channel = root?.Element("channel");
            if (root?.Name != "rss" || (string?)root.Attribute("version") != "2.0" || channel is null)
                throw new InvalidDataException("Use an RSS 2.0 feed.");
            var items = channel.Elements("item").Take(GraphicsLocalData.MaxRows + 1).ToArray();
            if (items.Length > GraphicsLocalData.MaxRows) throw new InvalidDataException("Feed has too many rows.");
            var rows = items.Select(item =>
            {
                cancellation.ThrowIfCancellationRequested();
                var guid = item.Element("guid")?.Value;
                return new
                {
                    id = (string.IsNullOrWhiteSpace(guid) ? item.Element("link")?.Value : guid)?.Trim() ?? "",
                    title = item.Element("title")?.Value ?? "",
                    description = item.Element("description")?.Value ?? "",
                    published = item.Element("pubDate")?.Value ?? ""
                };
            }).ToArray();
            return GraphicsLocalData.Parse(JsonSerializer.Serialize(rows), "json", "id");
        }
        catch (XmlException ex) { throw new InvalidDataException("Invalid RSS XML.", ex); }
    }
}
