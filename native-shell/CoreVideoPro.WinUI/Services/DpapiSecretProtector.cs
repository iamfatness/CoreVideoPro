using System.Security.Cryptography;
using System.Text;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Field-level DPAPI protection for credentials at rest (beta spec S4).
///
/// Protected values are stored as <c>"dpapi:" + base64(DPAPI blob)</c>, scoped
/// to the current Windows user (<see cref="DataProtectionScope.CurrentUser"/>).
/// The recognizable prefix keeps the surrounding JSON diffable/debuggable and
/// makes legacy plaintext values detectable: <see cref="Unprotect"/> passes a
/// non-prefixed value through unchanged, so plaintext files keep working and
/// callers re-save them encrypted (never lose a working token).
/// </summary>
public static class DpapiSecretProtector
{
    public const string Prefix = "dpapi:";

    // Optional entropy binds the blobs to this app; it is not a secret.
    private static readonly byte[] Entropy = Encoding.UTF8.GetBytes("CoreVideoPro.secret.v1");

    public static bool IsProtected(string? value) =>
        value is not null && value.StartsWith(Prefix, StringComparison.Ordinal);

    /// <summary>
    /// Encrypts <paramref name="plaintext"/> with DPAPI (CurrentUser). Idempotent:
    /// an already-protected value is returned unchanged so a double-protect bug
    /// can never make a stored secret unrecoverable.
    /// </summary>
    public static string Protect(string plaintext)
    {
        if (IsProtected(plaintext))
        {
            return plaintext;
        }

        var blob = ProtectedData.Protect(
            Encoding.UTF8.GetBytes(plaintext),
            Entropy,
            DataProtectionScope.CurrentUser);
        return Prefix + Convert.ToBase64String(blob);
    }

    /// <summary>
    /// Decrypts a <c>dpapi:</c>-prefixed value. A value without the prefix is
    /// returned unchanged (legacy plaintext pass-through). Throws
    /// <see cref="CryptographicException"/>/<see cref="FormatException"/> when a
    /// prefixed blob cannot be decrypted (e.g. copied from another Windows user).
    /// </summary>
    public static string Unprotect(string stored)
    {
        if (!IsProtected(stored))
        {
            return stored;
        }

        var blob = Convert.FromBase64String(stored[Prefix.Length..]);
        return Encoding.UTF8.GetString(
            ProtectedData.Unprotect(blob, Entropy, DataProtectionScope.CurrentUser));
    }
}
