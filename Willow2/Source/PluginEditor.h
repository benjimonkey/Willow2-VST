#pragma once
#include "PluginProcessor.h"

namespace WillowColours
{
    const juce::Colour bg      { 0xff14181a };
    const juce::Colour panel   { 0xff1e2427 };
    const juce::Colour panelHi { 0xff2a3236 };
    const juce::Colour accent  { 0xff9fc27a };
    const juce::Colour text    { 0xffd8e0d4 };
    const juce::Colour band[3] = { juce::Colour (0xffe8a14a), juce::Colour (0xff7ec27a), juce::Colour (0xff5aa9e6) };
}

//==============================================================================
// Display 1: time-domain, before/after compression, per band
class ScopeDisplay : public juce::Component, private juce::Timer
{
public:
    explicit ScopeDisplay (WillowAudioProcessor& p) : proc (p) { startTimerHz (30); }
    void paint (juce::Graphics& g) override;
private:
    void timerCallback() override { repaint(); }
    WillowAudioProcessor& proc;
};

//==============================================================================
// Display 2: output FFT with HUD (click to reset peak hold)
class SpectrumDisplay : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumDisplay (WillowAudioProcessor& p);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override { proc.resetPeaks(); }
private:
    void timerCallback() override;
    WillowAudioProcessor& proc;
    juce::dsp::FFT fft { WillowAudioProcessor::fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) WillowAudioProcessor::fftSize,
                                                 juce::dsp::WindowingFunction<float>::hann };
    std::array<float, 2 * WillowAudioProcessor::fftSize> fftData {};
    std::array<float, WillowAudioProcessor::fftSize / 2> smoothed {};
};

//==============================================================================
class WillowAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit WillowAudioProcessorEditor (WillowAudioProcessor&);
    ~WillowAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

    enum class Style { vertical, horizontal, rotary };

    struct LabelledSlider
    {
        LabelledSlider (juce::Component& parent, juce::AudioProcessorValueTreeState& apvts,
                        const juce::String& id, const juce::String& text,
                        juce::Colour colour, Style style = Style::vertical);
        void layout (juce::Rectangle<int> r);

        Style style;
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SA> attachment; // must stay last (destroyed first)
    };

    WillowAudioProcessor& proc;

    // Module 2
    std::vector<std::unique_ptr<LabelledSlider>> comp2Sliders;
    // Module 1
    std::array<std::vector<std::unique_ptr<LabelledSlider>>, 3> gateSliders;
    juce::Label gateTitle[3];
    juce::ToggleButton gateOn[3], gateMaster;
    // Module 3 (distortion knobs)
    std::vector<std::unique_ptr<LabelledSlider>> distSliders;
    // Crossover + stereo mix
    std::unique_ptr<LabelledSlider> xoLM, xoMH, stereoMix;

    ScopeDisplay scope;
    SpectrumDisplay spectrum;

    std::unique_ptr<BA> gateOnAtt[3], gateMasterAtt;

    juce::Rectangle<int> module2Area, module1Area, module3Area, xoverArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WillowAudioProcessorEditor)
};
