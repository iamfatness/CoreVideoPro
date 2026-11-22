using System.Text;
using System.Text.Json;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlManifestExportTests
{
    private static string Expected() => ControlManifest.Build().ToJson().Replace("\r\n", "\n") + "\n";

    [Fact]
    public void CheckedInManifestIsExactlyTheRegistryExport()
    {
        var path = RepoPaths.Contract("control.manifest.json");
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_CONTROL_CONTRACT") == "1")
        {
            File.WriteAllText(path, Expected(), new UTF8Encoding(false));
        }
        Assert.True(File.Exists(path), $"{path} is missing. Regenerate with COREVIDEO_WRITE_CONTROL_CONTRACT=1.");
        var actual = File.ReadAllText(path).Replace("\r\n", "\n");
        Assert.True(actual == Expected(),
            "contracts/control.manifest.json is stale. Regenerate with COREVIDEO_WRITE_CONTROL_CONTRACT=1, "
            + "then update contracts/control.platforms.json and run npm run contract:generate.");
    }

    [Fact]
    public void ExportListsEveryRegistryActionInRegistryOrderWithItsOscAddress()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(RepoPaths.Contract("control.manifest.json")));
        var actions = doc.RootElement.GetProperty("Actions").EnumerateArray().ToList();
        Assert.Equal(ControlActionRegistry.Actions.Select(a => a.Id), actions.Select(a => a.GetProperty("Id").GetString()));
        // Fixture-derived operands: the root and one hand-checked address (spec 2.2).
        Assert.Equal("/cvp", doc.RootElement.GetProperty("Root").GetString());
        Assert.Equal("/cvp/transport/virtualcam/name/set",
            actions.Single(a => a.GetProperty("Id").GetString() == "transport.virtualcam.name.set")
                .GetProperty("OscAddress").GetString());
        Assert.Equal(1, doc.RootElement.GetProperty("Version").GetInt32());
    }
}
