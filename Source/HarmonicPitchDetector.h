#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Polyphonic fundamental-frequency estimation from a magnitude spectrum.
//
// Rather than treating the loudest spectral peaks as notes, every plausible
// fundamental is scored by the energy found at its harmonic series
// (f0, 2*f0, 3*f0, ...). This finds the true fundamental even when it is weak
// or missing - common for low guitar strings through a magnetic pickup, where
// the 3rd/4th harmonics can dominate. Polyphony is handled iteratively: once a
// note is chosen, the peaks it explains are removed before searching again, so
// its harmonics are not reported as extra notes.
//
// Known limitation: a note whose harmonics all coincide with those of a lower
// played note (an octave above it, or an octave + fifth) cannot be separated
// from that note's overtones and will not be reported.
//
// Deliberately free of JUCE so it can be unit tested standalone.
class HarmonicPitchDetector
{
public:
    struct Settings
    {
        float minFrequency      = 80.0f;   // lowest fundamental considered
        float maxFrequency      = 1200.0f; // highest fundamental considered
        float relativeThreshold = 0.3f;    // extra notes need this fraction of the strongest note's score
        int   maxNotes          = 4;
    };

    struct Pitch
    {
        float frequency = 0.0f;
        float magnitude = 0.0f; // summed magnitude of the note's matched harmonics
    };

    HarmonicPitchDetector()
    {
        peaks.reserve(maxPeaks);
        candidates.reserve(maxPeaks * maxDivisor);
        result.reserve(16);
    }

    // magnitudes: bins 0..numBins-1 of a Hann-windowed FFT (numBins = fftSize / 2 + 1)
    const std::vector<Pitch>& process(const float* magnitudes, int numBins, float binResolution,
                                      const Settings& settings)
    {
        result.clear();
        findPeaks(magnitudes, numBins, binResolution, settings);
        if (peaks.empty())
            return result;

        buildCandidates(settings);

        float firstScore = 0.0f;

        while ((int) result.size() < settings.maxNotes)
        {
            // Pick the best-scoring fundamental given the peaks not yet explained
            int bestIndex = -1;
            Score best;
            for (int c = 0; c < (int) candidates.size(); ++c)
            {
                Score s = score(candidates[(size_t) c]);
                if (s.value > best.value)
                {
                    best = s;
                    bestIndex = c;
                }
            }

            if (bestIndex < 0 || best.value <= 0.0f)
                break;

            if (result.empty())
            {
                firstScore = best.value;
            }
            else
            {
                // Additional notes must be substantial and supported by more than
                // one partial (a lone leftover peak is usually an unexplained overtone).
                // A strong, unexplained fundamental is enough on its own: its
                // overtones may be merged with harmonics of notes already found
                // (e.g. C#4 over A2 + E3), which leaves it a low harmonic score.
                const bool harmonicEvidence = best.value >= settings.relativeThreshold * firstScore
                                              && best.numMatched >= 2;
                if (! harmonicEvidence && best.fundamentalWeight < strongFundamental)
                    break;
            }

            result.push_back(accept(candidates[(size_t) bestIndex]));
        }

        return result;
    }

private:
    struct Peak
    {
        float frequency;
        float magnitude;
        float weight;   // compressed magnitude used for scoring
        bool  explained;
    };

    struct Score
    {
        float value = 0.0f;
        int   numMatched = 0;
        float fundamentalWeight = 0.0f;
    };

    static constexpr int   maxPeaks          = 256;
    static constexpr int   maxDivisor        = 8;       // candidate f0 = peak / 1..8
    static constexpr int   maxHarmonics      = 16;
    static constexpr float maxAnalysisHz     = 5000.0f; // highest harmonic considered
    static constexpr float peakFloor         = 0.01f;   // peaks below -40 dB re. the loudest are ignored
    static constexpr float harmonicTolCents  = 35.0f;
    static constexpr float harmonicWeightExp = 0.5f;    // harmonic h weighted by 1 / h^exp
    static constexpr float strongFundamental = 0.5f;    // peak weight (sqrt magnitude) ~ -12 dB re. the loudest

    std::vector<Peak>  peaks;       // sorted by frequency
    std::vector<float> candidates;
    std::vector<Pitch> result;

    void findPeaks(const float* mag, int numBins, float binResolution, const Settings& settings)
    {
        peaks.clear();

        const int lo = std::max(1, (int) std::floor(settings.minFrequency * 0.9f / binResolution));
        const int hi = std::min(numBins - 2, (int) std::ceil(maxAnalysisHz / binResolution));
        if (hi <= lo)
            return;

        float maxMag = 0.0f;
        for (int b = lo; b <= hi; ++b)
            maxMag = std::max(maxMag, mag[b]);
        if (maxMag <= 0.0f)
            return;

        const float floorMag = maxMag * peakFloor;

        for (int b = lo; b <= hi && (int) peaks.size() < maxPeaks; ++b)
        {
            const float m = mag[b];
            if (m < floorMag || m <= mag[b - 1] || m < mag[b + 1])
                continue;

            // Gaussian (log-parabolic) interpolation: far less biased than a
            // linear-magnitude parabola for a Hann window.
            constexpr float eps = 1.0e-12f;
            const float l = std::log(mag[b - 1] + eps);
            const float c = std::log(m + eps);
            const float r = std::log(mag[b + 1] + eps);
            const float denom = l - 2.0f * c + r;
            float delta = denom < 0.0f ? 0.5f * (l - r) / denom : 0.0f;
            delta = std::clamp(delta, -0.5f, 0.5f);

            const float trueMag = std::exp(c - 0.25f * (l - r) * delta);
            peaks.push_back({ ((float) b + delta) * binResolution, trueMag,
                              std::sqrt(trueMag / maxMag), false });
        }
    }

    void buildCandidates(const Settings& settings)
    {
        candidates.clear();

        for (const auto& p : peaks)
        {
            for (int d = 1; d <= maxDivisor; ++d)
            {
                const float f0 = p.frequency / (float) d;
                if (f0 < settings.minFrequency * 0.97f)
                    break;
                if (f0 <= settings.maxFrequency * 1.03f)
                    candidates.push_back(f0);
            }
        }

        // Merge near-duplicates (within ~10 cents)
        std::sort(candidates.begin(), candidates.end());
        size_t out = 0;
        for (size_t i = 0; i < candidates.size(); ++i)
            if (out == 0 || candidates[i] > candidates[out - 1] * 1.006f)
                candidates[out++] = candidates[i];
        candidates.resize(out);
    }

    // Index of the unexplained peak nearest to 'target' within tolerance, or -1
    int findPeakNear(float target) const
    {
        const float tol = target * (std::exp2(harmonicTolCents / 1200.0f) - 1.0f);

        auto it = std::lower_bound(peaks.begin(), peaks.end(), target - tol,
                                   [] (const Peak& p, float f) { return p.frequency < f; });

        int bestIndex = -1;
        float bestDist = tol;
        for (; it != peaks.end() && it->frequency <= target + tol; ++it)
        {
            const float dist = std::abs(it->frequency - target);
            if (!it->explained && dist <= bestDist)
            {
                bestDist = dist;
                bestIndex = (int) (it - peaks.begin());
            }
        }
        return bestIndex;
    }

    Score score(float f0) const
    {
        Score s;
        for (int h = 1; h <= maxHarmonics && f0 * (float) h <= maxAnalysisHz; ++h)
        {
            const int idx = findPeakNear(f0 * (float) h);
            if (idx >= 0)
            {
                const float w = peaks[(size_t) idx].weight;
                s.value += w / std::pow((float) h, harmonicWeightExp);
                ++s.numMatched;
                if (h == 1)
                    s.fundamentalWeight = w;
            }
        }
        return s;
    }

    Pitch accept(float f0)
    {
        // Refine f0 from the matched low harmonics (magnitude-weighted), then
        // mark every matched peak as explained so it can't seed another note.
        float weightedSum = 0.0f, weightTotal = 0.0f, magnitude = 0.0f;
        int matchedIdx[maxHarmonics];
        int numMatched = 0;

        for (int h = 1; h <= maxHarmonics && f0 * (float) h <= maxAnalysisHz; ++h)
        {
            const int idx = findPeakNear(f0 * (float) h);
            if (idx < 0)
                continue;

            const auto& p = peaks[(size_t) idx];
            if (h <= 6)
            {
                weightedSum += (p.frequency / (float) h) * p.magnitude;
                weightTotal += p.magnitude;
            }
            magnitude += p.magnitude;
            matchedIdx[numMatched++] = idx;
        }

        for (int i = 0; i < numMatched; ++i)
            peaks[(size_t) matchedIdx[i]].explained = true;

        return { weightTotal > 0.0f ? weightedSum / weightTotal : f0, magnitude };
    }
};
