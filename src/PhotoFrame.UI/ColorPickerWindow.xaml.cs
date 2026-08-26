using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace PhotoFrame.UI;

public partial class ColorPickerWindow : Window
{
    public Color SelectedColor { get; private set; }

    private readonly Slider _rSlider;
    private readonly Slider _gSlider;
    private readonly Slider _bSlider;
    private readonly Border _preview;

    public ColorPickerWindow(Color initial)
    {
        Title = "Выбор цвета";
        Width = 300;
        Height = 220;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        ResizeMode = System.Windows.ResizeMode.NoResize;
        Background = new SolidColorBrush(Color.FromRgb(40, 40, 42));

        SelectedColor = initial;

        var stack = new StackPanel { Margin = new Thickness(12) };

        _preview = new Border
        {
            Height = 40,
            CornerRadius = new CornerRadius(4),
            Background = new SolidColorBrush(initial),
            Margin = new Thickness(0, 0, 0, 12)
        };
        stack.Children.Add(_preview);

        _rSlider = AddSlider(stack, "R", initial.R);
        _gSlider = AddSlider(stack, "G", initial.G);
        _bSlider = AddSlider(stack, "B", initial.B);

        var okBtn = new Button
        {
            Content = "OK",
            Height = 28,
            Margin = new Thickness(0, 12, 0, 0),
            Background = new SolidColorBrush(Color.FromRgb(60, 60, 65)),
            Foreground = Brushes.White,
            BorderThickness = new Thickness(0)
        };
        okBtn.Click += (_, _) => { SelectedColor = GetColor(); DialogResult = true; };
        stack.Children.Add(okBtn);

        Content = stack;

        _rSlider.ValueChanged += (_, _) => UpdatePreview();
        _gSlider.ValueChanged += (_, _) => UpdatePreview();
        _bSlider.ValueChanged += (_, _) => UpdatePreview();
    }

    private static Slider AddSlider(StackPanel parent, string label, byte value)
    {
        var grid = new Grid { Margin = new Thickness(0, 2, 0, 2) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(20) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(35) });

        var lbl = new TextBlock
        {
            Text = label,
            Foreground = Brushes.White,
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = 12
        };
        Grid.SetColumn(lbl, 0);

        var slider = new Slider
        {
            Minimum = 0,
            Maximum = 255,
            Value = value,
            VerticalAlignment = VerticalAlignment.Center
        };
        Grid.SetColumn(slider, 1);

        var valText = new TextBlock
        {
            Text = value.ToString(),
            Foreground = Brushes.White,
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Right,
            FontSize = 11,
            Margin = new Thickness(4, 0, 0, 0)
        };
        Grid.SetColumn(valText, 2);

        slider.ValueChanged += (_, e) => valText.Text = ((int)e.NewValue).ToString();

        grid.Children.Add(lbl);
        grid.Children.Add(slider);
        grid.Children.Add(valText);
        parent.Children.Add(grid);

        return slider;
    }

    private Color GetColor()
    {
        return Color.FromRgb((byte)_rSlider.Value, (byte)_gSlider.Value, (byte)_bSlider.Value);
    }

    private void UpdatePreview()
    {
        _preview.Background = new SolidColorBrush(GetColor());
    }
}
