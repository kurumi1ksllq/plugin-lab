#pragma once

#include <JuceHeader.h>
#include <vector>

/**
 * A reusable 2D plot widget that renders XY data series
 * with axes, gridlines, and legends.
 */
class PlotWidget : public juce::Component
{
public:
    PlotWidget();
    ~PlotWidget() override = default;

    //==============================================================================
    /** A single data series. */
    struct Series
    {
        juce::String name;
        std::vector<float> x, y;  // data points
        juce::Colour colour;
        float lineWidth = 2.0f;
    };

    //==============================================================================
    /** Add a data series to the plot. */
    void addSeries (const Series& series);
    void addSeries (Series&& series);

    /** Clear all data. */
    void clear();

    /** Set axis labels. */
    void setAxisLabels (const juce::String& xLabel, const juce::String& yLabel);

    /** Set axis ranges (auto-fit if min >= max). */
    void setXRange (float min, float max);
    void setYRange (float min, float max);

    /** Set auto-fitting. */
    void setAutoFitX (bool autoFit)  { autoFitX = autoFit; }
    void setAutoFitY (bool autoFit)  { autoFitY = autoFit; }

    /** Set grid visibility. */
    void setShowGrid (bool show)     { showGrid = show; }
    void setShowLegend (bool show)   { showLegend = show; }

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::String xLabel, yLabel;
    float xMin = 0, xMax = 0;
    float yMin = 0, yMax = 0;
    bool autoFitX = true, autoFitY = true;
    bool showGrid = true, showLegend = true;

    std::vector<Series> seriesList;

    //==============================================================================
    void calcAutoFit();
    juce::Rectangle<float> getPlotArea() const;
    juce::Point<float> dataToScreen (float x, float y, juce::Rectangle<float> area) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlotWidget)
};
