using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Outcome of one consented crash-report send, shaped for the InfoBar.</summary>
internal sealed record CrashReportSendOutcome(
    bool Success,
    bool CanRetry,
    string Message,
    string? ReportId,
    string? ZipPath);

/// <summary>
/// Orchestrates the S1 crash pipeline (docs/beta-engineering-spec.md §S1):
/// detect new WER dumps since the last-seen watermark, and — only after the
/// operator explicitly clicks Send — assemble the report zip and upload it.
///
/// Consent rules honored here:
/// - The scan advances the watermark immediately, so each dump is offered
///   exactly once (dismiss = never asked about that dump again).
/// - Nothing leaves the machine without the Send click; the zip stays on disk
///   under support-bundles before AND after upload so it is always inspectable.
/// - Endpoint/key unset (dev default) = the whole feature is quietly disabled.
/// </summary>
internal sealed class CrashReportCoordinator
{
    private readonly CrashReportingConfig _config;
    private readonly CrashReportWatermarkStore _watermarkStore;
    private readonly string _dumpDirectory;

    private CrashReportCoordinator(
        CrashReportingConfig config,
        CrashReportWatermarkStore watermarkStore,
        string dumpDirectory)
    {
        _config = config;
        _watermarkStore = watermarkStore;
        _dumpDirectory = dumpDirectory;
    }

    /// <summary>Null when crash reporting is not configured (no endpoint/key).</summary>
    internal static CrashReportCoordinator? CreateFromEnvironment()
    {
        var config = CrashReportingConfig.FromEnvironment();
        if (!config.Enabled)
        {
            return null;
        }

        return new CrashReportCoordinator(
            config,
            new CrashReportWatermarkStore(CrashReportWatermarkStore.DefaultPath()),
            CrashDumpScanner.DefaultCrashDumpDirectory());
    }

    /// <summary>
    /// Scans for dumps newer than the watermark, then advances the watermark
    /// regardless of what the operator later decides (offer once, never nag).
    /// Runs entirely on the calling (background) thread.
    /// </summary>
    internal IReadOnlyList<CrashDumpFile> ScanAndAdvanceWatermark()
    {
        var watermark = _watermarkStore.Load();
        var dumps = CrashDumpScanner.Scan(_dumpDirectory, watermark.LastSeenUtc);
        if (dumps.Count > 0)
        {
            watermark.LastSeenUtc = dumps.Max(static d => d.LastWriteUtc);
            _watermarkStore.Save(watermark);
            LaunchLog.Write(
                $"crash-report: found {dumps.Count} new dump(s) since watermark " +
                $"({string.Join(", ", dumps.Select(static d => Path.GetFileName(d.Path)))}) — offering once");
        }

        return dumps;
    }

    /// <summary>
    /// Assembles the report zip (dumps + bounded log tails + fresh redacted
    /// support bundle + manifest) and uploads it. <paramref name="writeSupportBundleAsync"/>
    /// is the existing SettingsViewModel export path; a failed bundle export is
    /// noted in the manifest, never fatal. Heavy work runs off the UI thread.
    /// </summary>
    internal async Task<CrashReportSendOutcome> SendAsync(
        IReadOnlyList<CrashDumpFile> dumps,
        Func<string, Task<bool>> writeSupportBundleAsync)
    {
        var stamp = DateTimeOffset.UtcNow.UtcDateTime;
        var appData = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro");
        var bundleDirectory = Path.Combine(appData, "support-bundles");
        var zipPath = Path.Combine(bundleDirectory, $"crash-report-{stamp:yyyyMMddHHmmss}.zip");
        var bundlePath = Path.Combine(bundleDirectory, $"crash-support-{stamp:yyyyMMddHHmmss}.json");

        string? supportBundlePath = null;
        try
        {
            // Invoked from the UI thread (the factory inside reads VM state);
            // the file write itself is async IO.
            if (await writeSupportBundleAsync(bundlePath).ConfigureAwait(false))
            {
                supportBundlePath = bundlePath;
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"crash-report: support bundle export failed ({ex.GetType().Name}: {ex.Message})");
        }

        CrashReportArchiveResult archive;
        try
        {
            archive = await Task.Run(() => CrashReportArchiveBuilder.Build(zipPath, new CrashReportArchiveRequest
            {
                Dumps = dumps,
                LogFiles =
                [
                    Path.Combine(appData, "launch.log"),
                    Path.Combine(appData, "media-core.log"),
                    Path.Combine(appData, "perf.log")
                ],
                SupportBundlePath = supportBundlePath
            })).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"crash-report: zip assembly failed ({ex.GetType().Name}: {ex.Message})");
            return new CrashReportSendOutcome(
                Success: false,
                CanRetry: false,
                Message: $"Could not assemble the crash report: {ex.Message}",
                ReportId: null,
                ZipPath: null);
        }

        LaunchLog.Write(
            $"crash-report: assembled {archive.ZipPath} ({archive.ZipLength} bytes; " +
            $"{archive.IncludedEntries.Count} entries, {archive.SkippedEntries.Count} skipped)");

        using var uploader = new CrashReportUploader();
        var result = await uploader.UploadAsync(new CrashReportUploadOptions
        {
            Endpoint = new Uri(_config.Endpoint!, UriKind.Absolute),
            ApiKey = _config.ApiKey!,
            Version = new SupportBundleAppInfo().Version,
            MachineClass = DescribeMachineClass(),
            Reason = DescribeReason(dumps)
        }, archive.ZipPath).ConfigureAwait(false);

        if (result.Status == CrashReportUploadStatus.Accepted)
        {
            RecordReport(result.ReportId, dumps);
            LaunchLog.Write($"crash-report: accepted, reportId={result.ReportId ?? "(none)"}");
            var idText = result.ReportId is null ? string.Empty : $" Reference ID: {result.ReportId}.";
            return new CrashReportSendOutcome(
                Success: true,
                CanRetry: false,
                Message: $"Thanks — the report was sent.{idText} A copy stays at {archive.ZipPath}.",
                ReportId: result.ReportId,
                ZipPath: archive.ZipPath);
        }

        var canRetry = result.Status == CrashReportUploadStatus.RetryLater;
        LaunchLog.Write(
            $"crash-report: upload {(canRetry ? "deferred" : "rejected")} " +
            $"(http={result.HttpStatus?.ToString() ?? "n/a"}): {result.Message}");
        return new CrashReportSendOutcome(
            Success: false,
            CanRetry: canRetry,
            Message: $"{result.Message} The report is saved at {archive.ZipPath} — nothing was lost.",
            ReportId: null,
            ZipPath: archive.ZipPath);
    }

    private void RecordReport(string? reportId, IReadOnlyList<CrashDumpFile> dumps)
    {
        if (string.IsNullOrWhiteSpace(reportId))
        {
            return;
        }

        try
        {
            var watermark = _watermarkStore.Load();
            watermark.Reports.Add(new CrashReportRecord
            {
                ReportId = reportId,
                SentAtUtc = DateTimeOffset.UtcNow,
                DumpFiles = dumps.Select(static d => Path.GetFileName(d.Path)).ToArray()
            });
            _watermarkStore.Save(watermark);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"crash-report: failed to record reportId ({ex.Message})");
        }
    }

    /// <summary>
    /// Coarse machine class (S1 crash metadata). Reuses the shared
    /// <see cref="MachineClassProbe.Describe"/> so the S1 and S3 pipelines emit
    /// the identical <c>win-x64-cpuN-ramNgb</c> string from one place.
    /// </summary>
    private static string DescribeMachineClass() => MachineClassProbe.Describe();

    private static string DescribeReason(IReadOnlyList<CrashDumpFile> dumps)
    {
        var processes = dumps
            .Select(static d => d.ProcessName)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        return processes.Length == 0 ? "wer-dump" : $"wer-dump: {string.Join(", ", processes)}";
    }
}
