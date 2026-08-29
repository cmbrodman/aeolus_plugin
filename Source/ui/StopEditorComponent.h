#pragma once

#include <functional>
#include "aeolus/engine.h"

namespace ui {

struct StopFootage
{
    const char* label;
    int fn;
    int fd;
};

class StopEditorComponent : public juce::Component
{
public:
    explicit StopEditorComponent(aeolus::Engine& engine);

    int getDivisionIndex() const;
    int getStopCount() const;
    juce::StringArray getPipeNames() const; // empty string = unused row

    void resized() override;

    std::function<void()> onOk{};
    std::function<void()> onCancel{};

private:

    void rebuildRows();
    void loadDivisionIntoRows();
    void populateStopCombo(juce::ComboBox& box, const juce::String& selected);
    void syncFootageFromPipe(int row);
    void applyFootageChange(int row);
    juce::StringArray scanStopNames() const;
    static const StopFootage* footageFromFnFd(int fn, int fd);
    static juce::String footageSlug(const StopFootage& f);
    static juce::String currentPipeOfStop(const aeolus::Stop& stop);

    aeolus::Engine& _engine;
    juce::StringArray _stopNames;
    bool _updating { false };

    juce::Label _titleLabel;
    juce::Label _divisionLabel;
    juce::ComboBox _divisionBox;
    juce::Label _countLabel;
    juce::Slider _countSlider;
    juce::Label _stopsTitle;

    juce::Viewport _listViewport;
    juce::Component _list;

    juce::OwnedArray<juce::Label> _numLabels;
    juce::OwnedArray<juce::ComboBox> _stopBoxes;
    juce::OwnedArray<juce::ComboBox> _footageBoxes;

    juce::TextButton _okButton;
    juce::TextButton _cancelButton;
};

} // namespace ui