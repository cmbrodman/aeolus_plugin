// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "PluginProcessor.h"

namespace ui {
class PerformanceSettingsComponent final : public juce::Component, private juce::Timer, private juce::ComponentListener {
public:
    enum class Page { VoiceCapacity, Meters, Diagnostics };
    PerformanceSettingsComponent(AeolusAudioProcessor&, Page, juce::Component* lifetimeOwner = nullptr);
    ~PerformanceSettingsComponent() override;
    void resized() override;
    std::function<void()> onClose;
private:
    void timerCallback() override;
    void componentBeingDeleted(juce::Component&) override;
    void apply();
    AeolusAudioProcessor& _processor;
    Page _page;
    juce::Component::SafePointer<juce::Component> _owner;
    bool _ownerDeleted = false;
    juce::Label _title, _description, _firstLabel, _secondLabel;
    juce::Slider _first, _second;
    juce::ToggleButton _poolDefault{"Use startup pool capacity"};
    juce::TextButton _apply{"Apply"}, _cancel{"Cancel"}, _reset{"Reset counters"};
};
}
