using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;
using PhotoFrame.Core.Models;
using PhotoFrame.Core.Services;

namespace PhotoFrame.UI;

public partial class MainWindow : Window
{
    private readonly SettingsService _settingsService = new();
    private readonly ImageLoaderService _imageLoader = new();
    private readonly SlideshowService _slideshow = new();
    private readonly AutoStartService _autoStart = new();
    private readonly DispatcherTimer _timer;
    private DispatcherTimer? _gradientTimer;
    private FileSystemWatcher? _watcher;
    private AppSettings _settings;

    private MenuItem _shuffleMenuItem = null!;
    private MenuItem _subfoldersMenuItem = null!;
    private MenuItem _topmostMenuItem = null!;
    private MenuItem _autoStartMenuItem = null!;
    private MenuItem _frameWood = null!;
    private MenuItem _frameMetal = null!;
    private MenuItem _frameGold = null!;
    private MenuItem _frameModern = null!;
    private MenuItem _gradientMenuItem = null!;
    private readonly List<MenuItem> _shapeMenuItems = new();

    private double _gradientAngle = 0;

    public MainWindow()
    {
        InitializeComponent();
        _settings = _settingsService.Load();
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(_settings.IntervalSeconds) };
        _timer.Tick += Timer_Tick;
        _slideshow.ImageChanged += OnImageChanged;
        _slideshow.ListEmptied += OnListEmptied;
        BuildContextMenu();
    }

    #region Context menu

    private void BuildContextMenu()
    {
        var menu = new ContextMenu();

        var selectFolder = new MenuItem { Header = "Выбрать папку..." };
        selectFolder.Click += Menu_SelectFolder;
        menu.Items.Add(selectFolder);
        menu.Items.Add(new Separator());

        var intervalMenu = new MenuItem { Header = "Интервал" };
        foreach (var (header, seconds) in new[] { ("5 сек", 5), ("10 сек", 10), ("30 сек", 30),
                                                    ("1 мин", 60), ("5 мин", 300), ("15 мин", 900) })
        {
            var mi = new MenuItem { Header = header, Tag = seconds.ToString() };
            mi.Click += Menu_Interval;
            intervalMenu.Items.Add(mi);
        }
        menu.Items.Add(intervalMenu);

        _shuffleMenuItem = new MenuItem { Header = "Случайный порядок", IsCheckable = true };
        _shuffleMenuItem.Click += (_, _) => { _settings.ShuffleOrder = _shuffleMenuItem.IsChecked; SaveSettings(); };
        menu.Items.Add(_shuffleMenuItem);
        menu.Items.Add(new Separator());

        // Frame style
        var frameMenu = new MenuItem { Header = "Материал рамки" };
        _frameWood = new MenuItem { Header = "Дерево", Tag = "Wood", IsCheckable = true };
        _frameMetal = new MenuItem { Header = "Металл", Tag = "Metal", IsCheckable = true };
        _frameGold = new MenuItem { Header = "Золото", Tag = "Gold", IsCheckable = true };
        _frameModern = new MenuItem { Header = "Чёрный матовый", Tag = "Modern", IsCheckable = true };
        _gradientMenuItem = new MenuItem { Header = "Градиент (свои цвета)", Tag = "Gradient", IsCheckable = true };
        _frameWood.Click += Menu_FrameStyle;
        _frameMetal.Click += Menu_FrameStyle;
        _frameGold.Click += Menu_FrameStyle;
        _frameModern.Click += Menu_FrameStyle;
        _gradientMenuItem.Click += Menu_FrameStyle;
        frameMenu.Items.Add(_frameWood);
        frameMenu.Items.Add(_frameMetal);
        frameMenu.Items.Add(_frameGold);
        frameMenu.Items.Add(_frameModern);
        frameMenu.Items.Add(new Separator());
        frameMenu.Items.Add(_gradientMenuItem);
        menu.Items.Add(frameMenu);

        // Shape
        var shapeMenu = new MenuItem { Header = "Форма" };
        foreach (var (header, tag) in new[] { ("Прямоугольник", "Rectangle"), ("Скруглённый", "Rounded"), ("Круг", "Circle") })
        {
            var mi = new MenuItem { Header = header, Tag = tag, IsCheckable = true };
            mi.Click += Menu_FrameShape;
            _shapeMenuItems.Add(mi);
            shapeMenu.Items.Add(mi);
        }
        menu.Items.Add(shapeMenu);

        // Thickness
        var thickMenu = new MenuItem { Header = "Толщина рамки" };
        foreach (var (header, val) in new[] { ("Тонкая", "4"), ("Стандарт", "10"), ("Толстая", "16"), ("Очень толстая", "24") })
        {
            var mi = new MenuItem { Header = header, Tag = val };
            mi.Click += Menu_Thickness;
            thickMenu.Items.Add(mi);
        }
        menu.Items.Add(thickMenu);

        // Gradient colors (only when gradient mode is active)
        var gradientColorMenu = new MenuItem { Header = "Цвета градиента" };
        var color1Item = new MenuItem { Header = "Цвет 1..." };
        color1Item.Click += (_, _) => PickGradientColor(1);
        var color2Item = new MenuItem { Header = "Цвет 2..." };
        color2Item.Click += (_, _) => PickGradientColor(2);
        gradientColorMenu.Items.Add(color1Item);
        gradientColorMenu.Items.Add(color2Item);
        menu.Items.Add(gradientColorMenu);

        var opacityMenu = new MenuItem { Header = "Прозрачность" };
        foreach (var (header, val) in new[] { ("100%", "1.0"), ("90%", "0.9"), ("80%", "0.8"),
                                                ("70%", "0.7"), ("60%", "0.6"), ("50%", "0.5") })
        {
            var mi = new MenuItem { Header = header, Tag = val };
            mi.Click += Menu_Opacity;
            opacityMenu.Items.Add(mi);
        }
        menu.Items.Add(opacityMenu);
        menu.Items.Add(new Separator());

        _subfoldersMenuItem = new MenuItem { Header = "Включая подпапки", IsCheckable = true };
        _subfoldersMenuItem.Click += (_, _) => { _settings.IncludeSubfolders = _subfoldersMenuItem.IsChecked; StartWatching(); LoadPhotos(); SaveSettings(); };
        menu.Items.Add(_subfoldersMenuItem);

        _topmostMenuItem = new MenuItem { Header = "Всегда поверх остальных", IsCheckable = true };
        _topmostMenuItem.Click += (_, _) => { _settings.AlwaysOnTop = _topmostMenuItem.IsChecked; Topmost = _settings.AlwaysOnTop; SaveSettings(); };
        menu.Items.Add(_topmostMenuItem);

        _autoStartMenuItem = new MenuItem { Header = "Автозапуск с Windows", IsCheckable = true };
        _autoStartMenuItem.Click += (_, _) => { _settings.AutoStart = _autoStartMenuItem.IsChecked; _autoStart.SetAutoStart(_settings.AutoStart); SaveSettings(); };
        menu.Items.Add(_autoStartMenuItem);
        menu.Items.Add(new Separator());

        var nextItem = new MenuItem { Header = "Следующее фото", InputGestureText = "Пробел" };
        nextItem.Click += (_, _) => _slideshow.Next(_shuffleMenuItem.IsChecked);
        menu.Items.Add(nextItem);
        var prevItem = new MenuItem { Header = "Предыдущее фото", InputGestureText = "Backspace" };
        prevItem.Click += (_, _) => _slideshow.Previous(_shuffleMenuItem.IsChecked);
        menu.Items.Add(prevItem);
        menu.Items.Add(new Separator());

        var exitItem = new MenuItem { Header = "Выход" };
        exitItem.Click += (_, _) => Close();
        menu.Items.Add(exitItem);

        RootGrid.ContextMenu = menu;
    }

    #endregion

    #region Window lifecycle

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        ApplySettings();
        StartWatching();
        LoadPhotos();
        _timer.Start();
        ApplyShape(_settings.FrameShape);
        if (_settings.FrameStyle == "Gradient")
            StartGradientAnimation();
    }

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _timer.Stop();
        _gradientTimer?.Stop();
        _watcher?.Dispose();
        SaveSettings();
    }

    #endregion

    #region Settings

    public void SaveSettings()
    {
        _settings.WindowX = Left;
        _settings.WindowY = Top;
        _settings.WindowWidth = Width;
        _settings.WindowHeight = Height;
        _settings.Opacity = Opacity;
        _settings.ShuffleOrder = _shuffleMenuItem.IsChecked;
        _settings.AlwaysOnTop = _topmostMenuItem.IsChecked;
        _settings.AutoStart = _autoStartMenuItem.IsChecked;
        _settings.IncludeSubfolders = _subfoldersMenuItem.IsChecked;
        _settingsService.Save(_settings);
    }

    private void ApplySettings()
    {
        Left = _settings.WindowX;
        Top = _settings.WindowY;
        Width = _settings.WindowWidth;
        Height = _settings.WindowHeight;
        Opacity = _settings.Opacity;
        Topmost = _settings.AlwaysOnTop;
        _shuffleMenuItem.IsChecked = _settings.ShuffleOrder;
        _subfoldersMenuItem.IsChecked = _settings.IncludeSubfolders;
        _topmostMenuItem.IsChecked = _settings.AlwaysOnTop;
        _autoStartMenuItem.IsChecked = _autoStart.IsAutoStartEnabled();
        ApplyFrameStyle(_settings.FrameStyle);
        UpdateStyleChecks();
        UpdateShapeChecks();
        ApplyThickness(_settings.FrameThickness);
    }

    #endregion

    #region Frame style — brush

    private void ApplyFrameStyle(string style)
    {
        StopGradientAnimation();

        Brush brush = style switch
        {
            "Wood" => new LinearGradientBrush(
                new GradientStopCollection
                {
                    new(Color.FromRgb(101, 67, 33), 0),
                    new(Color.FromRgb(139, 90, 43), 0.3),
                    new(Color.FromRgb(80, 50, 20), 0.6),
                    new(Color.FromRgb(120, 78, 35), 1),
                }, 45),
            "Metal" => new LinearGradientBrush(
                new GradientStopCollection
                {
                    new(Color.FromRgb(180, 180, 185), 0),
                    new(Color.FromRgb(120, 120, 128), 0.3),
                    new(Color.FromRgb(200, 200, 205), 0.6),
                    new(Color.FromRgb(140, 140, 148), 1),
                }, 90),
            "Gold" => new LinearGradientBrush(
                new GradientStopCollection
                {
                    new(Color.FromRgb(180, 145, 50), 0),
                    new(Color.FromRgb(220, 185, 80), 0.3),
                    new(Color.FromRgb(160, 125, 35), 0.6),
                    new(Color.FromRgb(210, 175, 70), 1),
                }, 45),
            "Modern" => new SolidColorBrush(Color.FromRgb(40, 40, 42)),
            "Gradient" => MakeGradientBrush(_gradientAngle),
            _ => new SolidColorBrush(Color.FromRgb(139, 90, 43)),
        };

        ApplyBrushToFrame(brush);

        if (style == "Gradient")
            StartGradientAnimation();

        ApplyShape(_settings.FrameShape);
    }

    private void ApplyBrushToFrame(Brush brush)
    {
        RectFrame.BorderBrush = brush;
        CircleRing.Fill = brush;
    }

    private LinearGradientBrush MakeGradientBrush(double angle)
    {
        var c1 = ParseColor(_settings.GradientColor1, Color.FromRgb(0, 120, 255));
        var c2 = ParseColor(_settings.GradientColor2, Color.FromRgb(255, 0, 120));
        return new LinearGradientBrush(
            new GradientStopCollection
            {
                new(c1, 0),
                new(c2, 0.5),
                new(c1, 1),
            }, angle);
    }

    private static Color ParseColor(string hex, Color fallback)
    {
        try
        {
            return (Color)ColorConverter.ConvertFromString(hex);
        }
        catch
        {
            return fallback;
        }
    }

    private void StartGradientAnimation()
    {
        _gradientTimer?.Stop();
        _gradientTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
        _gradientTimer.Tick += (_, _) =>
        {
            _gradientAngle = (_gradientAngle + 1.5) % 360;
            var brush = MakeGradientBrush(_gradientAngle);
            ApplyBrushToFrame(brush);
        };
        _gradientTimer.Start();
    }

    private void StopGradientAnimation()
    {
        _gradientTimer?.Stop();
        _gradientTimer = null;
    }

    private void PickGradientColor(int colorIndex)
    {
        var current = colorIndex == 1
            ? ParseColor(_settings.GradientColor1, Color.FromRgb(0, 120, 255))
            : ParseColor(_settings.GradientColor2, Color.FromRgb(255, 0, 120));

        var picker = new ColorPickerWindow(current) { Owner = this };
        if (picker.ShowDialog() == true)
        {
            var c = picker.SelectedColor;
            var hex = $"#{c.R:X2}{c.G:X2}{c.B:X2}";
            if (colorIndex == 1) _settings.GradientColor1 = hex;
            else _settings.GradientColor2 = hex;

            if (_settings.FrameStyle == "Gradient")
            {
                var brush = MakeGradientBrush(_gradientAngle);
                ApplyBrushToFrame(brush);
            }
            SaveSettings();
        }
    }

    private void UpdateStyleChecks()
    {
        _frameWood.IsChecked = _settings.FrameStyle == "Wood";
        _frameMetal.IsChecked = _settings.FrameStyle == "Metal";
        _frameGold.IsChecked = _settings.FrameStyle == "Gold";
        _frameModern.IsChecked = _settings.FrameStyle == "Modern";
        _gradientMenuItem.IsChecked = _settings.FrameStyle == "Gradient";
    }

    #endregion

    #region Frame shape

    private void ApplyShape(string shape)
    {
        _settings.FrameShape = shape;
        UpdateShapeChecks();

        double w = ActualWidth > 0 ? ActualWidth : Width;
        double h = ActualHeight > 0 ? ActualHeight : Height;
        if (w <= 0 || h <= 0) return;

        double thickness = 10;
        double.TryParse(_settings.FrameThickness, System.Globalization.CultureInfo.InvariantCulture, out thickness);

        if (shape == "Circle")
        {
            RectFrame.Visibility = Visibility.Collapsed;
            CircleFrame.Visibility = Visibility.Visible;
            CircleFrame.Width = w;
            CircleFrame.Height = h;

            double diameter = Math.Min(w, h);
            double radius = diameter / 2;
            double cx = w / 2, cy = h / 2;

            // Outer ring (frame)
            Canvas.SetLeft(CircleRing, cx - radius);
            Canvas.SetTop(CircleRing, cy - radius);
            CircleRing.Width = diameter;
            CircleRing.Height = diameter;

            // Inner photo circle
            double innerR = radius - thickness;
            if (innerR < 5) innerR = 5;
            Canvas.SetLeft(CirclePhotoBorder, cx - innerR);
            Canvas.SetTop(CirclePhotoBorder, cy - innerR);
            CirclePhotoBorder.Width = innerR * 2;
            CirclePhotoBorder.Height = innerR * 2;
            CirclePhotoBorder.Clip = new EllipseGeometry(new Point(innerR, innerR), innerR, innerR);

            // Copy brush
            CircleRing.Fill = RectFrame.BorderBrush;

            // Sync photo
            CirclePhotoImage.Source = PhotoImage.Source;

            // No photos text position
            CircleNoPhotosText.Width = innerR * 2;
            CircleNoPhotosText.Height = innerR * 2;
            Canvas.SetLeft(CircleNoPhotosText, cx - innerR);
            Canvas.SetTop(CircleNoPhotosText, cy - innerR);
            CircleNoPhotosText.Visibility = NoPhotosText.Visibility;
        }
        else
        {
            RectFrame.Visibility = Visibility.Visible;
            CircleFrame.Visibility = Visibility.Collapsed;

            if (shape == "Rectangle")
                RectFrame.CornerRadius = new CornerRadius(0);
            else
                RectFrame.CornerRadius = new CornerRadius(12);
        }
    }

    private void UpdateShapeChecks()
    {
        foreach (var mi in _shapeMenuItems)
            mi.IsChecked = mi.Tag?.ToString() == _settings.FrameShape;
    }

    #endregion

    #region Thickness

    private void ApplyThickness(string thickness)
    {
        _settings.FrameThickness = thickness;
        if (double.TryParse(thickness, System.Globalization.CultureInfo.InvariantCulture, out double t))
            RectFrame.BorderThickness = new Thickness(t);
        ApplyShape(_settings.FrameShape);
    }

    #endregion

    #region Photo loading

    private void LoadPhotos()
    {
        if (string.IsNullOrWhiteSpace(_settings.PhotoFolderPath) || !Directory.Exists(_settings.PhotoFolderPath))
        {
            NoPhotosText.Visibility = Visibility.Visible;
            CounterText.Visibility = Visibility.Collapsed;
            if (_settings.FrameShape == "Circle")
            {
                CircleNoPhotosText.Visibility = Visibility.Visible;
                CircleNoPhotosText.Visibility = Visibility.Visible;
            }
            return;
        }
        var images = _imageLoader.LoadImages(_settings.PhotoFolderPath, _settings.IncludeSubfolders);
        _slideshow.SetImages(images);
        UpdateCounter();
    }

    private void StartWatching()
    {
        _watcher?.Dispose();
        _watcher = _imageLoader.WatchFolder(_settings.PhotoFolderPath, () =>
            Dispatcher.BeginInvoke(LoadPhotos), _settings.IncludeSubfolders);
    }

    private void OnImageChanged(string imagePath)
    {
        try
        {
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.UriSource = new Uri(imagePath);
            bmp.CacheOption = BitmapCacheOption.OnLoad;
            bmp.DecodePixelWidth = 800;
            bmp.EndInit();
            bmp.Freeze();

            PhotoImage.Source = bmp;
            CirclePhotoImage.Source = bmp;

            NoPhotosText.Visibility = Visibility.Collapsed;
            CounterText.Visibility = Visibility.Visible;
            CircleNoPhotosText.Visibility = Visibility.Collapsed;
            UpdateCounter();
        }
        catch { _slideshow.Next(_shuffleMenuItem.IsChecked); }
    }

    private void OnListEmptied()
    {
        PhotoImage.Source = null;
        CirclePhotoImage.Source = null;
        NoPhotosText.Visibility = Visibility.Visible;
        CounterText.Visibility = Visibility.Collapsed;
        if (_settings.FrameShape == "Circle")
        {
            CircleNoPhotosText.Visibility = Visibility.Visible;
        }
    }

    private void UpdateCounter()
    {
        var text = $"{_slideshow.Count} фото";
        if (_slideshow.Count > 0)
        {
            CounterText.Text = text;
        }
    }

    #endregion

    #region Timer

    private void Timer_Tick(object? sender, EventArgs e) => _slideshow.Next(_shuffleMenuItem.IsChecked);

    private void ResetTimer()
    {
        _timer.Stop();
        _timer.Interval = TimeSpan.FromSeconds(_settings.IntervalSeconds);
        _timer.Start();
    }

    #endregion

    #region Input

    private void Window_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.LeftButton == MouseButtonState.Pressed) DragMove();
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.Key == Key.Space) _slideshow.Next(_shuffleMenuItem.IsChecked);
        else if (e.Key == Key.Back) _slideshow.Previous(_shuffleMenuItem.IsChecked);
        else if (e.Key == Key.Escape) Close();
    }

    private void Window_MouseRightButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (RootGrid.ContextMenu is ContextMenu menu) menu.IsOpen = true;
        e.Handled = true;
    }

    #endregion

    #region Menu handlers

    private void Menu_SelectFolder(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Выберите папку с фотографиями", FolderName = _settings.PhotoFolderPath };
        if (dialog.ShowDialog() == true)
        {
            _settings.PhotoFolderPath = dialog.FolderName;
            StartWatching(); LoadPhotos(); ResetTimer(); SaveSettings();
        }
    }

    private void Menu_Interval(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem item && item.Tag is string tag && int.TryParse(tag, out int s))
        { _settings.IntervalSeconds = s; ResetTimer(); SaveSettings(); }
    }

    private void Menu_FrameStyle(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem item && item.Tag is string style)
        { _settings.FrameStyle = style; ApplyFrameStyle(style); UpdateStyleChecks(); SaveSettings(); }
    }

    private void Menu_FrameShape(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem item && item.Tag is string shape)
        { ApplyShape(shape); SaveSettings(); }
    }

    private void Menu_Thickness(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem item && item.Tag is string tag)
            ApplyThickness(tag);
    }

    private void Menu_Opacity(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem item && item.Tag is string tag &&
            double.TryParse(tag, System.Globalization.CultureInfo.InvariantCulture, out double v))
        { Opacity = v; _settings.Opacity = v; SaveSettings(); }
    }

    #endregion

    #region Window position/size

    private void Window_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (IsLoaded)
        {
            _settings.WindowWidth = Width;
            _settings.WindowHeight = Height;
            if (_settings.FrameShape == "Circle")
                ApplyShape("Circle");
        }
    }

    private void Window_LocationChanged(object? sender, EventArgs e)
    {
        if (IsLoaded) { _settings.WindowX = Left; _settings.WindowY = Top; }
    }

    #endregion
}
