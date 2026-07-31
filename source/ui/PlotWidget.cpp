#include "PlotWidget.h"
#include <cmath>

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

    // Add 10% padding
    if (autoFitX && xMax > xMin)
    {
        float pad = (xMax - xMin) * 0.1f;
        xMin -= pad;
        xMax += pad;
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
    float sx = area.getX() + (x - xMin) / (xMax - xMin) * area.getWidth();
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
    int numTicks = 6;
    for (int i = 0; i <= numTicks; ++i)
    {
        float t = (float) i / numTicks;
        float xVal = xMin + t * (xMax - xMin);
        float yVal = yMin + t * (yMax - yMin);

        juce::String xStr = juce::String (xVal, 1);
        juce::String yStr = juce::String (yVal, 1);

        float sx = plotArea.getX() + t * plotArea.getWidth();
        float sy = plotArea.getY() + t * plotArea.getHeight();

        g.drawText (xStr, (int) sx - 25, (int) plotArea.getBottom() + 2, 50, 12,
                    juce::Justification::centred);
        g.drawText (yStr, (int) plotArea.getX() - 52, (int) sy - 6, 48, 12,
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
