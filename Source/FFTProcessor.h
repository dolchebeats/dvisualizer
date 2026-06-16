#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/**
 * FFTProcessor
 *
 * A lightweight, non-Component helper that encapsulates the FFT pipeline
 * shared by every frequency-domain visualizer:
 *
 *   1. Read one fftSize block from the processor's AbstractFifo (SPSC).
 *   2. Apply a Hann window to reduce spectral leakage.
 *   3. Run the real-input FFT (frequency-only / magnitude output).
 *   4. Map the raw FFT bins onto a log-frequency display grid using a
 *      proper exponential mapping (equal octave spacing).
 *   5. Convert to normalised dB and optionally smooth with asymmetric
 *      attack/release coefficients.
 *
 * Usage
 * ─────
 *   // In your component header:
 *   FFTProcessor fftProc { processor };
 *
 *   // In timerCallback():
 *   if (fftProc.pullAndProcess())        // returns true when new data arrived
 *       repaint();
 *
 *   // In paint() / drawXxx():
 *   const auto& mag = fftProc.getMagnitudes();   // displaySize floats, [0..1]
 *
 * Threading model
 * ───────────────
 * Intended to be constructed, called, and destroyed exclusively on the
 * message thread.  The only cross-thread operation is the AbstractFifo
 * read (prepareToRead / finishedRead), which is explicitly designed for
 * single-producer / single-consumer lock-free access.
 *
 * Memory model
 * ────────────
 * All buffers are fixed-size std::array members — no heap allocation
 * after construction.
 */
class FFTProcessor
{
public:
    //==========================================================================
    // Display resolution — number of log-spaced magnitude bins produced.
    // 256 is sufficient for both AlchemyComponent and GalaxyComponent;
    // SpectrumAnalyzerComponent uses 512.  You can template or construct
    // with a runtime value, but a compile-time constant keeps allocations
    // trivially known at build time.
    //
    // To accommodate both consumers we expose the largest size and let
    // each caller query only as many bins as it needs via getMagnitudes().

    static constexpr int maxDisplaySize = 512;

    //==========================================================================
    /**
     * @param processor    The owning audio processor — provides the FFT FIFO
     *                     and current sample rate.
     * @param displaySize  How many log-frequency bins to produce.
     *                     Must be <= maxDisplaySize.
     * @param attackCoeff  Smoothing coefficient for rising edges  [0..1].
     *                     1.0 = no smoothing, 0.0 = freeze.
     * @param releaseCoeff Smoothing coefficient for falling edges [0..1].
     */
    explicit FFTProcessor(DvisualizerAudioProcessor& processor,
        int   displaySize = maxDisplaySize,
        float attackCoeff = 0.70f,
        float releaseCoeff = 0.90f);

    //==========================================================================
    /**
     * Try to pull one FFT frame from the processor FIFO.
     *
     * Returns true if new data was available and processed (caller should
     * repaint), or false if the FIFO did not yet hold a full fftSize block.
     *
     * Call once per timerCallback() tick.
     */
    bool pullAndProcess();

    //==========================================================================
    /** Read-only view of the current log-frequency magnitude array.
     *  Values are normalised [0..1] (0 = mindB, 1 = maxdB).
     *  The array has maxDisplaySize entries; only the first displaySize
     *  entries are meaningful.  Use getDisplaySize() for the valid range. */
    const std::array<float, maxDisplaySize>& getMagnitudes() const noexcept
    {
        return magnitudes;
    }

    /** Number of valid entries in getMagnitudes(). */
    int getDisplaySize() const noexcept { return displaySize; }

    //==========================================================================
    // dB range constants — public so visualizers can use them for grid lines
    static constexpr float mindB = -100.0f;
    static constexpr float maxdB = 0.0f;

private:
    //==========================================================================
    DvisualizerAudioProcessor& processor;

    const int   displaySize;
    const float attackCoeff;
    const float releaseCoeff;

    // FFT engine — owns the twiddle factors, allocated once at construction.
    juce::dsp::FFT                      forwardFFT;
    juce::dsp::WindowingFunction<float> window;

    // Working buffer: real input in [0 .. fftSize-1],
    //                 imaginary (zeroed) in [fftSize .. 2*fftSize-1].
    std::array<float, DvisualizerAudioProcessor::fftSize * 2> fftData{};

    // Output — log-frequency magnitudes, smoothed.
    std::array<float, maxDisplaySize> magnitudes{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FFTProcessor)
};