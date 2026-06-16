#include "SpectrumAnalyzerComponent.h"
#include "PluginProcessor.h"

//==============================================================================
SpectrumAnalyzerComponent::SpectrumAnalyzerComponent(DvisualizerAudioProcessor& p)
    : VisualizerBase(60),          // 60 Hz timer, default rng seed
    processor(p),
    fftProc(p, scopeSize, attackCoeff, releaseCoeff)
{
}

//==============================================================================
void SpectrumAnalyzerComponent::setColours(juce::Colour background,
    juce::Colour primary)
{
    // Base handles storing bgColour / primaryColour + repaint().
    VisualizerBase::setColours(background, primary);
}

//==============================================================================
void SpectrumAnalyzerComponent::timerCallback()
{
    // Pull one FFT frame; repaint only when new data is available.
    if (fftProc.pullAndProcess())
        repaint();
}

//==============================================================================
/**
 * Builds a smooth Catmull-Rom cubic spline through the magnitude data.
 *
 * Catmull-Rom overview
 * ────────────────────
 * For each interior knot P[i] the tangent is defined as:
 *     T[i] = τ * (P[i+1] - P[i-1])
 * where τ = 0.5 is the classic value (lower → tighter curves).
 *
 * We convert the Catmull-Rom representation to the cubic Bézier form that
 * JUCE's Path::cubicTo() expects:
 *     cp1 = P[i]   + T[i]   / 3
 *     cp2 = P[i+1] - T[i+1] / 3
 *
 * We sample every `step` columns to reduce the number of Bézier segments
 * while remaining well below screen pixel resolution.
 */
juce::Path SpectrumAnalyzerComponent::buildCatmullRomPath(
    const std::array<float, FFTProcessor::maxDisplaySize>& data,
    int   displaySz,
    float w,
    float h,
    int   step)
{
    static constexpr float tau = 0.5f;

    // Pixel-space accessors
    auto xOf = [&](int i) -> float
        {
            return juce::jmap(static_cast<float> (i),
                0.0f, static_cast<float> (displaySz - 1),
                0.0f, w);
        };
    auto yOf = [&](int i) -> float
        {
            return juce::jmap(data[juce::jlimit(0, displaySz - 1, i)],
                0.0f, 1.0f,
                h, 0.0f);   // flip: 0 dB at top
        };

    juce::Path path;
    path.startNewSubPath(xOf(0), yOf(0));

    for (int k = step; k < displaySz - 1; k += step)
    {
        const int i0 = k - step;
        const int i1 = k;
        const int i2 = juce::jmin(k + step, displaySz - 1);
        const int i3 = juce::jmin(k + step + step, displaySz - 1);

        const float x0 = xOf(i0), y0 = yOf(i0);
        const float x1 = xOf(i1), y1 = yOf(i1);
        const float x2 = xOf(i2), y2 = yOf(i2);
        const float x3 = xOf(i3), y3 = yOf(i3);

        // Tangents at P1 and P2
        const float tx1 = tau * (x2 - x0), ty1 = tau * (y2 - y0);
        const float tx2 = tau * (x3 - x1), ty2 = tau * (y3 - y1);

        // Bézier control points
        const float cp1x = x1 + tx1 / 3.0f, cp1y = y1 + ty1 / 3.0f;
        const float cp2x = x2 - tx2 / 3.0f, cp2y = y2 - ty2 / 3.0f;

        path.cubicTo(cp1x, cp1y, cp2x, cp2y, x2, y2);
    }

    return path;
}

//==============================================================================
void SpectrumAnalyzerComponent::paint(juce::Graphics& g)
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());

    // ── Background ───────────────────────────────────────────────────────────
    g.fillAll(bgColour);

    const juce::Colour gridColour = primaryColour.withAlpha(0.15f);

    // ── Horizontal dB grid lines ─────────────────────────────────────────────
    g.setColour(gridColour);
    for (float dB : { -20.0f, -40.0f, -60.0f, -80.0f })
    {
        const float y = juce::jmap(dB, FFTProcessor::mindB, FFTProcessor::maxdB, h, 0.0f);
        g.drawHorizontalLine(static_cast<int> (y), 0.0f, w);
    }

    // ── Vertical frequency grid lines ────────────────────────────────────────
    {
        const float sr = static_cast<float> (processor.currentSampleRate);
        const float minFreq = 20.0f;
        const float maxFreq = juce::jmax(minFreq + 1.0f, sr * 0.5f);

        g.setColour(gridColour);
        for (float markerHz : { 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f })
        {
            if (markerHz >= maxFreq) continue;
            const float proportion = std::log(markerHz / minFreq)
                / std::log(maxFreq / minFreq);
            g.drawVerticalLine(static_cast<int> (proportion * w), 0.0f, h);
        }
    }

    // ── Smooth spectrum line ──────────────────────────────────────────────────
    const juce::Path linePath = buildCatmullRomPath(
        fftProc.getMagnitudes(), fftProc.getDisplaySize(), w, h, 8);

    g.setColour(primaryColour.brighter(0.35f));
    g.strokePath(linePath,
        juce::PathStrokeType(1.5f,
            juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded));
}