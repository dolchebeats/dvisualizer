#pragma once
#include <JuceHeader.h>

//==============================================================================
/**
 * DvisualizerAudioProcessor
 *
 * Owns all audio-thread state:
 *   - APVTS with three display parameters (mode, bg color, primary color)
 *   - AudioVisualiserComponent for waveform mode
 *   - Lock-free FFT FIFO for spectrum mode
 *
 * The editor reads from these objects; no audio-thread logic lives in the editor.
 */
class DvisualizerAudioProcessor : public juce::AudioProcessor
{
public:
    //==========================================================================
    // FFT constants — public so SpectrumAnalyzerComponent can share them
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;  // 2048 samples

    //==========================================================================
    DvisualizerAudioProcessor();
    ~DvisualizerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==========================================================================
    // APVTS — owns all user-facing parameters; public for editor attachment
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==========================================================================
    // Waveform visualiser
    // Owned here so it survives editor open/close cycles.
    juce::AudioVisualiserComponent waveform;

    //==========================================================================
    // FFT sample FIFO
    //
    // Audio thread → writes via prepareToWrite / finishedWrite  (processBlock)
    // GUI thread   → reads  via prepareToRead  / finishedRead   (Timer callback)
    //
    // fftFifoBuffer is sized once in the constructor and NEVER resized again,
    // which guarantees zero heap allocation inside processBlock.
    static constexpr int fifoCapacity = fftSize * 4;
    juce::AbstractFifo   fftFifo{ fifoCapacity };
    std::vector<float>   fftFifoBuffer;   // size == fifoCapacity, fixed after ctor

    // Host sample rate — written in prepareToPlay, read by the spectrum GUI
    double currentSampleRate{ 44100.0 };

    //==========================================================================
    // Cached atomic parameter pointers
    // Set once in the constructor; safe to read from any thread afterwards.
    std::atomic<float>* vizModeParam{ nullptr };
    std::atomic<float>* bgColorParam{ nullptr };
    std::atomic<float>* primaryColorParam{ nullptr };

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DvisualizerAudioProcessor)
};