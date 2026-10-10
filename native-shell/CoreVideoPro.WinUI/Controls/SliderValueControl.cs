using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>One value, relative adjustment and exact entry. Refresh never quantizes it.</summary>
public sealed class SliderValueControl : UserControl
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(nameof(Value), typeof(double),
        typeof(SliderValueControl), new PropertyMetadata(0d, (d, _) => ((SliderValueControl)d).RefreshValue(true)));
    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public string Header { get; set; } = "";
    public string Unit { get; set; } = "";
    public string Help { get; set; } = "";
    public string Format { get; set; } = "0.##";
    private double _minimum, _maximum=1;
    public double Minimum { get => _minimum; set { _minimum=value;if (IsLoaded) RefreshValue(false); } }
    public double Maximum { get => _maximum; set { _maximum=value;if (IsLoaded) RefreshValue(false); } }
    public double StepFrequency { get; set; } = .01;
    public double DefaultValue { get; set; }
    public double DisplayScale { get; set; } = 1;
    public bool Logarithmic { get; set; }
    public event EventHandler<double>? ValueChanged;
    public event EventHandler? EditStarted;
    public event EventHandler? EditCompleted;
    private readonly TextBlock _label = new(), _help = new() { FontSize = 11, TextWrapping = TextWrapping.Wrap, Opacity = .75 };
    private readonly TextBlock _error = new() { FontSize = 11, TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
    private readonly TextBox _entry = new() { Width = 76, MinWidth = 60, HorizontalAlignment = HorizontalAlignment.Right };
    private readonly AdjustmentSlider _slider = new();
    private readonly TextBlock _range = new() { FontSize = 10, Opacity = .65 };
    private bool _refreshing, _typing, _editing;
    private double _beforeEdit;
    private string _entryAtFocus = "";
    public SliderValueControl()
    {
        var root = new StackPanel { Spacing = 3 };
        var head = new Grid { ColumnSpacing = 8 };
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _label.VerticalAlignment = VerticalAlignment.Center;
        root.Children.Add(_label); head.Children.Add(_entry); Grid.SetColumn(_entry, 1);
        var reset = new Button { Content = "↺", Padding = new Thickness(7, 3, 7, 3) };
        AutomationProperties.SetName(reset, "Reset to default"); ToolTipService.SetToolTip(reset, "Reset to default");
        reset.Click += (_, _) => { BeginEdit(); Value = DefaultValue; EndEdit(); };
        head.Children.Add(reset); Grid.SetColumn(reset, 2);
        root.Children.Add(head); root.Children.Add(_slider); root.Children.Add(_range); root.Children.Add(_help); root.Children.Add(_error);
        Content = root;
        _slider.ValueChanged += (_, e) => { if (!_refreshing) Value = FromSlider(e.NewValue); };
        _slider.StartEdit=BeginEdit; _slider.FinishEdit=EndEdit;
        _entry.GotFocus += (_, _) => {
            _typing = true;_refreshing=true;
            try { _entry.Text=(Value*DisplayScale).ToString("G",CultureInfo.CurrentCulture); }
            finally { _refreshing=false; }
            _entryAtFocus=_entry.Text;BeginEdit();
        };
        _entry.LostFocus += (_, _) => { Commit(); _typing = false; EndEdit(); RefreshValue(false); };
        _entry.KeyDown += OnEntryKey;
        Loaded += (_, _) => RefreshValue(false);
    }
    private void BeginEdit() { if (_editing) return; _editing = true; _beforeEdit = Value; EditStarted?.Invoke(this, EventArgs.Empty); }
    private void EndEdit() { if (!_editing) return; _editing = false; EditCompleted?.Invoke(this, EventArgs.Empty); }
    private bool UseLog => Logarithmic && Minimum > 0 && Maximum > Minimum;
    private double FromSlider(double value) => UseLog ? Math.Exp(Math.Log(Minimum) + value * Math.Log(Maximum / Minimum)) : value;
    private void RefreshValue(bool notify)
    {
        if (_refreshing) return;
        _refreshing = true;
        try {
            _label.Text = Header; _help.Text = Help; _help.Visibility = string.IsNullOrEmpty(Help) ? Visibility.Collapsed : Visibility.Visible;
            AutomationProperties.SetName(_entry, $"{Header} {Unit}, exact value");
            AutomationProperties.SetName(_slider, $"{Header} {Unit}");
            if (double.IsFinite(Value)) {
                _slider.Minimum = UseLog ? 0 : Math.Min(Minimum, Value);
                _slider.Maximum = UseLog ? 1 : Math.Max(Maximum, Value);
                _slider.StepFrequency = UseLog ? .001 : StepFrequency;
                _slider.Value = UseLog ? Math.Clamp(Math.Log(Math.Max(Minimum, Value) / Minimum) / Math.Log(Maximum / Minimum), 0, 1) : Value;
                if (!_typing) _entry.Text = (Value * DisplayScale).ToString(Format, CultureInfo.CurrentCulture);
            }
            _range.Text = $"{Minimum * DisplayScale:0.##} — {Maximum * DisplayScale:0.##} {Unit} · default {DefaultValue * DisplayScale:0.##}";
        } finally { _refreshing = false; }
        if (notify) ValueChanged?.Invoke(this, Value);
    }
    private bool Commit()
    {
        // TextChanged can be delivered after LostFocus during fast navigation.
        // Compare the actual field with its focus snapshot instead of relying on
        // that asynchronous notification to decide whether to commit.
        if (_entry.Text==_entryAtFocus) return true;
        if (!double.TryParse(_entry.Text, NumberStyles.Float, CultureInfo.CurrentCulture, out var number) ||
            !double.IsFinite(number) || DisplayScale <= 0 || number / DisplayScale < Minimum || number / DisplayScale > Maximum) {
            _error.Text = $"Enter a value from {Minimum * DisplayScale:0.##} to {Maximum * DisplayScale:0.##} {Unit}.";
            _error.Visibility = Visibility.Visible; return false;
        }
        _error.Visibility = Visibility.Collapsed; Value = number / DisplayScale; _entryAtFocus=_entry.Text;return true;
    }
    private void OnEntryKey(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter) { if (Commit()) { EndEdit(); BeginEdit(); } e.Handled = true; }
        if (e.Key == VirtualKey.Escape) { Value = _beforeEdit; _entry.Text = (Value * DisplayScale).ToString(Format, CultureInfo.CurrentCulture);
            _error.Visibility = Visibility.Collapsed; _entryAtFocus=_entry.Text;EndEdit(); BeginEdit(); e.Handled = true; }
    }
    // Begin before the Slider's class handler changes its value, and suppress
    // wheel edits before that handler sees an unfocused scrolling gesture.
    private sealed class AdjustmentSlider : Slider
    {
        public Action? StartEdit, FinishEdit;
        protected override void OnPointerPressed(PointerRoutedEventArgs e) { StartEdit?.Invoke();base.OnPointerPressed(e); }
        protected override void OnPointerReleased(PointerRoutedEventArgs e) { base.OnPointerReleased(e);FinishEdit?.Invoke(); }
        protected override void OnPointerCaptureLost(PointerRoutedEventArgs e) { base.OnPointerCaptureLost(e);FinishEdit?.Invoke(); }
        protected override void OnKeyDown(KeyRoutedEventArgs e) { StartEdit?.Invoke();base.OnKeyDown(e); }
        protected override void OnKeyUp(KeyRoutedEventArgs e) { base.OnKeyUp(e);FinishEdit?.Invoke(); }
        protected override void OnPointerWheelChanged(PointerRoutedEventArgs e) {
            if (FocusState==FocusState.Unfocused) return;
            StartEdit?.Invoke();base.OnPointerWheelChanged(e);FinishEdit?.Invoke();
        }
    }
}
