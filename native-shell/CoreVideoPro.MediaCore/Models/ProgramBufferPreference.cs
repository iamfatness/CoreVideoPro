namespace CoreVideoPro.MediaCore.Models;

public static class ProgramBufferPreference
{
    public const int DefaultFrames = 3;
    public const string EnvironmentVariable = "COREVIDEO_PROGRAM_BUFFER_FRAMES";

    public static int Normalize(int frames) => frames is 2 or 3 ? frames : DefaultFrames;
}
