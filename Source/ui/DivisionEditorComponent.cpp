#include "ui/DivisionEditorComponent.h"

using namespace juce;

namespace ui {

namespace {
    const char* roman[] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X" };
}

String DivisionEditorComponent::defaultName(int indexZeroBased)
{
    return String("Division ") + roman[jlimit(0, 9, indexZeroBased)];
}

DivisionEditorComponent::DivisionEditorComponent(aeolus::Engine& engine)
    : _engine(engine)
    , _titleLabel{{}, "Division Editor"}
    , _countLabel{{}, "Number of divisions"}
    , _countSlider{Slider::IncDecButtons, Slider::TextBoxLeft}
    , _okButton{"OK"}
    , _cancelButton{"Cancel"}
{
    addAndMakeVisible(_titleLabel);
    _titleLabel.setJustificationType(Justification::centred);
    _titleLabel.setColour(Label::textColourId, Colours::lightyellow);
    auto font = _titleLabel.getFont();
    font.setHeight(font.getHeight() * 1.2f);
    _titleLabel.setFont(font);

    addAndMakeVisible(_countLabel);
    addAndMakeVisible(_countSlider);
    _countSlider.setTextBoxStyle(Slider::TextBoxLeft, false, 50, 20);
    _countSlider.setRange(1, 10, 1);
    _countSlider.setValue(_engine.getDivisionCount(), dontSendNotification);
    _countSlider.onValueChange = [this] { rebuildNameRows(); };

    for (int i = 0; i < 10; ++i)
    {
        auto* lab = _nameLabels.add(new Label({}, "Division " + String(roman[i]) + ":"));
        auto* ed  = _nameEditors.add(new TextEditor());
        lab->setJustificationType(Justification::centredLeft);
        ed->setTextToShowWhenEmpty(defaultName(i), Colours::grey);
        addChildComponent(lab);
        addChildComponent(ed);
    }

    const int n = jlimit(1, 10, _engine.getDivisionCount());
    for (int i = 0; i < n; ++i)
        _nameEditors[i]->setText(_engine.getDivisionByIndex(i)->getName(), dontSendNotification);

    addAndMakeVisible(_okButton);
    addAndMakeVisible(_cancelButton);
    _okButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _cancelButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _okButton.onClick = [this] { if (onOk) onOk(); };
    _cancelButton.onClick = [this] { if (onCancel) onCancel(); };

    rebuildNameRows();
}

int DivisionEditorComponent::getCount() const
{
    return (int)_countSlider.getValue();
}

StringArray DivisionEditorComponent::getNames() const
{
    StringArray names;
    const int n = getCount();
    for (int i = 0; i < n; ++i)
        names.add(_nameEditors[i]->getText().trim());
    return names;
}

void DivisionEditorComponent::rebuildNameRows()
{
    const int n = getCount();
    for (int i = 0; i < 10; ++i)
    {
        _nameLabels[i]->setVisible(i < n);
        _nameEditors[i]->setVisible(i < n);
    }
    resized();
}

void DivisionEditorComponent::resized()
{
    constexpr int margin = 8;
    auto bounds = getLocalBounds().reduced(margin);

    _titleLabel.setBounds(bounds.removeFromTop(22));
    bounds.removeFromTop(margin);

    auto countRow = bounds.removeFromTop(22);
    _countLabel.setBounds(countRow.removeFromLeft(160));
    _countSlider.setBounds(countRow.removeFromLeft(110));

    bounds.removeFromTop(margin);

    const int n = getCount();
    for (int i = 0; i < n; ++i)
    {
        auto row = bounds.removeFromTop(24);
        _nameLabels[i]->setBounds(row.removeFromLeft(100));
        _nameEditors[i]->setBounds(row);
        bounds.removeFromTop(4);
    }

    auto buttons = bounds.removeFromBottom(24);
    _cancelButton.setBounds(buttons.removeFromRight(70));
    buttons.removeFromRight(margin);
    _okButton.setBounds(buttons.removeFromRight(70));
}

} // namespace ui