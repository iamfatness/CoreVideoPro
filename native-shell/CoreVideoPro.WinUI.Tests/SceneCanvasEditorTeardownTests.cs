using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Guard for #513 (the 0xc000027b finalizer-release crash family): a code-behind
/// element factory must UNHOOK every handler it attached to a generated element
/// before it discards that element. A dropped XAML element whose handlers still
/// reference the control is retained on the finalizable queue until the GC
/// releases it off the UI thread — the exact population that feeds the crash.
///
/// Source-analysis test in the shape of OhgShowPageContentTests.Page_GuardsEveryUiCallback:
/// it reads the control's source so it also catches a NEW per-element handler
/// added later without a matching teardown.
/// </summary>
public sealed class SceneCanvasEditorTeardownTests
{
    [Fact]
    public void EveryPerElementHandlerHasAMatchingUnhook()
    {
        var code = ReadControl("SceneCanvasEditorControl.xaml.cs");

        // Attaches on a LOCAL element (receiver is a lowercase local like `frame`
        // or `button`), not on `this` (bare `Event +=`, which is control-lifetime
        // and legitimately never unhooked).
        var attaches = Regex.Matches(code, @"\b([a-z]\w*)\.(\w+)\s*\+=\s*(\w+)\s*;")
            .Select(m => (Receiver: m.Groups[1].Value, Event: m.Groups[2].Value, Handler: m.Groups[3].Value))
            .Where(a => a.Receiver != "this")
            .ToList();

        Assert.NotEmpty(attaches);
        foreach (var attach in attaches)
        {
            var unhook = new Regex($@"\.{Regex.Escape(attach.Event)}\s*-=\s*{Regex.Escape(attach.Handler)}\s*;");
            Assert.True(
                unhook.IsMatch(code),
                $"Handler '{attach.Handler}' attached to a generated element's {attach.Event} is never unhooked (-=). " +
                "Add the teardown at the discard site (see the #513 note).");
        }
    }

    [Fact]
    public void StaleLayerFramesAreDetachedBeforeRemoval()
    {
        var code = ReadControl("SceneCanvasEditorControl.xaml.cs");

        // The four pointer handlers CreateLayerFrame attaches must all be unhooked.
        foreach (var handler in new[] { "OnLayerPointerPressed", "OnLayerPointerMoved", "OnLayerPointerReleased" })
        {
            Assert.Matches(new Regex($@"-=\s*{handler}\s*;"), code);
        }

        // And the detach must run before the frame leaves the visual tree.
        var sync = MethodBody(code, "SyncLayers");
        var detachAt = sync.IndexOf("DetachLayerFrameHandlers", StringComparison.Ordinal);
        var removeAt = sync.IndexOf("LayerCanvas.Children.Remove", StringComparison.Ordinal);
        Assert.True(detachAt >= 0, "SyncLayers must detach a stale frame's handlers.");
        Assert.True(removeAt >= 0 && detachAt < removeAt, "Detach must run before the frame is removed from the canvas.");
    }

    [Fact]
    public void PresetButtonsAreUnhookedBeforeTheyAreCleared()
    {
        var code = ReadControl("SceneCanvasEditorControl.xaml.cs");
        var build = MethodBody(code, "BuildPresetButtons");
        var unhookAt = build.IndexOf("-= OnPresetClick", StringComparison.Ordinal);
        var clearAt = build.IndexOf("Children.Clear()", StringComparison.Ordinal);
        Assert.True(unhookAt >= 0, "BuildPresetButtons must unhook OnPresetClick from the outgoing buttons.");
        Assert.True(clearAt >= 0 && unhookAt < clearAt, "Unhook must run before the buttons are cleared.");
    }

    private static string MethodBody(string code, string methodName)
    {
        // Find the DEFINITION, not a call: the occurrence whose parameter list is
        // immediately followed by '{' (a call is followed by ';').
        foreach (Match m in Regex.Matches(code, Regex.Escape(methodName) + @"\s*\("))
        {
            var i = m.Index + m.Length;
            var depth = 1;
            for (; i < code.Length && depth > 0; i++)
            {
                if (code[i] == '(') depth++;
                else if (code[i] == ')') depth--;
            }
            while (i < code.Length && char.IsWhiteSpace(code[i])) i++;
            if (i >= code.Length || code[i] != '{') continue;  // a call site
            var brace = i;
            depth = 0;
            for (var j = brace; j < code.Length; j++)
            {
                if (code[j] == '{') depth++;
                else if (code[j] == '}' && --depth == 0)
                {
                    return code.Substring(brace, j - brace + 1);
                }
            }
        }
        throw new InvalidOperationException($"Definition of {methodName} not found.");
    }

    private static string ReadControl(string fileName)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(
                directory.FullName, "native-shell", "CoreVideoPro.WinUI", "Controls", fileName);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate {fileName} from the test output directory.");
    }
}
