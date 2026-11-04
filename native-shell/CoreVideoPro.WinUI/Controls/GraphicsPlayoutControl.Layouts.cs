using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsPlayoutControl
{
    private sealed record RevisionOption(string Label, ShotLayoutDataPin? Pin)
    {
        public override string ToString() => Label;
    }

    private sealed record LayoutFieldEditor(CheckBox Enabled, Func<JsonElement> Read, NativeGraphicsTemplateField Field);

    private static Func<ShotLayoutRowSettings?> AddLayoutRowEditor(
        StackPanel form, CheckBox include, StudioViewModel studio, GraphicsPlaylistRow row, ShotLayoutRowSettings? saved)
    {
        var document = studio.GraphicsLibrary.Document;
        var fields = GraphicsRowFields.TemplateFieldsOf(row, studio.GraphicsWorkspace.State, document.Templates)
            .Where(GraphicsRowFields.CanOverrideLayoutField).ToArray();
        if (row.DataBinding is null && fields.Length == 0 && saved is null)
            return () => null;

        var panel = new StackPanel { Spacing = 6, Margin = new Thickness(4, 0, 4, 6) };
        var expander = new Expander
        {
            Header = "Values and data · " + row.Name,
            Content = panel,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            IsExpanded = saved is not null,
            Visibility = include.IsChecked == true ? Visibility.Visible : Visibility.Collapsed
        };
        form.Children.Add(expander);
        include.Checked += (_, _) => expander.Visibility = Visibility.Visible;
        include.Unchecked += (_, _) => expander.Visibility = Visibility.Collapsed;

        ComboBox? revisionPicker = null;
        CheckBox? removePin = null;
        if (row.DataBinding is { } binding)
        {
            var options = new List<RevisionOption>
            {
                new($"Use published data (revision {binding.Revision})", null),
                new($"Pin revision {binding.Revision}", new(binding.SourceId, binding.Revision))
            };
            var loaded = document.DataSources.FirstOrDefault(source => source.Id == binding.SourceId);
            if (loaded is not null && loaded.Revision != binding.Revision)
                options.Add(new($"Pin revision {loaded.Revision}", new(binding.SourceId, loaded.Revision)));
            if (saved?.DataPin is { } savedPin && options.All(option => option.Pin != savedPin))
                options.Add(new($"Revision {savedPin.Revision} unavailable", savedPin));
            revisionPicker = new ComboBox
            {
                Header = "Data revision", ItemsSource = options, HorizontalAlignment = HorizontalAlignment.Stretch,
                SelectedIndex = options.FindIndex(option => option.Pin == saved?.DataPin)
            };
            panel.Children.Add(revisionPicker);
        }
        else if (saved?.DataPin is not null)
        {
            removePin = new CheckBox { Content = "Remove unavailable data pin" };
            panel.Children.Add(removePin);
        }

        var editors = new List<LayoutFieldEditor>();
        var invalid = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (var field in fields)
        {
            var value = saved?.Overrides.GetValueOrDefault(field.Id) ?? default;
            var hasOverride = value.ValueKind != JsonValueKind.Undefined;
            if (!GraphicsRowFields.ValidLayoutOverride(field, value))
            {
                if (hasOverride) invalid.Add(field.Id, value);
                value = row.Fields.GetValueOrDefault(field.Id, field.Default ?? default);
            }
            var replace = invalid.ContainsKey(field.Id);
            var enabled = new CheckBox { Content = (replace ? "Replace invalid " : "Override ") + GraphicsTemplateEditor.HumanizeId(field.Id),
                IsChecked = hasOverride && !replace };
            var (input, read) = CreateLayoutFieldInput(field, value);
            input.IsEnabled = enabled.IsChecked == true;
            enabled.Checked += (_, _) => input.IsEnabled = true;
            enabled.Unchecked += (_, _) => input.IsEnabled = false;
            panel.Children.Add(enabled);
            panel.Children.Add(input);
            editors.Add(new(enabled, read, field));
        }

        var unavailable = saved?.Overrides.Keys.Where(key => fields.All(field => field.Id != key)).ToArray() ?? [];
        var removeFields = new Dictionary<string, CheckBox>(StringComparer.Ordinal);
        foreach (var key in unavailable)
        {
            var remove = new CheckBox { Content = "Remove unavailable override " + key };
            removeFields.Add(key, remove);
            panel.Children.Add(remove);
        }

        return () =>
        {
            if (include.IsChecked != true) return null;
            var overrides = new Dictionary<string, JsonElement>(invalid, StringComparer.Ordinal);
            foreach (var (key, remove) in removeFields)
                if (remove.IsChecked != true) overrides.Add(key, saved!.Overrides[key]);
            foreach (var editor in editors)
            {
                if (editor.Enabled.IsChecked != true) continue;
                var value = editor.Read();
                if (!GraphicsRowFields.ValidLayoutOverride(editor.Field, value))
                    throw new InvalidDataException($"{row.Name}: check the value for {editor.Field.Id}.");
                overrides[editor.Field.Id] = value;
            }
            var pin = revisionPicker is not null ? (revisionPicker.SelectedItem as RevisionOption)?.Pin :
                removePin?.IsChecked == true ? null : saved?.DataPin;
            return pin is null && overrides.Count == 0 ? null : new() { DataPin = pin, Overrides = overrides };
        };
    }

    private static (Microsoft.UI.Xaml.Controls.Control Input, Func<JsonElement> Read) CreateLayoutFieldInput(NativeGraphicsTemplateField field, JsonElement value)
    {
        if (field.Choices.Count > 0)
        {
            var index = Array.FindIndex(field.Choices.ToArray(), choice => value.ValueKind != JsonValueKind.Undefined && JsonElement.DeepEquals(value, choice));
            var picker = new ComboBox
            {
                ItemsSource = field.Choices.Select(choice => choice.ToString()).ToArray(),
                SelectedIndex = Math.Max(0, index), HorizontalAlignment = HorizontalAlignment.Stretch
            };
            return (picker, () => picker.SelectedIndex >= 0 ? field.Choices[picker.SelectedIndex] : default);
        }
        if (field.Type == "boolean")
        {
            var toggle = new ToggleSwitch { IsOn = value.ValueKind == JsonValueKind.True };
            return (toggle, () => JsonSerializer.SerializeToElement(toggle.IsOn));
        }
        if (field.Type == "number")
        {
            var number = new NumberBox
            {
                Minimum = field.Minimum ?? -1e9, Maximum = field.Maximum ?? 1e9,
                Value = value.ValueKind == JsonValueKind.Number ? value.GetDouble() : Math.Clamp(0, field.Minimum ?? -1e9, field.Maximum ?? 1e9)
            };
            return (number, () => double.IsFinite(number.Value) ? JsonSerializer.SerializeToElement(number.Value) : default);
        }
        var text = new TextBox { Text = value.ValueKind == JsonValueKind.String ? value.GetString()! : "",
            MaxLength = field.MaxLength ?? (field.Type == "color" ? 9 : 512), PlaceholderText = field.Type == "color" ? "#RRGGBB" : "Value" };
        return (text, () => JsonSerializer.SerializeToElement(text.Text));
    }

    private void AddLayoutProgress(StudioViewModel studio, GraphicsLayoutProgress progress)
    {
        var message = studio.GraphicsLayoutTakePending ? "Confirming Take" : progress.State;
        var summary = new StackPanel { Spacing = 3 };
        var layout = studio.GraphicsPlayout.Layout(progress.LayoutId);
        var scene = layout?.SceneId is { Length: > 0 } sceneId
            ? studio.Scenes.FirstOrDefault(item => item.Id == sceneId)?.Name ?? sceneId : "No scene change";
        summary.Children.Add(new TextBlock { Text = $"{message} · {scene}", FontSize = 12, TextWrapping = TextWrapping.Wrap });
        foreach (var row in progress.Rows)
        {
            var revision = row.DataRevision is { } number ? $" · data revision {number}" : "";
            var accepted = row.DataReceivedAt is { } received ? $" · loaded {received.ToLocalTime():g}" : "";
            summary.Children.Add(new TextBlock
            {
                Text = $"{row.Name} · {row.State}{revision}{accepted}", FontSize = 11,
                Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap
            });
        }
        if (progress.Reason.Length > 0)
            summary.Children.Add(new TextBlock { Text = progress.Reason, FontSize = 11, Foreground = StudioBrushes.Program, TextWrapping = TextWrapping.Wrap });
        _layouts.Children.Add(summary);
    }
}
