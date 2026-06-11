/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
DvisualizerAudioProcessor::DvisualizerAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
    // Construct the visualiser with 2 channels (stereo).
    // The number of sample blocks retained in the ring buffer is set here;
    // 512 gives a smooth, readable waveform at typical block sizes.
    visualiser(2)
{
    // Set how many consecutive sample-blocks the visualiser retains.
    // More blocks = longer history / slower scrolling waveform.
    visualiser.setNumChannels(2);
    visualiser.setRepaintRate(60);   // target 60 fps repaints on the message thread
    visualiser.setSamplesPerBlock(32);
}

DvisualizerAudioProcessor::~DvisualizerAudioProcessor()
{
}



//==============================================================================
void DvisualizerAudioProcessor::prepareToPlay(double /*sampleRate*/,
    int    /*samplesPerBlock*/)
{
    // Resize the visualiser to match the actual number of input channels
    // reported by the host (could be mono or stereo).
    const int numChannels = getTotalNumInputChannels();
    visualiser.setNumChannels(numChannels);

    // Clear any stale samples from a previous session.
    visualiser.clear();
}

void DvisualizerAudioProcessor::releaseResources()
{
    visualiser.clear();
}

void DvisualizerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    visualiser.clear();
}
//==============================================================================
void DvisualizerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // ── Silence any output buses that have no corresponding input ──────────
    // This prevents garbage/denormal noise on extra output channels.
    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();

    for (int ch = totalNumInputChannels; ch < totalNumOutputChannels; ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // ── Feed audio into the visualiser ────────────────────────────────────
    // AudioVisualiserComponent::pushBuffer is documented as safe to call
    // from the audio thread – it writes into an internal lock-free ring
    // buffer that the GUI thread reads asynchronously.
    visualiser.pushBuffer(buffer);

    // ── Pass audio through unmodified ─────────────────────────────────────
    // This is a pure visualiser: we do not alter the audio samples.
}

//==============================================================================
juce::AudioProcessorEditor* DvisualizerAudioProcessor::createEditor()
{
    return new DvisualizerAudioProcessorEditor(*this);
}
void DvisualizerAudioProcessor::getStateInformation(juce::MemoryBlock& /*destData*/)
{
    // No state to save for a pure visualiser.
}

void DvisualizerAudioProcessor::setStateInformation(const void* /*data*/, int /*sizeInBytes*/)
{
    // No state to restore.
}

//==============================================================================
// Plugin entry point required by the JUCE plugin framework.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DvisualizerAudioProcessor();
}
