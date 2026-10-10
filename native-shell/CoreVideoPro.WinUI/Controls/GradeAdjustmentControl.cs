using CoreVideoPro.MediaCore.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed class GradeAdjustmentControl : UserControl
{
    public static readonly DependencyProperty AdjustmentProperty = DependencyProperty.Register(nameof(Adjustment), typeof(GradeOperation),
        typeof(GradeAdjustmentControl), new PropertyMetadata(null,(d,_) => ((GradeAdjustmentControl)d).LoadAdjustment()));
    public GradeOperation? Adjustment { get => (GradeOperation?)GetValue(AdjustmentProperty); set => SetValue(AdjustmentProperty,value); }
    public event EventHandler<GradeOperation>? AdjustmentEdited;
    public event EventHandler? EditStarted;
    public event EventHandler? EditCompleted;
    private readonly StackPanel _root = new() { Spacing = 8 };
    private readonly StackPanel _primaries = new() { Spacing = 6 }, _curves = new() { Spacing = 6 };
    private readonly CheckBox _enabled = new() { Content = "Adjustment enabled" };
    private readonly SliderValueControl _intensity = Number("Adjustment intensity",0,1,.01,1,"%",100);
    private readonly Dictionary<string,SliderValueControl> _numbers = [];
    private readonly ComboBox _channel = new() { ItemsSource = new[] { "Master", "Red", "Green", "Blue" }, SelectedIndex = 0 };
    private readonly GradeCurveControl _curve = new();
    private readonly SliderValueControl _pointX = Number("Point input",0,1,.01,0,"%",100), _pointY = Number("Point output",0,1,.01,0,"%",100);
    private bool _loading;
    public GradeAdjustmentControl()
    {
        Content = _root; _root.Children.Add(_enabled); _root.Children.Add(_intensity);
        TrackEdit(_intensity); TrackEdit(_pointX); TrackEdit(_pointY);
        _enabled.Checked += (_,_) => Edited(); _enabled.Unchecked += (_,_) => Edited(); _intensity.ValueChanged += (_,_) => Edited();
        Add("ExposureStops","Exposure",-8,8,.1,0,"stops","Brighter → right; darker → left."); Add("Contrast","Contrast",0,4,.05,1,"×","Spread around the pivot; 1 is neutral.");
        Add("Pivot","Contrast pivot (encoded)",0,1,.01,.5,"","Brightness level held by contrast."); Add("Saturation","Saturation",0,4,.05,1,"×","0 removes color; 1 is neutral.");
        Add("Temperature","Warm / cool balance",-1,1,.01,0,"","Cool ← → warm; 0 is neutral."); Add("Tint","Green / magenta balance",-1,1,.01,0,"","Green ← → magenta; 0 is neutral.");
        Add("Lift","Lift (encoded)",-1,1,.01,0,"","Adjust black level."); Add("Gamma","Gamma",.1,4,.05,1,"×","Adjust midtones; 1 is neutral."); Add("Gain","Gain",0,4,.05,1,"×","Adjust highlights; 1 is neutral.");
        _root.Children.Add(_primaries); _root.Children.Add(_curves);
        _curves.Children.Add(_channel); _curves.Children.Add(_curve); _curves.Children.Add(_pointX); _curves.Children.Add(_pointY);
        var reset = new Button { Content = "Reset channel" }; _curves.Children.Add(reset); reset.Click += (_,_) => _curve.Reset();
        _curves.Children.Add(new TextBlock { Text = "Click to add • drag to shape • double-click or Delete to remove. Endpoints move vertically. Linear interpolation.", TextWrapping = TextWrapping.Wrap, FontSize = 11 });
        _channel.SelectionChanged += (_,_) => LoadCurve();
        _curve.CurveEdited += (_,_) => {
            if (_loading || Adjustment is not { } a) return;
            var curves = a.Curves.Select(c=>c.ToArray()).ToArray(); curves[Math.Max(0,_channel.SelectedIndex)] = _curve.Points.ToArray();
            AdjustmentEdited?.Invoke(this,a with { Curves = curves });
        };
        _curve.PointSelected += (_,_) => {
            _loading = true; var i = _curve.SelectedIndex;
            _pointX.IsEnabled = _pointY.IsEnabled = i >= 0;
            if (i >= 0) { _pointX.Value = _curve.Points[i].X; _pointY.Value = _curve.Points[i].Y; }
            _loading = false;
        };
        _pointX.ValueChanged += (_,_) => { if (!_loading) _curve.SetSelected(_pointX.Value,_pointY.Value); };
        _pointY.ValueChanged += (_,_) => { if (!_loading) _curve.SetSelected(_pointX.Value,_pointY.Value); };
        LoadAdjustment();
    }
    private static SliderValueControl Number(string label,double min,double max,double step,double neutral=0,string unit="",double scale=1) => new() {
        Header = label, Minimum = min, Maximum = max, StepFrequency = step, DefaultValue = neutral, Unit = unit, DisplayScale = scale };
    private void TrackEdit(SliderValueControl control) {
        control.EditStarted += (_,_) => EditStarted?.Invoke(this,EventArgs.Empty);
        control.EditCompleted += (_,_) => EditCompleted?.Invoke(this,EventArgs.Empty);
    }
    private void Add(string key,string label,double min,double max,double step,double neutral,string unit,string help) {
        var box = Number(label,min,max,step,neutral,unit); box.Help = help; TrackEdit(box);
        _numbers.Add(key,box); _primaries.Children.Add(box); box.ValueChanged += (_,_) => Edited();
    }
    private void LoadAdjustment()
    {
        _loading = true; IsEnabled = Adjustment is not null;
        var a = Adjustment ?? new(); _enabled.IsChecked = a.Enabled; _intensity.Value = a.Intensity;
        foreach (var item in _numbers) item.Value.Value = (double)typeof(GradeOperation).GetProperty(item.Key)!.GetValue(a)!;
        _primaries.Visibility = a.Kind == "primaries" ? Visibility.Visible : Visibility.Collapsed;
        _curves.Visibility = a.Kind == "curves" ? Visibility.Visible : Visibility.Collapsed;
        LoadCurve(); _loading = false;
    }
    private void LoadCurve() { if (Adjustment is { } a) _curve.SetPoints(a.Curves[Math.Max(0,_channel.SelectedIndex)]); }
    private void Edited()
    {
        if (_loading || Adjustment is not { } a || _numbers.Values.Any(n=>!double.IsFinite(n.Value)) || !double.IsFinite(_intensity.Value)) return;
        AdjustmentEdited?.Invoke(this,a with { Enabled = _enabled.IsChecked == true, Intensity = _intensity.Value,
            ExposureStops = _numbers["ExposureStops"].Value, Contrast = _numbers["Contrast"].Value, Pivot = _numbers["Pivot"].Value,
            Saturation = _numbers["Saturation"].Value, Temperature = _numbers["Temperature"].Value, Tint = _numbers["Tint"].Value,
            Lift = _numbers["Lift"].Value, Gamma = _numbers["Gamma"].Value, Gain = _numbers["Gain"].Value });
    }
}
