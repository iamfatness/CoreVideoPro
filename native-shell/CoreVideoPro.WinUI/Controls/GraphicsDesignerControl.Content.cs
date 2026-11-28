using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsDesignerControl
{
    private static string ContentSignature(JsonElement node)
    {
        var properties = new[] { "stroke", "strokeWidth", "diagonal", "outline", "shadow", "minFontSize", "minWidth", "maxWidth", "speed", "gap", "direction", "mode", "backdrop" };
        return string.Join('|', properties.Select(name => ReadProperty(node, name).ToString())) + IsAutoWidth(node);
    }

    private static JsonElement ReadProperty(JsonElement node, string property) =>
        node.ValueKind == JsonValueKind.Object && node.TryGetProperty(property, out var value) ? value : default;

    private static double ReadNumber(JsonElement node, string property, double fallback) =>
        ReadProperty(node, property) is { ValueKind: JsonValueKind.Number } number ? number.GetDouble() : fallback;

    private static string ReadString(JsonElement node, string property, string fallback) =>
        ReadProperty(node, property) is { ValueKind: JsonValueKind.String } value ? value.GetString() ?? fallback : fallback;

    private static bool IsAutoWidth(JsonElement node) => ReadString(node, "width", "") == "auto";

    private void AddContentControls(GraphicsLayerInspection layer, JsonElement node)
    {
        if (layer.Kind is "Shape" or "Ellipse" or "Line") BuildStrokeControls(layer, node);
        if (layer.Kind == "Shape") BuildGlassControls(layer, node);
        if (layer.Kind is "Shape" or "Group") BuildSizingControls(layer, node);
        if (layer.Kind is "Text" or "Clock") BuildTextEffects(layer, node);
    }

    private void BuildStrokeControls(GraphicsLayerInspection layer, JsonElement node)
    {
        var stroke = ContentText("Stroke colour", ReadString(node, "stroke", "#FFFFFF"));
        var width = ContentNumber("Stroke width", layer.Kind == "Line" ? 1 : 0, 4096,
            ReadNumber(node, "strokeWidth", layer.Kind == "Line" ? 4 : 0));
        stroke.CheckContext = new GraphicsColorCheckContext { LineWidthPx = width.Value };
        width.ValueChanged += (_, e) => { if (!double.IsNaN(e.NewValue)) stroke.CheckContext = new GraphicsColorCheckContext { LineWidthPx = e.NewValue }; };
        var panel = ContentOptions(stroke, width);
        ComboBox? direction = null;
        if (layer.Kind == "Line")
        {
            direction = new ComboBox { Header = "Diagonal", ItemsSource = new[] { "Down", "Up" },
                SelectedIndex = ReadString(node, "diagonal", "down") == "up" ? 1 : 0 };
            _inspectorInputs.Add(direction);
            panel.Children.Add(direction);
        }
        AddContentApply(panel, "Apply stroke", () => Apply(s => GraphicsTemplateEditor.SetStroke(s, layer.Id,
            stroke.Literal, width.Value, direction?.SelectedIndex == 1 ? "up" : "down")));
        _layerBody.Children.Add(new Expander { Header = "Shape stroke", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    /// <summary>P9 glass: the compositor blurs and tints the picture behind this plate (true backdrop
    /// blur, no frosted stand-in). Sits with the plate's other fill controls.</summary>
    private void BuildGlassControls(GraphicsLayerInspection layer, JsonElement node)
    {
        var backdrop = ReadProperty(node, "backdrop");
        var enabled = new CheckBox { Content = "Glass (blur what is behind)", IsChecked = backdrop.ValueKind == JsonValueKind.Object };
        AutomationProperties.SetName(enabled, "Glass (blur what is behind)");
        _inspectorInputs.Add(enabled);
        var blur = ContentNumber("Glass blur (px at 1080p)", GraphicsTemplateEditor.BackdropBlurMinimum,
            GraphicsTemplateEditor.BackdropBlurMaximum, ReadNumber(backdrop, "blur", 32));
        var tint = ContentText("Glass tint", ReadString(backdrop, "tint", "#0A0F1866"));
        var panel = ContentOptions(enabled, blur, tint, Note("Blur costs GPU time on every output. Up to four glass plates per graphic."));
        AddContentApply(panel, "Apply glass", () => Apply(s => GraphicsTemplateEditor.SetBackdrop(s, layer.Id,
            enabled.IsChecked == true, blur.Value, tint.Text)));
        _layerBody.Children.Add(new Expander { Header = "Glass", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    private void BuildSizingControls(GraphicsLayerInspection layer, JsonElement node)
    {
        var enabled = new CheckBox { Content = "Size to content", IsChecked = IsAutoWidth(node) };
        _inspectorInputs.Add(enabled);
        var minimum = ContentNumber("Minimum width", 1, 8192, ReadNumber(node, "minWidth", 80));
        var maximum = ContentNumber("Maximum width", 1, 8192, ReadNumber(node, "maxWidth", Math.Max(80, layer.Width)));
        var panel = ContentOptions(enabled, minimum, maximum);
        AddContentApply(panel, "Apply width", () =>
        {
            var measured = _session?.AuthoringLayout?.Nodes.FirstOrDefault(item => item.Id == layer.Id)?.Width;
            Apply(s => GraphicsTemplateEditor.SetAutoWidth(s, layer.Id, enabled.IsChecked == true, minimum.Value, maximum.Value,
                Math.Clamp(measured ?? (layer.Width > 0 ? layer.Width : minimum.Value), 1, 8192)));
        });
        _layerBody.Children.Add(new Expander { Header = "Content width", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    private void BuildTextEffects(GraphicsLayerInspection layer, JsonElement node)
    {
        var outline = ReadProperty(node, "outline");
        var shadow = ReadProperty(node, "shadow");
        var minimum = ContentNumber("Minimum font size (0 keeps size)", 0, Math.Max(1, layer.FontSize), ReadNumber(node, "minFontSize", 0));
        var outlineColour = ContentText("Outline colour", ReadString(outline, "color", "#000000"));
        var outlineWidth = ContentNumber("Outline width", 0, 8, ReadNumber(outline, "width", 0));
        var shadowEnabled = new CheckBox { Content = "Drop shadow", IsChecked = shadow.ValueKind == JsonValueKind.Object };
        _inspectorInputs.Add(shadowEnabled);
        var shadowColour = ContentText("Shadow colour", ReadString(shadow, "color", "#00000080"));
        var shadowX = ContentNumber("Shadow X", -256, 256, ReadNumber(shadow, "x", 2));
        var shadowY = ContentNumber("Shadow Y", -256, 256, ReadNumber(shadow, "y", 4));
        var blur = ContentNumber("Shadow blur", 0, 16, ReadNumber(shadow, "blur", 4));
        var panel = ContentOptions(minimum, outlineColour, outlineWidth, shadowEnabled, shadowColour, shadowX, shadowY, blur);
        AddContentApply(panel, "Apply text effects", () => Apply(s => GraphicsTemplateEditor.SetTextEffects(s, layer.Id,
            minimum.Value, outlineColour.Literal, outlineWidth.Value, shadowEnabled.IsChecked == true ? shadowColour.Literal : null,
            shadowX.Value, shadowY.Value, blur.Value)));
        _layerBody.Children.Add(new Expander { Header = "Text effects", Content = panel, HorizontalAlignment = HorizontalAlignment.Stretch });
    }

    private static StackPanel ContentOptions(params UIElement[] elements)
    {
        var panel = new StackPanel { Spacing = 6 };
        foreach (var element in elements) panel.Children.Add(element);
        return panel;
    }

    /// <summary>A literal colour (stroke, outline, shadow) in the shared colour field (graphics styles slice 2).</summary>
    private CvColorField ContentText(string label, string value)
    {
        var field = new CvColorField { Header = label, AllowAlpha = true };
        field.SetValue(value);
        AutomationProperties.SetName(field, label);
        _inspectorInputs.Add(field);
        return field;
    }

    private NumberBox ContentNumber(string label, double minimum, double maximum, double value)
    {
        var field = new NumberBox { Header = label, Minimum = minimum, Maximum = maximum, Value = value,
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline };
        AutomationProperties.SetName(field, label);
        _inspectorInputs.Add(field);
        return field;
    }

    private void AddContentApply(StackPanel panel, string label, Action apply)
    {
        var button = StudioBrushes.GhostButton(label);
        AutomationProperties.SetName(button, label);
        button.Click += (_, _) => { CommitPendingText(); apply(); };
        _inspectorInputs.Add(button);
        panel.Children.Add(button);
    }

    private async Task PickDesignImageAsync()
    {
        if (!CanEdit || _session is not { } session || session.ImportBusy) return;
        CommitPendingText();
        try
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            foreach (var extension in new[] { ".png", ".jpg", ".jpeg", ".svg" }) picker.FileTypeFilter.Add(extension);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync();
            if (file is null || !ReferenceEquals(_session, session)) return;
            await session.ImportDesignImageAsync(file.Path);
            Say(session.ImportStatus);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            Say("Could not import the image. " + ex.Message);
        }
    }
}
