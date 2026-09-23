#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "AudioToMidiLookAndFeel.h"

class AudioToMidiProcessorEditor : public juce::AudioProcessorEditor
{
public:
    AudioToMidiProcessorEditor (AudioToMidiProcessor&);
    ~AudioToMidiProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    class InputLevelMeter : public juce::Component, public juce::Timer
    {
    public:
        InputLevelMeter(AudioToMidiProcessor& proc) : processor(proc)
        {
            startTimerHz(30); // Update 30 times per second
        }

        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            const float corner = 4.0f;

            // Recessed background
            g.setColour(midiui::insetFill);
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(midiui::panelStroke);
            g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);

            // Level
            const float level = processor.currentInputLevel.load();
            const float dbLevel = juce::Decibels::gainToDecibels(level, -60.0f);
            const float norm = juce::jlimit(0.0f, 1.0f,
                                            juce::jmap(dbLevel, -60.0f, 0.0f, 0.0f, 1.0f));

            auto inner = bounds.reduced(2.0f);

            if (norm > 0.001f)
            {
                juce::Graphics::ScopedSaveState save(g);
                juce::Path clip;
                clip.addRoundedRectangle(inner, corner - 1.5f);
                g.reduceClipRegion(clip);

                // Accent gradient: cyan -> amber -> red across the full scale
                juce::ColourGradient grad(midiui::accent, inner.getX(), 0.0f,
                                          midiui::red,    inner.getRight(), 0.0f, false);
                grad.addColour(0.70, midiui::accent);
                grad.addColour(0.86, midiui::amber);
                g.setGradientFill(grad);
                g.fillRect(inner.withWidth(inner.getWidth() * norm));
            }

            // dB readout overlaid on the right
            g.setColour(midiui::textBright.withAlpha(0.85f));
            g.setFont(juce::Font(juce::FontOptions(10.5f)));
            g.drawText(juce::String(dbLevel, 1) + " dB",
                       getLocalBounds().reduced(8, 0),
                       juce::Justification::centredRight, false);
        }

        void timerCallback() override
        {
            repaint();
        }

    private:
        AudioToMidiProcessor& processor;
    };

    InputLevelMeter inputLevelMeter;

    class MidiNoteDisplay : public juce::Component, public juce::Timer
    {
    public:
        MidiNoteDisplay(AudioToMidiProcessor& proc) : processor(proc)
        {
            startTimerHz(30);
        }

        void paint(juce::Graphics& g) override
        {
            // Section panel
            auto bounds = getLocalBounds().toFloat();
            g.setColour(midiui::panelFill);
            g.fillRoundedRectangle(bounds, 8.0f);
            g.setColour(midiui::panelStroke);
            g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);

            auto content = getLocalBounds().reduced(14, 10);

            g.setColour(midiui::textLabel);
            g.setFont(midiui::sectionFont());
            g.drawText("MIDI OUTPUT", content.removeFromTop(16),
                       juce::Justification::centredLeft, false);
            content.removeFromTop(6);

            juce::ScopedLock lock(processor.midiNoteLock);

            bool anyActive = false;
            auto rowArea = content;

            for (const auto& noteInfo : processor.currentMidiNotes)
            {
                if (noteInfo.noteNumber < 0)
                    continue;

                anyActive = true;

                if (rowArea.getHeight() < 18)
                    break;

                auto row = rowArea.removeFromTop(18);

                // Accent indicator dot with soft glow
                auto dot = row.removeFromLeft(14).toFloat().withSizeKeepingCentre(7.0f, 7.0f);
                g.setColour(midiui::accent.withAlpha(0.3f));
                g.fillEllipse(dot.expanded(2.5f));
                g.setColour(midiui::accent);
                g.fillEllipse(dot);
                row.removeFromLeft(6);

                // Note name
                juce::String noteName = juce::MidiMessage::getMidiNoteName(
                    noteInfo.noteNumber, true, true, 4);
                g.setColour(midiui::textBright);
                g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
                g.drawText(noteName, row.removeFromLeft(48),
                           juce::Justification::centredLeft, false);

                // Velocity readout (right) and bar (remaining middle)
                auto velText = row.removeFromRight(56);
                auto barArea = row.reduced(6, 0).toFloat();
                barArea = barArea.withSizeKeepingCentre(barArea.getWidth(), 5.0f);

                g.setColour(midiui::insetFill);
                g.fillRoundedRectangle(barArea, 2.5f);

                const float v = juce::jlimit(0.0f, 1.0f, (float) noteInfo.velocity / 127.0f);
                if (v > 0.0f)
                {
                    g.setColour(midiui::accent);
                    g.fillRoundedRectangle(
                        barArea.withWidth(juce::jmax(5.0f, barArea.getWidth() * v)), 2.5f);
                }

                g.setColour(midiui::textLabel);
                g.setFont(midiui::valueFont());
                g.drawText("VEL " + juce::String(noteInfo.velocity), velText,
                           juce::Justification::centredRight, false);

                rowArea.removeFromTop(4);
            }

            if (!anyActive)
            {
                g.setColour(midiui::textFaint);
                g.setFont(juce::Font(juce::FontOptions(12.5f)));
                g.drawText("No notes detected", content, juce::Justification::centred, false);
            }
        }

        void timerCallback() override
        {
            repaint();
        }

    private:
        AudioToMidiProcessor& processor;
    };

    MidiNoteDisplay midiNoteDisplay;

    class SpectrumVisualizer : public juce::Component, public juce::Timer
    {
    public:
        SpectrumVisualizer(AudioToMidiProcessor& proc) : processor(proc)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (auto& p : particles)
            {
                p.x     = rng.nextFloat();
                p.y     = rng.nextFloat();
                p.phase = rng.nextFloat() * juce::MathConstants<float>::twoPi;
                p.speed = 0.015f + rng.nextFloat() * 0.035f;
                p.size  = 0.6f + rng.nextFloat() * 0.9f;
                p.band  = (int) (std::pow(rng.nextFloat(), 1.4f)
                                 * (float) (AudioToMidiProcessor::numVizBands - 1));
            }
            smoothed.fill(0.0f);
            startTimerHz(30);
        }

        void timerCallback() override
        {
            for (size_t i = 0; i < smoothed.size(); ++i)
            {
                // Perceptual shaping + fast attack / slow release smoothing
                float target = std::pow(juce::jlimit(0.0f, 1.0f, processor.vizBands[i].load()), 0.35f);
                if (target > smoothed[i])
                    smoothed[i] += (target - smoothed[i]) * 0.5f;
                else
                    smoothed[i] *= 0.85f;
            }

            for (auto& p : particles)
                p.phase += p.speed;

            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            g.setColour(midiui::panelFill);
            g.fillRoundedRectangle(bounds, 8.0f);
            g.setColour(midiui::panelStroke);
            g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);

            auto content = getLocalBounds().reduced(14, 10);
            g.setColour(midiui::textLabel);
            g.setFont(midiui::sectionFont());
            g.drawText("SPECTRUM", content.removeFromTop(16),
                       juce::Justification::centredLeft, false);
            content.removeFromTop(4);

            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(content);

            auto area = content.toFloat();
            int style = (int) *processor.parameters.getRawParameterValue("vizStyle");

            if (style == 1)      drawOrb(g, area);
            else if (style == 2) drawNebula(g, area);
            else                 drawAurora(g, area);
        }

    private:
        static constexpr int numBands = AudioToMidiProcessor::numVizBands;

        void drawAurora(juce::Graphics& g, juce::Rectangle<float> r)
        {
            struct LayerSpec { juce::Colour colour; float centre, width, alpha; };
            const LayerSpec layers[4] = {
                { midiui::accentDark, 0.12f, 0.38f, 0.50f },
                { midiui::accent,     0.40f, 0.30f, 0.38f },
                { midiui::amber,      0.65f, 0.28f, 0.30f },
                { midiui::pinkAcc,    0.88f, 0.30f, 0.26f } };

            for (const auto& layer : layers)
            {
                juce::Path p;
                p.startNewSubPath(r.getX(), r.getBottom());
                float prevX = r.getX(), prevY = r.getBottom();

                for (int i = 0; i < numBands; ++i)
                {
                    float t = (float) i / (float) (numBands - 1);
                    float w = std::exp(-juce::square((t - layer.centre) / layer.width));
                    float v = smoothed[(size_t) i] * w;

                    float x = r.getX() + t * r.getWidth();
                    float y = r.getBottom() - v * r.getHeight() * 0.95f;

                    p.quadraticTo(prevX, prevY, (prevX + x) * 0.5f, (prevY + y) * 0.5f);
                    prevX = x; prevY = y;
                }

                p.lineTo(r.getRight(), r.getBottom());
                p.closeSubPath();

                g.setColour(layer.colour.withAlpha(layer.alpha));
                g.fillPath(p);
            }
        }

        void drawOrb(juce::Graphics& g, juce::Rectangle<float> r)
        {
            auto centre = r.getCentre();
            float maxR = juce::jmin(r.getWidth(), r.getHeight()) * 0.5f - 3.0f;
            const int bandsPerRing = numBands / 3;

            struct RingSpec { juce::Colour colour; float base, gain, alpha; };
            const RingSpec rings[3] = {
                { midiui::accentDark, 0.55f, 0.42f, 0.40f },
                { midiui::accent,     0.38f, 0.38f, 0.45f },
                { midiui::amber,      0.22f, 0.34f, 0.50f } };

            auto mid = [](juce::Point<float> a, juce::Point<float> b)
            {
                return juce::Point<float>((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            };

            for (int ring = 0; ring < 3; ++ring)
            {
                std::array<juce::Point<float>, 16> pts;
                for (int k = 0; k < bandsPerRing; ++k)
                {
                    float e = smoothed[(size_t) (ring * bandsPerRing + k)];
                    float angle = juce::MathConstants<float>::twoPi * (float) k / (float) bandsPerRing
                                  - juce::MathConstants<float>::halfPi;
                    float radius = (rings[ring].base + e * rings[ring].gain) * maxR;
                    pts[(size_t) k] = { centre.x + std::cos(angle) * radius,
                                        centre.y + std::sin(angle) * radius };
                }

                juce::Path blob;
                blob.startNewSubPath(mid(pts[bandsPerRing - 1], pts[0]));
                for (int k = 0; k < bandsPerRing; ++k)
                    blob.quadraticTo(pts[(size_t) k],
                                     mid(pts[(size_t) k], pts[(size_t) ((k + 1) % bandsPerRing)]));
                blob.closeSubPath();

                g.setColour(rings[ring].colour.withAlpha(rings[ring].alpha));
                g.fillPath(blob);
            }

            // Centre glow scaled by overall energy
            float total = 0.0f;
            for (float v : smoothed) total += v;
            total /= (float) numBands;

            float cr = 5.0f + total * 14.0f;
            g.setColour(midiui::textBright.withAlpha(0.12f + 0.5f * total));
            g.fillEllipse(centre.x - cr, centre.y - cr, cr * 2.0f, cr * 2.0f);
        }

        void drawNebula(juce::Graphics& g, juce::Rectangle<float> r)
        {
            for (const auto& p : particles)
            {
                float e = smoothed[(size_t) p.band];
                if (e < 0.03f)
                    continue;

                float px = r.getX() + (p.x + 0.06f * std::sin(p.phase)) * r.getWidth();
                float py = r.getY() + (p.y + 0.06f * std::cos(p.phase * 0.8f)) * r.getHeight();
                px = juce::jlimit(r.getX(), r.getRight(), px);
                py = juce::jlimit(r.getY(), r.getBottom(), py);

                float t = (float) p.band / (float) (numBands - 1);
                juce::Colour c = t < 0.33f ? midiui::accentDark
                               : t < 0.66f ? midiui::accent
                               : t < 0.85f ? midiui::amber : midiui::pinkAcc;

                float radius = (3.0f + 11.0f * e) * p.size;

                g.setColour(c.withAlpha(0.10f + 0.15f * e));
                g.fillEllipse(px - radius * 2.0f, py - radius * 2.0f, radius * 4.0f, radius * 4.0f);
                g.setColour(c.withAlpha(0.25f + 0.45f * e));
                g.fillEllipse(px - radius, py - radius, radius * 2.0f, radius * 2.0f);
            }
        }

        struct Particle { float x = 0, y = 0, phase = 0, speed = 0, size = 1; int band = 0; };

        AudioToMidiProcessor& processor;
        std::array<float, AudioToMidiProcessor::numVizBands> smoothed {};
        std::array<Particle, 40> particles;
    };

    SpectrumVisualizer spectrumVisualizer;

private:
    AudioToMidiProcessor& audioProcessor;

    AudioToMidiLookAndFeel lookAndFeel;

    // Panel regions computed in resized(), drawn in paint()
    juce::Rectangle<int> headerArea;
    juce::Rectangle<int> detectionPanel;
    juce::Rectangle<int> pitchBendPanel;

    // UI Components
    juce::Slider thresholdSlider;
    juce::Label thresholdLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thresholdAttachment;

    juce::Slider sensitivitySlider;
    juce::Label sensitivityLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sensitivityAttachment;

    juce::Slider noiseFloorSlider;
    juce::Label noiseFloorLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noiseFloorAttachment;

    juce::Slider maxPolyphonySlider;
    juce::Label maxPolyphonyLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> maxPolyphonyAttachment;

    juce::ToggleButton enablePitchBendToggle;
    juce::Label enablePitchBendLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enablePitchBendAttachment;

    juce::Slider pitchBendRangeSlider;
    juce::Label pitchBendRangeLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchBendRangeAttachment;

    juce::ComboBox vizStyleBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> vizStyleAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioToMidiProcessorEditor)
};
