#include "PluginEditor.h"

using namespace WillowColours;
using namespace WillowIDs;

static juce::String dbText (float lin)
{
    if (lin < 0.00001f) return "-inf";
    return juce::String (juce::Decibels::gainToDecibels (lin), 1);
}

//==============================================================================
// Display 1
void ScopeDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, 6.0f);

    g.setColour (text);
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.drawText ("DISPLAY 1  -  Time domain, before (grey) / after (colour) compression",
                r.reduced (8, 4).removeFromTop (16), juce::Justification::centredLeft);

    auto area = r.reduced (8.0f).withTrimmedTop (20.0f);
    const float rowH = area.getHeight() / 3.0f;
    const int N = WillowAudioProcessor::scopePoints;
    const int w = proc.scopeWritePos.load();
    const char* names[3] = { "LOW", "MID", "HIGH" };

    for (int b = 0; b < 3; ++b)
    {
        auto row = area.removeFromTop (rowH).reduced (0.0f, 2.0f);
        g.setColour (bg);
        g.fillRect (row);
        g.setColour (panelHi);
        g.drawHorizontalLine ((int) row.getCentreY(), row.getX(), row.getRight());

        auto draw = [&] (const auto& arr, juce::Colour c, float thickness)
        {
            juce::Path p;
            for (int i = 0; i < N; ++i)
            {
                const float v = juce::jlimit (-1.0f, 1.0f, arr[b][(size_t) ((w + i) % N)].load (std::memory_order_relaxed));
                const float x = row.getX() + row.getWidth() * (float) i / (float) (N - 1);
                const float y = row.getCentreY() - v * row.getHeight() * 0.5f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (thickness));
        };
        draw (proc.scopePre,  juce::Colours::grey.withAlpha (0.8f), 1.0f);
        draw (proc.scopePost, band[b], 1.5f);

        g.setColour (band[b]);
        g.setFont (11.0f);
        g.drawText (names[b], row.reduced (4.0f, 2.0f), juce::Justification::topLeft);
    }
}

//==============================================================================
// Display 2
SpectrumDisplay::SpectrumDisplay (WillowAudioProcessor& p) : proc (p)
{
    smoothed.fill (-120.0f);
    startTimerHz (30);
}

void SpectrumDisplay::timerCallback()
{
    constexpr int N = WillowAudioProcessor::fftSize;
    const int w = proc.fftWritePos.load();
    for (int i = 0; i < N; ++i)
        fftData[(size_t) i] = proc.fftBuffer[(size_t) ((w + i) % N)].load (std::memory_order_relaxed);
    std::fill (fftData.begin() + N, fftData.end(), 0.0f);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) N);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    for (int i = 0; i < N / 2; ++i)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] / (N / 4.0f), -120.0f);
        auto& s = smoothed[(size_t) i];
        s += (db - s) * (db > s ? 0.6f : 0.15f);
    }
    repaint();
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, 6.0f);

    g.setColour (text);
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.drawText ("DISPLAY 2  -  Output FFT", r.reduced (8, 4).removeFromTop (16), juce::Justification::centredLeft);

    auto plot = r.reduced (8.0f).withTrimmedTop (20.0f).withTrimmedBottom (14.0f);
    g.setColour (bg);
    g.fillRect (plot);

    // grid
    g.setFont (9.0f);
    const float freqs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    for (float f : freqs)
    {
        const float x = plot.getX() + plot.getWidth() * std::log10 (f / 20.0f) / 3.0f;
        g.setColour (panelHi);
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (text.withAlpha (0.6f));
        g.drawText (f >= 1000 ? juce::String (f / 1000.0f, 0) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> (x - 15, plot.getBottom(), 30, 12), juce::Justification::centred);
    }
    for (int db = -80; db <= 0; db += 20)
    {
        const float y = plot.getBottom() - plot.getHeight() * (float) (db + 90) / 90.0f;
        g.setColour (panelHi);
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (text.withAlpha (0.6f));
        g.drawText (juce::String (db), juce::Rectangle<float> (plot.getX() + 2, y - 10, 30, 10), juce::Justification::left);
    }

    // spectrum
    const float sr = (float) juce::jmax (8000.0, proc.getSampleRate());
    const int bins = WillowAudioProcessor::fftSize / 2;
    juce::Path p;
    const int W = (int) plot.getWidth();
    for (int px = 0; px <= W; ++px)
    {
        const float f = 20.0f * std::pow (1000.0f, (float) px / (float) W);
        const float binF = juce::jlimit (0.0f, (float) (bins - 2), f * (float) WillowAudioProcessor::fftSize / sr);
        const int b0 = (int) binF;
        const float frac = binF - (float) b0;
        const float db = smoothed[(size_t) b0] * (1.0f - frac) + smoothed[(size_t) b0 + 1] * frac;
        const float y = plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f);
        const float x = plot.getX() + (float) px;
        if (px == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    juce::Path fill (p);
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.lineTo (plot.getX(), plot.getBottom());
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.25f));
    g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (p, juce::PathStrokeType (1.5f));

    // HUD
    auto hud = juce::Rectangle<float> (plot.getRight() - 215.0f, plot.getY() + 6.0f, 209.0f, 62.0f);
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.fillRoundedRectangle (hud, 5.0f);
    g.setColour (accent);
    g.drawRoundedRectangle (hud, 5.0f, 1.0f);
    g.setColour (text);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    auto h = hud.reduced (6.0f, 4.0f);
    g.drawText ("MAX  L " + dbText (proc.peakL.load()) + "  R " + dbText (proc.peakR.load()) + " dBFS",
                h.removeFromTop (16.0f), juce::Justification::centredLeft);
    g.drawText ("RMS  L " + dbText (proc.rmsL.load()) + "  R " + dbText (proc.rmsR.load()) + " dBFS",
                h.removeFromTop (16.0f), juce::Justification::centredLeft);
    g.setColour (text.withAlpha (0.5f));
    g.setFont (9.0f);
    g.drawText ("click display to reset max", h, juce::Justification::centredLeft);
}

//==============================================================================
// Slider helper
WillowAudioProcessorEditor::LabelledSlider::LabelledSlider (juce::Component& parent,
                                                           juce::AudioProcessorValueTreeState& apvts,
                                                           const juce::String& id, const juce::String& text,
                                                           juce::Colour colour, Style styleIn)
    : style (styleIn)
{
    const bool horizontal = (style == Style::horizontal);
    if (style == Style::horizontal)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 18);
    }
    else if (style == Style::rotary)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 16);
        slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
        slider.setColour (juce::Slider::rotarySliderOutlineColourId, bg);
    }
    else
    {
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
    }
    slider.setColour (juce::Slider::thumbColourId, colour);
    slider.setColour (juce::Slider::trackColourId, colour.withAlpha (0.7f));
    slider.setColour (juce::Slider::backgroundColourId, bg);
    slider.setColour (juce::Slider::textBoxTextColourId, WillowColours::text);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (horizontal ? juce::Justification::centredLeft : juce::Justification::centred);
    label.setFont (juce::Font (10.5f));
    label.setColour (juce::Label::textColourId, WillowColours::text);

    parent.addAndMakeVisible (slider);
    parent.addAndMakeVisible (label);
    attachment = std::make_unique<SA> (apvts, id, slider);
}

void WillowAudioProcessorEditor::LabelledSlider::layout (juce::Rectangle<int> r)
{
    if (style == Style::horizontal)
    {
        label.setBounds (r.removeFromLeft (120));
        slider.setBounds (r);
    }
    else if (style == Style::rotary)
    {
        label.setBounds (r.removeFromBottom (18));
        slider.setBounds (r);
    }
    else
    {
        label.setBounds (r.removeFromTop (30));
        slider.setBounds (r);
    }
}

//==============================================================================
WillowAudioProcessorEditor::WillowAudioProcessorEditor (WillowAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), scope (p), spectrum (p)
{
    auto& apvts = proc.apvts;

    // ---- Module 2: 11 sliders in the requested order ----
    struct Def { const char* id; const char* text; int bandIdx; };
    const Def defs[] = {
        { "inGain",    "Input Gain",       -1 },
        { "lowGain",   "Low Gain",          0 }, { "lowThresh",  "Low Threshold",    0 }, { "lowComp",  "Low Compression",  0 },
        { "midGain",   "Mid Gain",          1 }, { "midThresh",  "Mid Threshold",    1 }, { "midComp",  "Mid Compression",  1 },
        { "highGain",  "High Gain",         2 }, { "highThresh", "High Threshold",   2 }, { "highComp", "High Compression", 2 },
        { "outLevel",  "Output Level",     -1 }
    };
    for (auto& d : defs)
        comp2Sliders.push_back (std::make_unique<LabelledSlider> (*this, apvts, d.id, d.text,
                                                                  d.bandIdx < 0 ? accent : band[d.bandIdx]));

    // ---- Module 1: gate sliders + buttons per band ----
    const char* bn[3] = { "LOW", "MID", "HIGH" };
    for (int b = 0; b < 3; ++b)
    {
        gateSliders[(size_t) b].push_back (std::make_unique<LabelledSlider> (*this, apvts, bandId (b, "GateThresh"),  "Gate Threshold", band[b]));
        gateSliders[(size_t) b].push_back (std::make_unique<LabelledSlider> (*this, apvts, bandId (b, "GateRange"),   "Gate Range",     band[b]));
        gateSliders[(size_t) b].push_back (std::make_unique<LabelledSlider> (*this, apvts, bandId (b, "GateAttack"),  "Gate Attack",    band[b]));
        gateSliders[(size_t) b].push_back (std::make_unique<LabelledSlider> (*this, apvts, bandId (b, "GateRelease"), "Gate Release",   band[b]));

        gateTitle[b].setText (juce::String (bn[b]) + " GATE", juce::dontSendNotification);
        gateTitle[b].setJustificationType (juce::Justification::centredLeft);
        gateTitle[b].setFont (juce::Font (12.0f, juce::Font::bold));
        gateTitle[b].setColour (juce::Label::textColourId, band[b]);
        addAndMakeVisible (gateTitle[b]);

        gateOn[b].setButtonText ("Gate On");
        gateOn[b].setColour (juce::ToggleButton::textColourId, text);
        gateOn[b].setColour (juce::ToggleButton::tickColourId, band[b]);
        addAndMakeVisible (gateOn[b]);
        gateOnAtt[b] = std::make_unique<BA> (apvts, bandId (b, "GateOn"), gateOn[b]);
    }
    gateMaster.setButtonText ("Gate Master");
    gateMaster.setColour (juce::ToggleButton::textColourId, text);
    gateMaster.setColour (juce::ToggleButton::tickColourId, accent);
    addAndMakeVisible (gateMaster);
    gateMasterAtt = std::make_unique<BA> (apvts, "gateMaster", gateMaster);

    // ---- Module 3: distortion knobs ----
    const char* distText[3] = { "Low Dist", "Mid Dist", "High Dist" };
    for (int b = 0; b < 3; ++b)
        distSliders.push_back (std::make_unique<LabelledSlider> (*this, apvts, bandId (b, "Dist"),
                                                                 distText[b], band[b], Style::rotary));

    // ---- Crossover + stereo mix ----
    xoLM = std::make_unique<LabelledSlider> (*this, apvts, "xoverLM", "Low / Mid crossover",  accent, Style::horizontal);
    xoMH = std::make_unique<LabelledSlider> (*this, apvts, "xoverMH", "Mid / High crossover", accent, Style::horizontal);
    stereoMix = std::make_unique<LabelledSlider> (*this, apvts, "stereoMix", "Stereo Mix", accent, Style::horizontal);

    addAndMakeVisible (scope);
    addAndMakeVisible (spectrum);

    setSize (1180, 790);
}

void WillowAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    g.setColour (accent);
    g.setFont (juce::Font (26.0f, juce::Font::bold));
    g.drawText ("Willow2", 14, 6, 200, 36, juce::Justification::centredLeft);
    g.setColour (text.withAlpha (0.6f));
    g.setFont (11.0f);
    g.drawText ("3-band gate + distortion + compressor", 130, 14, 300, 20, juce::Justification::centredLeft);

    for (auto* a : { &module2Area, &module1Area, &module3Area, &xoverArea })
    {
        g.setColour (panel);
        g.fillRoundedRectangle (a->toFloat(), 6.0f);
    }
    g.setColour (text);
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.drawText ("MODULE 2  -  Gain / Threshold / Compression", module2Area.reduced (8, 4).removeFromTop (18),
                juce::Justification::centredLeft);
    g.drawText ("MODULE 1  -  Gating (per band)", module1Area.reduced (8, 4).removeFromTop (18),
                juce::Justification::centredLeft);
    g.drawText ("MODULE 3  -  Distortion (tanh)", module3Area.reduced (8, 4).removeFromTop (18),
                juce::Justification::centredLeft);
    g.drawText ("CROSSOVER / STEREO EFFECT", xoverArea.reduced (8, 4).removeFromTop (18),
                juce::Justification::centredLeft);
}

void WillowAudioProcessorEditor::resized()
{
    module2Area = { 10, 50, 610, 300 };
    module1Area = { 10, 360, 610, 270 };
    module3Area = { 10, 640, 250, 140 };
    xoverArea   = { 270, 640, 350, 140 };

    // Module 2: 11 columns
    {
        auto inner = module2Area.reduced (8).withTrimmedTop (22);
        const int colW = inner.getWidth() / 11;
        for (auto& s : comp2Sliders)
            s->layout (inner.removeFromLeft (colW));
    }

    // Module 1: 3 bands x (title, button, 4 sliders)
    {
        gateMaster.setBounds (module1Area.getRight() - 130, module1Area.getY() + 4, 120, 20);
        auto inner = module1Area.reduced (8).withTrimmedTop (22);
        const int colW = inner.getWidth() / 3;
        for (int b = 0; b < 3; ++b)
        {
            auto col = inner.removeFromLeft (colW).reduced (4, 0);
            auto top = col.removeFromTop (24);
            gateTitle[b].setBounds (top.removeFromLeft (70));
            gateOn[b].setBounds (top);
            const int sw = col.getWidth() / 4;
            for (auto& s : gateSliders[(size_t) b])
                s->layout (col.removeFromLeft (sw));
        }
    }

    // Module 3: three rotary knobs
    {
        auto inner = module3Area.reduced (8).withTrimmedTop (18);
        const int colW = inner.getWidth() / 3;
        for (auto& s : distSliders)
            s->layout (inner.removeFromLeft (colW));
    }

    // Crossover + stereo mix: three horizontal rows
    {
        auto inner = xoverArea.reduced (8, 4).withTrimmedTop (20);
        const int rowH = inner.getHeight() / 3;
        xoLM->layout (inner.removeFromTop (rowH));
        xoMH->layout (inner.removeFromTop (rowH));
        stereoMix->layout (inner);
    }

    scope.setBounds (630, 50, 540, 330);
    spectrum.setBounds (630, 390, 540, 390);
}
