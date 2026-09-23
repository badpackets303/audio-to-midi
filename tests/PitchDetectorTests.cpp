// Regression tests for HarmonicPitchDetector.
//
// Synthesizes guitar-like tones (harmonic series with slight string
// inharmonicity, random phases and a little noise), runs them through the same
// analysis the plug-in uses (4096-point Hann-windowed FFT magnitudes) and checks
// the detected MIDI notes.

#include "HarmonicPitchDetector.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
constexpr int fftOrder = 12;
constexpr int fftSize  = 1 << fftOrder;
constexpr double pi    = 3.14159265358979323846;

void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const std::complex<double> wl = std::polar(1.0, -2.0 * pi / (double) len);
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w = 1.0;
            for (size_t k = 0; k < len / 2; ++k, w *= wl)
            {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

// Magnitude spectrum as the plug-in computes it (juce::WindowingFunction::hann is symmetric)
std::vector<float> magnitudeSpectrum(const std::vector<double>& x)
{
    std::vector<std::complex<double>> a(fftSize);
    for (int i = 0; i < fftSize; ++i)
        a[(size_t) i] = x[(size_t) i] * (0.5 - 0.5 * std::cos(2.0 * pi * i / (fftSize - 1)));
    fft(a);

    std::vector<float> mag(fftSize / 2 + 1);
    for (size_t i = 0; i < mag.size(); ++i)
        mag[i] = (float) std::abs(a[i]);
    return mag;
}

double midiToHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }
int hzToMidi(double hz)   { return (int) std::lround(69.0 + 12.0 * std::log2(hz / 440.0)); }

std::string noteName(int n)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

struct Voice
{
    int midiNote;
    std::vector<double> harmonicAmps; // relative amplitude of harmonics 1, 2, 3...
};

std::vector<double> synthesize(const std::vector<Voice>& voices, double sampleRate, unsigned seed)
{
    constexpr double inharmonicity = 1.0e-4; // f_h = h * f0 * sqrt(1 + B h^2), typical of steel strings
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> phase(0.0, 2.0 * pi);
    std::normal_distribution<double> noise(0.0, 0.002);

    std::vector<double> x(fftSize, 0.0);
    for (const auto& v : voices)
    {
        const double f0 = midiToHz(v.midiNote);
        for (size_t k = 0; k < v.harmonicAmps.size(); ++k)
        {
            const double h = (double) (k + 1);
            const double f = h * f0 * std::sqrt(1.0 + inharmonicity * h * h);
            if (f >= sampleRate * 0.45)
                break;
            const double ph = phase(rng);
            for (int i = 0; i < fftSize; ++i)
                x[(size_t) i] += 0.1 * v.harmonicAmps[k] * std::sin(2.0 * pi * f * i / sampleRate + ph);
        }
    }
    for (auto& s : x)
        s += noise(rng);
    return x;
}

// Harmonic profiles (amplitudes of harmonics 1..10)
const std::vector<double> strongFundamental  { 1.0, 0.8, 0.6, 0.5, 0.3, 0.2, 0.15, 0.1, 0.07, 0.05 };
const std::vector<double> weakFundamental    { 0.2, 0.5, 0.6, 1.0, 0.5, 0.3, 0.2, 0.1, 0.08, 0.05 };
const std::vector<double> veryWeakFund       { 0.1, 0.25, 0.7, 1.0, 0.6, 0.3, 0.2, 0.1, 0.08, 0.05 };
const std::vector<double> missingFundamental { 0.0, 0.6, 0.8, 1.0, 0.6, 0.4, 0.3, 0.2, 0.1, 0.05 };
const std::vector<double> brightHighString   { 1.0, 0.5, 0.35, 0.2, 0.1, 0.05 };

struct TestCase
{
    std::string name;
    std::vector<Voice> voices;
    int maxNotes;
};

int failures = 0, total = 0, sweepTotal = 0;
bool quiet = false;

void run(const TestCase& tc, double sampleRate)
{
    std::set<int> expected;
    for (const auto& v : tc.voices)
        expected.insert(v.midiNote);

    for (unsigned seed = 1; seed <= 5; ++seed)
    {
        const auto mag = magnitudeSpectrum(synthesize(tc.voices, sampleRate, seed));

        HarmonicPitchDetector detector;
        HarmonicPitchDetector::Settings settings;
        settings.maxNotes = tc.maxNotes;
        const auto& pitches = detector.process(mag.data(), (int) mag.size(),
                                               (float) (sampleRate / fftSize), settings);

        std::set<int> got;
        double worstCents = 0.0;
        for (const auto& p : pitches)
        {
            const int n = hzToMidi(p.frequency);
            got.insert(n);
            if (expected.count(n))
                worstCents = std::max(worstCents, std::abs(1200.0 * std::log2(p.frequency / midiToHz(n))));
        }

        // Tuning error must stay well inside a semitone (the synth adds a few cents of inharmonicity)
        const bool pass = got == expected && worstCents < 20.0;
        ++total;
        if (!pass)
            ++failures;

        if (!pass || (seed == 1 && !quiet))
        {
            std::string gotStr;
            for (int n : got)
                gotStr += noteName(n) + " ";
            std::printf("%s %-48s @%5.0f Hz seed %u -> %s(max %.1f cents)\n",
                        pass ? "PASS" : "FAIL", tc.name.c_str(), sampleRate, seed,
                        gotStr.c_str(), worstCents);
        }
    }
}
void runQuiet(const TestCase& tc, double sampleRate)
{
    quiet = true;
    const int before = total;
    run(tc, sampleRate);
    sweepTotal += total - before;
    quiet = false;
}
} // namespace

int main()
{
    const int E2 = 40, F2 = 41, A2 = 45, B2 = 47, C3 = 48, D3 = 50, E3 = 52, G3 = 55,
              B3 = 59, Cs4 = 61, E4 = 64, G4 = 67, E5 = 76, A5 = 81, D6 = 86;

    const std::vector<TestCase> cases {
        // Single notes, polyphony 4: harmonics must not be reported as extra notes
        { "E2 strong fundamental",                { { E2, strongFundamental } },  4 },
        { "E2 weak fundamental (was B3 G#4 E3)",  { { E2, weakFundamental } },    4 },
        { "E2 very weak fundamental (was E4 ...)",{ { E2, veryWeakFund } },       4 },
        { "E2 missing fundamental",               { { E2, missingFundamental } }, 4 },
        { "F2 weak fundamental",                  { { F2, weakFundamental } },    4 },
        { "A2 weak fundamental (was E4 A3 C#5)",  { { A2, weakFundamental } },    4 },
        { "D3 weak fundamental",                  { { D3, weakFundamental } },    4 },
        { "G3 strong fundamental",                { { G3, strongFundamental } },  4 },
        { "B3 strong fundamental",                { { B3, strongFundamental } },  4 },
        { "E4 bright",                            { { E4, brightHighString } },   4 },
        { "E5 bright (12th fret high E)",         { { E5, brightHighString } },   4 },
        { "A5 bright (17th fret high E)",         { { A5, brightHighString } },   4 },
        { "D6 bright (22nd fret high E)",         { { D6, brightHighString } },   4 },

        // Single note, mono setting
        { "E2 very weak fundamental, maxNotes 1", { { E2, veryWeakFund } },       1 },

        // Chords. Note: a note whose every harmonic coincides with a harmonic of a
        // lower played note (an octave, or an octave + fifth such as A2 + E4) is
        // indistinguishable from that lower note's overtones and is not detected.
        // Low notes closer than ~3-4 semitones (e.g. A2 + C3) fall within the
        // 4096-point FFT's resolution and merge into a single peak.
        { "E2 + B2 power chord",                  { { E2, strongFundamental }, { B2, strongFundamental } }, 4 },
        { "E2 + B2 power chord, weak fundamentals",{ { E2, weakFundamental }, { B2, weakFundamental } },   4 },
        { "C3 + E3 + G3 major triad",             { { C3, strongFundamental }, { E3, strongFundamental },
                                                    { G3, strongFundamental } }, 4 },
        { "E3 + G3 + B3 minor triad",             { { E3, strongFundamental }, { G3, strongFundamental },
                                                    { B3, strongFundamental } }, 4 },
        { "A2 + E3 + C#4 open A shape",           { { A2, strongFundamental }, { E3, strongFundamental },
                                                    { Cs4, brightHighString } }, 4 },
        { "E4 + G4 double stop",                  { { E4, brightHighString }, { G4, brightHighString } }, 4 },
    };

    for (double sr : { 44100.0, 48000.0 })
        for (const auto& tc : cases)
            run(tc, sr);

    // Every semitone across the guitar's range, with every harmonic profile.
    // Only failures are printed.
    const int passedBeforeSweep = total - failures;
    for (double sr : { 44100.0, 48000.0 })
        for (int note = E2; note <= D6; ++note)
            for (const auto* profile : { &strongFundamental, &weakFundamental, &veryWeakFund,
                                         &missingFundamental, &brightHighString })
                runQuiet({ "sweep " + noteName(note), { { note, *profile } }, 4 }, sr);
    std::printf("sweep: %d / %d passed\n", (total - failures) - passedBeforeSweep, sweepTotal);

    std::printf("\n%d / %d passed\n", total - failures, total);
    return failures == 0 ? 0 : 1;
}
