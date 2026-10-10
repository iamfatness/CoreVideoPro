using System.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class ColorGradeEditorWindow
{
    private bool _loadingDocument = true;
    internal void InitializeOffscreenBindings() => Bindings.Initialize();
    private void InitializeAdvancedWorkspace()
    {
        AdjustmentEditor.AdjustmentEdited += OnAdjustmentEdited;
        AdjustmentEditor.EditStarted += OnAdjustmentEditStarted;
        AdjustmentEditor.EditCompleted += OnAdjustmentEditCompleted;
        GradeIntensity.EditStarted += OnAdjustmentEditStarted;
        GradeIntensity.EditCompleted += OnAdjustmentEditCompleted;
        RoiGuide.RoiChanged += (_,roi) => ViewModel.SetScopeRoi(roi);
        RoiGuide.ToolChanged += (_,_) => RefreshRoi();
        ViewModel.PropertyChanged += OnWorkspacePropertyChanged;
        ViewModel.AdvancedDocumentChanged += OnAdvancedDocumentChanged;
        RefreshWorkspace(); RefreshDocumentControls(); RefreshRoi();
    }
    private void StopAdvancedWorkspace()
    {
        AdjustmentEditor.AdjustmentEdited -= OnAdjustmentEdited;
        AdjustmentEditor.EditStarted -= OnAdjustmentEditStarted;
        AdjustmentEditor.EditCompleted -= OnAdjustmentEditCompleted;
        GradeIntensity.EditStarted -= OnAdjustmentEditStarted;
        GradeIntensity.EditCompleted -= OnAdjustmentEditCompleted;
        ViewModel.PropertyChanged -= OnWorkspacePropertyChanged;
        ViewModel.AdvancedDocumentChanged -= OnAdvancedDocumentChanged;
    }
    private void OnAdjustmentEdited(object? sender,GradeOperation operation) => ViewModel.EditAdjustment(operation);
    private void OnAdjustmentEditStarted(object? sender,EventArgs e) => ViewModel.BeginAdjustmentGesture();
    private void OnAdjustmentEditCompleted(object? sender,EventArgs e) => ViewModel.EndAdjustmentGesture();
    private void OnWorkspacePropertyChanged(object? sender,PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ViewModel.SelectedAdjustment)) RefreshDocumentControls();
        if (e.PropertyName == nameof(ViewModel.AdvancedExpanded) && ViewModel.AdvancedExpanded && AppWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter { State: Microsoft.UI.Windowing.OverlappedPresenterState.Restored })
            AppWindow.Resize(new Windows.Graphics.SizeInt32(1320,920));
        if (e.PropertyName is nameof(ViewModel.AdvancedExpanded) or nameof(ViewModel.ScopesEnabled)) RefreshWorkspace();
        if (e.PropertyName is nameof(ViewModel.AdvancedExpanded) or nameof(ViewModel.ScopesEnabled) or nameof(ViewModel.ScopeRoiGuideVisible) or nameof(ViewModel.ScopeRoi) or nameof(ViewModel.NativeSurface)) RefreshRoi();
    }
    private void RefreshWorkspace()
    {
        var advanced = ViewModel.AdvancedExpanded;
        AdvancedPanel.Visibility = advanced ? Visibility.Visible : Visibility.Collapsed;
        BasicPanel.Visibility = advanced ? Visibility.Collapsed : Visibility.Visible;
        ScopePanel.Visibility = advanced && ViewModel.ScopesEnabled ? Visibility.Visible : Visibility.Collapsed;
    }
    private void OnAdvancedDocumentChanged(object? sender,EventArgs e) => RefreshDocumentControls();
    private void RefreshDocumentControls()
    {
        _loadingDocument = true;
        BypassGrade.IsChecked = ViewModel.Document.Bypass; GradeIntensity.Value = ViewModel.Document.Intensity;
        AdjustmentSelector.SelectedIndex = ViewModel.SelectedAdjustment is not { } selected ? -1 :
            ViewModel.Adjustments.Select((o,i)=>(o,i)).Where(p=>p.o.Id==selected.Id).Select(p=>p.i).DefaultIfEmpty(-1).First();
        _loadingDocument = false;
    }
    private void OnBypassGrade(object sender,RoutedEventArgs e) { if (!_loadingDocument) ViewModel.EditDocument(ViewModel.Document with { Bypass = BypassGrade.IsChecked == true }); }
    private void OnGradeIntensityChanged(object? sender,double value) { if (!_loadingDocument && double.IsFinite(value)) ViewModel.EditDocument(ViewModel.Document with { Intensity = value }); }
    private void OnAdjustmentSelected(object sender,SelectionChangedEventArgs e)
    {
        if (!_loadingDocument && AdjustmentSelector.SelectedItem is GradeOperation selected)
            ViewModel.SelectedAdjustment = ViewModel.Document.Operations.FirstOrDefault(o=>o.Id==selected.Id);
    }
    private void OnCopyGrade(object sender,RoutedEventArgs e) { var data = new DataPackage(); data.SetText(ViewModel.CopyGradeJson()); Clipboard.SetContent(data); }
    private async void OnPasteGrade(object sender,RoutedEventArgs e)
    {
        try { var content = Clipboard.GetContent(); if (content.Contains(StandardDataFormats.Text)) ViewModel.PasteGradeJson(await content.GetTextAsync()); }
        catch (Exception ex) { ViewModel.EditStatus = ex.Message; }
    }
    private async void OnSavePreset(object sender,RoutedEventArgs e)
    {
        try {
            var picker = new FileSavePicker { SuggestedFileName = "Source grade" };
            picker.FileTypeChoices.Add("CoreVideo grade",new List<string> { ".cvgrade" }); InitializeWithWindow.Initialize(picker,WindowNative.GetWindowHandle(this));
            var file = await picker.PickSaveFileAsync(); if (file is not null) await FileIO.WriteTextAsync(file,ViewModel.CopyGradeJson());
        } catch (Exception ex) { ViewModel.EditStatus = ex.Message; }
    }
    private async void OnLoadPreset(object sender,RoutedEventArgs e)
    {
        try { var file = await PickFile(".cvgrade"); if (file is not null) {
            var properties = await file.GetBasicPropertiesAsync(); if (properties.Size > 16_500_000) throw new ArgumentException("Preset is too large.");
            ViewModel.PasteGradeJson(await FileIO.ReadTextAsync(file));
        } } catch (Exception ex) { ViewModel.EditStatus = ex.Message; }
    }
    private async void OnImportLut(object sender,RoutedEventArgs e)
    {
        try { var file = await PickFile(".cube"); if (file is not null) {
            var properties = await file.GetBasicPropertiesAsync(); if (properties.Size > 2_000_000) throw new ArgumentException("LUT exceeds 2 MB.");
            ViewModel.ImportCube(await FileIO.ReadTextAsync(file),file.DisplayName);
        } } catch (Exception ex) { ViewModel.EditStatus = ex.Message; }
    }
    private async Task<StorageFile?> PickFile(string extension)
    {
        var picker = new FileOpenPicker(); picker.FileTypeFilter.Add(extension); InitializeWithWindow.Initialize(picker,WindowNative.GetWindowHandle(this));
        return await picker.PickSingleFileAsync();
    }
}
