#pragma once

#include <JuceHeader.h>
#include "HarmonicPitchDetector.h"
#include <vector>
#include <array>

// Cycfi Q pitch detection (optional; enabled when external/q is present)
#if __has_include(<q/pitch/pitch_detector.hpp>)
 #define HAVE_CYCFI_Q 1
 #include <q/support/literals.hpp>
 #include <q/pitch/pitch_detector.hpp>
 #include <q/fx/signal_conditioner.hpp>
#endif

class AudioToMidiProcessor : public juce::AudioProcessor
{
public:
    AudioToMidiProcessor();
    ~AudioToMidiProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState parameters;

    // Pitch detection parameters (need to be accessible for UI)
    static constexpr int maxPolyphony = 6;

    // Input level monitoring (public for UI access)
    std::atomic<float> currentInputLevel{0.0f};

    // MIDI note monitoring for UI (public for UI access)
    struct MidiNoteInfo {
        int noteNumber = -1;
        float frequency = 0.0f;
        int velocity = 0;
    };
    std::array<MidiNoteInfo, maxPolyphony> currentMidiNotes;
    juce::CriticalSection midiNoteLock;

    // True when the Cycfi Q mono engine was compiled in (public for UI access)
#if defined(HAVE_CYCFI_Q)
    static constexpr bool monoEngineAvailable = true;
#else
    static constexpr bool monoEngineAvailable = false;
#endif

    // Spectrum visualization bands, log-spaced 60 Hz - 8 kHz (public for UI access)
    static constexpr int numVizBands = 48;
    std::array<std::atomic<float>, numVizBands> vizBands {};

private:
    // Pitch detection parameters
    static constexpr int fftOrder = 12;  // 2^12 = 4096 samples
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr float minFrequency = 80.0f;    // E2 - lowest guitar string
    static constexpr float maxFrequency = 1200.0f;  // Roughly 3 octaves above

    // Overlapped analysis: full window re-analyzed every hopSize samples
    static constexpr int hopSize = 1024;

    // Temporal onset gating: consecutive frames a pitch must persist before note-on
    static constexpr int onsetFramesRequired = 3;

    // FFT
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> fftData;
    HarmonicPitchDetector harmonicDetector;

    // Audio analysis buffer (circular) + linearized copy for FFT
    juce::AudioBuffer<float> analysisBuffer;
    int analysisBufferIndex = 0;   // circular write position
    int samplesSinceHop = 0;
    std::vector<float> orderedWindow;

    // Consecutive-detection counts per MIDI note (for onset gating)
    std::array<int, 128> noteCandidateFrames {};

    // Polyphonic note tracking
    struct ActiveNote {
        int midiNote = -1;
        float baseFrequency = 0.0f;      // Base frequency of the MIDI note
        float currentFrequency = 0.0f;   // Current detected frequency
        float amplitude = 0.0f;
        int framesSinceDetection = 0;
    };
    std::vector<ActiveNote> activeNotes;
    int polyLastBend = 8192;             // Last pitch bend sent on channel 1 (8192 = center)
    static constexpr int maxFramesSilence = 10; // Frames before note off

    // Mono tracking (Cycfi Q) --------------------------------------------
#if defined(HAVE_CYCFI_Q)
    std::unique_ptr<cycfi::q::signal_conditioner> monoConditioner;
    std::unique_ptr<cycfi::q::pitch_detector>     monoDetector;
#endif
    static constexpr int monoStableRequired = 2; // consecutive estimates before (re)trigger

    int   monoCurrentNote    = -1;
    float monoBaseFrequency  = 0.0f;
    int   monoLastBend       = 8192;
    int   monoCandidateNote  = -1;
    int   monoStableCount    = 0;
    int   monoGateOffSamples = 0;
    int   lastTrackingMode   = -1;

    void monoHandleEstimate(float frequency, float envelope, juce::MidiBuffer& midiMessages, int samplePosition);
    void monoNoteOn(int midiNote, float frequency, float envelope, juce::MidiBuffer& midiMessages, int samplePosition);
    void monoNoteOff(juce::MidiBuffer& midiMessages, int samplePosition);
    void flushAllNotes(juce::MidiBuffer& midiMessages);
    // ---------------------------------------------------------------------

    // Sample rate
    double currentSampleRate = 44100.0;

    // Pitch detection methods
    std::vector<std::pair<float, float>> detectPitches(const float* audioData, int numSamples, int maxNotes);
    int frequencyToMidiNote(float frequency);
    float midiNoteToFrequency(int midiNote);
    int calculatePitchBend(float currentFrequency, float baseFrequency, float pitchBendRange);
    void updateActiveNotes(const std::vector<std::pair<float, float>>& detectedPitches, juce::MidiBuffer& midiMessages, int samplePosition);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioToMidiProcessor)
};
