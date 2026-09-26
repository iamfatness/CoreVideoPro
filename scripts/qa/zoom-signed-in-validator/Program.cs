using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using CoreVideoPro.MediaCore.Services;

// Run the real native validator through the same signed-in OAuth credential
// path as WinUI. Credentials stay in memory and in the child environment;
// neither the test report nor command line contains them.
if (!OperatingSystem.IsWindows())
{
    Console.Error.WriteLine("The signed-in validation launcher requires Windows DPAPI.");
    return 2;
}

var root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../../"));
if (!File.Exists(Path.Combine(root, "scripts", "validate-live-zoom.mjs")))
{
    Console.Error.WriteLine("Could not locate the CoreVideo repository root.");
    return 2;
}

static string Unprotect(string value)
{
    const string prefix = "dpapi:";
    if (!value.StartsWith(prefix, StringComparison.Ordinal)) return value;
    var entropy = Encoding.UTF8.GetBytes("CoreVideoPro.secret.v1");
    var bytes = Convert.FromBase64String(value[prefix.Length..]);
    return Encoding.UTF8.GetString(ProtectedData.Unprotect(bytes, entropy, DataProtectionScope.CurrentUser));
}

try
{
    var store = new FileZoomTokenStore(FileZoomTokenStore.DefaultTokenStorePath(), decrypt: Unprotect);
    var oauth = new ZoomOAuthService(store, ZoomOAuthManifest.Load(root));
    var status = await oauth.GetStatusAsync();
    if (!status.SignedIn)
    {
        Console.Error.WriteLine("No signed-in local Zoom session is available for the live test.");
        return 3;
    }

    var credentials = await oauth.EnsureJoinCredentialsAsync();
    if (string.IsNullOrWhiteSpace(credentials.UserZak))
    {
        Console.Error.WriteLine("The signed-in Zoom session did not provide a user ZAK.");
        return 3;
    }

    var start = new ProcessStartInfo("node")
    {
        WorkingDirectory = root,
        UseShellExecute = false
    };
    start.ArgumentList.Add(Path.Combine(root, "scripts", "validate-live-zoom.mjs"));
    foreach (var arg in args) start.ArgumentList.Add(arg);
    start.Environment["COREVIDEO_ZOOM_USER_ZAK"] = credentials.UserZak;
    if (!string.IsNullOrWhiteSpace(credentials.SdkJwt))
        start.Environment["COREVIDEO_ZOOM_SDK_JWT"] = credentials.SdkJwt;

    using var child = Process.Start(start);
    if (child is null) throw new InvalidOperationException("Could not launch Node.js validator.");
    await child.WaitForExitAsync();
    return child.ExitCode;
}
catch (Exception error)
{
    // OAuth errors are safe to report; never include token values or HTTP bodies.
    Console.Error.WriteLine($"Signed-in validation could not start: {error.GetType().Name}");
    return 3;
}
