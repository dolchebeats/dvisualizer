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
    // Stop the timer before any members are destroyed.
    // Since the timer fires on the message thread and the destructor also
    // runs on the message thread, stopTimer() guarantees no further callbacks.
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

    // ── Spectrum path ───────────────────────────────────────────────────────
    juce::Path spectrumPath;
    spectrumPath.preallocateSpace(scopeSize + 4);

    // Anchor the filled shape at the bottom-left corner
    spectrumPath.startNewSubPath(0.0f, h);

    for (int i = 0; i < scopeSize; ++i)
    {
        const float x = juce::jmap(static_cast<float>(i),
            0.0f, static_cast<float>(scopeSize - 1),
            0.0f, w);
        // smoothedScopeData is 0 = silence (bottom) … 1 = full scale (top)
        const float y = juce::jmap(smoothedScopeData[i],
            0.0f, 1.0f,
            h, 0.0f);   // inverted: Y=0 is top of component
        spectrumPath.lineTo(x, y);
    }

    // Close path along the bottom edge to create a fillable shape
    spectrumPath.lineTo(w, h);
    spectrumPath.closeSubPath();

    // Vertical gradient: opaque primary at the top, transparent at the bottom
    juce::ColourGradient gradient(primaryColour.withAlpha(0.85f), 0.0f, 0.0f,
        primaryColour.withAlpha(0.08f), 0.0f, h,
        false /* not radial */);
    g.setGradientFill(gradient);
    g.fillPath(spectrumPath);

    // Bright outline trace along the top edge of the spectrum
    g.setColour(primaryColour.brighter(0.35f));
    g.strokePath(spectrumPath, juce::PathStrokeType(1.5f));
}