#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Colour palettes
// Index ordering MUST match the AudioParameterChoice StringArrays in the processor.
//==============================================================================
juce::Colour DvisualizerAudioProcessorEditor::getBgColour(int index)
{
    static const juce::Colour palette[]
    {
        juce::Colour(0xff0d0d1a),   // 0 — Dark Navy  (default)
        juce::Colour(0xff000000),   // 1 — Pitch Black
        juce::Colour(0xff081a08),   // 2 — Deep Forest
        juce::Colour(0xff1a0808),   // 3 — Dark Crimson
        juce::Colour(0xff141414),   // 4 — Slate
    };
    return palette[juce::jlimit(0, 4, index)];
}

juce::Colour DvisualizerAudioProcessorEditor::getPrimaryColour(int index)
{
    static const juce::Colour palette[]
    {
        juce::Colour(0xff00d4ff),   // 0 — Cyan    (default)
        juce::Colour(0xff39ff14),   // 1 — Lime
        juce::Colour(0xffff7700),   // 2 — Orange
        juce::Colour(0xffff0099),   // 3 — Pink
        juce::Colour(0xffffffff),   // 4 — White
        juce::Colour(0xffffd700),   // 5 — Gold
    };
    return palette[juce::jlimit(0, 5, index)];
}

//==============================================================================
DvisualizerAudioProcessorEditor::DvisualizerAudioProcessorEditor(
    DvisualizerAudioProcessor& p)
    : AudioProcessorEditor(&p),
    audioProcessor(p),
    spectrumAnalyzer(p),
    alchemyViz(p),         // Step 8 — construct with processor reference
    galaxyViz(p)           // Galaxy mode — construct with processor reference
{
    setSize(640, 480);

    // ── Shared styling lambdas ─────────────────────────────────────────────
    auto styleLabel = [](juce::Label& lb, const juce::String& text)
        {
            lb.setText(text, juce::dontSendNotification);
            lb.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
            lb.setColour(juce::Label::textColourId, juce::Colour(0xff8888aa));
            lb.setJustificationType(juce::Justification::centredLeft);
        };

    auto styleComboBox = [](juce::ComboBox& cb)
        {
            cb.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1e1e2e));
            cb.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a3a5a));
            cb.setColour(juce::ComboBox::textColourId, juce::Colours::white);
            cb.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff00d4ff));
        };

    // ── Title ──────────────────────────────────────────────────────────────
    titleLabel.setText("dvisualizer", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions()
        .withHeight(20.0f).withStyle("Bold")));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    // ── Mode control ───────────────────────────────────────────────────────
    styleLabel(modeLabel, "MODE");
    addAndMakeVisible(modeLabel);

    // "Alchemy" is index 2, "Galaxy" is index 3 — must match createParameterLayout()
    modeComboBox.addItemList({ "Waveform", "Spectrum", "Alchemy", "Galaxy" }, 1);
    styleComboBox(modeComboBox);
    addAndMakeVisible(modeComboBox);

    // ── Background color control ───────────────────────────────────────────
    styleLabel(bgColorLabel, "BACKGROUND");
    addAndMakeVisible(bgColorLabel);

    bgColorComboBox.addItemList(
        { "Dark Navy", "Pitch Black", "Deep Forest", "Dark Crimson", "Slate" }, 1);
    styleComboBox(bgColorComboBox);
    addAndMakeVisible(bgColorComboBox);

    // ── Primary color control ──────────────────────────────────────────────
    styleLabel(primaryColorLabel, "PRIMARY COLOR");
    addAndMakeVisible(primaryColorLabel);

    primaryColorComboBox.addItemList(
        { "Cyan", "Lime", "Orange", "Pink", "White", "Gold" }, 1);
    styleComboBox(primaryColorComboBox);
    addAndMakeVisible(primaryColorComboBox);

    // ── APVTS attachments ──────────────────────────────────────────────────
    modeAttachment = std::make_unique<ComboAttachment>(
        audioProcessor.apvts, "vizMode", modeComboBox);
    bgColorAttachment = std::make_unique<ComboAttachment>(
        audioProcessor.apvts, "bgColor", bgColorComboBox);
    primaryColorAttachment = std::make_unique<ComboAttachment>(
        audioProcessor.apvts, "primaryColor", primaryColorComboBox);

    // ── Visualization components ───────────────────────────────────────────
    addAndMakeVisible(audioProcessor.waveform);
    addAndMakeVisible(spectrumAnalyzer);
    addAndMakeVisible(alchemyViz);          // Step 8
    addAndMakeVisible(galaxyViz);           // Galaxy mode

    // ── Parameter polling timer ────────────────────────────────────────────
    startTimerHz(30);
    timerCallback();   // force immediate sync on open
}

DvisualizerAudioProcessorEditor::~DvisualizerAudioProcessorEditor()
{
    stopTimer();

    // The waveform is owned by the processor — remove it from our hierarchy
    // before JUCE tears down the editor to avoid an ownership assertion.
    audioProcessor.waveform.setVisible(false);
    removeChildComponent(&audioProcessor.waveform);
}

//==============================================================================
void DvisualizerAudioProcessorEditor::timerCallback()
{
    const int mode = static_cast<int>(*audioProcessor.vizModeParam);
    const int bg = static_cast<int>(*audioProcessor.bgColorParam);
    const int primary = static_cast<int>(*audioProcessor.primaryColorParam);

    if (mode != lastKnownMode ||
        bg != lastKnownBg ||
        primary != lastKnownPrimary)
    {
        lastKnownMode = mode;
        lastKnownBg = bg;
        lastKnownPrimary = primary;
        updateVisibleMode();
        updateColours();
    }
}

//==============================================================================
void DvisualizerAudioProcessorEditor::updateVisibleMode()
{
    const int mode = static_cast<int>(*audioProcessor.vizModeParam);

    // Exactly one component is visible at a time; others are hidden but
    // still receive their timer callbacks so they warm up instantly on switch.
    audioProcessor.waveform.setVisible(mode == 0);
    spectrumAnalyzer.setVisible(mode == 1);
    alchemyViz.setVisible(mode == 2);  // Step 8
    galaxyViz.setVisible(mode == 3);  // Galaxy mode
}

void DvisualizerAudioProcessorEditor::updateColours()
{
    const juce::Colour bg = getBgColour(static_cast<int>(*audioProcessor.bgColorParam));
    const juce::Colour primary = getPrimaryColour(static_cast<int>(*audioProcessor.primaryColorParam));

    currentBgColour = bg;

    // Push colours into all visualisers so mode switches are instant
    audioProcessor.waveform.setColours(bg, primary);
    spectrumAnalyzer.setColours(bg, primary);
    alchemyViz.setColours(bg, primary);   // Step 8
    galaxyViz.setColours(bg, primary);   // Galaxy mode

    // Tint ComboBox arrows with the current primary colour
    for (auto* cb : { &modeComboBox, &bgColorComboBox, &primaryColorComboBox })
        cb->setColour(juce::ComboBox::arrowColourId, primary);

    repaint();
}

//==============================================================================
void DvisualizerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(currentBgColour);

    // Subtle separator line between the control strip and the visualization area
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawHorizontalLine(94, 10.0f, static_cast<float>(getWidth() - 10));
}

void DvisualizerAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // ── Title (28 px) ──────────────────────────────────────────────────────
    titleLabel.setBounds(bounds.removeFromTop(28));
    bounds.removeFromTop(6);

    // ── Control strip (52 px) ─────────────────────────────────────────────
    auto controlsRow = bounds.removeFromTop(52);
    bounds.removeFromTop(10);

    const int colWidth = controlsRow.getWidth() / 3;

    auto layoutControl = [&](juce::Label& label, juce::ComboBox& cb)
        {
            auto area = controlsRow.removeFromLeft(colWidth).reduced(4, 0);
            label.setBounds(area.removeFromTop(18));
            area.removeFromTop(3);
            cb.setBounds(area.removeFromTop(28));
        };

    layoutControl(modeLabel, modeComboBox);
    layoutControl(bgColorLabel, bgColorComboBox);
    layoutControl(primaryColorLabel, primaryColorComboBox);

    // ── Visualization area — all components share the same rectangle ──────
    audioProcessor.waveform.setBounds(bounds);
    spectrumAnalyzer.setBounds(bounds);
    alchemyViz.setBounds(bounds);   // Step 8
    galaxyViz.setBounds(bounds);    // Galaxy mode
}