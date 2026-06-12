#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Parameter layout — called once during construction.
// Adding new modes later means appending to the "vizMode" StringArray only.
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
DvisualizerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Visualization mode — extend this list as new modes are added
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "vizMode", 1 },
        "Visualization Mode",
        juce::StringArray{ "Waveform", "Spectrum" },
        0));  // default: Waveform

    // Background color palette
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "bgColor", 1 },
        "Background Color",
        juce::StringArray{ "Dark Navy", "Pitch Black", "Deep Forest", "Dark Crimson", "Slate" },
        0));  // default: Dark Navy

    // Primary / accent color palette
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "primaryColor", 1 },
        "Primary Color",
        juce::StringArray{ "Cyan", "Lime", "Orange", "Pink", "White", "Gold" },
        0));  // default: Cyan

    return layout;
}

//==============================================================================
DvisualizerAudioProcessor::DvisualizerAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
    visualiser(2),
    apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    // Waveform visualiser defaults
    visualiser.setNumChannels(2);
    visualiser.setRepaintRate(60);
    visualiser.setSamplesPerBlock(32);

    // Pre-allocate the FFT FIFO buffer here in the constructor.
    // It is NEVER resized after this point, so processBlock stays allocation-free.
    fftFifoBuffer.resize(static_cast<size_t> (fifoCapacity), 0.0f);

    // Cache atomic parameter pointers for lock-free reads in processBlock
    vizModeParam = apvts.getRawParameterValue("vizMode");
    bgColorParam = apvts.getRawParameterValue("bgColor");
    primaryColorParam = apvts.getRawParameterValue("primaryColor");
}

DvisualizerAudioProcessor::~DvisualizerAudioProcessor() {}

//==============================================================================
void DvisualizerAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;

    visualiser.setNumChannels(getTotalNumInputChannels());
    visualiser.clear();

    // Reset FIFO read/write indices — no reallocation, just zero the counters.
    fftFifo.reset();
    std::fill(fftFifoBuffer.begin(), fftFifoBuffer.end(), 0.0f);
}

void DvisualizerAudioProcessor::releaseResources()
{
    visualiser.clear();
}

//==============================================================================
void DvisualizerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // Silence extra output channels that have no matching input
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // ── Waveform FIFO (always feed, regardless of current mode) ───────────
    // Feeding both FIFOs at all times means mode switching is instant with
    // no warm-up period on either visualizer.
    visualiser.pushBuffer(buffer);

    // ── FFT FIFO (mono-mix from channel 0) ────────────────────────────────
    // AbstractFifo::prepareToWrite / finishedWrite is the JUCE-documented
    // lock-free pattern for single-producer / single-consumer use.
    // No allocation, no mutex, no logging — fully real-time safe.
    const int    numSamples = buffer.getNumSamples();
    const float* channelData = buffer.getReadPointer(0);

    int start1, size1, start2, size2;
    fftFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

    if (size1 > 0)
        juce::FloatVectorOperations::copy(fftFifoBuffer.data() + start1,
            channelData, size1);
    if (size2 > 0)
        juce::FloatVectorOperations::copy(fftFifoBuffer.data() + start2,
            channelData + size1, size2);

    fftFifo.finishedWrite(size1 + size2);
}

//==============================================================================
juce::AudioProcessorEditor* DvisualizerAudioProcessor::createEditor()
{
    return new DvisualizerAudioProcessorEditor(*this);
}

//==============================================================================
void DvisualizerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Serialise the full APVTS state (mode, bg color, primary color) to XML
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void DvisualizerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DvisualizerAudioProcessor();
}