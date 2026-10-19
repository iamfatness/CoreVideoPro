using System.Security.Cryptography;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;
public sealed record GraphicsImportedImage(GraphicsPackageInput Package,NativeGraphicsImageReference Reference);

/// <summary>Explicit local image input. Copies bounded immutable bytes into the
/// existing content-addressed package cache; the native loader independently
/// verifies hash/header/dimensions before any image can be previewed or aired.</summary>
public static class GraphicsImageImport
{
    public static Task<GraphicsImportedImage> StageAsync(string path,string root,CancellationToken cancellation=default)=>Task.Run(()=> {
        root=Path.GetFullPath(root);path=Path.GetFullPath(path);GraphicsPackageReader.NoReparse(root);GraphicsPackageReader.NoReparse(path);
        var extension=Path.GetExtension(path).ToLowerInvariant();if(extension is not (".png" or ".jpg" or ".jpeg")) throw new InvalidDataException("Choose a local PNG or JPEG image.");
        if(extension==".jpeg") extension=".jpg";
        using var input=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read);
        if(input.Length is <16 or >16*1024*1024) throw new InvalidDataException("Graphics images must be no larger than 16 MiB.");
        using var memory=new MemoryStream();var buffer=new byte[65536];int read;
        while((read=input.Read(buffer))>0) {cancellation.ThrowIfCancellationRequested();if(memory.Length+read>16*1024*1024) throw new InvalidDataException("Graphics image grew beyond its byte limit.");memory.Write(buffer,0,read);}
        var bytes=memory.ToArray();var hash=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var directory=Path.Combine(root,"assets");GraphicsPackageReader.NoReparse(directory);Directory.CreateDirectory(directory);
        var files=Directory.EnumerateFiles(directory).Select(f=>new FileInfo(f)).ToArray();foreach(var file in files) GraphicsPackageReader.NoReparse(file.FullName);
        var fileName="assets/"+hash+extension;var destination=Path.Combine(root,fileName);GraphicsPackageReader.NoReparse(destination);
        if(File.Exists(destination)) {using var current=File.OpenRead(destination);if(!SHA256.HashData(current).AsSpan().SequenceEqual(SHA256.HashData(bytes))) throw new InvalidDataException("Graphics cache content hash mismatch.");}
        else {
            if(files.Length>=256 || files.Sum(f=>f.Length)>512L*1024*1024-bytes.Length) throw new InvalidDataException("Graphics asset cache is full.");
            var temporary=destination+"."+Guid.NewGuid().ToString("N")+".tmp";
            try {using(var output=new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {output.Write(bytes);output.Flush(true);}GraphicsPackageReader.NoReparse(directory);File.Move(temporary,destination);}
            finally {if(File.Exists(temporary)) File.Delete(temporary);}
        }
        var id="image-"+hash[..32];
        // A real bug consumer makes the image package independently reusable;
        // compatibility lower thirds reference these same verified asset bytes.
        const string nodeType="type";
        var manifest=JsonSerializer.Serialize(new {schemaVersion=1,templateId=id,templateVersion=1,name="Imported logo "+hash[..8],family="bug",
            canvas=new {width=1920,height=1080},bounds=new {width=192,height=192},brandTokens=Array.Empty<string>(),fields=Array.Empty<object>(),
            animations=new { @in=new {type="none",durationMs=0,easing="linear"}, @out=new {type="none",durationMs=0,easing="linear"} },
            layers=new[] {new Dictionary<string,object> { ["id"]="logo-image",[nodeType]="image",["asset"]="logo",["x"]=0,["y"]=0,["width"]=192,["height"]=192,["fit"]="contain"}},
            assets=new[] {new {id="logo",file=fileName,sha256=hash}}});
        return new GraphicsImportedImage(new(manifest,root),new(id,1,"logo"));
    },cancellation);
}
