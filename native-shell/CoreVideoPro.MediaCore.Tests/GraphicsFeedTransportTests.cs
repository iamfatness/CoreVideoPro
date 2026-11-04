using System.IO.Compression;
using System.Net;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsFeedTransportTests
{
    private static readonly GraphicsFeedDefinition Feed = new("feed", "Fixture", "json", "https://example.com/data", "id");

    [Theory]
    [InlineData("127.0.0.1")]
    [InlineData("169.254.169.254")]
    [InlineData("100.64.0.1")]
    [InlineData("::ffff:10.0.0.1")]
    [InlineData("fe80::1")]
    [InlineData("2002:7f00:1::1")]
    public async Task RejectsNonPublicDnsAnswersEvenAlongsidePublicOnes(string address)
    {
        await Assert.ThrowsAsync<InvalidDataException>(() => GraphicsFeedTransport.ResolvePublicAsync("example.com",
            (_, _) => Task.FromResult(new[] { IPAddress.Parse("8.8.8.8"), IPAddress.Parse(address) }), default));
    }

    [Fact]
    public async Task RechecksResolvedAddressesOnEveryConnection()
    {
        var resolutions = 0;
        Task<IPAddress[]> Resolve(string host, CancellationToken cancellation) =>
            Task.FromResult(new[] { IPAddress.Parse(++resolutions == 1 ? "8.8.8.8" : "10.0.0.1") });
        Assert.Equal(IPAddress.Parse("8.8.8.8"), Assert.Single(await GraphicsFeedTransport.ResolvePublicAsync("example.com", Resolve, default)));
        await Assert.ThrowsAsync<InvalidDataException>(() => GraphicsFeedTransport.ResolvePublicAsync("example.com", Resolve, default));
    }

    [Fact]
    public async Task RedirectToPrivateIpIsRefusedBeforeSending()
    {
        int requests = 0;
        var transport = new GraphicsFeedTransport((_, _) =>
        {
            requests++;
            var response = new HttpResponseMessage(HttpStatusCode.Redirect);
            response.Headers.Location = new Uri("https://127.0.0.1/secret");
            return Task.FromResult(response);
        });
        await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(Feed, default));
        Assert.Equal(1, requests);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RefusesOversizedRawOrDecodedBody(bool gzip)
    {
        var bytes = new byte[GraphicsLocalData.ByteLimit + 1];
        var content = new ByteArrayContent(bytes);
        if (gzip)
        {
            using var target = new MemoryStream();
            using (var encoder = new GZipStream(target, CompressionLevel.Fastest, leaveOpen: true))
                encoder.Write(bytes);
            content = new ByteArrayContent(target.ToArray());
            content.Headers.ContentEncoding.Add("gzip");
        }
        var transport = new GraphicsFeedTransport((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = content }));
        await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(Feed, default));
    }

    [Theory]
    [InlineData("json", "https://example.com/data", """[{"id":"a","name":"First"}]""")]
    [InlineData("sheets", "https://docs.google.com/spreadsheets/d/e/2PACX-fixture/pub?gid=0&single=true&output=csv", "id,name\na,First")]
    [InlineData("rss", "https://example.com/rss", "<rss version='2.0'><channel><item><guid>a</guid><title>First</title></item></channel></rss>")]
    public async Task NormalizesTheApprovedProviderFormats(string format, string endpoint, string payload)
    {
        var transport = FromBody(payload);
        var result = await transport.FetchAsync(Feed with { Format = format, Endpoint = endpoint }, default);
        Assert.Equal("a", Assert.Single(result.Rows).Key);
        Assert.Contains("First", result.Rows[0].Cells);
    }

    [Theory]
    [InlineData("json", """[{"id":"a"},{"id":"a"}]""")]
    [InlineData("rss", "<!DOCTYPE rss [<!ENTITY x SYSTEM 'file:///private'>]><rss version='2.0'><channel><item><guid>a</guid><title>&x;</title></item></channel></rss>")]
    public async Task RejectsAmbiguousKeysAndXmlEntities(string format, string payload) =>
        await Assert.ThrowsAsync<InvalidDataException>(() => FromBody(payload).FetchAsync(Feed with { Format = format }, default));

    private static GraphicsFeedTransport FromBody(string payload) =>
        new((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(payload) }));
}
