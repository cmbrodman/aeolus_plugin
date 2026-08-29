#pragma once

#include <functional>
#include "aeolus/engine.h"
#include <map>

namespace ui {

class CouplerEditorComponent : public juce::Component
{
public:
    explicit CouplerEditorComponent(aeolus::Engine& engine);

    struct DivisionEdits
    {
        int sourceIndex = 0;
        bool hasBass = false;
        std::vector<aeolus::Division::LinkSpec> specs;
    };

    juce::Array<DivisionEdits> getAllEdits();

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
    void storeCurrent();
    int getSourceIndex() const;
    std::vector<aeolus::Division::LinkSpec> specsFromRows(int sourceIndex) const;

    int _shownSource { -1 };
    std::map<int, DivisionEdits> _pending;

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