#pragma once
#include <JuceHeader.h>

//==============================================================================
/**
 * DvisualizerAudioProcessor
 *
 * Owns all audio-thread state:
 *   - APVTS with display parameters (mode, bg color, primary color)
 *   - AudioVisualiserComponent for waveform mode
 *   - Lock-free FFT FIFO for spectrum mode
 *   - Band energy followers + beat detector for Alchemy mode (Steps 1–2)
 *
 * Threading model
 * ───────────────
 * processBlock  — audio thread  → writes FIFOs and atomics
 * GUI timer     — message thread → reads FIFOs and atomics
 * No mutex is ever held across the thread boundary; all shared state uses
 * AbstractFifo (SPSC lock-free) or std::atomic<float>.
 */
class DvisualizerAudioProcessor : public juce::AudioProcessor
{
public:
    //==========================================================================
    // FFT constants — public so SpectrumAnalyzerComponent can share them
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;   // 2048 samples

    //==========================================================================
    DvisualizerAudioProcessor();
    ~DvisualizerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources()                                     override;
    void processBlock(juce::AudioBuffer<float>&,
        juce::MidiBuffer&)                      override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor()  const override { return true; }

    const juce::String getName()        const override { return JucePlugin_Name; }
    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const  override { return 0.0; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData)              override;
    void setStateInformation(const void* data, int sizeInBytes)        override;

    //==========================================================================
    // APVTS
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout
        createParameterLayout();

    //==========================================================================
    // Waveform visualiser — owned here, survives editor open/close
    juce::AudioVisualiserComponent waveform;

    //==========================================================================
    // FFT sample FIFO
    // Audio thread writes, GUI thread reads — SPSC lock-free via AbstractFifo.
    // fftFifoBuffer is sized once in the constructor and never resized.
    static constexpr int fifoCapacity = fftSize * 4;
    juce::AbstractFifo   fftFifo{ fifoCapacity };
    std::vector<float>   fftFifoBuffer;   // fixed size after ctor

    // Host sample rate — written in prepareToPlay, read by the GUI
    double currentSampleRate{ 44100.0 };

    //==========================================================================
    // ── Step 1: Band energy + beat detection ─────────────────────────────
    //
    // Six frequency bands exposed as lock-free atomics.
    // Audio thread writes via relaxed store; GUI timer reads via relaxed load.
    // The index ordering matches bandNames[] below.
    //
    //  0 — Sub-bass   20–60 Hz
    //  1 — Bass       60–250 Hz
    //  2 — Low-mid    250–800 Hz
    //  3 — High-mid   800–3 kHz
    //  4 — Presence   3–8 kHz
    //  5 — Air        8–20 kHz
    static constexpr int numBands = 6;

    // Per-band smoothed energy [0..1] — read by AlchemyComponent
    std::atomic<float> bandEnergy[numBands];

    // Beat/onset pulse: written 1.0f by audio thread on a transient,
    // then decays toward 0 on the GUI thread.  GUI reads this once per
    // timer tick and resets it to 0 after reading.
    std::atomic<float> beatPulse{ 0.0f };

    //==========================================================================
    // Cached atomic parameter pointers (set once in ctor, read anywhere)
    std::atomic<float>* vizModeParam{ nullptr };
    std::atomic<float>* bgColorParam{ nullptr };
    std::atomic<float>* primaryColorParam{ nullptr };

private:
    //==========================================================================
    // ── Band energy DSP state (audio thread only) ─────────────────────────
    // IIR bandpass filters — one per band, prepared in prepareToPlay.
    // Using first-order shelving / peak pairs implemented via
    // juce::dsp::IIR::Filter so all coefficient memory is pre-allocated.

    // We use cascaded second-order sections (biquads) for each band.
    // Each band has a high-pass and a low-pass boundary that together act
    // as a bandpass; we just measure RMS within the band.
    // For simplicity: one lowpass per band boundary, applied serially.
    // Two filters suffice per band: HPF at lower edge, LPF at upper edge.
    // But for CPU efficiency we use a single-pass approach:
    //   measure the total signal, then subtract adjacent bands.
    // Actually cleanest approach: cascade HP+LP biquad pairs per band.

    struct BandFilter
    {
        juce::dsp::IIR::Filter<float> hp;   // high-pass at band low edge
        juce::dsp::IIR::Filter<float> lp;   // low-pass  at band high edge
        float envelope{ 0.0f };            // smoothed RMS per band
    };

    BandFilter bandFilters[numBands];

    // Band edge frequencies in Hz — paired low/high per band
    static constexpr float bandEdges[numBands][2] =
    {
        {  20.0f,   60.0f },   // Sub-bass
        {  60.0f,  250.0f },   // Bass
        { 250.0f,  800.0f },   // Low-mid
        { 800.0f, 3000.0f },   // High-mid
        {3000.0f, 8000.0f },   // Presence
        {8000.0f,20000.0f },   // Air
    };

    // Envelope follower coefficients — computed in prepareToPlay
    float bandAttackCoeff{ 0.0f };
    float bandReleaseCoeff{ 0.0f };

    // Beat detector state
    float beatEnvelope{ 0.0f };   // smoothed RMS of full signal
    float beatThreshold{ 0.0f };   // adaptive threshold
    static constexpr float beatAttack = 0.90f;
    static constexpr float beatRelease = 0.9995f;
    static constexpr float beatThresholdDecay = 0.9998f;

    //==========================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DvisualizerAudioProcessor)
};