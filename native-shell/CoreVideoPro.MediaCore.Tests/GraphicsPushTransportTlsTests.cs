using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsPushTransportTlsTests
{
    private static readonly IPAddress VettedAddress = IPAddress.Parse("8.8.8.8");

    [Fact]
    public async Task UpgradesWithTrustedHostnameAndReadsAFragmentedMessage()
    {
        using var fixture = new TlsFixture();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var server = fixture.ServeAsync("upgrade", timeout.Token);
        var connect = fixture.Transport().ConnectAsync(Feed("feed.example.com"), new(TimeProvider.System), timeout.Token);
        try { await connect; }
        catch { await server; throw; }
        using var connection = await connect;
        Assert.Equal("hello", Encoding.UTF8.GetString((await connection.ReadAsync(timeout.Token))!));
        Assert.True(await server);
    }

    [Theory]
    [InlineData("feed.example.com", false)]
    [InlineData("other.example.com", true)]
    public async Task RefusesUntrustedCertificatesAndWrongHostnames(string host, bool trustRoot)
    {
        using var fixture = new TlsFixture();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var server = fixture.ServeAsync("upgrade", timeout.Token, allowTlsFailure: true);
        await Assert.ThrowsAsync<WebSocketException>(() => fixture.Transport(trustRoot).ConnectAsync(Feed(host),
            new(TimeProvider.System), timeout.Token));
        Assert.False(await server);
        Assert.Equal(1, fixture.Dials);
    }

    [Theory]
    [InlineData("redirect", typeof(WebSocketException))]
    [InlineData("extension", typeof(InvalidDataException))]
    public async Task RejectsRedirectsAndUnsolicitedCompression(string response, Type failure)
    {
        using var fixture = new TlsFixture();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var server = fixture.ServeAsync(response, timeout.Token);
        await Assert.ThrowsAsync(failure, () => fixture.Transport().ConnectAsync(Feed("feed.example.com"),
            new(TimeProvider.System), timeout.Token));
        Assert.True(await server);
        Assert.Equal(1, fixture.Dials);
    }

    [Fact]
    public async Task RejectsACompressedFrameWithoutNegotiation()
    {
        using var fixture = new TlsFixture();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var server = fixture.ServeAsync("compressed frame", timeout.Token);
        using var connection = await fixture.Transport().ConnectAsync(Feed("feed.example.com"),
            new(TimeProvider.System), timeout.Token);
        await Assert.ThrowsAsync<WebSocketException>(() => connection.ReadAsync(timeout.Token));
        Assert.True(await server);
    }

    private static GraphicsFeedDefinition Feed(string host) =>
        new("feed", "Fixture", "wss", $"wss://{host}/live", "id");

    private sealed class TlsFixture : IDisposable
    {
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly X509Certificate2 _root;
        private readonly X509Certificate2 _server;
        private int _dials;

        public TlsFixture()
        {
            using var rootKey = RSA.Create(2048);
            var rootRequest = new CertificateRequest("CN=Graphics Transport Test Root", rootKey,
                HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            rootRequest.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
            rootRequest.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.KeyCertSign, true));
            var now = DateTimeOffset.UtcNow;
            _root = rootRequest.CreateSelfSigned(now.AddDays(-2), now.AddDays(2));

            using var serverKey = RSA.Create(2048);
            var serverRequest = new CertificateRequest("CN=feed.example.com", serverKey,
                HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            var names = new SubjectAlternativeNameBuilder();
            names.AddDnsName("feed.example.com");
            serverRequest.CertificateExtensions.Add(names.Build());
            using var leaf = serverRequest.Create(_root, now.AddDays(-1), now.AddDays(1), [1]);
            using var certificateWithKey = leaf.CopyWithPrivateKey(serverKey);
            // Schannel requires an imported private key for server authentication.
            _server = X509CertificateLoader.LoadPkcs12(certificateWithKey.Export(X509ContentType.Pkcs12), null);
            _listener.Start();
        }

        public int Dials => Volatile.Read(ref _dials);

        public GraphicsPushTransport Transport(bool trustRoot = true)
        {
            var trust = new X509ChainPolicy
            {
                TrustMode = X509ChainTrustMode.CustomRootTrust,
                RevocationMode = X509RevocationMode.NoCheck,
                DisableCertificateDownloads = true
            };
            trust.CustomTrustStore.Add(_root);
            return new((_, _) => Task.FromResult(new[] { VettedAddress }), async (addresses, port, cancellation) =>
            {
                Assert.Equal(VettedAddress, Assert.Single(addresses));
                Assert.Equal(443, port);
                Interlocked.Increment(ref _dials);
                return await GraphicsFeedTransport.DialAsync([IPAddress.Loopback],
                    ((IPEndPoint)_listener.LocalEndpoint).Port, cancellation);
            }, trustRoot ? trust : null);
        }

        public async Task<bool> ServeAsync(string response, CancellationToken cancellation, bool allowTlsFailure = false)
        {
            using var client = await _listener.AcceptTcpClientAsync(cancellation);
            using var stream = new SslStream(client.GetStream());
            try
            {
                await stream.AuthenticateAsServerAsync(new SslServerAuthenticationOptions { ServerCertificate = _server }, cancellation);
            }
            catch (Exception ex) when (allowTlsFailure && ex is IOException or AuthenticationException)
            {
                return false;
            }

            using var request = new StreamReader(stream, Encoding.ASCII, leaveOpen: true);
            string? requestLine;
            try { requestLine = await request.ReadLineAsync(cancellation); }
            catch (IOException) when (allowTlsFailure) { return false; }
            if (requestLine is null) return false;
            Assert.Equal("GET /live HTTP/1.1", requestLine);
            var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            while (await request.ReadLineAsync(cancellation) is { Length: > 0 } line)
            {
                var colon = line.IndexOf(':');
                headers.Add(line[..colon], line[(colon + 1)..].Trim());
            }
            Assert.Equal("feed.example.com", headers["Host"]);
            Assert.DoesNotContain("Sec-WebSocket-Extensions", headers.Keys, StringComparer.OrdinalIgnoreCase);
            Assert.DoesNotContain("Cookie", headers.Keys, StringComparer.OrdinalIgnoreCase);
            if (response == "redirect")
            {
                await stream.WriteAsync("HTTP/1.1 302 Found\r\nLocation: https://other.example.com/redirected\r\nContent-Length: 0\r\n\r\n"u8.ToArray(), cancellation);
                return true;
            }

            var accept = Convert.ToBase64String(SHA1.HashData(Encoding.ASCII.GetBytes(
                headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
            var extension = response == "extension" ? "Sec-WebSocket-Extensions: permessage-deflate\r\n" : "";
            await stream.WriteAsync(Encoding.ASCII.GetBytes(
                $"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n{extension}\r\n"), cancellation);
            if (response == "compressed frame")
                await stream.WriteAsync(new byte[] { 0xc1, 0x03, 0xca, 0x48, 0x00 }, cancellation);
            else if (response == "upgrade")
                await stream.WriteAsync(new byte[] { 0x01, 0x02, (byte)'h', (byte)'e', 0x80, 0x03, (byte)'l', (byte)'l', (byte)'o' }, cancellation);
            return true;
        }

        public void Dispose()
        {
            _listener.Stop();
            _server.Dispose();
            _root.Dispose();
        }
    }
}
