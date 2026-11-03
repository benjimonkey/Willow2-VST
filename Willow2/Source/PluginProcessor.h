#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>

//==============================================================================
// Parameter ID helpers.  Band index: 0 = low, 1 = mid, 2 = high
namespace WillowIDs
{
    inline const char* const bandPrefix[3] = { "low", "mid", "high" };
    inline juce::String bandId (int band, const char* suffix)
    {
        return juce::String (bandPrefix[band]) + suffix;
    }
}

//==============================================================================
// 4th-order Linkwitz-Riley building blocks
class Biquad
{
public:
    void set (float sampleRate, float freq, float q, bool highpass)
    {
        const float w0 = juce::MathConstants<float>::twoPi * freq / sampleRate;
        const float cw = std::cos (w0);
        const float alpha = std::sin (w0) / (2.0f * q);
        const float a0 = 1.0f + alpha;
        if (highpass)
        {
            b0 = ((1.0f + cw) * 0.5f) / a0;
            b1 = -(1.0f + cw) / a0;
            b2 = b0;
        }
        else
        {
            b0 = ((1.0f - cw) * 0.5f) / a0;
            b1 = (1.0f - cw) / a0;
            b2 = b0;
        }
        a1 = (-2.0f * cw) / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0f; }
private:
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

struct Crossover
{
    Biquad lp[2][2], hp[2][2]; // [channel][stage]

    void setFreq (float sr, float f)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < 2; ++s)
            {
                lp[ch][s].set (sr, f, 0.70710678f, false);
                hp[ch][s].set (sr, f, 0.70710678f, true);
            }
    }
    void process (int ch, float x, float& low, float& high)
    {
        low  = lp[ch][1].process (lp[ch][0].process (x));
        high = hp[ch][1].process (hp[ch][0].process (x));
    }
    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < 2; ++s) { lp[ch][s].reset(); hp[ch][s].reset(); }
    }
};

//==============================================================================
// Stereo effect helper: STFT (Hann, 50% overlap) that keeps only the odd OR
// only the even FFT bins of one channel.  Latency = N - 1 samples.
struct SpectralChannel
{
    static constexpr int order = 10;
    static constexpr int N = 1 << order;   // 1024
    static constexpr int H = N / 2;        // hop size

    explicit SpectralChannel (bool keepOddBins = true) : keepOdd (keepOddBins) {}

    void reset()
    {
        inRing.fill (0.0f);
        outRing.fill (0.0f);
        work.fill (0.0f);
        pos = 0;
        hop = 0;
    }

    // Returns the filtered sample; dryDelayed receives the unfiltered sample
    // delayed by the same latency (so the two can be mixed in phase).
    float process (float x, float& dryDelayed, const std::array<float, N>& window)
    {
        inRing[(size_t) pos] = x;
        if (++hop >= H)
        {
            hop = 0;
            processFrame (window);
        }
        const int rd = (pos + 1) % N;
        const float wet = outRing[(size_t) rd];
        outRing[(size_t) rd] = 0.0f;
        dryDelayed = inRing[(size_t) rd];
        pos = rd;
        return wet;
    }

private:
    void processFrame (const std::array<float, N>& window)
    {
        for (int j = 0; j < N; ++j)
            work[(size_t) j] = inRing[(size_t) ((pos + 1 + j) % N)] * window[(size_t) j];
        std::fill (work.begin() + N, work.end(), 0.0f);

        fft.performRealOnlyForwardTransform (work.data(), true);

        for (int k = 0; k <= N / 2; ++k)
        {
            const bool isOdd = (k & 1) != 0;
            if (isOdd != keepOdd)
            {
                work[(size_t) (2 * k)]     = 0.0f;
                work[(size_t) (2 * k + 1)] = 0.0f;
            }
        }

        fft.performRealOnlyInverseTransform (work.data());

        for (int j = 0; j < N; ++j)
            outRing[(size_t) ((pos + 1 + j) % N)] += work[(size_t) j];
    }

    juce::dsp::FFT fft { order };
    std::array<float, N> inRing {}, outRing {};
    std::array<float, 2 * N> work {};
    int pos = 0, hop = 0;
    bool keepOdd;
};

//==============================================================================
class WillowAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int numBands    = 3;
    static constexpr int scopePoints = 512;
    static constexpr int fftOrder    = 11;
    static constexpr int fftSize     = 1 << fftOrder;

    WillowAudioProcessor();
    ~WillowAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Willow2"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    //==== Data shared with the editor (written by audio thread, read by UI) ====
    std::array<std::array<std::atomic<float>, scopePoints>, numBands> scopePre, scopePost;
    std::atomic<int> scopeWritePos { 0 };

    std::array<std::atomic<float>, fftSize> fftBuffer;
    std::atomic<int> fftWritePos { 0 };

    std::atomic<float> peakL { 0 }, peakR { 0 }, rmsL { 0 }, rmsR { 0 };
    std::atomic<bool> resetPeaksFlag { false };
    void resetPeaks() { resetPeaksFlag.store (true); }

private:
    struct BandPtrs
    {
        std::atomic<float> *gain, *thresh, *comp, *gateOn, *gateThresh, *gateRange,
                           *gateAtk, *gateRel, *dist;
    };
    BandPtrs bandPtrs[numBands];
    std::atomic<float> *inGainP, *outLevelP, *xoverLMP, *xoverMHP, *gateMasterP, *stereoMixP;

    struct BandState
    {
        float gateEnv = 0.0f, gateGain = 1.0f, grDb = 0.0f, distAmt = 0.0f;
        void reset() { gateEnv = 0.0f; gateGain = 1.0f; grDb = 0.0f; distAmt = 0.0f; }
    };
    BandState bands[numBands];

    Crossover xoverLM, xoverMH, xoverAP; // AP = allpass-compensation of the low band
    float lastLM = -1.0f, lastMH = -1.0f;

    SpectralChannel spec[2] { SpectralChannel (true), SpectralChannel (false) }; // L = odd bins, R = even bins
    std::array<float, SpectralChannel::N> hann {};

    juce::SmoothedValue<float> inGainSm, outGainSm, stereoMixSm, bandGainSm[numBands];

    float sr = 44100.0f;
    float gateDetRel = 0.99f, compAtk = 0.99f, compRel = 0.999f;
    float rmsStateL = 0.0f, rmsStateR = 0.0f, rmsCoef = 0.0001f;
    float peakHoldL = 0.0f, peakHoldR = 0.0f;

    int decCount = 0;
    float accPre[numBands] {}, accPost[numBands] {};
    static constexpr int decimation = 32;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WillowAudioProcessor)
};
