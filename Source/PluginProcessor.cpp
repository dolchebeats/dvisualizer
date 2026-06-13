#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Parameter layout
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
DvisualizerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Visualization mode — "Alchemy" added as index 2
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "vizMode", 1 },
        "Visualization Mode",
        juce::StringArray{ "Waveform", "Spectrum", "Alchemy" },
        0));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "bgColor", 1 },
        "Background Color",
        juce::StringArray{ "Dark Navy", "Pitch Black", "Deep Forest",
                           "Dark Crimson", "Slate" },
        0));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "primaryColor", 1 },
        "Primary Color",
        juce::StringArray{ "Cyan", "Lime", "Orange", "Pink", "White", "Gold" },
        0));

    return layout;
}

//==============================================================================
DvisualizerAudioProcessor::DvisualizerAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
    waveform(2),
    apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    waveform.setNumChannels(2);
    waveform.setRepaintRate(60);
    waveform.setSamplesPerBlock(32);

    // Pre-allocate FFT FIFO — never resized after this point
    fftFifoBuffer.resize(static_cast<size_t>(fifoCapacity), 0.0f);

    // Initialise band energy atomics
    for (int i = 0; i < numBands; ++i)
        bandEnergy[i].store(0.0f, std::memory_order_relaxed);

    vizModeParam = apvts.getRawParameterValue("vizMode");
    bgColorParam = apvts.getRawParameterValue("bgColor");
    primaryColorParam = apvts.getRawParameterValue("primaryColor");
}

DvisualizerAudioProcessor::~DvisualizerAudioProcessor() {}

//==============================================================================
void DvisualizerAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;

    waveform.setNumChannels(getTotalNumInputChannels());
    waveform.clear();

    fftFifo.reset();
    std::fill(fftFifoBuffer.begin(), fftFifoBuffer.end(), 0.0f);

    // ── Prepare band-pass filters ─────────────────────────────────────────
    // Each band is a cascaded HP + LP biquad.
    // juce::dsp::IIR::Filter::prepare() takes a ProcessSpec.
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(getBlockSize());
    spec.numChannels = 1;   // mono analysis

    const float nyquist = static_cast<float>(sampleRate * 0.5);

    for (int b = 0; b < numBands; ++b)
    {
        // Clamp edges to Nyquist so filter design never gets an invalid freq
        const float lo = juce::jmin(bandEdges[b][0], nyquist * 0.98f);
        const float hi = juce::jmin(bandEdges[b][1], nyquist * 0.98f);

        // High-pass at the low edge
        *bandFilters[b].hp.coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, lo);
        bandFilters[b].hp.prepare(spec);
        bandFilters[b].hp.reset();

        // Low-pass at the high edge
        *bandFilters[b].lp.coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, hi);
        bandFilters[b].lp.prepare(spec);
        bandFilters[b].lp.reset();

        bandFilters[b].envelope = 0.0f;
    }

    // Envelope follower time constants
    // ~10 ms attack, ~100 ms release at the given sample rate
    const float dt = 1.0f / static_cast<float>(sampleRate);
    bandAttackCoeff = std::exp(-dt / 0.010f);
    bandReleaseCoeff = std::exp(-dt / 0.100f);

    beatEnvelope = 0.0f;
    beatThreshold = 0.0f;
}

void DvisualizerAudioProcessor::releaseResources()
{
    waveform.clear();
}

//==============================================================================
void DvisualizerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& /*midi*/)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numInputs = getTotalNumInputChannels();
    const int numOutputs = getTotalNumOutputChannels();

    // Silence extra output channels
    for (int ch = numInputs; ch < numOutputs; ++ch)
        buffer.clear(ch, 0, numSamples);

    // ── Waveform FIFO ──────────────────────────────────────────────────────
    waveform.pushBuffer(buffer);

    // ── FFT FIFO (mono, channel 0) ─────────────────────────────────────────
    const float* ch0 = buffer.getReadPointer(0);
    {
        int start1, size1, start2, size2;
        fftFifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        if (size1 > 0)
            juce::FloatVectorOperations::copy(
                fftFifoBuffer.data() + start1, ch0, size1);
        if (size2 > 0)
            juce::FloatVectorOperations::copy(
                fftFifoBuffer.data() + start2, ch0 + size1, size2);

        fftFifo.finishedWrite(size1 + size2);
    }

    // ── Step 1: Band energy extraction ────────────────────────────────────
    // For each sample we run it through each band's HP+LP pair and update
    // a per-band peak envelope follower.  All per-sample work; no allocation.
    for (int s = 0; s < numSamples; ++s)
    {
        // Mono-mix input (average channels if stereo)
        float mono = ch0[s];
        if (numInputs > 1)
            mono = (mono + buffer.getReadPointer(1)[s]) * 0.5f;

        for (int b = 0; b < numBands; ++b)
        {
            // Run through HP then LP to isolate the band
            float x = bandFilters[b].hp.processSample(mono);
            x = bandFilters[b].lp.processSample(x);

            // Absolute value → envelope follower
            const float absX = std::abs(x);
            const float coeff = (absX > bandFilters[b].envelope)
                ? bandAttackCoeff : bandReleaseCoeff;
            bandFilters[b].envelope = coeff * bandFilters[b].envelope
                + (1.0f - coeff) * absX;
        }

        // ── Beat detector ─────────────────────────────────────────────────
        // Track the overall signal envelope with a fast attack / slow release.
        const float absInput = std::abs(mono);
        const float envCoeff = (absInput > beatEnvelope) ? beatAttack : beatRelease;
        beatEnvelope = envCoeff * beatEnvelope + (1.0f - envCoeff) * absInput;

        // Adaptive threshold decays slowly upward to the signal level
        beatThreshold = beatThresholdDecay * beatThreshold
            + (1.0f - beatThresholdDecay) * beatEnvelope;

        // Fire a pulse when the envelope exceeds 1.4× the running threshold
        // and there is actually some signal (threshold > near-silence floor).
        if (beatEnvelope > beatThreshold * 1.4f && beatThreshold > 0.001f)
        {
            // Only write 1.0 if the current stored value is < 0.5 to avoid
            // retriggering on the same transient across multiple processBlock calls.
            float expected = beatPulse.load(std::memory_order_relaxed);
            if (expected < 0.5f)
                beatPulse.store(1.0f, std::memory_order_relaxed);

            // Bump threshold to prevent rapid re-triggering within same hit
            beatThreshold = beatEnvelope * 1.2f;
        }
    }

    // Publish per-band energies — normalised to a useful display range
    // (linear RMS ~0..0.5 maps to 0..1 via soft clip)
    for (int b = 0; b < numBands; ++b)
    {
        // Simple normalisation: tanh-ish soft-clip so loud signals reach 1.0
        const float e = bandFilters[b].envelope * 6.0f;   // scale up quiet signals
        const float normalised = e / (1.0f + e);           // pseudo-sigmoid [0..1)
        bandEnergy[b].store(normalised, std::memory_order_relaxed);
    }
}

//==============================================================================
juce::AudioProcessorEditor* DvisualizerAudioProcessor::createEditor()
{
    return new DvisualizerAudioProcessorEditor(*this);
}

//==============================================================================
void DvisualizerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
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