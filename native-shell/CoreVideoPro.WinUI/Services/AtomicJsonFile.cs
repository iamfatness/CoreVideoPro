using System.Security.Cryptography;
using System.Text;
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("CoreVideoPro.WinUI.Tests")]

namespace CoreVideoPro.WinUI.Services;

/// <summary>Same-volume replacement with durable staging and cross-instance/process serialization.</summary>
internal sealed class AtomicJsonFile(string path, Action<string>? beforeReplace = null)
{
    public string Path { get; } = System.IO.Path.GetFullPath(path);
    public string BackupPath => Path + ".bak";

    public T Locked<T>(Func<T> operation)
    {
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.ToUpperInvariant())));
        using var mutex = new Mutex(false, "Local\\CoreVideoPro.Preferences." + key);
        try { mutex.WaitOne(); }
        catch (AbandonedMutexException) { /* A terminated writer still grants ownership. */ }
        try { return operation(); }
        finally { mutex.ReleaseMutex(); }
    }

    // Call under Locked. The caller supplies a validated (and protected) previous
    // document; a corrupt primary must never overwrite the last usable backup.
    public void Write(string json, string? previousJson)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
        if (previousJson is not null)
            Replace(BackupPath, previousJson);
        Replace(Path, json);
    }

    private void Replace(string destination, string json)
    {
        var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                var bytes = Encoding.UTF8.GetBytes(json);
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            beforeReplace?.Invoke(destination);
            if (File.Exists(destination))
                File.Replace(temporary, destination, null);
            else
                File.Move(temporary, destination);
        }
        finally
        {
            try { File.Delete(temporary); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}
