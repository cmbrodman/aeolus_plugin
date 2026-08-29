#include "ui/CouplerEditorComponent.h"

using namespace juce;

namespace ui {

CouplerEditorComponent::CouplerEditorComponent(aeolus::Engine& engine)
    : _engine(engine)
    , _titleLabel{{}, "Coupler Editor"}
    , _divisionLabel{{}, "Division"}
    , _destTitle{{}, "Destinations"}
{
    addAndMakeVisible(_titleLabel);
    _titleLabel.setJustificationType(Justification::centred);
    _titleLabel.setColour(Label::textColourId, Colours::lightyellow);
    auto font = _titleLabel.getFont();
    font.setHeight(font.getHeight() * 1.2f);
    _titleLabel.setFont(font);

    addAndMakeVisible(_divisionLabel);
    addAndMakeVisible(_divisionBox);
    addAndMakeVisible(_bassButton);
    addAndMakeVisible(_destTitle);
    _destTitle.setJustificationType(Justification::centred);
    _destTitle.setColour(Label::textColourId, Colours::lightyellow);

    for (int i = 0; i < _engine.getDivisionCount(); ++i)
    {
        auto* d = _engine.getDivisionByIndex(i);
        if (d->isPedal())
            continue;
        _divisionBox.addItem(d->getName(), i + 1);
    }
    if (_divisionBox.getNumItems() > 0)
        _divisionBox.setSelectedItemIndex(0, dontSendNotification);

    _divisionBox.onChange = [this] {
        storeCurrent();
        loadSource();
    };

    addAndMakeVisible(_okButton);
    addAndMakeVisible(_cancelButton);
    _okButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _cancelButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _okButton.onClick = [this] { if (onOk) onOk(); };
    _cancelButton.onClick = [this] { if (onCancel) onCancel(); };

    loadSource();
}

int CouplerEditorComponent::getSourceIndex() const
{
    return jmax(0, _divisionBox.getSelectedId() - 1);
}

void CouplerEditorComponent::storeCurrent()
{
    if (_shownSource < 0)
        return;

    DivisionEdits e;
    e.sourceIndex = _shownSource;
    e.hasBass = _bassButton.getToggleState();
    e.specs = specsFromRows(_shownSource);
    _pending[_shownSource] = std::move(e);
}

std::vector<aeolus::Division::LinkSpec> CouplerEditorComponent::specsFromRows(int sourceIndex) const
{
    std::vector<aeolus::Division::LinkSpec> specs;
    auto* src = _engine.getDivisionByIndex(sourceIndex);

    for (auto* row : _rows)
    {
        const bool pass = row->pass.getToggleState();
        auto add = [&](int shift, bool on)
        {
            if (!on) return;
            aeolus::Division::LinkSpec spec;
            spec.targetName = row->dest->getName();
            spec.octaveShift = shift;
            spec.passThrough = pass;
            specs.push_back(spec);
        };

        if (row->dest != src)
            add(0, row->unison.getToggleState());
        add(12, row->super.getToggleState());
        add(-12, row->sub.getToggleState());
    }
    return specs;
}

juce::Array<CouplerEditorComponent::DivisionEdits> CouplerEditorComponent::getAllEdits()
{
    storeCurrent();

    juce::Array<DivisionEdits> out;
    for (auto& kv : _pending)
        out.add(kv.second);
    return out;
}

void CouplerEditorComponent::loadSource()
{
    _rows.clear();
    _shownSource = getSourceIndex();
    auto* src = _engine.getDivisionByIndex(_shownSource);
    if (src == nullptr)
        return;

    const bool fromPending = _pending.count(_shownSource) > 0;
    if (fromPending)
        _bassButton.setToggleState(_pending[_shownSource].hasBass, dontSendNotification);
    else
        _bassButton.setToggleState(src->hasBassCoupler(), dontSendNotification);

    for (int i = 0; i < _engine.getDivisionCount(); ++i)
    {
        auto* dest = _engine.getDivisionByIndex(i);
        auto* row = _rows.add(new DestRow());
        row->dest = dest;
        row->name.setText(dest->getName(), dontSendNotification);
        addAndMakeVisible(row->name);
        addAndMakeVisible(row->unison);
        addAndMakeVisible(row->super);
        addAndMakeVisible(row->sub);
        addAndMakeVisible(row->pass);

        const bool self = (dest == src);
        const bool pedal = dest->isPedal();
        row->unison.setVisible(!self);
        row->super.setVisible(!pedal);
        row->sub.setVisible(!pedal);
        row->pass.setVisible(!pedal && !self);

        auto applySpec = [&](const aeolus::Division::LinkSpec& spec)
        {
            if (spec.targetName != dest->getName())
                return;
            if (spec.octaveShift == 0) row->unison.setToggleState(true, dontSendNotification);
            if (spec.octaveShift == 12) row->super.setToggleState(true, dontSendNotification);
            if (spec.octaveShift == -12) row->sub.setToggleState(true, dontSendNotification);
            if (spec.passThrough) row->pass.setToggleState(true, dontSendNotification);
        };

        if (fromPending)
        {
            for (const auto& spec : _pending[_shownSource].specs)
                applySpec(spec);
        }
        else
        {
            for (int l = 0; l < src->getLinksCount(); ++l)
            {
                auto& link = src->getLinkByIndex(l);
                if (link.division != dest)
                    continue;
                aeolus::Division::LinkSpec spec;
                spec.targetName = dest->getName();
                spec.octaveShift = link.octaveShift;
                spec.passThrough = link.passThrough;
                applySpec(spec);
            }
        }
    }
    resized();
}

void CouplerEditorComponent::resized()
{
    constexpr int margin = 8;
    auto bounds = getLocalBounds().reduced(margin);
    _titleLabel.setBounds(bounds.removeFromTop(22));
    bounds.removeFromTop(6);

    auto top = bounds.removeFromTop(24);
    _divisionLabel.setBounds(top.removeFromLeft(60));
    _divisionBox.setBounds(top.removeFromLeft(150));
    top.removeFromLeft(12);
    _bassButton.setBounds(top.removeFromLeft(140));

    bounds.removeFromTop(8);
    _destTitle.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(4);

    auto buttons = bounds.removeFromBottom(28);
    _cancelButton.setBounds(buttons.removeFromRight(70));
    buttons.removeFromRight(margin);
    _okButton.setBounds(buttons.removeFromRight(70));

    for (auto* row : _rows)
    {
        auto r = bounds.removeFromTop(26);
        row->name.setBounds(r.removeFromLeft(100));
        row->unison.setBounds(r.removeFromLeft(70));
        row->super.setBounds(r.removeFromLeft(80));
        row->sub.setBounds(r.removeFromLeft(80));
        row->pass.setBounds(r);
        bounds.removeFromTop(4);
    }
}

} // namespace ui