#pragma once

#include <functional>
#include "aeolus/engine.h"

namespace ui {

class CouplerEditorComponent : public juce::Component
{
public:
    explicit CouplerEditorComponent(aeolus::Engine& engine);

    int getSourceIndex() const;
    bool getHasBassCoupler() const;
    std::vector<aeolus::Division::LinkSpec> getSpecs() const;

    void resized() override;

    std::function<void()> onOk{};
    std::function<void()> onCancel{};

private:
    struct DestRow
    {
        aeolus::Division* dest = nullptr;
        juce::Label name;
        juce::ToggleButton unison { "Unison" };
        juce::ToggleButton super { "Super 4'" };
        juce::ToggleButton sub { "Sub 16'" };
        juce::ToggleButton pass { "Pass-through" };
    };

    void loadSource();

    aeolus::Engine& _engine;
    juce::Label _titleLabel;
    juce::Label _divisionLabel;
    juce::ComboBox _divisionBox;
    juce::ToggleButton _bassButton { "Bass coupler" };
    juce::Label _destTitle;
    juce::OwnedArray<DestRow> _rows;
    juce::TextButton _okButton { "OK" };
    juce::TextButton _cancelButton { "Cancel" };
};

} // namespace ui