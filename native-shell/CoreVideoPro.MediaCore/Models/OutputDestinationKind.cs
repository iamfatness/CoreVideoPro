namespace CoreVideoPro.MediaCore.Models;

/// <summary>Classifies output destination ids so NDI, recording and network streams stay separate.</summary>
public static class OutputDestinationKind
{
    public static bool IsNdi(string? destinationId) =>
        destinationId is "ndi" or "ndi-graphics";

    public static bool IsStream(string? destinationId) =>
        !string.IsNullOrEmpty(destinationId) && !IsNdi(destinationId) && destinationId != "recording";
}
