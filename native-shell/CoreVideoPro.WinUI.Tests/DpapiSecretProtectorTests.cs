using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class DpapiSecretProtectorTests
{
    [Fact]
    public void ProtectThenUnprotect_RoundTrips()
    {
        const string secret = "rtmp-stream-key-123/секрет";

        var stored = DpapiSecretProtector.Protect(secret);

        Assert.StartsWith(DpapiSecretProtector.Prefix, stored, StringComparison.Ordinal);
        Assert.DoesNotContain(secret, stored, StringComparison.Ordinal);
        Assert.True(DpapiSecretProtector.IsProtected(stored));
        Assert.Equal(secret, DpapiSecretProtector.Unprotect(stored));
    }

    [Fact]
    public void Unprotect_PassesPlaintextThroughUnchanged()
    {
        // Legacy plaintext files depend on this pass-through for migration.
        Assert.Equal("legacy-plaintext-key", DpapiSecretProtector.Unprotect("legacy-plaintext-key"));
        Assert.False(DpapiSecretProtector.IsProtected("legacy-plaintext-key"));
    }

    [Fact]
    public void Protect_IsIdempotent()
    {
        var once = DpapiSecretProtector.Protect("secret");
        var twice = DpapiSecretProtector.Protect(once);

        Assert.Equal(once, twice);
        Assert.Equal("secret", DpapiSecretProtector.Unprotect(twice));
    }

    [Fact]
    public void Unprotect_CorruptBlob_Throws()
    {
        // A corrupt/foreign-user blob must fail LOUDLY (callers decide the
        // fallback), never silently return garbage.
        Assert.ThrowsAny<Exception>(() =>
            DpapiSecretProtector.Unprotect(DpapiSecretProtector.Prefix + "AAAA"));
    }
}
