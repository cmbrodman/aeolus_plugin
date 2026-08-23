#pragma once

#include "aeolus/globals.h"
#include "aeolus/engine.h"
#include "ui/MidiChannelsComponent.h"

namespace ui {

class MidiSettingsComponent : public juce::Component
{
public:
    MidiSettingsComponent(aeolus::Engine& engine, juce::AudioDeviceManager* deviceManager = nullptr);

    void resized() override;
    void paint(juce::Graphics& g) override;

    std::function<void()> onClose{};

private:
    void rebuildDivisionSelectors();

    aeolus::Engine& _engine;
    juce::AudioDeviceManager* _deviceManager;

    juce::Label _titleLabel;
    juce::Label _inputsLabel;
    juce::OwnedArray<juce::ToggleButton> _midiInputToggles;

    juce::Label _channelsLabel;
    juce::OwnedArray<juce::Label> _divisionLabels;
    juce::OwnedArray<MidiChannelsComponent> _divisionSelectors;

    juce::Label _controlLabel;
    MidiChannelsComponent _controlChannels;

    juce::Label _swellLabel;
    MidiChannelsComponent _swellChannels;

    juce::TextButton _closeButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiSettingsComponent)
};

} // namespace ui