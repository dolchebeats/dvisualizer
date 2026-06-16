#include "FFTProcessor.h"

//==============================================================================
FFTProcessor::FFTProcessor(DvisualizerAudioProcessor& proc,
    int   displaySz,
    float attack,
    float release)
    : processor(proc),
    displaySize(juce::jlimit(1, maxDisplaySize, displaySz)),
    attackCoeff(attack),
    releaseCoeff(release),
    forwardFFT(DvisualizerAudioProcessor::fftOrder),
    window(DvisualizerAudioProcessor::fftSize,
        juce::dsp::WindowingFunction<float>::hann)
{
    fftData.fill(0.0f);
    magnitudes.fill(0.0f);
}

//==============================================================================
bool FFTProcessor::pullAndProcess()
{
    // ── 1. Check availability ───────────────────────────────────────────────
    // The FIFO is a single-producer / single-consumer ring buffer.
    // getNumReady() is safe to call from the consumer (message) thread.
    if (processor.fftFifo.getNumReady() < DvisualizerAudioProcessor::fftSize)
        return false;

    // ── 2. Read one block from the FIFO ────────────────────────────────────
    // AbstractFifo may return two non-contiguous spans (wrap-around case),
    // so we handle both with FloatVectorOperations::copy for efficiency.
    {
        int start1, size1, start2, size2;
        processor.fftFifo.prepareToRead(
            DvisualizerAudioProcessor::fftSize, start1, size1, start2, size2);

        if (size1 > 0)
            juce::FloatVectorOperations::copy(
                fftData.data(),
                processor.fftFifoBuffer.data() + start1,
                size1);

        if (size2 > 0)
            juce::FloatVectorOperations::copy(
                fftData.data() + size1,
                processor.fftFifoBuffer.data() + start2,
                size2);

        processor.fftFifo.finishedRead(size1 + size2);
    }

    // ── 3. Zero the imaginary upper half ────────────────────────────────────
    // performFrequencyOnlyForwardTransform requires the buffer to be 2×fftSize;
    // the upper half is the imaginary input and must be zeroed for a real signal.
    std::fill(fftData.begin() + DvisualizerAudioProcessor::fftSize,
        fftData.end(), 0.0f);

    // ── 4. Window + FFT ─────────────────────────────────────────────────────
    // The Hann window suppresses spectral leakage at bin boundaries.
    // After the call fftData[0 .. fftSize/2] contains bin magnitudes.
    window.multiplyWithWindowingTable(fftData.data(),
        DvisualizerAudioProcessor::fftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftData.data());

    // ── 5. Log-frequency mapping ────────────────────────────────────────────
    // Map displaySize display columns onto FFT bins using a true exponential
    // (constant-octave) spacing so that each octave occupies equal screen width.
    // This matches how human hearing perceives pitch.
    const float sr = static_cast<float> (processor.currentSampleRate);
    const float freqPerBin = sr / static_cast<float> (DvisualizerAudioProcessor::fftSize);
    const int   nyquist = DvisualizerAudioProcessor::fftSize / 2;
    const float minFreq = 20.0f;
    const float maxFreq = juce::jmax(minFreq + 1.0f, sr * 0.5f);

    // Pre-compute the log range once per frame (avoids repeated log calls).
    const float logRange = std::log(maxFreq / minFreq);

    for (int i = 0; i < displaySize; ++i)
    {
        // Proportion 0..1 maps to 20 Hz..Nyquist on a logarithmic scale.
        const float p = static_cast<float>(i) / static_cast<float>(displaySize - 1);
        const float freq = minFreq * std::exp(p * logRange);
        const int   bin = juce::jlimit(1, nyquist - 1,
            static_cast<int>(freq / freqPerBin));

        // Convert magnitude → dB, subtract the FFT-size normalisation gain,
        // clamp to our display range, then map to [0, 1].
        const float dBVal = juce::Decibels::gainToDecibels(fftData[bin])
            - juce::Decibels::gainToDecibels(
                static_cast<float> (DvisualizerAudioProcessor::fftSize));

        const float newVal = juce::jmap(juce::jlimit(mindB, maxdB, dBVal),
            mindB, maxdB, 0.0f, 1.0f);

        // ── 6. Asymmetric smoothing (fast attack, slow release) ─────────────
        // Values closer to 1.0 = slower response (more temporal averaging).
        // Different coefficients for rising vs falling edges so peaks snap up
        // quickly and then glide back down slowly — a classic metering look.
        const float coeff = (newVal > magnitudes[i]) ? attackCoeff : releaseCoeff;
        magnitudes[i] = coeff * magnitudes[i] + (1.0f - coeff) * newVal;
    }

    return true;
}