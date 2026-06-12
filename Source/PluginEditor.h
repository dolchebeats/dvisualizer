#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "SpectrumAnalyzerComponent.h"

//==============================================================================
/**
 * DvisualizerAudioProcessorEditor
 *
 * Owns:
 *   - Three ComboBox controls (mode, background color, primary color)
 *   - APVTS attachments that keep the controls in sync with the processor
 *   - A SpectrumAnalyzerComponent (editor-lifetime; spectrum data lives in processor)
 *   - A reference to the processor's AudioVisualiserComponent (processor-lifetime)
 *
 * A juce::Timer at 30 Hz polls the three atomic parameter values and updates
 * the visible visualization component + colours whenever they change.  This
 * approach handles all change sources uniformly: user drag, host automation,
 * and preset recall — with no thread-marshalling complexity.
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
    void timerCallback() override;
    void updateVisibleMode();
    void updateColours();

    // Colour palette helpers — indices must match AudioParameterChoice arrays
    static juce::Colour getBgColour(int index);
    static juce::Colour getPrimaryColour(int index);

    //==========================================================================
    DvisualizerAudioProcessor& audioProcessor;

    // ── Control strip ────────────────────────────────────────────────────
    juce::Label titleLabel;
    juce::Label modeLabel, bgColorLabel, primaryColorLabel;

    juce::ComboBox modeComboBox;
    juce::ComboBox bgColorComboBox;
    juce::ComboBox primaryColorComboBox;

    // Attachments keep ComboBox ↔ APVTS in sync (two-way, including host automation)
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<ComboAttachment> modeAttachment;
    std::unique_ptr<ComboAttachment> bgColorAttachment;
    std::unique_ptr<ComboAttachment> primaryColorAttachment;

    // ── Visualization components ──────────────────────────────────────────
    // spectrumAnalyzer is owned by this editor.
    // audioProcessor.visualiser is owned by the processor (shared reference).
    SpectrumAnalyzerComponent spectrumAnalyzer;

    // ── State tracking for timer-based polling ────────────────────────────
    int lastKnownMode{ -1 };
    int lastKnownBg{ -1 };
    int lastKnownPrimary{ -1 };

    // Background colour used in paint() — updated by updateColours()
    juce::Colour currentBgColour{ 0xff0d0d1a };

    //==========================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DvisualizerAudioProcessorEditor)
};