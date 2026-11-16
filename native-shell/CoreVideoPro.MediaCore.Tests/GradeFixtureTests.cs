using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// Grading plan 2026-11-16, Task 8: contracts/grade.fixtures.json is read by the native core
// (AdvancedGrade.SharedFixturesHaveTheDeclaredVerdictInTheCore), this file, and the Mac shell
// harness, so a validator change on any one platform shows up as a verdict mismatch.
public sealed class GradeFixtureTests
{
    private static string Fixture()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(directory.FullName, "contracts", "grade.fixtures.json");
            if (File.Exists(path)) return path;
            directory = directory.Parent;
        }
        throw new FileNotFoundException("contracts/grade.fixtures.json");
    }

    [Fact]
    public void PortableVerdictsMatchAdvancedGradeDocumentValidate()
    {
        using var file = JsonDocument.Parse(File.ReadAllText(Fixture()));
        var checkedCount = 0;
        foreach (var entry in file.RootElement.GetProperty("documents").EnumerateArray())
        {
            var name = entry.GetProperty("name").GetString();
            var expected = entry.GetProperty("portable").GetString() == "valid";
            bool valid;
            try
            {
                var doc = entry.GetProperty("json").Deserialize<AdvancedGradeDocument>(new JsonSerializerOptions(JsonSerializerDefaults.Web));
                doc!.Validate(); valid = true;
            }
            catch (Exception ex) when (ex is JsonException or ArgumentException or InvalidOperationException or NullReferenceException) { valid = false; }
            Assert.True(valid == expected, $"{name}: expected {(expected ? "valid" : "invalid")}");
            ++checkedCount;
        }
        Assert.Equal(24, checkedCount);
    }

    [Fact]
    public void TheMacEncodedDocumentValidatesInCSharp()
    {
        using var file = JsonDocument.Parse(File.ReadAllText(Fixture()));
        foreach (var entry in file.RootElement.GetProperty("macEncoded").EnumerateArray())
        {
            var doc = JsonSerializer.Deserialize<AdvancedGradeDocument>(entry.GetProperty("text").GetString()!, new JsonSerializerOptions(JsonSerializerDefaults.Web));
            doc!.Validate();
        }
    }
}
