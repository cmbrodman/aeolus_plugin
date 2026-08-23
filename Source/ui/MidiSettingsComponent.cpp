#include "ui/MidiSettingsComponent.h"

using namespace juce;

namespace ui {

MidiSettingsComponent::MidiSettingsComponent(aeolus::Engine& engine, AudioDeviceManager* deviceManager)
    : _engine(engine)
    , _deviceManager(deviceManager)
    , _titleLabel({}, "MIDI Settings")
    , _inputsLabel({}, "Active MIDI inputs")
    , _channelsLabel({}, "Division MIDI channels")
    , _controlLabel({}, "Control")
    , _swellLabel({}, "Swell")
    , _closeButton("Close")
{
    addAndMakeVisible(_titleLabel);
    _titleLabel.setJustificationType(Justification::centred);
    _titleLabel.setColour(Label::textColourId, Colours::lightyellow);
    auto font = _titleLabel.getFont();
    font.setHeight(font.getHeight() * 1.3f);
    _titleLabel.setFont(font);

    addAndMakeVisible(_inputsLabel);
    addAndMakeVisible(_channelsLabel);
    addAndMakeVisible(_controlLabel);
    addAndMakeVisible(_swellLabel);
    addAndMakeVisible(_controlChannels);
    addAndMakeVisible(_swellChannels);
    addAndMakeVisible(_closeButton);

    // Active MIDI inputs
    if (_deviceManager != nullptr)
    {
        for (auto& device : MidiInput::getAvailableDevices())
        {
            auto* toggle = _midiInputToggles.add(new ToggleButton(device.name));
            toggle->setToggleState(_deviceManager->isMidiInputDeviceEnabled(device.identifier), dontSendNotification);
            toggle->onClick = [this, id = device.identifier, toggle]()
            {
                if (_deviceManager != nullptr)
                    _deviceManager->setMidiInputDeviceEnabled(id, toggle->getToggleState());
            };
            addAndMakeVisible(toggle);
        }
    }

    // Per-division channel selectors (use real names from the organ)
    rebuildDivisionSelectors();

    // Global Control / Swell
    _controlChannels.currentChannelsMaskProvider = [this]() -> int {
        return _engine.getMIDIControlChannelsMask();
    };
    _controlChannels.onChannelsSelectionChanged = [this](int mask) {
        _engine.setMIDIControlChannelsMask(mask);
    };
    _controlChannels.updateLabel();

    _swellChannels.currentChannelsMaskProvider = [this]() -> int {
        return _engine.getMIDISwellChannelsMask();
    };
    _swellChannels.onChannelsSelectionChanged = [this](int mask) {
        _engine.setMIDISwellChannelsMask(mask);
    };
    _swellChannels.updateLabel();

    _closeButton.onClick = [this]() {
        if (onClose) onClose();
    };
    _closeButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
}

void MidiSettingsComponent::rebuildDivisionSelectors()
{
    _divisionLabels.clear();
    _divisionSelectors.clear();

    for (int i = 0; i < _engine.getDivisionCount(); ++i)
    {
        auto* div = _engine.getDivisionByIndex(i);
        if (div == nullptr)
            continue;

        auto* label = _divisionLabels.add(new Label({}, div->getName()));
        addAndMakeVisible(label);

        auto* selector = _divisionSelectors.add(new MidiChannelsComponent());
        selector->currentChannelsMaskProvider = [div]() -> int {
            return div->getMIDIChannelsMask();
        };
        selector->onChannelsSelectionChanged = [div](int mask) {
            div->setMIDIChannelsMask(mask);
        };
        selector->updateLabel();
        addAndMakeVisible(selector);
    }
}

void MidiSettingsComponent::resized()
{
    constexpr int margin = 10;
    constexpr int rowH = 28;
    auto bounds = getLocalBounds().reduced(margin);

    _titleLabel.setBounds(bounds.removeFromTop(28));
    bounds.removeFromTop(margin);

    _inputsLabel.setBounds(bounds.removeFromTop(22));
    for (auto* toggle : _midiInputToggles)
    {
        toggle->setBounds(bounds.removeFromTop(rowH).reduced(4, 2));
    }

    bounds.removeFromTop(margin);
    _channelsLabel.setBounds(bounds.removeFromTop(22));

    const int n = jmin(_divisionLabels.size(), _divisionSelectors.size());
    for (int i = 0; i < n; ++i)
    {
        auto row = bounds.removeFromTop(rowH);
        _divisionLabels[i]->setBounds(row.removeFromLeft(120));
        _divisionSelectors[i]->setBounds(row);
    }

    bounds.removeFromTop(margin);

    {
        auto row = bounds.removeFromTop(rowH);
        _controlLabel.setBounds(row.removeFromLeft(120));
        _controlChannels.setBounds(row);
    }
    {
        auto row = bounds.removeFromTop(rowH);
        _swellLabel.setBounds(row.removeFromLeft(120));
        _swellChannels.setBounds(row);
    }

    bounds.removeFromTop(margin);
    _closeButton.setBounds(bounds.removeFromBottom(28).removeFromRight(80));
}

void MidiSettingsComponent::paint(Graphics& g)
{
    g.fillAll(Colour(0x2A, 0x2A, 0x2A));
}

} // namespace ui