// End-to-end tests for AudioToMidiProcessor's MIDI output.
//
// Feeds synthesized, slightly detuned guitar-like audio through processBlock()
// and checks the resulting MIDI stream. Pitch bend is channel-wide, so it must
// stay centered whenever more than one note is sounding.

#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace
{
constexpr double sampleRate = 44100.0;
constexpr int    blockSize  = 512;
constexpr double pi         = 3.14159265358979323846;

struct Tone
{
    int    midiNote;
    double detuneCents;
    double startSeconds;
    double endSeconds;
    double vibratoCents = 0.0;  // depth of a slow (4 Hz) pitch wobble
};

struct TimedEvent
{
    int64_t sample;
    juce::MidiMessage message;
};

// Renders the tones block by block through the processor and returns every MIDI event
std::vector<TimedEvent> runProcessor(const std::vector<Tone>& tones, double lengthSeconds, bool pitchBend,
                                     int maxPolyphony = 4)
{
    AudioToMidiProcessor processor;
    processor.parameters.getParameter("enablePitchBend")->setValueNotifyingHost(pitchBend ? 1.0f : 0.0f);
    processor.parameters.getParameter("maxPolyphony")->setValueNotifyingHost(
        processor.parameters.getParameter("maxPolyphony")->convertTo0to1((float) maxPolyphony));
    processor.prepareToPlay(sampleRate, blockSize);

    const std::vector<double> harmonics { 1.0, 0.8, 0.6, 0.5, 0.3, 0.2, 0.15, 0.1, 0.07, 0.05 };
    const int64_t totalSamples = (int64_t) (lengthSeconds * sampleRate);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    std::vector<TimedEvent> events;
    std::vector<double> phases(tones.size() * harmonics.size(), 0.0);

    for (int64_t blockStart = 0; blockStart < totalSamples; blockStart += blockSize)
    {
        buffer.clear();
        for (int i = 0; i < blockSize; ++i)
        {
            const double t = (double) (blockStart + i) / sampleRate;
            double x = 0.0;
            for (size_t n = 0; n < tones.size(); ++n)
            {
                const auto& tone = tones[n];
                if (t < tone.startSeconds || t >= tone.endSeconds)
                    continue;
                const double cents = tone.detuneCents + tone.vibratoCents * std::sin(2.0 * pi * 4.0 * t);
                const double f0 = 440.0 * std::pow(2.0, (tone.midiNote - 69 + cents / 100.0) / 12.0);
                for (size_t k = 0; k < harmonics.size(); ++k)
                {
                    // Integrate phase so the pitch can change smoothly
                    auto& phase = phases[n * harmonics.size() + k];
                    phase += 2.0 * pi * f0 * (double) (k + 1) / sampleRate;
                    x += 0.05 * harmonics[k] * std::sin(phase + (double) k);
                }
            }
            buffer.setSample(0, i, (float) x);
        }

        midi.clear();
        processor.processBlock(buffer, midi);
        for (const auto metadata : midi)
            events.push_back({ blockStart + metadata.samplePosition, metadata.getMessage() });
    }

    processor.releaseResources();
    return events;
}

double bendToCents(int bend, double rangeSemitones = 2.0)
{
    return (bend - 8192) / 8191.0 * rangeSemitones * 100.0;
}

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (! condition)
        ++failures;
}

// Walks the MIDI stream and reports the first time a non-centered pitch bend
// is in effect while two or more notes are sounding (checked after all events
// sharing a timestamp have been applied).
bool bendCenteredDuringChords(const std::vector<TimedEvent>& events, bool verbose)
{
    std::set<int> sounding;
    int bend = 8192;

    for (size_t i = 0; i < events.size(); ++i)
    {
        const auto& m = events[i].message;
        if (m.isNoteOn())
            sounding.insert(m.getNoteNumber());
        else if (m.isNoteOff())
            sounding.erase(m.getNoteNumber());
        else if (m.isPitchWheel())
            bend = m.getPitchWheelValue();

        const bool lastAtTimestamp = i + 1 == events.size() || events[i + 1].sample != events[i].sample;
        if (lastAtTimestamp && sounding.size() >= 2 && bend != 8192)
        {
            if (verbose)
                std::printf("     at %.3f s: %zu notes sounding with bend %+.1f cents\n",
                            (double) events[i].sample / sampleRate, sounding.size(), bendToCents(bend));
            return false;
        }
    }
    return true;
}

int countNoteOns(const std::vector<TimedEvent>& events)
{
    int count = 0;
    for (const auto& e : events)
        count += e.message.isNoteOn() ? 1 : 0;
    return count;
}

std::set<int> notesPlayed(const std::vector<TimedEvent>& events)
{
    std::set<int> notes;
    for (const auto& e : events)
        if (e.message.isNoteOn())
            notes.insert(e.message.getNoteNumber());
    return notes;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const int D2 = 38, A2 = 45, C3 = 48, E3 = 52, G3 = 55;

    // 1. Single detuned note: bend is sent before the note-on and matches the detune
    {
        std::printf("Single note A2 +30 cents\n");
        const auto events = runProcessor({ { A2, 30.0, 0.0, 1.5 } }, 2.0, true);

        int noteOnIndex = -1, bendBeforeNoteOn = -1;
        for (int i = 0; i < (int) events.size(); ++i)
        {
            const auto& m = events[(size_t) i].message;
            if (m.isNoteOn() && noteOnIndex < 0)
                noteOnIndex = i;
            if (m.isPitchWheel() && noteOnIndex < 0)
                bendBeforeNoteOn = m.getPitchWheelValue();
        }

        check(notesPlayed(events) == std::set<int> { A2 }, "plays only A2");
        check(bendBeforeNoteOn >= 0, "pitch bend sent before the note-on");
        if (bendBeforeNoteOn >= 0)
        {
            std::printf("     initial bend %+.1f cents\n", bendToCents(bendBeforeNoteOn));
            check(std::abs(bendToCents(bendBeforeNoteOn) - 30.0) < 5.0, "initial bend within 5 cents of +30");
        }
    }

    // 2. Chord with each note detuned differently: bend must stay centered
    {
        std::printf("Chord C3 +25, E3 -20, G3 +10 cents\n");
        const auto events = runProcessor({ { C3, 25.0, 0.0, 1.5 }, { E3, -20.0, 0.0, 1.5 }, { G3, 10.0, 0.0, 1.5 } },
                                         2.0, true);
        check(notesPlayed(events) == std::set<int> { C3, E3, G3 }, "plays C3 E3 G3");
        check(bendCenteredDuringChords(events, true), "pitch bend centered while chord sounds");
    }

    // 3. Single note, then a second note joins: bend follows the first note, then recenters
    {
        std::printf("A2 +30 cents, E3 -20 cents joins at 1 s\n");
        const auto events = runProcessor({ { A2, 30.0, 0.0, 2.0 }, { E3, -20.0, 1.0, 2.0 } }, 2.5, true);

        bool bentWhileAlone = false;
        std::set<int> sounding;
        for (const auto& e : events)
        {
            if (e.message.isNoteOn())
                sounding.insert(e.message.getNoteNumber());
            else if (e.message.isNoteOff())
                sounding.erase(e.message.getNoteNumber());
            else if (e.message.isPitchWheel() && sounding.size() <= 1 && e.message.getPitchWheelValue() != 8192)
                bentWhileAlone = true;
        }

        check(notesPlayed(events) == std::set<int> { A2, E3 }, "plays A2 and E3");
        check(bentWhileAlone, "bends while A2 sounds alone");
        check(bendCenteredDuringChords(events, true), "pitch bend centered once E3 joins");
    }

    // 4. Pitch bend disabled: no bends at all
    {
        std::printf("Pitch bend disabled, A2 +30 cents\n");
        const auto events = runProcessor({ { A2, 30.0, 0.0, 1.5 } }, 2.0, false);
        bool anyBend = false;
        for (const auto& e : events)
            anyBend |= e.message.isPitchWheel() && e.message.getPitchWheelValue() != 8192;
        check(! anyBend, "no pitch bend sent");
    }

    // 5. Out-of-tune note wobbling across the halfway point between A2 and A#2
    //    must stay one note (hysteresis) in both engines, not retrigger
    for (int polyphony : { 4, 1 })
    {
        std::printf("A2 +45 cents with +/-15 cent vibrato, polyphony %d\n", polyphony);
        Tone tone { A2, 45.0, 0.0, 2.5 };
        tone.vibratoCents = 15.0;
        const auto events = runProcessor({ tone }, 3.0, true, polyphony);
        std::printf("     %d note-on(s)\n", countNoteOns(events));
        check(countNoteOns(events) == 1, "a single note-on");
    }

    // 6. A real one-semitone step must still register as a new note
    for (int polyphony : { 4, 1 })
    {
        std::printf("A2 then A#2 (semitone step at 1 s), polyphony %d\n", polyphony);
        const auto events = runProcessor({ { A2, 0.0, 0.0, 1.0 }, { A2 + 1, 0.0, 1.0, 2.0 } }, 2.5, true, polyphony);
        check(notesPlayed(events) == std::set<int> { A2, A2 + 1 }, "plays A2 then A#2");
    }

    // 7. Drop-D low string
    {
        std::printf("D2 (drop D)\n");
        const auto events = runProcessor({ { D2, 0.0, 0.0, 1.5 } }, 2.0, true);
        check(notesPlayed(events) == std::set<int> { D2 }, "plays only D2");
    }

    std::printf("\n%s\n", failures == 0 ? "All processor tests passed" : "Processor tests FAILED");
    return failures == 0 ? 0 : 1;
}
