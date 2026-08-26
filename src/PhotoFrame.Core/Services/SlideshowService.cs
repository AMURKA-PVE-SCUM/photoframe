namespace PhotoFrame.Core.Services;

public class SlideshowService
{
    private readonly List<string> _images = new();
    private int _currentIndex;
    private readonly Random _random = new();

    public event Action<string>? ImageChanged;
    public event Action? ListEmptied;

    public string? CurrentImage => _Images.Count > 0 ? _Images[_currentIndex] : null;
    public int Count => _Images.Count;

    private List<string> _Images => _images;

    public void SetImages(List<string> images)
    {
        _images.Clear();
        _images.AddRange(images);
        _currentIndex = 0;

        if (_images.Count > 0)
            RaiseImageChanged();
        else
            ListEmptied?.Invoke();
    }

    public string Next(bool shuffle)
    {
        if (_images.Count == 0)
            return string.Empty;

        _currentIndex = shuffle
            ? _random.Next(_images.Count)
            : (_currentIndex + 1) % _images.Count;

        RaiseImageChanged();
        return _images[_currentIndex];
    }

    public string Previous(bool shuffle)
    {
        if (_images.Count == 0)
            return string.Empty;

        _currentIndex = shuffle
            ? _random.Next(_images.Count)
            : (_currentIndex - 1 + _images.Count) % _images.Count;

        RaiseImageChanged();
        return _images[_currentIndex];
    }

    private void RaiseImageChanged()
    {
        if (_currentIndex >= 0 && _currentIndex < _images.Count)
            ImageChanged?.Invoke(_images[_currentIndex]);
    }
}
