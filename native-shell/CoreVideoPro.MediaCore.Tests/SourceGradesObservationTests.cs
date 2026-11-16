using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// Grading plan 2026-11-16, Task 2: the macOS-only sourceGrades node is "typed: omit", so the
// Windows shell's typed sync path is unchanged while qualification and control still see it.
public sealed class SourceGradesObservationTests
{
    private const string Session = "{\"sourceGrades\":[{\"sourceId\":\"16778240\",\"revision\":2,\"appliedEpoch\":7,\"liveEpoch\":8,\"livePresent\":true,\"active\":false}],\"activeSpeakerId\":\"42\"}";

    [Fact]
    public void SourceGradesNeverReachesTheTypedViewButReachesQualificationAndControl()
    {
        var model = CoreObservationModel.Parse(Session);
        Assert.DoesNotContain("sourceGrades", model.TypedJson());
        Assert.Contains("\"sourceGrades\"", model.QualificationJson());
        Assert.Contains("\"sourceGrades\"", model.ControlJson());
        Assert.Contains("\"activeSpeakerId\"", model.TypedJson()); // confirmatory: the typed view still works
    }
}
