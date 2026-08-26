namespace PhotoFrame.Core.Models;

public class AppSettings
{
    public string PhotoFolderPath { get; set; } = string.Empty;
    public int IntervalSeconds { get; set; } = 10;
    public bool ShuffleOrder { get; set; } = true;
    public double Opacity { get; set; } = 0.95;
    public string FrameStyle { get; set; } = "Wood";
    public string FrameShape { get; set; } = "Rounded";
    public string FrameThickness { get; set; } = "10";
    public string GradientColor1 { get; set; } = "#0078FF";
    public string GradientColor2 { get; set; } = "#FF0078";
    public bool AlwaysOnTop { get; set; } = true;
    public bool AutoStart { get; set; } = false;
    public bool IncludeSubfolders { get; set; } = false;
    public double WindowX { get; set; } = 100;
    public double WindowY { get; set; } = 100;
    public double WindowWidth { get; set; } = 400;
    public double WindowHeight { get; set; } = 320;
}
