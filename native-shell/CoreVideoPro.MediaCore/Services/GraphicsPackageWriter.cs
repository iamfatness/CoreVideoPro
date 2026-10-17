using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;
public static class GraphicsPackageWriter
{
    public static Task WriteAsync(NativeGraphicsTemplate template,string destination,CancellationToken cancellation=default)=>Task.Run(()=> {
        if(template.Definition is not { } definition || template.Runtime!="declarative") throw new InvalidDataException("Select an installed declarative template. Compatibility appearances are saved as cue/style documents.");
        var manifest=Encoding.UTF8.GetBytes(definition.GetRawText());if(manifest.Length>256*1024) throw new InvalidDataException("Template exceeds its manifest budget.");
        destination=Path.GetFullPath(destination);GraphicsPackageReader.NoReparse(destination);var parent=Path.GetDirectoryName(destination)!;
        if(!Directory.Exists(parent)) throw new DirectoryNotFoundException("Choose an existing destination directory.");
        var temporary=Path.Combine(parent,".cvgraphics-"+Guid.NewGuid().ToString("N")+".tmp");
        try {
            using(var output=new FileStream(temporary,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.None)) {
                using(var zip=new ZipArchive(output,ZipArchiveMode.Create,true)) {
                    using(var entry=zip.CreateEntry("template.json").Open()) entry.Write(manifest);
                    long expanded=manifest.Length;var seen=new HashSet<string>(StringComparer.Ordinal);
                    if(definition.TryGetProperty("assets",out var assets)) {
                        if(assets.ValueKind!=JsonValueKind.Array || assets.GetArrayLength()>32) throw new InvalidDataException("Invalid package assets.");
                        foreach(var asset in assets.EnumerateArray()) {
                            cancellation.ThrowIfCancellationRequested();var file=asset.GetProperty("file").GetString()!;var hash=asset.GetProperty("sha256").GetString()!;
                            var optional=asset.TryGetProperty("optional",out var flag) && flag.ValueKind==JsonValueKind.True;
                            var extension=Path.GetExtension(file);
                            if(hash.Length!=64 || !hash.All(Uri.IsHexDigit) || extension is not (".png" or ".jpg" or ".jpeg") || file!="assets/"+hash+extension || !seen.Add(file)) throw new InvalidDataException("Invalid package asset path.");
                            Stream? input=null;
                            if(template.PackageRoot.Length>0) {
                                var root=Path.TrimEndingDirectorySeparator(Path.GetFullPath(template.PackageRoot));var path=Path.GetFullPath(Path.Combine(root,file));
                                if(!path.StartsWith(root+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Asset escaped package root.");
                                GraphicsPackageReader.NoReparse(path);
                                if(File.Exists(path)) input=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read);
                            } else {
                                var builtin=typeof(GraphicsPackageWriter).Assembly.GetManifestResourceStream("CoreVideoPro.Graphics.BuiltinLogo.png");
                                if(builtin is not null && Convert.ToHexString(SHA256.HashData(builtin)).Equals(hash,StringComparison.OrdinalIgnoreCase)) {builtin.Position=0;input=builtin;} else builtin?.Dispose();
                            }
                            if(input is null) {if(optional) continue;throw new InvalidDataException("A required image is missing. Repair the package before export; the existing destination is retained.");}
                            using(input) {
                                if(input.Length is <1 or >16*1024*1024 || expanded>128L*1024*1024-input.Length) throw new InvalidDataException("Package exceeds its image or expanded byte budget.");
                                using var memory=new MemoryStream();var buffer=new byte[65536];int read;
                                while((read=input.Read(buffer))>0) {cancellation.ThrowIfCancellationRequested();if(memory.Length+read>16*1024*1024) throw new InvalidDataException("Asset exceeded its byte budget.");memory.Write(buffer,0,read);}
                                var bytes=memory.ToArray();if(!Convert.ToHexString(SHA256.HashData(bytes)).Equals(hash,StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Image hash changed. Export refused; previous destination retained.");
                                using var target=zip.CreateEntry(file,CompressionLevel.Fastest).Open();target.Write(bytes);expanded+=bytes.Length;
                            }
                        }
                    }
                }
                if(output.Length>32L*1024*1024) throw new InvalidDataException("Compressed package exceeds 32 MiB.");
                output.Flush(true);
            }
            cancellation.ThrowIfCancellationRequested();GraphicsPackageReader.NoReparse(destination);File.Move(temporary,destination,true);
        } finally {if(File.Exists(temporary)) File.Delete(temporary);}
    },cancellation);
}
