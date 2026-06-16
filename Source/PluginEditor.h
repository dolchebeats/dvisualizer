#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "SpectrumAnalyzerComponent.h"
#include "AlchemyComponent.h"
#include "GalaxyComponent.h"

//==============================================================================
/**
 * DvisualizerAudioProcessorEditor
 *
 * Owns:
 *   - Three ComboBox controls (mode, background colour, primary colour)
 *   - APVTS attachments that keep the controls in sync with the processor
 *   - A SpectrumAnalyzerComponent  (editor-lifetime)
 *   - An AlchemyComponent          (editor-lifetime)
 *   - A GalaxyComponent            (editor-lifetime)
 *   - A reference to the processor's AudioVisualiserComponent (processor-lifetime)
 *
 * A juce::Timer at 30 Hz polls the three atomic parameter values and updates
 * the visible visualization component + colours whenever they change.
 *
 * All three visualizer components run their own internal 60 Hz timers
 * (via VisualizerBase) even when hidden, so switching modes is instant.
 */
class DvisualizerAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Timer
{
public:
    explicit DvisualizerAudioProcessorEditor(DvisualizerAudioProcessor&);
    ~DvisualizerAudioProcessorEditor() noexcept override;

    void paint(juce::Graphics&) override;
    void resized()                override;

private:
    //==========================================================================
    void timerCallback()    override;
    void updateVisibleMode();
    void updateColours();

    // Colour palette helpers — indices must match AudioParameterChoice arrays
    static juce::Colour getBgColour(int index);
    static juce::Colour getPrimaryColour(int index);

    //==========================================================================
    DvisualizerAudioProcessor& audioProcessor;

    // ── Control strip ─────────────────────────────────────────────────────
    juce::Label    titleLabel;
    juce::Label    modeLabel, bgColorLabel, primaryColorLabel;
    juce::ComboBox modeComboBox;
    juce::ComboBox bgColorComboBox;
    juce::ComboBox primaryColorComboBox;

    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<ComboAttachment> modeAttachment;
    std::unique_ptr<ComboAttachment> bgColorAttachment;
    std::unique_ptr<ComboAttachment> primaryColorAttachment;

    // ── Visualization components ───────────────────────────────────────────
    // waveform         — owned by the processor (survives editor close)
    // All others       — owned by this editor
    SpectrumAnalyzerComponent spectrumAnalyzer;
    AlchemyComponent          alchemyViz;
    GalaxyComponent           galaxyViz;

    // ── State tracking ────────────────────────────────────────────────────
    int lastKnownMode{ -1 };
    int lastKnownBg{ -1 };
    int lastKnownPrimary{ -1 };
    juce::Colour currentBgColour{ 0xff0d0d1a };

    //==========================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DvisualizerAudioProcessorEditor)
};