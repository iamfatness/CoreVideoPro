using System.Buffers.Binary;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;
public static class GraphicsThumbnailBytes
{
    public static byte[]? Decode(NativeGraphicsThumbnail? thumbnail)
    {
        if(thumbnail is not {State:"ready",Width:>=1 and <=320,Height:>=1 and <=180} || thumbnail.PngBase64.Length>350000) return null;
        byte[] bytes;try {bytes=Convert.FromBase64String(thumbnail.PngBase64);} catch(FormatException) {return null;}
        if(bytes.Length<33 || bytes.Length>256*1024 || !bytes.AsSpan(0,8).SequenceEqual(new byte[] {137,80,78,71,13,10,26,10}) ||
            BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(8,4))!=13 || !bytes.AsSpan(12,4).SequenceEqual("IHDR"u8) ||
            BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(16,4))!=thumbnail.Width || BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(20,4))!=thumbnail.Height) return null;
        return bytes;
    }
}
