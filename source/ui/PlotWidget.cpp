#include "PlotWidget.h"
#include <cmath>
#include <limits>

namespace
{
    /** Format a frequency value as a compact decade label (e.g. "1k", "10k", "20"). */
    juce::String formatFrequency (float f)
    {
        if (f >= 1.0e6f)  return juce::String (f / 1.0e6f, 2) + "M";
        if (f >= 1.0e3f)  return juce::String (f / 1.0e3f, 2) + "k";
        return juce::String (f, 0);
    }
}

PlotWidget::PlotWidget()
{
    setOpaque (true);
}

void PlotWidget::addSeries (const Series& series)
{
    seriesList.push_back (series);
    repaint();
}

void PlotWidget::addSeries (Series&& series)
{
    seriesList.push_back (std::move (series));
    repaint();
}

void PlotWidget::clear()
{
    seriesList.clear();
    repaint();
}

void PlotWidget::setAxisLabels (const juce::String& x, const juce::String& y)
{
    xLabel = x;
    yLabel = y;
    repaint();
}

void PlotWidget::setXRange (float mn, float mx)
{
    xMin = mn;
    xMax = mx;
    autoFitX = false;
    repaint();
}

void PlotWidget::setYRange (float mn, float mx)
{
    yMin = mn;
    yMax = mx;
    autoFitY = false;
    repaint();
}

void PlotWidget::setXAxisLog (bool logXEnabled)
{
    logX = logXEnabled;
    repaint();
}

std::vector<juce::Colour> PlotWidget::getPalette (int count)
{
    count = juce::jlimit (1, 16, count);

    std::vector<juce::Colour> palette;
    palette.reserve (static_cast<size_t> (count));

    for (int i = 0; i < count; ++i)
    {
        const float hue = static_cast<float> (i) / static_cast<float> (count);
        palette.emplace_back (juce::Colour::fromHSV (hue, 0.7f, 0.6f, 1.0f));
    }

    return palette;
}

juce::Rectangle<float> PlotWidget::getPlotArea() const
{
    auto bounds = getLocalBounds().toFloat();
    return bounds.reduced (60.0f, 10.0f).withTrimmedBottom (30.0f);
}

void PlotWidget::calcAutoFit()
{
    if (seriesList.empty()) return;

    bool first = true;
    for (auto& s : seriesList)
    {
        for (size_t i = 0; i < s.x.size() && i < s.y.size(); ++i)
        {
            // Points with non-positive X can't be shown on a log axis — skip them.
            if (logX && s.x[i] <= 0.0f)
                continue;

            if (first)
            {
                if (autoFitX) { xMin = xMax = s.x[i]; }
                if (autoFitY) { yMin = yMax = s.y[i]; }
                first = false;
            }
            else
            {
                if (autoFitX) { xMin = std::min (xMin, s.x[i]); xMax = std::max (xMax, s.x[i]); }
                if (autoFitY) { yMin = std::min (yMin, s.y[i]); yMax = std::max (yMax, s.y[i]); }
            }
        }
    }

    // Add 10% padding (log axis pads by a factor so the decade span grows by 20%).
    if (autoFitX && xMax > xMin)
    {
        if (logX && xMin > 0.0f)
        {
            float padFactor = std::pow (10.0f, std::log10 (xMax / xMin) * 0.1f);
            xMin /= padFactor;
            xMax *= padFactor;
        }
        else
        {
            float pad = (xMax - xMin) * 0.1f;
            xMin -= pad;
            xMax += pad;
        }
    }
    if (autoFitY && yMax > yMin)
    {
        float pad = (yMax - yMin) * 0.1f;
        yMin -= pad;
        yMax += pad;
    }

    // Default ranges if no data
    if (first)
    {
        xMin = 0; xMax = 1;
        yMin = 0; yMax = 1;
    }
}

juce::Point<float> PlotWidget::dataToScreen (float x, float y,
                                               juce::Rectangle<float> area) const
{
    float sx;
    if (logX)
    {
        // Log mapping: clamp non-positive X to the (positive) range floor so
        // degenerate points land on the left edge instead of producing NaN.
        const float xMinSafe = std::max (xMin, std::numeric_limits<float>::epsilon());
        const float xMaxSafe = std::max (xMax, xMinSafe);
        const float xSafe    = std::max (x, xMinSafe);
        const float logMin   = std::log10 (xMinSafe);
        const float logSpan  = std::max (std::log10 (xMaxSafe) - logMin, 1.0e-6f);
        sx = area.getX() + (std::log10 (xSafe) - logMin) / logSpan * area.getWidth();
    }
    else
    {
        sx = area.getX() + (x - xMin) / (xMax - xMin) * area.getWidth();
    }

    float sy = area.getBottom() - (y - yMin) / (yMax - yMin) * area.getHeight();
    return { sx, sy };
}

void PlotWidget::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    auto plotArea = getPlotArea();
    calcAutoFit();

    // --- Grid ---
    if (showGrid)
    {
        g.setColour (juce::Colours::darkgrey.withAlpha (0.5f));
        int numGridLines = 8;
        for (int i = 0; i <= numGridLines; ++i)
        {
            float t = (float) i / numGridLines;
            float x = plotArea.getX() + t * plotArea.getWidth();
            float y = plotArea.getY() + t * plotArea.getHeight();
            g.drawVerticalLine ((int) x, plotArea.getY(), plotArea.getBottom());
            g.drawHorizontalLine ((int) y, plotArea.getX(), plotArea.getRight());
        }
    }

    // --- Border ---
    g.setColour (juce::Colours::lightgrey);
    g.drawRect (plotArea, 1.0f);

    // --- Data series ---
    for (auto& s : seriesList)
    {
        if (s.x.size() < 2 || s.y.size() < 2)
            continue;

        g.setColour (s.colour);
        juce::Path path;
        bool first = true;

        size_t n = std::min (s.x.size(), s.y.size());
        for (size_t i = 0; i < n; ++i)
        {
            auto pt = dataToScreen (s.x[i], s.y[i], plotArea);
            if (first)
            {
                path.startNewSubPath (pt);
                first = false;
            }
            else
            {
                path.lineTo (pt);
            }
        }
        g.strokePath (path, juce::PathStrokeType (s.lineWidth));
    }

    // --- Axis labels ---
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (12.0f));

    // Y axis label
    if (yLabel.isNotEmpty())
    {
        g.saveState();
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi)
                        .translated (15.0f, plotArea.getCentreY() + yLabel.length() * 3.0f));
        g.drawText (yLabel, 0, 0, 200, 20, juce::Justification::centred);
        g.restoreState();
    }

    // X axis label
    if (xLabel.isNotEmpty())
    {
        g.drawText (xLabel,
                    (int) plotArea.getX(),
                    (int) plotArea.getBottom() + 8,
                    (int) plotArea.getWidth(),
                    20,
                    juce::Justification::centred);
    }

    // --- Tick labels ---
    g.setFont (juce::FontOptions (10.0f));

    if (logX)
    {
        // Decade ticks on a log axis (10/100/1k/10k ...), using original X values.
        const float xMinSafe = std::max (xMin, std::numeric_limits<float>::epsilon());
        const float logMin   = std::log10 (xMinSafe);
        const float logSpan  = std::max (std::log10 (std::max (xMax, xMinSafe)) - logMin,
                                         1.0e-6f);

        int firstDecade = (int) std::ceil (logMin);
        int lastDecade  = (int) std::floor (std::log10 (std::max (xMax, xMinSafe)));
        const int maxDecades = 10;
        int step = (lastDecade - firstDecade + 1 > maxDecades)
                       ? (lastDecade - firstDecade + 1 + maxDecades - 1) / maxDecades
                       : 1;

        for (int d = firstDecade; d <= lastDecade; d += step)
        {
            float t = ((float) d - logMin) / logSpan;
            float sx = plotArea.getX() + t * plotArea.getWidth();
            g.drawText (formatFrequency (std::pow (10.0f, (float) d)),
                        (int) sx - 25, (int) plotArea.getBottom() + 2, 50, 12,
                        juce::Justification::centred);
        }
    }
    else
    {
        int numTicks = 6;
        for (int i = 0; i <= numTicks; ++i)
        {
            float t = (float) i / numTicks;
            float xVal = xMin + t * (xMax - xMin);
            float sx = plotArea.getX() + t * plotArea.getWidth();
            g.drawText (juce::String (xVal, 1),
                        (int) sx - 25, (int) plotArea.getBottom() + 2, 50, 12,
                        juce::Justification::centred);
        }
    }

    // Y ticks (always uniform — only X is log-scaled).
    int numYTicks = 6;
    for (int i = 0; i <= numYTicks; ++i)
    {
        float t = (float) i / numYTicks;
        float yVal = yMin + t * (yMax - yMin);
        float sy = plotArea.getY() + t * plotArea.getHeight();
        g.drawText (juce::String (yVal, 1),
                    (int) plotArea.getX() - 52, (int) sy - 6, 48, 12,
                    juce::Justification::centredRight);
    }

    // --- Legend ---
    if (showLegend && ! seriesList.empty())
    {
        int legendY = 15;
        for (auto& s : seriesList)
        {
            g.setColour (s.colour);
            g.fillRect ((float) plotArea.getRight() - 100, (float) legendY, 10.0f, 10.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (s.name, (int) plotArea.getRight() - 85, legendY, 80, 12,
                        juce::Justification::centredLeft);
            legendY += 14;
        }
    }
}

void PlotWidget::resized()
{
    repaint();
}
