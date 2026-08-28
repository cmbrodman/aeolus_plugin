#pragma once

#include <functional>
#include "aeolus/engine.h"

namespace ui {

class DivisionEditorComponent : public juce::Component
{
public:
    explicit DivisionEditorComponent(aeolus::Engine& engine);

    int getCount() const;
    juce::StringArray getNames() const;

    void resized() override;

    std::function<void()> onOk{};
    std::function<void()> onCancel{};

private:
    void rebuildNameRows();
    static juce::String defaultName(int indexZeroBased);

    aeolus::Engine& _engine;

    juce::Label _titleLabel;
    juce::Label _countLabel;
    juce::Slider _countSlider;

    juce::OwnedArray<juce::Label> _nameLabels;
    juce::OwnedArray<juce::TextEditor> _nameEditors;

    juce::TextButton _okButton;
    juce::TextButton _cancelButton;
};

} // namespace ui