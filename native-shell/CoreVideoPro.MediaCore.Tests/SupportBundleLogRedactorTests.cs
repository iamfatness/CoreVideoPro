using System.Diagnostics;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// #998: one 200,000-character ordinary string did not finish redaction in minutes. The
/// secret-assignment pattern began with a greedy <c>\w*</c>, so on a long run of word characters
/// every start position scanned to the end of the run and backed up looking for a keyword
/// (quadratic). Redaction must stay linear, and must still redact exactly what it did before.
/// </summary>
public sealed class SupportBundleLogRedactorTests
{
    private static readonly TimeSpan Bound = TimeSpan.FromSeconds(20);

    private static string Redacted(string input)
    {
        var task = Task.Factory.StartNew(() => SupportBundleLogRedactor.Redact(input),
            CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        return task.WaitAsync(Bound).GetAwaiter().GetResult();
    }

    public static TheoryData<string, string> LongInputs => new()
    {
        { "word characters only", new string('a', 200_000) },
        { "keyword run", string.Concat(Enumerable.Repeat("key", 66_666)) },
        { "keyword run after an open quote", "\"" + string.Concat(Enumerable.Repeat("key", 66_666)) },
        { "equals run", string.Concat(Enumerable.Repeat("a=", 100_000)) },
        { "token run with spaces", string.Concat(Enumerable.Repeat("tokenx ", 30_000)) },
        { "long url path", "rtmp://h/" + new string('p', 200_000) },
        { "bearer run", "Bearer " + new string('b', 200_000) },
        { "jwt prefix run", string.Concat(Enumerable.Repeat("eyJ-", 50_000)) },
        { "jwt starts, two segments, dash tail", string.Concat(Enumerable.Repeat("eyJ-", 25_000)) + ".aaaaaaaa." + new string('-', 100_000) },
        { "jwt prefix run then dot", string.Concat(Enumerable.Repeat("eyJ-", 50_000)) + "." },
        { "jwt prefix run, dot, short tail", string.Concat(Enumerable.Repeat("eyJ-", 50_000)) + ".abc" },
        { "jwt prefix run with dots", string.Concat(Enumerable.Repeat("eyJaaaaaaaa.", 20_000)) },
    };

    [Theory]
    [MemberData(nameof(LongInputs))]
    public void LongOrdinaryStringsRedactInBoundedTime(string name, string input)
    {
        var clock = Stopwatch.StartNew();
        var result = Redacted(input);
        clock.Stop();

        Assert.NotNull(result);
        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(5), $"{name}: {clock.Elapsed.TotalSeconds:F1}s for {input.Length} chars");
    }

    [Fact]
    public void LongTextAroundASecretStillRedactsTheSecretAndKeepsTheRest()
    {
        var filler = new string('a', 200_000);
        var result = Redacted(filler + " streamKey=abc123&x=1 " + filler);

        Assert.Contains("streamKey=[redacted]&x=1", result);
        Assert.DoesNotContain("abc123", result);
        Assert.Equal(2 * filler.Length, result.Split(' ').Where(s => s.Length > 0 && s[0] == 'a').Sum(s => s.Length));
    }

    [Theory]
    [InlineData("access_token=abc", "access_token=[redacted]")]
    [InlineData("pwd=hunter2&a=b", "pwd=[redacted]&a=b")]
    [InlineData("srtPassphrase=xyz end", "srtPassphrase=[redacted] end")]
    [InlineData("USERZAK=zzz", "USERZAK=[redacted]")]
    [InlineData("key=value", "key=[redacted]")]
    [InlineData("x_key_y=1", "x_key_y=1")]
    [InlineData("{\"streamKey\":\"abc\"}", "{\"streamKey\":\"[redacted]\"}")]
    [InlineData("rtmp://live.example/app/SECRET", "rtmp://live.example/[redacted]")]
    [InlineData("tok eyJhbGciOiJI.eyJzdWIiOiIx.abcd end", "tok [redacted] end")]
    [InlineData("eyJaaaaaaaa.bbbbbbbb.cccc-", "[redacted]-")]
    [InlineData("Authorization: Bearer abc.def", "Authorization: Bearer [redacted]")]
    public void RedactsWhatItAlwaysRedacted(string input, string expected)
    {
        Assert.Equal(expected, SupportBundleLogRedactor.Redact(input));
    }
}
