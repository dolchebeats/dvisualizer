/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
DvisualizerAudioProcessorEditor::DvisualizerAudioProcessorEditor (DvisualizerAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // Make sure that before the constructor has finished, you've set the
    // editor's size to whatever you need it to be.
    setSize (640, 480);

    // ── Title label ───────────────────────────────────────────────────────
    titleLabel.setText("Waveform Viewer", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions().withHeight(18.0f)
        .withStyle("Bold")));
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    // ── Visualiser ────────────────────────────────────────────────────────
    // The visualiser component lives inside the processor (processor lifetime
    // > editor lifetime), so we just make it visible here.
    // We customise a few colours to give it a clean oscilloscope look.
    audioProcessor.visualiser.setColours(
        juce::Colour(0xff1a1a2e),   // dark background
        juce::Colour(0xff00d4ff));  // bright cyan waveform trace

    addAndMakeVisible(audioProcessor.visualiser);
}

DvisualizerAudioProcessorEditor::~DvisualizerAudioProcessorEditor()
{
    // The visualiser belongs to the processor, NOT the editor.
// We must remove it from the editor's component hierarchy before the
// editor is destroyed; otherwise JUCE will try to delete a component
// it doesn't own and trigger an assertion.
    audioProcessor.visualiser.setVisible(false);
    removeChildComponent(&audioProcessor.visualiser);
}

//==============================================================================
void DvisualizerAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Fill the background with a deep dark colour.
    g.fillAll(juce::Colour(0xff0d0d1a));
}

void DvisualizerAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(8); // 8 px margin on all sides

    // Reserve 30 px at the top for the title label.
    titleLabel.setBounds(bounds.removeFromTop(30));

    // Leave a small gap between title and waveform.
    bounds.removeFromTop(4);

    // The remainder of the area is given to the visualiser.
    audioProcessor.visualiser.setBounds(bounds);
}
