#pragma once

#include <JuceHeader.h>

//==============================================================================
/** Central colour palette for the plugin UI. */
namespace midiui
{
    // Background
    const juce::Colour bgTop        { 0xff17191d };
    const juce::Colour bgBottom     { 0xff0e1013 };

    // Panels
    const juce::Colour panelFill    { 0xff1d2126 };
    const juce::Colour panelStroke  { 0xff2c3138 };
    const juce::Colour insetFill    { 0xff101317 };

    // Accent (cyan/teal) + meter warm tones
    const juce::Colour accent       { 0xff2dd4cf };
    const juce::Colour accentDark   { 0xff148d8a };
    const juce::Colour amber        { 0xfff2b24a };
    const juce::Colour red          { 0xffef5350 };
    const juce::Colour pinkAcc      { 0xffed93b1 };

    // Typography
    const juce::Colour textBright   { 0xffeceef0 };
    const juce::Colour textLabel    { 0xff8b929c };
    const juce::Colour textFaint    { 0xff5a616b };

    inline juce::Font titleFont()   { return juce::Font (juce::FontOptions (20.0f, juce::Font::bold)).withExtraKerningFactor (0.08f); }
    inline juce::Font sectionFont() { return juce::Font (juce::FontOptions (11.0f, juce::Font::bold)).withExtraKerningFactor (0.18f); }
    inline juce::Font labelFont()   { return juce::Font (juce::FontOptions (10.5f, juce::Font::bold)).withExtraKerningFactor (0.12f); }
    inline juce::Font valueFont()   { return juce::Font (juce::FontOptions (12.0f)); }
}

//==============================================================================
/** Dark, modern LookAndFeel: filled-arc rotary knobs with a glowing accent,
    pill-style toggle switch, borderless slider text boxes.
*/
class AudioToMidiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AudioToMidiLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, midiui::bgBottom);

        setColour (juce::Slider::rotarySliderFillColourId,    midiui::accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2c3138));
        setColour (juce::Slider::thumbColourId,               midiui::accent);
        setColour (juce::Slider::textBoxTextColourId,         midiui::textBright);
        setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId,    midiui::accent.withAlpha (0.35f));

        setColour (juce::Label::textColourId, midiui::textLabel);

        setColour (juce::TextEditor::focusedOutlineColourId,  midiui::accent.withAlpha (0.6f));
        setColour (juce::TextEditor::highlightColourId,       midiui::accent.withAlpha (0.35f));
        setColour (juce::TextEditor::textColourId,            midiui::textBright);
        setColour (juce::CaretComponent::caretColourId,       midiui::accent);

        setColour (juce::ToggleButton::textColourId,          midiui::textBright);
        setColour (juce::ToggleButton::tickColourId,          midiui::accent);

        setColour (juce::ComboBox::backgroundColourId,        midiui::insetFill);
        setColour (juce::ComboBox::textColourId,              midiui::textLabel);
        setColour (juce::ComboBox::outlineColourId,           midiui::panelStroke);
        setColour (juce::ComboBox::arrowColourId,             midiui::textLabel);
        setColour (juce::ComboBox::buttonColourId,            midiui::insetFill);

        setColour (juce::PopupMenu::backgroundColourId,            midiui::panelFill);
        setColour (juce::PopupMenu::textColourId,                  midiui::textLabel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, midiui::accent.withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId,       midiui::textBright);
    }

    //==============================================================================
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height)
                          .reduced (6.0f);

        const float size    = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto  centre  = bounds.getCentre();
        const float radius  = size * 0.5f;
        const float arcThickness = juce::jmax (2.5f, radius * 0.11f);
        const float arcRadius    = radius - arcThickness * 0.5f;
        const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        const bool enabled = slider.isEnabled();
        auto fillColour  = enabled ? slider.findColour (juce::Slider::rotarySliderFillColourId)
                                   : midiui::textFaint;
        auto trackColour = slider.findColour (juce::Slider::rotarySliderOutlineColourId);

        // Background track arc
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (trackColour);
        g.strokePath (track, juce::PathStrokeType (arcThickness,
                       juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Value arc with a soft glow underneath
        if (sliderPosProportional > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 rotaryStartAngle, angle, true);

            if (enabled)
            {
                g.setColour (fillColour.withAlpha (0.25f));
                g.strokePath (value, juce::PathStrokeType (arcThickness * 2.4f,
                               juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            g.setColour (fillColour);
            g.strokePath (value, juce::PathStrokeType (arcThickness,
                           juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Knob body
        const float bodyRadius = arcRadius - arcThickness * 1.6f;
        auto bodyBounds = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f)
                              .withCentre (centre);

        juce::ColourGradient bodyGrad (juce::Colour (0xff31363d), centre.x, centre.y - bodyRadius,
                                       juce::Colour (0xff1a1d21), centre.x, centre.y + bodyRadius, false);
        g.setGradientFill (bodyGrad);
        g.fillEllipse (bodyBounds);

        g.setColour (juce::Colour (0xff43494f));
        g.drawEllipse (bodyBounds, 1.0f);

        // Pointer
        const float pointerLen = bodyRadius * 0.42f;
        const float pointerThickness = juce::jmax (2.0f, bodyRadius * 0.14f);
        juce::Path pointer;
        pointer.addRoundedRectangle (-pointerThickness * 0.5f, -bodyRadius + 3.0f,
                                     pointerThickness, pointerLen, pointerThickness * 0.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
        g.setColour (enabled ? fillColour : midiui::textFaint);
        g.fillPath (pointer);
    }

    //==============================================================================
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/) override
    {
        const bool on = button.getToggleState();

        const float pillW = 52.0f, pillH = 24.0f;
        auto area = button.getLocalBounds().toFloat();
        juce::Rectangle<float> pill (pillW, pillH);

        const bool hasText = button.getButtonText().isNotEmpty();
        pill = hasText ? pill.withPosition (area.getX(), area.getCentreY() - pillH * 0.5f)
                       : pill.withCentre (area.getCentre());

        // Track
        auto trackColour = on ? midiui::accent.withAlpha (0.28f) : juce::Colour (0xff14171a);
        g.setColour (trackColour);
        g.fillRoundedRectangle (pill, pillH * 0.5f);

        g.setColour (on ? midiui::accent.withAlpha (0.9f)
                        : (shouldDrawButtonAsHighlighted ? juce::Colour (0xff4a5158)
                                                         : juce::Colour (0xff353b42)));
        g.drawRoundedRectangle (pill.reduced (0.5f), pillH * 0.5f, 1.2f);

        // Thumb
        const float thumbD = pillH - 7.0f;
        auto thumbCentreX = on ? pill.getRight() - thumbD * 0.5f - 3.5f
                               : pill.getX() + thumbD * 0.5f + 3.5f;
        juce::Rectangle<float> thumb (thumbD, thumbD);
        thumb = thumb.withCentre ({ thumbCentreX, pill.getCentreY() });

        if (on)
        {
            g.setColour (midiui::accent.withAlpha (0.35f));
            g.fillEllipse (thumb.expanded (3.5f));
            g.setColour (midiui::accent);
        }
        else
        {
            g.setColour (juce::Colour (0xff8b929c));
        }
        g.fillEllipse (thumb);

        // Text beside the pill (if any)
        if (hasText)
        {
            g.setColour (button.isEnabled() ? (on ? midiui::textBright : midiui::textLabel)
                                            : midiui::textFaint);
            g.setFont (midiui::valueFont());
            auto textArea = area.withTrimmedLeft (pillW + 10.0f);
            g.drawText (button.getButtonText(), textArea, juce::Justification::centredLeft, true);
        }
    }

    //==============================================================================
    juce::Font getLabelFont (juce::Label& label) override
    {
        // Slider text boxes are Labels owned by a Slider parent
        if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
            return midiui::valueFont();

        return label.getFont();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioToMidiLookAndFeel)
};
