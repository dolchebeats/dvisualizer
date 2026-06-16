#pragma once
#include "VisualizerBase.h"
#include "FFTProcessor.h"

//==============================================================================
/**
 * SpectrumAnalyzerComponent
 *
 * A self-contained, log-frequency spectrum analyzer that derives from
 * VisualizerBase (shared colour state + timer lifecycle + LCG) and delegates
 * all FFT work to an FFTProcessor member.
 *
 * Visual output
 * ─────────────
 * A smooth Catmull-Rom cubic spline is drawn through the log-frequency
 * magnitude bins with asymmetric attack/release smoothing applied by
 * FFTProcessor.  A faint dB grid and frequency-marker grid are overlaid.
 *
 * Signal path per frame (timerCallback)
 * ──────────────────────────────────────
 *   FFTProcessor::pullAndProcess()
 *     → FIFO read → Hann window → FFT → log-freq mapping → smoothing
 *   → repaint() → paint() → buildCatmullRomPath() → strokePath()
 *
 * Threading model
 * ───────────────
 * timerCallback() and paint() both run on the message thread (60 Hz via
 * VisualizerBase).  All cross-thread I/O is handled inside FFTProcessor.
 */
class SpectrumAnalyzerComponent final : public VisualizerBase
{
public:
    explicit SpectrumAnalyzerComponent(DvisualizerAudioProcessor& processor);
    ~SpectrumAnalyzerComponent() noexcept override = default;

    // VisualizerBase override — no extra work needed here
    void setColours(juce::Colour background, juce::Colour primary) override;

    void paint(juce::Graphics&) override;
    void resized()                override {}   // no off-screen canvas needed

private:
    //==========================================================================
    void timerCallback() override;

    // Builds a Catmull-Rom cubic spline through the magnitude data.
    // Declared static because it has no side effects on this object.
    static juce::Path buildCatmullRomPath(
        const std::array<float, FFTProcessor::maxDisplaySize>& data,
        int   displaySize,
        float w,
        float h,
        int   step = 8);

    //==========================================================================
    DvisualizerAudioProcessor& processor;

    // FFTProcessor is configured for 512 display bins, fast attack / slow
    // release — appropriate for a smooth line spectrum display.
    static constexpr int   scopeSize = 512;
    static constexpr float attackCoeff = 0.70f;
    static constexpr float releaseCoeff = 0.92f;

    FFTProcessor fftProc;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyzerComponent)
};