using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class AudioValidationChecklistBuilder
{
    private const double StaleCaptureAgeMs = 1000;

    public static IReadOnlyList<string> Build(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null)
    {
        var captureState = BuildCaptureState(capture);
        var programHasNativeSignal = HasCurrentProgramSignal(audio);
        var programState = BuildProgramState(audio, capture, captureState);
        return
        [
            captureState,
            programState,
            BuildMonitorState(audio, capture, captureState, programHasNativeSignal),
            BuildStreamState(capture, captureState, programHasNativeSignal, outputSenderSession),
            BuildRecordingState(capture, captureState, recording)
        ];
    }

    public static string Format(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null) =>
        string.Join(" | ", Build(audio, capture, outputSenderSession, recording));

    public static string SummarizeAcceptance(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null) =>
        SummarizeAcceptance(Build(audio, capture, outputSenderSession, recording));

    public static string FindFirstBlockingStage(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null) =>
        FindFirstBlockingStage(Build(audio, capture, outputSenderSession, recording));

    public static string SummarizeFullChain(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null) =>
        SummarizeFullChain(Build(audio, capture, outputSenderSession, recording));

    public static string SummarizeFullChain(IReadOnlyList<string> checklist)
    {
        var missing = new List<string>();
        if (!Find(checklist, "CAPTURE").StartsWith("CAPTURE ok", StringComparison.OrdinalIgnoreCase))
        {
            missing.Add("capture");
        }

        if (!Find(checklist, "PGM").StartsWith("PGM ok", StringComparison.OrdinalIgnoreCase))
        {
            missing.Add("PGM");
        }

        if (!Find(checklist, "MON").StartsWith("MON ok", StringComparison.OrdinalIgnoreCase))
        {
            missing.Add("MON");
        }

        if (!Find(checklist, "STREAM").StartsWith("STREAM ok", StringComparison.OrdinalIgnoreCase))
        {
            missing.Add("stream");
        }

        if (!Find(checklist, "RECORD").StartsWith("RECORD ok", StringComparison.OrdinalIgnoreCase))
        {
            missing.Add("recording");
        }

        return missing.Count == 0
            ? "Full audio chain validated: capture, PGM, MON, stream, and recording all have current proof."
            : $"Full audio chain incomplete: {string.Join(", ", missing)} not yet proven.";
    }

    public static string FindFirstBlockingStage(IReadOnlyList<string> checklist)
    {
        var capture = Find(checklist, "CAPTURE");
        var program = Find(checklist, "PGM");
        if (!IsAcceptableProgram(program))
        {
            if (!IsAcceptableCapture(capture))
            {
                return FormatBlockingStage("CAPTURE", capture);
            }

            return FormatBlockingStage("PGM", program);
        }

        var monitor = Find(checklist, "MON");
        if (!IsAcceptableMonitor(monitor))
        {
            return FormatBlockingStage("MON", monitor);
        }

        var stream = Find(checklist, "STREAM");
        if (!IsAcceptableStream(stream))
        {
            return FormatBlockingStage("STREAM", stream);
        }

        var record = Find(checklist, "RECORD");
        if (!IsAcceptableRecord(record))
        {
            return FormatBlockingStage("RECORD", record);
        }

        return "No blocking audio stage detected.";
    }

    public static string SummarizeAcceptance(IReadOnlyList<string> checklist)
    {
        var capture = Find(checklist, "CAPTURE");
        var program = Find(checklist, "PGM");
        var monitor = Find(checklist, "MON");
        var stream = Find(checklist, "STREAM");
        var record = Find(checklist, "RECORD");

        if (IsAcceptableProgram(program) &&
            IsAcceptableMonitor(monitor) &&
            IsAcceptableStream(stream) &&
            IsAcceptableRecord(record))
        {
            return FormatValidatedSummary(IsAcceptableCapture(capture), capture, monitor, stream, record);
        }

        if (capture.StartsWith("CAPTURE silent", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: fix source audio first; play signal through the selected source or choose another input/loopback/process.";
        }

        if (capture.StartsWith("CAPTURE stale", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: source audio is stale; restart or reselect the capture source.";
        }

        if (capture.StartsWith("CAPTURE unverified", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: capture has PCM counters but no current per-source signal proof.";
        }

        if (capture.StartsWith("CAPTURE wait no PCM", StringComparison.OrdinalIgnoreCase) ||
            capture.StartsWith("CAPTURE wait source", StringComparison.OrdinalIgnoreCase) ||
            capture.Equals("CAPTURE idle", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: add or enable a local/capture audio source, then play signal through it.";
        }

        if (program.StartsWith("PGM wait route", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: route source audio to master/PGM.";
        }

        if (program.StartsWith("PGM wait", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: fix capture source audio before checking program routing.";
        }

        if (monitor.StartsWith("MON wait route", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: route source audio to MON or turn monitor off.";
        }

        if (monitor.StartsWith("MON wait output", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: MON has PCM; check monitor output device and volume.";
        }

        if (stream.StartsWith("STREAM wait sender", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: stream has bus audio but the sender has not accepted audio frames yet.";
        }

        if (stream.StartsWith("STREAM wait route", StringComparison.OrdinalIgnoreCase) ||
            stream.StartsWith("STREAM wait audio", StringComparison.OrdinalIgnoreCase) ||
            stream.StartsWith("STREAM fallback", StringComparison.OrdinalIgnoreCase) ||
            stream.StartsWith("STREAM unverified", StringComparison.OrdinalIgnoreCase) ||
            stream.StartsWith("STREAM stale", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: route verified source audio to stream or master before streaming.";
        }

        if (record.StartsWith("RECORD wait writer", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: master bus has PCM but the recording writer has not accepted audio yet.";
        }

        if (record.StartsWith("RECORD wait proof", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: active recording has not reported native audio proof telemetry yet.";
        }

        if (record.StartsWith("RECORD wait route", StringComparison.OrdinalIgnoreCase) ||
            record.StartsWith("RECORD wait audio", StringComparison.OrdinalIgnoreCase) ||
            record.StartsWith("RECORD fallback", StringComparison.OrdinalIgnoreCase) ||
            record.StartsWith("RECORD unverified", StringComparison.OrdinalIgnoreCase) ||
            record.StartsWith("RECORD stale", StringComparison.OrdinalIgnoreCase))
        {
            return "Audio validation incomplete: route verified source audio to master before recording.";
        }

        return "Audio validation incomplete: review the audio proof checklist for the first waiting or unverified stage.";
    }

    private static string BuildCaptureState(NativeMediaCoreCaptureAudioSources capture)
    {
        if (capture.SourceCount <= 0)
        {
            return "CAPTURE idle";
        }

        if (capture.Sources.Count > 0)
        {
            var streamingSources = capture.Sources
                .Where(source => source.CaptureStreaming)
                .ToArray();
            var staleFrames = streamingSources
                .Where(source => source.CaptureFramesReceived > 0 &&
                    source.CaptureLastFrameAgeMs > StaleCaptureAgeMs)
                .Sum(source => source.CaptureFramesReceived);
            if (staleFrames > 0)
            {
                return $"CAPTURE stale {staleFrames}f age>{StaleCaptureAgeMs:0}ms";
            }

            var signaledFrames = streamingSources
                .Where(source => source.CaptureFramesReceived > 0 && source.SignalPresent)
                .Sum(source => source.CaptureFramesReceived);
            if (signaledFrames > 0)
            {
                return $"CAPTURE ok {signaledFrames}f";
            }

            var silentFrames = streamingSources
                .Where(source => source.CaptureFramesReceived > 0)
                .Sum(source => source.CaptureFramesReceived);
            if (silentFrames > 0)
            {
                return $"CAPTURE silent {silentFrames}f";
            }

            if (streamingSources.Length > 0)
            {
                return "CAPTURE wait no PCM";
            }

            return capture.CaptureFramesReceived > 0
                ? $"CAPTURE stale {capture.CaptureFramesReceived}f"
                : "CAPTURE wait source";
        }

        return capture.StreamingCount > 0
            ? capture.CaptureFramesReceived > 0
                ? $"CAPTURE unverified {capture.CaptureFramesReceived}f"
                : "CAPTURE wait no PCM"
            : capture.CaptureFramesReceived > 0
                ? $"CAPTURE stale {capture.CaptureFramesReceived}f"
                : "CAPTURE wait source";
    }

    private static string BuildProgramState(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        string captureState)
    {
        if (HasCurrentProgramSignal(audio))
        {
            return $"PGM ok {audio.MixedFrameCount}f";
        }

        if (IsCaptureOk(captureState))
        {
            return capture.RoutedMasterFrames > 0
                ? $"PGM ok {capture.RoutedMasterFrames}f"
                : "PGM wait route";
        }

        if (captureState.StartsWith("CAPTURE silent", StringComparison.OrdinalIgnoreCase))
        {
            return capture.RoutedMasterFrames > 0
                ? $"PGM silent source {capture.RoutedMasterFrames}f"
                : "PGM wait source";
        }

        if (captureState.StartsWith("CAPTURE stale", StringComparison.OrdinalIgnoreCase))
        {
            return capture.RoutedMasterFrames > 0
                ? $"PGM stale source {capture.RoutedMasterFrames}f"
                : "PGM wait source";
        }

        if (captureState.StartsWith("CAPTURE unverified", StringComparison.OrdinalIgnoreCase))
        {
            return capture.RoutedMasterFrames > 0
                ? $"PGM unverified source {capture.RoutedMasterFrames}f"
                : "PGM wait source";
        }

        return capture.CaptureFramesReceived > 0
            ? "PGM wait source"
            : "PGM wait PCM";
    }

    private static string BuildMonitorState(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        string captureState,
        bool programHasNativeSignal)
    {
        var captureOk = IsCaptureOk(captureState);
        if (!audio.MonitorEnabled)
        {
            return captureOk && capture.RoutedMonitorFrames > 0
                ? $"MON off {capture.RoutedMonitorFrames}f ready"
                : "MON off";
        }

        if (!captureOk && !programHasNativeSignal)
        {
            return capture.CaptureFramesReceived > 0 || capture.RoutedMonitorFrames > 0 || audio.MonitorFramesPlayed > 0
                ? "MON wait source"
                : "MON wait route";
        }

        return (capture.RoutedMonitorFrames > 0 || programHasNativeSignal) &&
               audio.MonitorFramesPlayed > 0 && IsMonitorCurrentlyPlaying(audio.MonitorStatus)
            ? $"MON ok {audio.MonitorFramesPlayed}f"
            : capture.RoutedMonitorFrames > 0 || programHasNativeSignal
                ? $"MON wait output {(capture.RoutedMonitorFrames > 0 ? capture.RoutedMonitorFrames : audio.MixedFrameCount)}f"
                : "MON wait route";
    }

    private static bool IsCaptureOk(string captureState) =>
        captureState.StartsWith("CAPTURE ok", StringComparison.OrdinalIgnoreCase);

    private static string Find(IReadOnlyList<string> checklist, string prefix) =>
        checklist.FirstOrDefault(item => item.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) ?? string.Empty;

    private static string FormatBlockingStage(string stage, string item) =>
        string.IsNullOrWhiteSpace(item)
            ? $"First audio block: {stage} has no telemetry."
            : $"First audio block: {item}.";

    private static bool IsAcceptableCapture(string item) =>
        item.StartsWith("CAPTURE ok", StringComparison.OrdinalIgnoreCase);

    private static bool IsAcceptableProgram(string item) =>
        item.StartsWith("PGM ok", StringComparison.OrdinalIgnoreCase);

    private static bool IsAcceptableMonitor(string item) =>
        item.StartsWith("MON ok", StringComparison.OrdinalIgnoreCase) ||
        item.Equals("MON off", StringComparison.OrdinalIgnoreCase) ||
        item.StartsWith("MON off ", StringComparison.OrdinalIgnoreCase);

    private static bool IsAcceptableStream(string item) =>
        item.StartsWith("STREAM ok", StringComparison.OrdinalIgnoreCase) ||
        item.Equals("STREAM idle", StringComparison.OrdinalIgnoreCase);

    private static bool IsAcceptableRecord(string item) =>
        item.StartsWith("RECORD ok", StringComparison.OrdinalIgnoreCase) ||
        item.Equals("RECORD idle", StringComparison.OrdinalIgnoreCase);

    private static string FormatValidatedSummary(
        bool captureValidated,
        string capture,
        string monitor,
        string stream,
        string record)
    {
        var validated = new List<string>();
        if (captureValidated)
        {
            validated.Add("capture");
        }
        validated.Add("PGM");
        var idle = new List<string>();

        if (monitor.StartsWith("MON ok", StringComparison.OrdinalIgnoreCase))
        {
            validated.Add("MON");
        }
        else
        {
            idle.Add("monitor");
        }

        if (stream.StartsWith("STREAM ok", StringComparison.OrdinalIgnoreCase))
        {
            validated.Add("stream");
        }
        else
        {
            idle.Add("stream");
        }

        if (record.StartsWith("RECORD ok", StringComparison.OrdinalIgnoreCase))
        {
            validated.Add("recording");
        }
        else
        {
            idle.Add("record");
        }

        var validatedText = string.Join(", ", validated);
        var localCaptureNote = captureValidated
            ? string.Empty
            : $" Local capture is optional and remains unvalidated ({capture}).";
        if (idle.Count == 0)
        {
            return $"Audio validated: {validatedText} all have current proof.{localCaptureNote}";
        }

        return $"Audio validated: {validatedText}. {string.Join(", ", idle)} idle.{localCaptureNote}";
    }

    private static bool HasCurrentProgramSignal(NativeMediaCoreAudioMixSession audio) =>
        audio.MixedFrameCount > 0 &&
        (audio.MasterLevel > 0 ||
         audio.Participants.Any(participant =>
             !participant.Muted &&
             (participant.InputLevel > 0 || participant.OutputLevel > 0 || participant.PeakDbfs > -90)));

    private static bool IsMonitorCurrentlyPlaying(string? status) =>
        status is not null &&
        (status.Equals("playing", StringComparison.OrdinalIgnoreCase) ||
         status.Equals("stub-monitor", StringComparison.OrdinalIgnoreCase));

    private static string BuildStreamState(
        NativeMediaCoreCaptureAudioSources capture,
        string captureState,
        bool programHasNativeSignal,
        NativeMediaCoreOutputSenderSession? outputSenderSession)
    {
        if (outputSenderSession is not { ActiveSenderCount: > 0 })
        {
            return "STREAM idle";
        }

        var liveSenders = outputSenderSession.Senders
            .Where(sender => string.Equals(sender.Status, "live", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (liveSenders.Length == 0)
        {
            return "STREAM wait sender";
        }

        var frames = liveSenders.Sum(sender => sender.AudioFramesSent);
        if (programHasNativeSignal)
        {
            if (frames > 0)
            {
                return $"STREAM ok {frames}f";
            }

            return IsCaptureOk(captureState) && capture.RoutedStreamFrames <= 0
                ? "STREAM wait route"
                : "STREAM wait sender";
        }

        if (captureState.StartsWith("CAPTURE silent", StringComparison.OrdinalIgnoreCase))
        {
            return frames > 0
                ? $"STREAM silent source {frames}f"
                : "STREAM wait source";
        }

        if (captureState.StartsWith("CAPTURE stale", StringComparison.OrdinalIgnoreCase))
        {
            return frames > 0
                ? $"STREAM stale source {frames}f"
                : "STREAM wait source";
        }

        if (captureState.StartsWith("CAPTURE unverified", StringComparison.OrdinalIgnoreCase))
        {
            return frames > 0
                ? $"STREAM unverified source {frames}f"
                : "STREAM wait source";
        }

        if (!IsCaptureOk(captureState))
        {
            return frames > 0
                ? $"STREAM wait source {frames}f"
                : "STREAM wait source";
        }

        if (capture.RoutedStreamFrames <= 0)
        {
            return "STREAM wait route";
        }

        return frames > 0 ? $"STREAM ok {frames}f" : "STREAM wait sender";
    }

    private static string BuildRecordingState(
        NativeMediaCoreCaptureAudioSources capture,
        string captureState,
        NativeMediaCoreRecordingSession? recording)
    {
        if (recording is not { Active: true })
        {
            return "RECORD idle";
        }

        var proof = recording.Proof;
        if (proof is null)
        {
            return IsCaptureOk(captureState) && capture.RoutedMasterFrames <= 0
                ? "RECORD wait route"
                : "RECORD wait proof";
        }

        var samples = proof.AudioSampleCount;
        var packets = proof.AudioPacketsObserved;
        if (proof.AudioPresent && samples > 0)
        {
            return $"RECORD ok {samples} samples";
        }

        if (IsCaptureOk(captureState) && capture.RoutedMasterFrames <= 0)
        {
            return "RECORD wait route";
        }

        if (captureState.StartsWith("CAPTURE silent", StringComparison.OrdinalIgnoreCase))
        {
            return samples > 0
                ? $"RECORD silent source {samples} samples"
                : "RECORD wait source";
        }

        if (captureState.StartsWith("CAPTURE stale", StringComparison.OrdinalIgnoreCase))
        {
            return samples > 0
                ? $"RECORD stale source {samples} samples"
                : "RECORD wait source";
        }

        if (captureState.StartsWith("CAPTURE unverified", StringComparison.OrdinalIgnoreCase))
        {
            return samples > 0
                ? $"RECORD unverified source {samples} samples"
                : "RECORD wait source";
        }

        if (!IsCaptureOk(captureState))
        {
            return samples > 0
                ? $"RECORD wait source {samples} samples"
                : packets > 0
                    ? $"RECORD wait source {packets}pkt"
                    : "RECORD wait source";
        }

        return packets > 0
                ? $"RECORD unverified {packets}pkt"
                : "RECORD wait writer";
    }
}
