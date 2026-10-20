// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <JuceHeader.h>

namespace ui {
class PerformanceIndicator final : public juce::Component {
public:
    PerformanceIndicator() { addAndMakeVisible(_label); _label.setFont(juce::Font(juce::FontOptions(11.0f))); }
    void update(float value, float peak, int warning, int critical, bool overload,
                const juce::String& text, const juce::String& detail) {
        _value = juce::jlimit(0.0f, 1.0f, value); _peak = juce::jlimit(0.0f, 1.0f, peak);
        _warning = warning / 100.0f; _critical = critical / 100.0f;
        _colour = overload || value >= _critical ? juce::Colours::red
            : value >= _warning ? juce::Colours::orange : juce::Colours::lightgreen;
        _label.setText(text, juce::dontSendNotification);
        _label.setColour(juce::Label::textColourId, _colour);
        _label.setTooltip(detail); repaint();
    }
    void resized() override { _label.setBounds(0, 0, getWidth(), getHeight() - 8); }
    void paint(juce::Graphics& g) override {
        auto bar = getLocalBounds().removeFromBottom(6).toFloat();
        g.setColour(juce::Colour(20, 20, 20)); g.fillRect(bar);
        g.setColour(_colour); g.fillRect(bar.withWidth(bar.getWidth() * _value));
        g.setColour(juce::Colours::orange); g.drawVerticalLine(int(bar.getWidth() * _warning), bar.getY(), bar.getBottom());
        g.setColour(juce::Colours::red); g.drawVerticalLine(int(bar.getWidth() * _critical), bar.getY(), bar.getBottom());
        g.setColour(juce::Colours::white); g.drawVerticalLine(juce::jlimit(0, getWidth()-1, int(bar.getWidth()*_peak)), bar.getY(), bar.getBottom());
    }
private:
    juce::Label _label;
    float _value = 0, _peak = 0, _warning = .75f, _critical = .9f;
    juce::Colour _colour = juce::Colours::lightgreen;
};
}
