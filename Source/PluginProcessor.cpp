#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

AudioToMidiProcessor::AudioToMidiProcessor()
    : AudioProcessor (BusesProperties()
                     .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     ),
      parameters (*this, nullptr, juce::Identifier ("PARAMETERS"), createParameterLayout()),
      fft(fftOrder),
      window(fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    fftData.resize(fftSize * 2, 0.0f);
    orderedWindow.assign(fftSize, 0.0f);
}

AudioToMidiProcessor::~AudioToMidiProcessor()
{
}

const juce::String AudioToMidiProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioToMidiProcessor::acceptsMidi() const
{
    return false;
}

bool AudioToMidiProcessor::producesMidi() const
{
    return true;
}

bool AudioToMidiProcessor::isMidiEffect() const
{
    return false;
}

double AudioToMidiProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioToMidiProcessor::getNumPrograms()
{
    return 1;
}

int AudioToMidiProcessor::getCurrentProgram()
{
    return 0;
}

void AudioToMidiProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioToMidiProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioToMidiProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

void AudioToMidiProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;

    // Initialize analysis buffer
    analysisBuffer.setSize(1, fftSize);
    analysisBuffer.clear();
    analysisBufferIndex = 0;
    samplesSinceHop = 0;
    orderedWindow.assign(fftSize, 0.0f);

    // Initialize active notes
    activeNotes.clear();
    activeNotes.resize(maxPolyphony);
    noteCandidateFrames.fill(0);
    polyLastBend = 8192;

    // Initialize mono tracking
#if defined(HAVE_CYCFI_Q)
    {
        namespace q = cycfi::q;
        using namespace q::literals;

        auto lowest  = q::frequency(minFrequency);
        auto highest = q::frequency(maxFrequency);

        monoConditioner = std::make_unique<q::signal_conditioner>(
            q::signal_conditioner::config{}, lowest, highest, (float) sampleRate);
        monoDetector = std::make_unique<q::pitch_detector>(
            lowest, highest, (float) sampleRate, -45_dB);
    }
#endif
    monoCurrentNote    = -1;
    monoBaseFrequency  = 0.0f;
    monoLastBend       = 8192;
    monoCandidateNote  = -1;
    monoStableCount    = 0;
    monoGateOffSamples = 0;
}

void AudioToMidiProcessor::releaseResources()
{
}

void AudioToMidiProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midiMessages)
{
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Get parameter values
    float noiseFloor = *parameters.getRawParameterValue("noiseFloor");
    int maxNotes = static_cast<int>(*parameters.getRawParameterValue("maxPolyphony"));

    // Polyphony 1 = mono engine (Cycfi Q, when available); 2+ = FFT poly engine
    const bool monoMode = monoEngineAvailable && maxNotes <= 1;

    // Flush hanging notes when the engine changes
    const int modeNow = monoMode ? 1 : 0;
    if (modeNow != lastTrackingMode)
    {
        if (lastTrackingMode >= 0)
            flushAllNotes(midiMessages);
        lastTrackingMode = modeNow;
    }

    // Calculate and store current input level for monitoring
    // Mix all input channels to mono for analysis
    float rmsLevel = 0.0f;
    int numSamples = buffer.getNumSamples();

    if (totalNumInputChannels > 0 && numSamples > 0)
    {
        // Calculate RMS across all input channels
        for (int ch = 0; ch < totalNumInputChannels; ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                rmsLevel += channelData[i] * channelData[i];
        }
        rmsLevel = std::sqrt(rmsLevel / (numSamples * totalNumInputChannels));
    }
    currentInputLevel.store(rmsLevel);

    // Process input - mix all channels to mono for pitch detection
    if (totalNumInputChannels > 0 && numSamples > 0)
    {

        // Copy input samples to analysis buffer (mix all channels to mono)
        for (int i = 0; i < numSamples; ++i)
        {
            // Mix all input channels together
            float mixedSample = 0.0f;
            for (int ch = 0; ch < totalNumInputChannels; ++ch)
            {
                mixedSample += buffer.getReadPointer(ch)[i];
            }
            mixedSample /= totalNumInputChannels; // Average the channels

            // Mono tracking: per-sample pitch detection (Cycfi Q)
#if defined(HAVE_CYCFI_Q)
            if (monoMode && monoDetector != nullptr)
            {
                float conditioned = (*monoConditioner)(mixedSample);

                if (monoConditioner->gate())
                {
                    monoGateOffSamples = 0;

                    if ((*monoDetector)(conditioned))
                        monoHandleEstimate(monoDetector->get_frequency(),
                                           monoConditioner->gate_env(),
                                           midiMessages, i);
                }
                else if (monoCurrentNote >= 0
                         && ++monoGateOffSamples > (int) (0.05 * currentSampleRate))
                {
                    monoNoteOff(midiMessages, i);
                    monoDetector->reset();
                    monoCandidateNote = -1;
                    monoStableCount = 0;
                }
            }
#endif

            // Write into circular buffer
            analysisBuffer.setSample(0, analysisBufferIndex, mixedSample);
            analysisBufferIndex = (analysisBufferIndex + 1) % fftSize;

            // Analyze the full window every hopSize samples (overlapped frames)
            if (++samplesSinceHop >= hopSize)
            {
                samplesSinceHop = 0;

                // Linearize circular buffer: oldest sample first
                const float* circular = analysisBuffer.getReadPointer(0);
                for (int j = 0; j < fftSize; ++j)
                    orderedWindow[j] = circular[(analysisBufferIndex + j) % fftSize];

                // Check RMS level
                float rms = 0.0f;
                for (int j = 0; j < fftSize; ++j)
                    rms += orderedWindow[j] * orderedWindow[j];
                rms = std::sqrt(rms / fftSize);

                if (rms >= noiseFloor)
                {
                    // Detect pitches (also refreshes the spectrum visualization)
                    auto detectedPitches = detectPitches(orderedWindow.data(), fftSize, maxNotes);

                    if (!monoMode)
                        updateActiveNotes(detectedPitches, midiMessages, i);
                }
                else
                {
                    if (!monoMode)
                    {
                        // No signal - send note offs for all active notes
                        updateActiveNotes({}, midiMessages, i);
                    }

                    // Let the visualization decay smoothly to silence
                    for (auto& band : vizBands)
                        band.store(band.load() * 0.7f);
                }
            }
        }
    }

    // Clear all output channels so no audio passes through (only MIDI output)
    for (int i = 0; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());
}

std::vector<std::pair<float, float>> AudioToMidiProcessor::detectPitches(const float* audioData, int numSamples, int maxNotes)
{
    juce::ignoreUnused(numSamples);

    // Copy audio data to FFT buffer
    for (int i = 0; i < fftSize; ++i)
    {
        fftData[i] = audioData[i];
        fftData[i + fftSize] = 0.0f; // Imaginary part
    }

    // Apply window
    window.multiplyWithWindowingTable(fftData.data(), fftSize);

    // Perform FFT
    fft.performFrequencyOnlyForwardTransform(fftData.data());

    float binResolution = static_cast<float>(currentSampleRate) / fftSize;

    // Update visualization bands (log-spaced 60 Hz - 8 kHz, peak magnitude per band,
    // normalized so a full-scale sine is ~1.0 with the Hann window)
    {
        const float vizMinHz = 60.0f;
        const float vizMaxHz = 8000.0f;
        const float vizNorm  = 4.0f / (float) fftSize;

        for (int b = 0; b < numVizBands; ++b)
        {
            float lo = vizMinHz * std::pow(vizMaxHz / vizMinHz, (float) b / (float) numVizBands);
            float hi = vizMinHz * std::pow(vizMaxHz / vizMinHz, (float) (b + 1) / (float) numVizBands);

            int binLo = std::max(1, (int) (lo / binResolution));
            int binHi = std::min(fftSize / 2 - 1, std::max(binLo + 1, (int) (hi / binResolution)));

            float bandPeak = 0.0f;
            for (int j = binLo; j < binHi; ++j)
                bandPeak = std::max(bandPeak, fftData[j]);

            vizBands[(size_t) b].store(juce::jlimit(0.0f, 1.0f, bandPeak * vizNorm));
        }
    }

    // Pick fundamentals by harmonic pattern rather than raw peak loudness, so a
    // dominant overtone (e.g. the 4th harmonic of a low string) isn't reported
    // as the note, and a note's harmonics aren't reported as extra notes.
    HarmonicPitchDetector::Settings settings;
    settings.minFrequency      = minFrequency;
    settings.maxFrequency      = maxFrequency;
    settings.relativeThreshold = *parameters.getRawParameterValue("threshold");
    settings.maxNotes          = maxNotes;

    std::vector<std::pair<float, float>> result;
    for (const auto& pitch : harmonicDetector.process(fftData.data(), fftSize / 2 + 1, binResolution, settings))
        result.push_back({ pitch.frequency, pitch.magnitude });

    return result;
}

void AudioToMidiProcessor::updateActiveNotes(const std::vector<std::pair<float, float>>& detectedPitches,
                                            juce::MidiBuffer& midiMessages, int samplePosition)
{
    float sensitivity = *parameters.getRawParameterValue("sensitivity");
    bool enablePitchBend = *parameters.getRawParameterValue("enablePitchBend") > 0.5f;
    float pitchBendRange = *parameters.getRawParameterValue("pitchBendRange");

    // Convert detected pitches to MIDI notes
    std::vector<int> detectedMidiNotes;
    std::vector<float> detectedFrequencies;
    std::vector<float> detectedAmplitudes;

    // Find max amplitude for normalization
    float maxAmplitude = 0.0f;
    for (const auto& pitch : detectedPitches)
    {
        maxAmplitude = std::max(maxAmplitude, pitch.second);
    }

    for (const auto& pitch : detectedPitches)
    {
        // Keep an already-sounding note while the pitch stays near it
        int midiNote = frequencyToMidiNote(pitch.first);
        for (const auto& note : activeNotes)
        {
            if (note.midiNote >= 0 && isNearNote(pitch.first, note.baseFrequency))
            {
                midiNote = note.midiNote;
                break;
            }
        }

        const bool duplicate = std::find(detectedMidiNotes.begin(), detectedMidiNotes.end(), midiNote)
                                   != detectedMidiNotes.end();
        if (midiNote >= 0 && midiNote <= 127 && ! duplicate)
        {
            detectedMidiNotes.push_back(midiNote);
            detectedFrequencies.push_back(pitch.first);
            // Normalize amplitude relative to the loudest peak
            float normalizedAmplitude = maxAmplitude > 0.0f ? pitch.second / maxAmplitude : 0.0f;
            detectedAmplitudes.push_back(normalizedAmplitude);
        }
    }

    // Temporal onset gating: count consecutive frames each candidate note has
    // been detected; reset the count for notes absent this frame.
    {
        std::array<bool, 128> presentThisFrame {};
        for (int midiNote : detectedMidiNotes)
            presentThisFrame[(size_t) midiNote] = true;

        for (size_t n = 0; n < noteCandidateFrames.size(); ++n)
        {
            if (presentThisFrame[n])
                noteCandidateFrames[n] = std::min(noteCandidateFrames[n] + 1, 1000);
            else
                noteCandidateFrames[n] = 0;
        }
    }

    // Update existing active notes
    for (auto& note : activeNotes)
    {
        if (note.midiNote >= 0)
        {
            // Check if this note is still detected
            auto it = std::find(detectedMidiNotes.begin(), detectedMidiNotes.end(), note.midiNote);

            if (it != detectedMidiNotes.end())
            {
                // Note is still active
                note.framesSinceDetection = 0;
                int index = std::distance(detectedMidiNotes.begin(), it);
                note.amplitude = detectedAmplitudes[index];
                note.currentFrequency = detectedFrequencies[index];
            }
            else
            {
                // Note not detected
                note.framesSinceDetection++;

                if (note.framesSinceDetection > maxFramesSilence)
                {
                    // Send note off
                    midiMessages.addEvent(juce::MidiMessage::noteOff(1, note.midiNote), samplePosition);

                    // Update UI tracking
                    {
                        juce::ScopedLock lock(midiNoteLock);
                        for (auto& midiNote : currentMidiNotes)
                        {
                            if (midiNote.noteNumber == note.midiNote)
                            {
                                midiNote.noteNumber = -1;
                                midiNote.frequency = 0.0f;
                                midiNote.velocity = 0;
                                break;
                            }
                        }
                    }

                    note.midiNote = -1;
                    note.baseFrequency = 0.0f;
                    note.currentFrequency = 0.0f;
                    note.amplitude = 0.0f;
                }
            }
        }
    }

    // Add newly detected notes (only once they've persisted long enough).
    // Slots are claimed here; the note-ons are sent after the pitch bend below.
    struct PendingNoteOn { size_t slot; int velocity; };
    std::array<PendingNoteOn, maxPolyphony> pendingNoteOns;
    size_t numPendingNoteOns = 0;

    for (size_t i = 0; i < detectedMidiNotes.size(); ++i)
    {
        int midiNote = detectedMidiNotes[i];

        // Onset gating: skip candidates that haven't persisted across enough frames
        if (noteCandidateFrames[(size_t) midiNote] < onsetFramesRequired)
            continue;

        // Check if this note is already active
        bool alreadyActive = false;
        for (const auto& note : activeNotes)
        {
            if (note.midiNote == midiNote)
            {
                alreadyActive = true;
                break;
            }
        }

        if (!alreadyActive)
        {
            // Find an empty slot
            for (size_t noteIdx = 0; noteIdx < activeNotes.size(); ++noteIdx)
            {
                auto& note = activeNotes[noteIdx];
                if (note.midiNote < 0)
                {
                    note.midiNote = midiNote;
                    note.baseFrequency = midiNoteToFrequency(midiNote);
                    note.currentFrequency = detectedFrequencies[i];
                    note.amplitude = detectedAmplitudes[i];
                    note.framesSinceDetection = 0;

                    // Calculate velocity based on amplitude and sensitivity
                    // Amplitude is normalized (0-1), apply power curve for natural response
                    // Use square root curve to make velocity more responsive to dynamics
                    float normalizedVelocity = std::sqrt(detectedAmplitudes[i]) * sensitivity;

                    // Map to MIDI velocity range (1-127)
                    int velocity = static_cast<int>(juce::jlimit(1.0f, 127.0f, normalizedVelocity * 127.0f));
                    pendingNoteOns[numPendingNoteOns++] = { noteIdx, velocity };

                    // Update UI tracking
                    {
                        juce::ScopedLock lock(midiNoteLock);
                        if (noteIdx < currentMidiNotes.size())
                        {
                            currentMidiNotes[noteIdx].noteNumber = midiNote;
                            currentMidiNotes[noteIdx].frequency = detectedFrequencies[i];
                            currentMidiNotes[noteIdx].velocity = velocity;
                        }
                    }

                    break;
                }
            }
        }
    }

    // Pitch bend is channel-wide: every sounding note on channel 1 bends with
    // it. Only follow the detected pitch while exactly one note sounds; with a
    // chord, hold it at center so one note's bend doesn't detune the others.
    // With no notes sounding leave it alone, so a synth's release tail doesn't jump.
    {
        const ActiveNote* soundingNote = nullptr;
        int numSounding = 0;
        for (const auto& note : activeNotes)
        {
            if (note.midiNote >= 0)
            {
                soundingNote = &note;
                ++numSounding;
            }
        }

        int targetBend = polyLastBend;
        if (! enablePitchBend || numSounding > 1)
            targetBend = 8192;
        else if (numSounding == 1)
            targetBend = calculatePitchBend(soundingNote->currentFrequency, soundingNote->baseFrequency, pitchBendRange);

        // A new note must start at the right pitch, so always send before its note-on;
        // otherwise skip tiny changes to avoid jitter.
        const bool changed = targetBend == 8192 ? polyLastBend != 8192
                                                : std::abs(targetBend - polyLastBend) > 20;
        if (targetBend != polyLastBend && (changed || numPendingNoteOns > 0))
        {
            midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, targetBend), samplePosition);
            polyLastBend = targetBend;
        }
    }

    for (size_t p = 0; p < numPendingNoteOns; ++p)
    {
        const auto& pending = pendingNoteOns[p];
        const auto& note = activeNotes[pending.slot];
        midiMessages.addEvent(juce::MidiMessage::noteOn(1, note.midiNote, (juce::uint8) pending.velocity), samplePosition);
    }
}

void AudioToMidiProcessor::monoHandleEstimate(float frequency, float envelope,
                                              juce::MidiBuffer& midiMessages, int samplePosition)
{
    int note = frequencyToMidiNote(frequency);

    // Keep the sounding note while the pitch stays near it
    if (monoCurrentNote >= 0 && isNearNote(frequency, monoBaseFrequency))
        note = monoCurrentNote;

    if (note < 0 || note > 127)
        return;

    if (monoCurrentNote < 0 || note != monoCurrentNote)
    {
        // New or changed pitch: require a couple of consecutive matching
        // estimates before (re)triggering to reject glitch estimates.
        if (note == monoCandidateNote)
            ++monoStableCount;
        else
        {
            monoCandidateNote = note;
            monoStableCount = 1;
        }

        if (monoStableCount >= monoStableRequired)
        {
            if (monoCurrentNote >= 0)
                monoNoteOff(midiMessages, samplePosition); // legato transition

            monoNoteOn(note, frequency, envelope, midiMessages, samplePosition);
            monoCandidateNote = -1;
            monoStableCount = 0;
        }
    }
    else
    {
        // Same note sustained: track pitch bend
        monoCandidateNote = -1;
        monoStableCount = 0;

        bool enablePitchBend = *parameters.getRawParameterValue("enablePitchBend") > 0.5f;
        float pitchBendRange = *parameters.getRawParameterValue("pitchBendRange");

        if (enablePitchBend)
        {
            int newBend = calculatePitchBend(frequency, monoBaseFrequency, pitchBendRange);
            if (std::abs(newBend - monoLastBend) > 20)
            {
                midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, newBend), samplePosition);
                monoLastBend = newBend;
            }
        }
        else if (monoLastBend != 8192)
        {
            midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, 8192), samplePosition);
            monoLastBend = 8192;
        }
    }
}

void AudioToMidiProcessor::monoNoteOn(int midiNote, float frequency, float envelope,
                                      juce::MidiBuffer& midiMessages, int samplePosition)
{
    float sensitivity = *parameters.getRawParameterValue("sensitivity");
    bool enablePitchBend = *parameters.getRawParameterValue("enablePitchBend") > 0.5f;
    float pitchBendRange = *parameters.getRawParameterValue("pitchBendRange");

    // Velocity from the conditioner's envelope (dB-mapped for a natural feel)
    float db = juce::Decibels::gainToDecibels(envelope, -60.0f);
    float norm = juce::jlimit(0.0f, 1.0f, juce::jmap(db, -50.0f, -6.0f, 0.0f, 1.0f));
    int velocity = (int) juce::jlimit(1.0f, 127.0f, std::sqrt(norm) * sensitivity * 127.0f);

    monoCurrentNote   = midiNote;
    monoBaseFrequency = midiNoteToFrequency(midiNote);
    monoLastBend      = 8192;

    // Bend first so the note starts at the detected pitch
    if (enablePitchBend)
    {
        int initialBend = calculatePitchBend(frequency, monoBaseFrequency, pitchBendRange);
        midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, initialBend), samplePosition);
        monoLastBend = initialBend;
    }

    midiMessages.addEvent(juce::MidiMessage::noteOn(1, midiNote, (juce::uint8) velocity), samplePosition);

    {
        juce::ScopedLock lock(midiNoteLock);
        currentMidiNotes[0].noteNumber = midiNote;
        currentMidiNotes[0].frequency  = frequency;
        currentMidiNotes[0].velocity   = velocity;
    }
}

void AudioToMidiProcessor::monoNoteOff(juce::MidiBuffer& midiMessages, int samplePosition)
{
    if (monoCurrentNote < 0)
        return;

    midiMessages.addEvent(juce::MidiMessage::noteOff(1, monoCurrentNote), samplePosition);

    {
        juce::ScopedLock lock(midiNoteLock);
        for (auto& midiNote : currentMidiNotes)
        {
            if (midiNote.noteNumber == monoCurrentNote)
            {
                midiNote.noteNumber = -1;
                midiNote.frequency  = 0.0f;
                midiNote.velocity   = 0;
                break;
            }
        }
    }

    monoCurrentNote   = -1;
    monoBaseFrequency = 0.0f;
    monoLastBend      = 8192;
}

void AudioToMidiProcessor::flushAllNotes(juce::MidiBuffer& midiMessages)
{
    // Poly engine notes
    for (auto& note : activeNotes)
    {
        if (note.midiNote >= 0)
        {
            midiMessages.addEvent(juce::MidiMessage::noteOff(1, note.midiNote), 0);
            note = ActiveNote{};
        }
    }
    noteCandidateFrames.fill(0);
    polyLastBend = 8192;

    // Mono engine note
    monoNoteOff(midiMessages, 0);
    monoCandidateNote = -1;
    monoStableCount = 0;

    // Clear the UI display
    {
        juce::ScopedLock lock(midiNoteLock);
        for (auto& midiNote : currentMidiNotes)
        {
            midiNote.noteNumber = -1;
            midiNote.frequency  = 0.0f;
            midiNote.velocity   = 0;
        }
    }

    midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
}

int AudioToMidiProcessor::frequencyToMidiNote(float frequency)
{
    if (frequency <= 0.0f) return -1;

    // MIDI note number = 69 + 12 * log2(frequency / 440)
    float midiNoteFloat = 69.0f + 12.0f * std::log2(frequency / 440.0f);

    // Round to nearest MIDI note
    return static_cast<int>(std::round(midiNoteFloat));
}

bool AudioToMidiProcessor::isNearNote(float frequency, float noteFrequency)
{
    if (frequency <= 0.0f || noteFrequency <= 0.0f)
        return false;

    return std::abs(12.0f * std::log2(frequency / noteFrequency)) < noteHysteresisSemitones;
}

float AudioToMidiProcessor::midiNoteToFrequency(int midiNote)
{
    // Frequency = 440 * 2^((midiNote - 69) / 12)
    return 440.0f * std::pow(2.0f, (midiNote - 69) / 12.0f);
}

int AudioToMidiProcessor::calculatePitchBend(float currentFrequency, float baseFrequency, float pitchBendRange)
{
    if (baseFrequency <= 0.0f || currentFrequency <= 0.0f)
        return 8192; // Center position (no bend)

    // Calculate the deviation in semitones
    float semitoneDeviation = 12.0f * std::log2(currentFrequency / baseFrequency);

    // Convert to pitch bend value (0-16383, where 8192 is center)
    // pitchBendRange is in semitones (typically 2)
    float bendAmount = semitoneDeviation / pitchBendRange;

    // Clamp to valid range [-1, 1] and convert to MIDI pitch bend range
    bendAmount = juce::jlimit(-1.0f, 1.0f, bendAmount);
    int pitchBendValue = static_cast<int>(8192.0f + bendAmount * 8191.0f);

    // Ensure valid MIDI pitch bend range
    return juce::jlimit(0, 16383, pitchBendValue);
}

bool AudioToMidiProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* AudioToMidiProcessor::createEditor()
{
    return new AudioToMidiProcessorEditor (*this);
}

void AudioToMidiProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void AudioToMidiProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessorValueTreeState::ParameterLayout AudioToMidiProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>("threshold", "Threshold", 0.1f, 0.9f, 0.3f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("sensitivity", "Sensitivity", 0.1f, 2.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("noiseFloor", "Noise Floor", 0.0001f, 0.1f, 0.01f));
    params.push_back(std::make_unique<juce::AudioParameterInt>("maxPolyphony", "Max Polyphony", 1, 6, 4));
    params.push_back(std::make_unique<juce::AudioParameterBool>("enablePitchBend", "Enable Pitch Bend", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("pitchBendRange", "Pitch Bend Range", 1.0f, 12.0f, 2.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>("vizStyle", "Visualizer Style",
        juce::StringArray { "Aurora", "Orb", "Nebula" }, 0));

    return { params.begin(), params.end() };
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioToMidiProcessor();
}
