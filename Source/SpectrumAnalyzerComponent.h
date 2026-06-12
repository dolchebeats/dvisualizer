#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"   // full include needed to access DvisualizerAudioProcessor::fftSize
// (a forward declaration cannot see static constexpr members)

//==============================================================================
/**
 * SpectrumAnalyzerComponent
 *
 * A self-contained, log-frequency spectrum analyzer.
 *
 * Threading model
 * ───────────────
 * - timerCallback()  runs on the message thread at 60 Hz
 * - paint()          runs on the message thread (triggered by repaint())
 * - Since both are on the same thread they are never concurrent — no locking
 *   is needed for fftData / scopeData / smoothedScopeData.
 * - The only cross-thread operation is reading from the processor's
 *   AbstractFifo, which is explicitly designed for single-producer /
 *   single-consumer lock-free access.
 *
 * Signal path per frame
 * ─────────────────────
 *   FIFO read → Hann window → FFT → dB conversion → log-freq mapping
 *   → exponential smoothing → paint()
 */
class SpectrumAnalyzerComponent : public juce::Component,
    private juce::Timer
{
public:
    explicit SpectrumAnalyzerComponent(DvisualizerAudioProcessor& processor);
    ~SpectrumAnalyzerComponent() noexcept override;

    /** Set display colours from the message thread. */
    void setColours(juce::Colour background, juce::Colour primary);

    void paint(juce::Graphics&) override;
    void resized()                override {}

private:
    void timerCallback()           override;
    void drawNextFrameOfSpectrum();

    //==========================================================================
    DvisualizerAudioProcessor& processor;

    // FFT engine and windowing — GUI thread only
    juce::dsp::FFT                      forwardFFT;
    juce::dsp::WindowingFunction<float> window;

    // fftData must be 2 × fftSize for performFrequencyOnlyForwardTransform:
    //   [0 .. fftSize-1]   = real input samples (then overwritten with magnitudes)
    //   [fftSize .. 2N-1]  = imaginary part (zeroed before each transform)
    std::array<float, DvisualizerAudioProcessor::fftSize * 2> fftData{};

    // Display resolution: each entry maps to one horizontal display column
    static constexpr int   scopeSize = 512;
    static constexpr float mindB = -100.0f;
    static constexpr float maxdB = 0.0f;

    std::array<float, scopeSize> scopeData{};  // raw, current frame
    std::array<float, scopeSize> smoothedScopeData{};  // exponentially smoothed

    juce::Colour bgColour{ 0xff0d0d1a };
    juce::Colour primaryColour{ 0xff00d4ff };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyzerComponent)
};