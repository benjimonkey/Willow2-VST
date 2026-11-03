#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace WillowIDs;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout WillowAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto addF = [&] (const juce::String& id, const juce::String& name,
                     juce::NormalisableRange<float> range, float def, const juce::String& unit)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, range, def,
            juce::AudioParameterFloatAttributes().withLabel (unit)));
    };
    auto addB = [&] (const juce::String& id, const juce::String& name, bool def)
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def));
    };

    const char* names[3] = { "Low", "Mid", "High" };

    // ---- Module 2 ----
    addF ("inGain",   "Input Gain",   { -24.0f, 24.0f, 0.1f }, 0.0f, "dB");
    for (int b = 0; b < 3; ++b)
    {
        juce::String n (names[b]);
        addF (bandId (b, "Gain"),   n + " Gain",        { -24.0f, 24.0f, 0.1f }, 0.0f,  "dB");
        addF (bandId (b, "Thresh"), n + " Threshold",   { -60.0f, 0.0f, 0.1f },  -18.0f, "dB");
        addF (bandId (b, "Comp"),   n + " Compression", { 1.0f, 20.0f, 0.1f, 0.5f }, 1.0f, ":1");
    }
    addF ("outLevel", "Output Level", { -24.0f, 12.0f, 0.1f }, 0.0f, "dB");

    // ---- Module 1 (gates) ----
    addB ("gateMaster", "Gate Master", true);
    for (int b = 0; b < 3; ++b)
    {
        juce::String n (names[b]);
        addB (bandId (b, "GateOn"),        n + " Gate On", false);
        addF (bandId (b, "GateThresh"),    n + " Gate Threshold", { -90.0f, 0.0f, 0.1f },  -60.0f, "dB");
        addF (bandId (b, "GateRange"),     n + " Gate Range",     { -80.0f, 0.0f, 0.1f },  -40.0f, "dB");
        addF (bandId (b, "GateAttack"),    n + " Gate Attack",    { 0.1f, 100.0f, 0.1f, 0.4f }, 1.0f, "ms");
        addF (bandId (b, "GateRelease"),   n + " Gate Release",   { 5.0f, 1000.0f, 1.0f, 0.4f }, 100.0f, "ms");
    }

    // ---- Module 3 (tanh distortion, per band) ----
    for (int b = 0; b < 3; ++b)
        addF (bandId (b, "Dist"), juce::String (names[b]) + " Dist", { 0.0f, 100.0f, 0.1f }, 0.0f, "%");

    // ---- Crossover + stereo effect ----
    addF ("xoverLM", "Low/Mid Crossover",  { 40.0f, 800.0f, 1.0f, 0.4f },    200.0f,  "Hz");
    addF ("xoverMH", "Mid/High Crossover", { 800.0f, 12000.0f, 1.0f, 0.4f }, 2500.0f, "Hz");
    addF ("stereoMix", "Stereo Mix",       { 0.0f, 100.0f, 0.1f },           0.0f,    "%");

    return layout;
}

//==============================================================================
WillowAudioProcessor::WillowAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "WILLOW2_STATE", createLayout())
{
    for (int b = 0; b < numBands; ++b)
    {
        auto& p = bandPtrs[b];
        p.gain       = apvts.getRawParameterValue (bandId (b, "Gain"));
        p.thresh     = apvts.getRawParameterValue (bandId (b, "Thresh"));
        p.comp       = apvts.getRawParameterValue (bandId (b, "Comp"));
        p.gateOn     = apvts.getRawParameterValue (bandId (b, "GateOn"));
        p.gateThresh = apvts.getRawParameterValue (bandId (b, "GateThresh"));
        p.gateRange  = apvts.getRawParameterValue (bandId (b, "GateRange"));
        p.gateAtk    = apvts.getRawParameterValue (bandId (b, "GateAttack"));
        p.gateRel    = apvts.getRawParameterValue (bandId (b, "GateRelease"));
        p.dist       = apvts.getRawParameterValue (bandId (b, "Dist"));
    }
    inGainP     = apvts.getRawParameterValue ("inGain");
    outLevelP   = apvts.getRawParameterValue ("outLevel");
    xoverLMP    = apvts.getRawParameterValue ("xoverLM");
    xoverMHP    = apvts.getRawParameterValue ("xoverMH");
    gateMasterP = apvts.getRawParameterValue ("gateMaster");
    stereoMixP  = apvts.getRawParameterValue ("stereoMix");

    for (auto& band : scopePre)  for (auto& v : band) v.store (0.0f);
    for (auto& band : scopePost) for (auto& v : band) v.store (0.0f);
    for (auto& v : fftBuffer) v.store (0.0f);

    // periodic Hann window: sums to exactly 1 at 50% overlap
    for (int j = 0; j < SpectralChannel::N; ++j)
        hann[(size_t) j] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) j
                                                     / (float) SpectralChannel::N);

    setLatencySamples (SpectralChannel::N - 1);
}

bool WillowAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && l.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
}

void WillowAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = (float) sampleRate;
    gateDetRel = std::exp (-1.0f / (0.010f * sr));   // gate detector release 10 ms
    compAtk    = std::exp (-1.0f / (0.005f * sr));   // compressor attack 5 ms
    compRel    = std::exp (-1.0f / (0.080f * sr));   // compressor release 80 ms
    rmsCoef    = 1.0f - std::exp (-1.0f / (0.300f * sr)); // RMS window ~300 ms

    for (auto& b : bands) b.reset();
    xoverLM.reset(); xoverMH.reset(); xoverAP.reset();
    lastLM = lastMH = -1.0f;
    spec[0].reset(); spec[1].reset();

    inGainSm.reset (sampleRate, 0.02);  inGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inGainP->load()));
    outGainSm.reset (sampleRate, 0.02); outGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outLevelP->load()));
    stereoMixSm.reset (sampleRate, 0.05); stereoMixSm.setCurrentAndTargetValue (stereoMixP->load() * 0.01f);
    for (int b = 0; b < numBands; ++b)
    {
        bandGainSm[b].reset (sampleRate, 0.02);
        bandGainSm[b].setCurrentAndTargetValue (juce::Decibels::decibelsToGain (bandPtrs[b].gain->load()));
    }
    rmsStateL = rmsStateR = peakHoldL = peakHoldR = 0.0f;
    decCount = 0;
    for (int b = 0; b < numBands; ++b) accPre[b] = accPost[b] = 0.0f;

    setLatencySamples (SpectralChannel::N - 1);
}

//==============================================================================
void WillowAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2) return;

    if (resetPeaksFlag.exchange (false)) peakHoldL = peakHoldR = 0.0f;

    // ---------- fetch parameters once per block ----------
    inGainSm.setTargetValue  (juce::Decibels::decibelsToGain (inGainP->load()));
    outGainSm.setTargetValue (juce::Decibels::decibelsToGain (outLevelP->load()));
    stereoMixSm.setTargetValue (stereoMixP->load() * 0.01f);

    const float lm = xoverLMP->load();
    const float mh = std::max (xoverMHP->load(), lm * 1.5f);
    if (lm != lastLM || mh != lastMH)
    {
        xoverLM.setFreq (sr, lm);
        xoverMH.setFreq (sr, mh);
        xoverAP.setFreq (sr, mh);
        lastLM = lm; lastMH = mh;
    }

    struct BandParams { bool gateOn; float gateThr, gateRange, gateAtk, gateRel, compThr, ratio, distTarget; } bp[numBands];
    const bool master = gateMasterP->load() > 0.5f;
    for (int b = 0; b < numBands; ++b)
    {
        auto& p = bandPtrs[b];
        bp[b].gateOn    = master && p.gateOn->load() > 0.5f;
        bp[b].gateThr   = juce::Decibels::decibelsToGain (p.gateThresh->load());
        bp[b].gateRange = juce::Decibels::decibelsToGain (p.gateRange->load());
        bp[b].gateAtk   = std::exp (-1.0f / (p.gateAtk->load() * 0.001f * sr));
        bp[b].gateRel   = std::exp (-1.0f / (p.gateRel->load() * 0.001f * sr));
        bp[b].compThr   = p.thresh->load();
        bp[b].ratio     = juce::jmax (1.0f, p.comp->load());
        bp[b].distTarget = p.dist->load() * 0.01f;
        bandGainSm[b].setTargetValue (juce::Decibels::decibelsToGain (p.gain->load()));
    }

    float* chL = buffer.getWritePointer (0);
    float* chR = buffer.getWritePointer (1);

    int fftPos = fftWritePos.load();

    for (int i = 0; i < numSamples; ++i)
    {
        // 0/1. input from DAW -> Module 2 input gain
        const float ig = inGainSm.getNextValue();
        const float inL = chL[i] * ig;
        const float inR = chR[i] * ig;

        // 2. split into Low / Mid / High (LR4, phase aligned)
        float lowL, restL, lowR, restR;
        xoverLM.process (0, inL, lowL, restL);
        xoverLM.process (1, inR, lowR, restR);

        float midL, highL, midR, highR;
        xoverMH.process (0, restL, midL, highL);
        xoverMH.process (1, restR, midR, highR);

        {
            float a, b2;
            xoverAP.process (0, lowL, a, b2); lowL = a + b2;
            xoverAP.process (1, lowR, a, b2); lowR = a + b2;
        }

        float bl[numBands] = { lowL, midL, highL };
        float br[numBands] = { lowR, midR, highR };
        float outL = 0.0f, outR = 0.0f;

        for (int b = 0; b < numBands; ++b)
        {
            auto& s = bands[b];
            auto& p = bp[b];
            float l = bl[b], r = br[b];

            // 3. Module 1: gate (per band, stereo-linked)
            {
                const float det = std::max (std::abs (l), std::abs (r));
                s.gateEnv = det > s.gateEnv ? det : det + gateDetRel * (s.gateEnv - det);
                const float target = (p.gateOn && s.gateEnv < p.gateThr) ? p.gateRange : 1.0f;
                const float c = target > s.gateGain ? p.gateAtk : p.gateRel;
                s.gateGain = target + c * (s.gateGain - target);
                l *= s.gateGain; r *= s.gateGain;
            }

            // 4. Module 3: tanh distortion (per band). Amount 0 = exact bypass.
            {
                s.distAmt += 0.002f * (p.distTarget - s.distAmt); // de-zipper
                const float a = s.distAmt;
                if (a > 0.0001f)
                {
                    const float d = 1.0f + 19.0f * a;
                    const float inv = 1.0f / std::tanh (d);
                    l += a * (std::tanh (d * l) * inv - l);
                    r += a * (std::tanh (d * r) * inv - r);
                }
            }

            const float preMono = 0.5f * (l + r);

            // 5. Module 2: compression + band gain
            {
                const float det = std::max (std::abs (l), std::abs (r));
                const float lvl = juce::Decibels::gainToDecibels (det, -120.0f);
                const float gr  = lvl > p.compThr ? (p.compThr - lvl) * (1.0f - 1.0f / p.ratio) : 0.0f;
                const float c   = gr < s.grDb ? compAtk : compRel;
                s.grDb = gr + c * (s.grDb - gr);
                const float g = juce::Decibels::decibelsToGain (s.grDb) * bandGainSm[b].getNextValue();
                l *= g; r *= g;
            }

            // 6. Display 1 capture
            const float postMono = 0.5f * (l + r);
            if (std::abs (preMono)  > std::abs (accPre[b]))  accPre[b]  = preMono;
            if (std::abs (postMono) > std::abs (accPost[b])) accPost[b] = postMono;

            // 7. combine bands
            outL += l; outR += r;
        }

        if (++decCount >= decimation)
        {
            decCount = 0;
            const int w = scopeWritePos.load();
            for (int b = 0; b < numBands; ++b)
            {
                scopePre[b][(size_t) w].store (accPre[b], std::memory_order_relaxed);
                scopePost[b][(size_t) w].store (accPost[b], std::memory_order_relaxed);
                accPre[b] = accPost[b] = 0.0f;
            }
            scopeWritePos.store ((w + 1) % scopePoints);
        }

        // 8. output level
        const float og = outGainSm.getNextValue();
        outL *= og; outR *= og;

        // 9. stereo effect: left keeps odd FFT bins, right keeps even FFT bins
        {
            float dryL, dryR;
            const float wetL = spec[0].process (outL, dryL, hann);
            const float wetR = spec[1].process (outR, dryR, hann);
            const float m = stereoMixSm.getNextValue();
            outL = dryL + m * (wetL - dryL);
            outR = dryR + m * (wetR - dryR);
        }

        // Display 2 / HUD statistics taken at the final output
        fftBuffer[(size_t) fftPos].store (0.5f * (outL + outR), std::memory_order_relaxed);
        fftPos = (fftPos + 1) % fftSize;

        peakHoldL = std::max (peakHoldL, std::abs (outL));
        peakHoldR = std::max (peakHoldR, std::abs (outR));
        rmsStateL += rmsCoef * (outL * outL - rmsStateL);
        rmsStateR += rmsCoef * (outR * outR - rmsStateR);

        // 10. back to the DAW
        chL[i] = outL;
        chR[i] = outR;
    }

    fftWritePos.store (fftPos);
    peakL.store (peakHoldL);
    peakR.store (peakHoldR);
    rmsL.store (std::sqrt (rmsStateL));
    rmsR.store (std::sqrt (rmsStateR));
}

//==============================================================================
juce::AudioProcessorEditor* WillowAudioProcessor::createEditor()
{
    return new WillowAudioProcessorEditor (*this);
}

void WillowAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void WillowAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WillowAudioProcessor();
}
