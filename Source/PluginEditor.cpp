#include "PluginProcessor.h"
#include "PluginEditor.h"

AudioToMidiProcessorEditor::AudioToMidiProcessorEditor (AudioToMidiProcessor& p)
    : AudioProcessorEditor (p), audioProcessor (p), inputLevelMeter(p), midiNoteDisplay(p), spectrumVisualizer(p)
{
    setLookAndFeel(&lookAndFeel);

    // Add input level meter
    addAndMakeVisible(inputLevelMeter);

    // Add MIDI note display
    addAndMakeVisible(midiNoteDisplay);

    // Add spectrum visualizer + style picker (combo added after so it sits on top)
    addAndMakeVisible(spectrumVisualizer);

    vizStyleBox.addItem("Aurora", 1);
    vizStyleBox.addItem("Orb", 2);
    vizStyleBox.addItem("Nebula", 3);
    addAndMakeVisible(vizStyleBox);

    vizStyleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        audioProcessor.parameters, "vizStyle", vizStyleBox);

    // Polyphony 1 = mono engine: grey out the Threshold knob, which only
    // affects the FFT poly engine's peak picking
    auto updateControlEnablement = [this]
    {
        bool mono = AudioToMidiProcessor::monoEngineAvailable
                    && maxPolyphonySlider.getValue() <= 1.5;
        thresholdSlider.setEnabled(!mono);
        thresholdLabel.setEnabled(!mono);
    };
    maxPolyphonySlider.onValueChange = updateControlEnablement;
    updateControlEnablement(); // apply restored state

    auto styleLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(midiui::labelFont());
        label.setColour(juce::Label::textColourId, midiui::textLabel);
        addAndMakeVisible(label);
    };

    auto styleKnob = [this] (juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
        addAndMakeVisible(slider);
    };

    // Set up threshold slider
    styleKnob(thresholdSlider);
    styleLabel(thresholdLabel, "THRESHOLD");

    thresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "threshold", thresholdSlider);

    // Set up sensitivity slider
    styleKnob(sensitivitySlider);
    styleLabel(sensitivityLabel, "SENSITIVITY");

    sensitivityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "sensitivity", sensitivitySlider);

    // Set up noise floor slider
    styleKnob(noiseFloorSlider);
    styleLabel(noiseFloorLabel, "NOISE FLOOR");

    noiseFloorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "noiseFloor", noiseFloorSlider);

    // Set up max polyphony slider
    styleKnob(maxPolyphonySlider);
    styleLabel(maxPolyphonyLabel, "POLYPHONY");

    maxPolyphonyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "maxPolyphony", maxPolyphonySlider);

    // Set up Enable Pitch Bend toggle (drawn as a pill switch by the LookAndFeel)
    enablePitchBendToggle.setButtonText("");
    addAndMakeVisible(enablePitchBendToggle);
    styleLabel(enablePitchBendLabel, "ENABLE");

    enablePitchBendAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.parameters, "enablePitchBend", enablePitchBendToggle);

    // Set up Pitch Bend Range slider
    styleKnob(pitchBendRangeSlider);
    styleLabel(pitchBendRangeLabel, "BEND RANGE");

    pitchBendRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.parameters, "pitchBendRange", pitchBendRangeSlider);

    setSize (520, 656);
}

AudioToMidiProcessorEditor::~AudioToMidiProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void AudioToMidiProcessorEditor::paint (juce::Graphics& g)
{
    // Near-black vertical gradient background
    juce::ColourGradient bg (midiui::bgTop, 0.0f, 0.0f,
                             midiui::bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    // Header: accent tab + bold tracked-out title
    auto titleArea = headerArea.reduced(0, 12);
    g.setColour(midiui::accent);
    g.fillRoundedRectangle(titleArea.removeFromLeft(4).toFloat(), 2.0f);
    titleArea.removeFromLeft(10);

    g.setColour(midiui::textBright);
    g.setFont(midiui::titleFont());
    g.drawText("AUDIO TO MIDI", titleArea, juce::Justification::centredLeft, false);

    // "IN" caption next to the level meter
    auto meterBounds = inputLevelMeter.getBounds();
    g.setColour(midiui::textFaint);
    g.setFont(midiui::labelFont());
    g.drawText("IN",
               juce::Rectangle<int>(meterBounds.getX() - 26, meterBounds.getY(),
                                    18, meterBounds.getHeight()),
               juce::Justification::centredRight, false);

    // Section panels
    auto drawPanel = [&g] (juce::Rectangle<int> r, const juce::String& caption)
    {
        auto rf = r.toFloat();
        g.setColour(midiui::panelFill);
        g.fillRoundedRectangle(rf, 8.0f);
        g.setColour(midiui::panelStroke);
        g.drawRoundedRectangle(rf.reduced(0.5f), 8.0f, 1.0f);

        g.setColour(midiui::textLabel);
        g.setFont(midiui::sectionFont());
        g.drawText(caption, r.reduced(14, 0).removeFromTop(30),
                   juce::Justification::centredLeft, false);
    };

    drawPanel(detectionPanel, "DETECTION");
    drawPanel(pitchBendPanel, "PITCH BEND");
}

void AudioToMidiProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced(16);

    // Header: title on the left, input meter on the right
    headerArea = bounds.removeFromTop(44);
    inputLevelMeter.setBounds(headerArea.removeFromRight(190).reduced(0, 14));
    headerArea.removeFromRight(30); // room for the "IN" caption

    bounds.removeFromTop(8);

    // Detection panel: four rotary knobs
    detectionPanel = bounds.removeFromTop(168);
    {
        auto content = detectionPanel.reduced(14, 0);
        content.removeFromTop(28);     // section caption row
        content.removeFromBottom(10);

        const int cellW = content.getWidth() / 4;

        auto layoutKnob = [&content, cellW] (juce::Label& label, juce::Slider& slider)
        {
            auto cell = content.removeFromLeft(cellW).reduced(4, 0);
            label.setBounds(cell.removeFromTop(14));
            slider.setBounds(cell);
        };

        layoutKnob(thresholdLabel,    thresholdSlider);
        layoutKnob(sensitivityLabel,  sensitivitySlider);
        layoutKnob(noiseFloorLabel,   noiseFloorSlider);
        layoutKnob(maxPolyphonyLabel, maxPolyphonySlider);
    }

    bounds.removeFromTop(8);

    // Pitch bend panel: pill toggle + bend range knob
    pitchBendPanel = bounds.removeFromTop(136);
    {
        auto content = pitchBendPanel.reduced(14, 0);
        content.removeFromTop(28);
        content.removeFromBottom(10);

        auto left = content.removeFromLeft(content.getWidth() / 2).reduced(4, 0);
        enablePitchBendLabel.setBounds(left.removeFromTop(14));
        enablePitchBendToggle.setBounds(left.withSizeKeepingCentre(52, 26));

        auto right = content.reduced(4, 0);
        pitchBendRangeLabel.setBounds(right.removeFromTop(14));
        pitchBendRangeSlider.setBounds(right);
    }

    bounds.removeFromTop(8);

    // Spectrum visualizer strip with style picker in its caption row
    auto vizArea = bounds.removeFromTop(128);
    spectrumVisualizer.setBounds(vizArea);
    vizStyleBox.setBounds(vizArea.getRight() - 96 - 12, vizArea.getY() + 8, 96, 20);

    bounds.removeFromTop(8);

    // MIDI output display fills the rest
    midiNoteDisplay.setBounds(bounds);
}
