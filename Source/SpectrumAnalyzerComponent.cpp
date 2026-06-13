#include "SpectrumAnalyzerComponent.h"
#include "PluginProcessor.h"

//==============================================================================
SpectrumAnalyzerComponent::SpectrumAnalyzerComponent(DvisualizerAudioProcessor& p)
    : processor(p),
    forwardFFT(DvisualizerAudioProcessor::fftOrder),
    window(DvisualizerAudioProcessor::fftSize,
        juce::dsp::WindowingFunction<float>::hann)
{
    fftData.fill(0.0f);
    scopeData.fill(0.0f);
    smoothedScopeData.fill(0.0f);

    startTimerHz(60);
}

SpectrumAnalyzerComponent::~SpectrumAnalyzerComponent()
{
    stopTimer();
}

void SpectrumAnalyzerComponent::setColours(juce::Colour background,
    juce::Colour primary)
{
    bgColour = background;
    primaryColour = primary;
    repaint();
}

//==============================================================================
void SpectrumAnalyzerComponent::timerCallback()
{
    // Only process when a full FFT window's worth of samples is available.
    // getFreeSpace is safe to call from the consumer (read) thread.
    if (processor.fftFifo.getNumReady() < DvisualizerAudioProcessor::fftSize)
        return;

    // -- Read one fftSize block from the lock-free FIFO -----------------------
    // AbstractFifo::prepareToRead / finishedRead is the documented consumer API.
    int start1, size1, start2, size2;
    processor.fftFifo.prepareToRead(
        DvisualizerAudioProcessor::fftSize, start1, size1, start2, size2);

    if (size1 > 0)
        juce::FloatVectorOperations::copy(fftData.data(),
            processor.fftFifoBuffer.data() + start1,
            size1);
    if (size2 > 0)
        juce::FloatVectorOperations::copy(fftData.data() + size1,
            processor.fftFifoBuffer.data() + start2,
            size2);

    processor.fftFifo.finishedRead(size1 + size2);

    // Zero the imaginary / upper half of the working buffer before the FFT
    std::fill(fftData.begin() + DvisualizerAudioProcessor::fftSize,
        fftData.end(), 0.0f);

    drawNextFrameOfSpectrum();
    repaint();
}

//==============================================================================
void SpectrumAnalyzerComponent::drawNextFrameOfSpectrum()
{
    // ── 1. Hann window ────────────────────────────────────────────────────
    // Reduces spectral leakage at bin boundaries. The window is applied to
    // the first fftSize elements of fftData (the real input samples).
    window.multiplyWithWindowingTable(fftData.data(),
        DvisualizerAudioProcessor::fftSize);

    // ── 2. FFT ────────────────────────────────────────────────────────────
    // performFrequencyOnlyForwardTransform overwrites fftData[0..fftSize/2]
    // with the magnitude of each positive-frequency bin.
    forwardFFT.performFrequencyOnlyForwardTransform(fftData.data());

    // ── 3. Log-frequency bin mapping ──────────────────────────────────────
    // Map scopeSize display columns to FFT bins using a true exponential
    // (base-frequency) scale so that each octave occupies equal screen space.
    // This matches how human hearing perceives pitch.
    const float sampleRate = static_cast<float> (processor.currentSampleRate);
    const float freqPerBin = sampleRate / static_cast<float> (DvisualizerAudioProcessor::fftSize);
    const int   nyquist = DvisualizerAudioProcessor::fftSize / 2;

    const float minFreq = 20.0f;
    const float maxFreq = juce::jmax(minFreq + 1.0f, sampleRate * 0.5f);

    for (int i = 0; i < scopeSize; ++i)
    {
        // Proportion 0..1 across the display maps to 20 Hz..Nyquist on a log scale
        const float proportion = static_cast<float>(i) / static_cast<float>(scopeSize - 1);
        const float freq = minFreq * std::pow(maxFreq / minFreq, proportion);
        const int   binIndex = juce::jlimit(1, nyquist - 1,
            static_cast<int>(freq / freqPerBin));

        // Convert raw FFT magnitude → dB, subtract FFT-size gain, clamp, normalise
        const float dBValue = juce::Decibels::gainToDecibels(fftData[binIndex])
            - juce::Decibels::gainToDecibels(
                static_cast<float> (DvisualizerAudioProcessor::fftSize));

        scopeData[i] = juce::jmap(juce::jlimit(mindB, maxdB, dBValue),
            mindB, maxdB, 0.0f, 1.0f);
    }

    // ── 4. Exponential smoothing ───────────────────────────────────────────
    // Asymmetric coefficients create a professional "fast attack, slow decay"
    // look where peaks snap up quickly then glide back down.
    // Values closer to 1.0 = slower response (more smoothing).
    static constexpr float attackCoeff = 0.70f;   // rising edge
    static constexpr float releaseCoeff = 0.92f;   // falling edge (peaks linger)

    for (int i = 0; i < scopeSize; ++i)
    {
        const float coeff = (scopeData[i] > smoothedScopeData[i]) ? attackCoeff
            : releaseCoeff;
        smoothedScopeData[i] = coeff * smoothedScopeData[i]
            + (1.0f - coeff) * scopeData[i];
    }
}

//==============================================================================
/**
 * Builds a smooth cubic-spline path through the scope points.
 *
 * Strategy: Catmull-Rom cubic segments.
 * For each interior point P[i], the control points of the cubic Bézier that
 * connects P[i-1] → P[i] → P[i+1] → P[i+2] are derived from the neighbouring
 * positions.  The tangent at each knot is the chord between its two neighbours
 * scaled by a tension factor τ (0 = flat, 1 = full Catmull-Rom).
 *
 * Only a cubic bezier path call is needed here since JUCE's Path::cubicTo()
 * takes two control points and an end point, mapping perfectly to Catmull-Rom.
 *
 * We sample the smoothedScopeData at a reduced stride (every `step` columns)
 * to lower the number of Bézier segments while still capturing the true shape.
 * At 512 scope points × 640 px, every 4th point is already sub-pixel accurate.
 */
static juce::Path buildCatmullRomPath(
    const std::array<float, 512>& data,
    int                            scopeSize,
    float                          w,
    float                          h,
    int                            step = 4)        // downsample stride
{
    // ── Tension ───────────────────────────────────────────────────────────
    // 0.5 is the classic Catmull-Rom value; lower values give a tighter curve.
    static constexpr float tau = 0.5f;

    // Helper: map scope index → pixel (x, y)
    auto xOf = [&](int i) -> float
        {
            return juce::jmap(static_cast<float>(i),
                0.0f, static_cast<float>(scopeSize - 1), 0.0f, w);
        };
    auto yOf = [&](int i) -> float
        {
            return juce::jmap(data[juce::jlimit(0, scopeSize - 1, i)],
                0.0f, 1.0f, h, 0.0f);
        };

    juce::Path path;
    path.startNewSubPath(xOf(0), yOf(0));

    // Walk through knots at the chosen stride.
    // Each iteration emits one cubic segment from knot k1 → k2,
    // using k0 (before) and k3 (after) to compute tangents.
    for (int k = step; k < scopeSize - 1; k += step)
    {
        const int i0 = k - step;          // previous knot
        const int i1 = k;                 // current knot  (segment start)
        const int i2 = juce::jmin(k + step, scopeSize - 1); // next knot (segment end)
        const int i3 = juce::jmin(k + step + step, scopeSize - 1); // knot after next

        // Pixel coordinates of the four surrounding knots
        const float x0 = xOf(i0), y0 = yOf(i0);
        const float x1 = xOf(i1), y1 = yOf(i1);
        const float x2 = xOf(i2), y2 = yOf(i2);
        const float x3 = xOf(i3), y3 = yOf(i3);

        // Catmull-Rom tangents at P1 and P2
        // Tangent at P[i] = τ * (P[i+1] - P[i-1])
        const float tx1 = tau * (x2 - x0);   const float ty1 = tau * (y2 - y0);
        const float tx2 = tau * (x3 - x1);   const float ty2 = tau * (y3 - y1);

        // Convert to Bézier control points
        // cp1 = P1 + tangent1/3,  cp2 = P2 - tangent2/3
        const float cp1x = x1 + tx1 / 3.0f;  const float cp1y = y1 + ty1 / 3.0f;
        const float cp2x = x2 - tx2 / 3.0f;  const float cp2y = y2 - ty2 / 3.0f;

        path.cubicTo(cp1x, cp1y, cp2x, cp2y, x2, y2);
    }

    return path;
}

//==============================================================================
void SpectrumAnalyzerComponent::paint(juce::Graphics& g)
{
    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());

    // ── Background ─────────────────────────────────────────────────────────
    g.fillAll(bgColour);

    const juce::Colour gridColour = primaryColour.withAlpha(0.15f);

    // ── Horizontal dB grid lines ───────────────────────────────────────────
    g.setColour(gridColour);
    for (float dB : { -20.0f, -40.0f, -60.0f, -80.0f })
    {
        const float y = juce::jmap(dB, mindB, maxdB, h, 0.0f);
        g.drawHorizontalLine(static_cast<int> (y), 0.0f, w);
    }

    // ── Vertical frequency grid lines ──────────────────────────────────────
    // Use the same log mapping as drawNextFrameOfSpectrum for exact alignment.
    {
        const float sampleRate = static_cast<float> (processor.currentSampleRate);
        const float minFreq = 20.0f;
        const float maxFreq = juce::jmax(minFreq + 1.0f, sampleRate * 0.5f);

        g.setColour(gridColour);
        for (float markerHz : { 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f })
        {
            if (markerHz >= maxFreq) continue;
            const float proportion = std::log(markerHz / minFreq)
                / std::log(maxFreq / minFreq);
            g.drawVerticalLine(static_cast<int> (proportion * w), 0.0f, h);
        }
    }

    // ── Smooth spectrum line ────────────────────────────────────────────────
    // Build the Catmull-Rom spline through the smoothed scope data.
    // stride=4 → ~128 cubic segments across 640 px, well below pixel resolution.
    juce::Path linePath = buildCatmullRomPath(smoothedScopeData, scopeSize, w, h, 8);

    // Draw only the stroke — no fill, so the background colour shows through.
    g.setColour(primaryColour.brighter(0.35f));
    g.strokePath(linePath, juce::PathStrokeType(
        1.5f,
        juce::PathStrokeType::curved,      // smooth join style
        juce::PathStrokeType::rounded));   // rounded end caps
}