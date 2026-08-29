#include "ui/StopEditorComponent.h"
#include "aeolus/addsynth.h"

using namespace juce;

namespace ui {

static const StopFootage kFootages[] = {
    { "32'",     1, 4 },
    { "16'",     1, 2 },
    { "10 2/3'", 3, 4 },
    { "8'",      1, 1 },
    { "5 1/3'",  3, 2 },
    { "4'",      2, 1 },
    { "2 2/3'",  3, 1 },
    { "2'",      4, 1 },
    { "1 3/5'",  5, 1 },
    { "1 1/3'",  6, 1 },
    { "1'",      8, 1 },
};

StopEditorComponent::StopEditorComponent(aeolus::Engine& engine)
    : _engine(engine)
    , _titleLabel{{}, "Stop Editor"}
    , _divisionLabel{{}, "Division"}
    , _countLabel{{}, "Number of stops"}
    , _countSlider{Slider::IncDecButtons, Slider::TextBoxLeft}
    , _stopsTitle{{}, "Stops"}
    , _okButton{"OK"}
    , _cancelButton{"Cancel"}
{
    _stopNames = scanStopNames();

    addAndMakeVisible(_titleLabel);
    _titleLabel.setJustificationType(Justification::centred);
    _titleLabel.setColour(Label::textColourId, Colours::lightyellow);
    auto font = _titleLabel.getFont();
    font.setHeight(font.getHeight() * 1.2f);
    _titleLabel.setFont(font);

    addAndMakeVisible(_divisionLabel);
    addAndMakeVisible(_divisionBox);
    for (int i = 0; i < _engine.getDivisionCount(); ++i)
        _divisionBox.addItem(_engine.getDivisionByIndex(i)->getName(), i + 1);
    _divisionBox.setSelectedId(1, dontSendNotification);
    _divisionBox.onChange = [this] { loadDivisionIntoRows(); };

    addAndMakeVisible(_countLabel);
    addAndMakeVisible(_countSlider);
    _countSlider.setTextBoxStyle(Slider::TextBoxLeft, false, 40, 20);
    _countSlider.setRange(1, 30, 1);
    _countSlider.onValueChange = [this] { rebuildRows(); };

    addAndMakeVisible(_stopsTitle);
    _stopsTitle.setJustificationType(Justification::centred);
    _stopsTitle.setColour(Label::textColourId, Colours::lightyellow);

    addAndMakeVisible(_listViewport);
    _listViewport.setViewedComponent(&_list, false);

    for (int i = 0; i < 30; ++i)
    {
        auto* num = _numLabels.add(new Label({}, String(i + 1) + "."));
        auto* stop = _stopBoxes.add(new ComboBox());
        auto* ft   = _footageBoxes.add(new ComboBox());

        num->setJustificationType(Justification::centredRight);
        populateStopCombo(*stop, {});
        for (int f = 0; f < (int)(sizeof(kFootages) / sizeof(kFootages[0])); ++f)
            ft->addItem(kFootages[f].label, f + 1);

        const int row = i;
        stop->onChange = [this, row] {
            if (_updating) return;
            syncFootageFromPipe(row);
        };
        ft->onChange = [this, row] {
            if (_updating) return;
            applyFootageChange(row);
        };

        _list.addChildComponent(num);
        _list.addChildComponent(stop);
        _list.addChildComponent(ft);
    }

    addAndMakeVisible(_okButton);
    addAndMakeVisible(_cancelButton);
    _okButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _cancelButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _okButton.onClick = [this] { if (onOk) onOk(); };
    _cancelButton.onClick = [this] { if (onCancel) onCancel(); };

    loadDivisionIntoRows();
}

int StopEditorComponent::getDivisionIndex() const
{
    return jmax(0, _divisionBox.getSelectedId() - 1);
}

int StopEditorComponent::getStopCount() const
{
    return (int)_countSlider.getValue();
}

StringArray StopEditorComponent::getPipeNames() const
{
    StringArray names;
    const int n = getStopCount();
    for (int i = 0; i < n; ++i)
        names.add(_stopBoxes[i]->getText());
    return names;
}

StringArray StopEditorComponent::scanStopNames() const
{
    StringArray names;
    const StringArray skip{ "organ_config.json", "organ_state.json", "default_organ.json" };

    for (auto dir : aeolus::getStopSearchDirectories())
    {
        if (!dir.isDirectory())
            continue;

        for (DirectoryEntry entry : RangedDirectoryIterator(dir, false))
        {
            auto file = entry.getFile();
            const auto ext = file.getFileExtension().toLowerCase();
            if (ext != ".json" && ext != ".ae0")
                continue;
            if (skip.contains(file.getFileName()))
                continue;
            names.addIfNotAlreadyThere(file.getFileNameWithoutExtension());
        }
    }

    names.sortNatural();
    return names;
}

void StopEditorComponent::populateStopCombo(ComboBox& box, const String& selected)
{
    box.clear(dontSendNotification);
    box.addItem("(none)", 1);
    int selectId = 1;
    for (int i = 0; i < _stopNames.size(); ++i)
    {
        box.addItem(_stopNames[i], i + 2);
        if (_stopNames[i] == selected)
            selectId = i + 2;
    }
    box.setSelectedId(selectId, dontSendNotification);
}

String StopEditorComponent::currentPipeOfStop(const aeolus::Stop& stop)
{
    if (stop.getZones().empty() || stop.getZones()[0].rankwaves.empty())
        return {};
    return stop.getZones()[0].rankwaves[0]->getStopName();
}

const StopFootage* StopEditorComponent::footageFromFnFd(int fn, int fd)
{
    for (auto& f : kFootages)
        if (f.fn == fn && f.fd == fd)
            return &f;
    return nullptr;
}

String StopEditorComponent::footageSlug(const StopFootage& f)
{
    String s(f.label);
    s = s.replace("'", "ft").replace(" ", "-").replace("/", "-");
    return s;
}

void StopEditorComponent::loadDivisionIntoRows()
{
    auto* div = _engine.getDivisionByIndex(getDivisionIndex());
    if (div == nullptr)
        return;

    const int n = jlimit(1, 30, jmax(1, div->getStopsCount()));
    _countSlider.setValue(n, dontSendNotification);

    _updating = true;
    for (int i = 0; i < 30; ++i)
    {
        String pipe;
        if (i < div->getStopsCount())
            pipe = currentPipeOfStop(div->getStopByIndex(i));

        populateStopCombo(*_stopBoxes[i], pipe);
        syncFootageFromPipe(i);
    }
    _updating = false;

    rebuildRows();
}

void StopEditorComponent::syncFootageFromPipe(int row)
{
    const auto name = _stopBoxes[row]->getText();
    auto* model = aeolus::Model::getInstance();
    auto* synth = (name.isEmpty() || name == "(none)") ? nullptr : model->getStopByName(name);

    _updating = true;
    if (synth == nullptr)
    {
        _footageBoxes[row]->setSelectedId(0, dontSendNotification);
    }
    else if (auto* f = footageFromFnFd(synth->getFn(), synth->getFd()))
    {
        const int id = (int)(f - kFootages) + 1;
        _footageBoxes[row]->setSelectedId(id, dontSendNotification);
    }
    _updating = false;
}

void StopEditorComponent::applyFootageChange(int row)
{
    const auto pipe = _stopBoxes[row]->getText();
    if (pipe.isEmpty() || pipe == "(none)")
        return;

    const int fid = _footageBoxes[row]->getSelectedId();
    if (fid <= 0)
        return;

    const auto& f = kFootages[fid - 1];
    auto* model = aeolus::Model::getInstance();
    auto* src = model->getStopByName(pipe);
    if (src == nullptr)
        return;

    if (src->getFn() == f.fn && src->getFd() == f.fd)
        return;

    const String newName = pipe + "__" + footageSlug(f);
    auto stopsDir = File::getSpecialLocation(File::userDocumentsDirectory)
                        .getChildFile("Aeolus").getChildFile("Stops");
    stopsDir.createDirectory();
    const auto outFile = stopsDir.getChildFile(newName + ".json");

    if (!outFile.existsAsFile())
    {
        auto v = src->toVar();
        if (auto* obj = v.getDynamicObject())
        {
            obj->setProperty("fn", f.fn);
            obj->setProperty("fd", f.fd);
            obj->setProperty("name", newName);
        }
        outFile.replaceWithText(JSON::toString(v, true));
    }

    aeolus::EngineGlobal::getInstance()->reloadStopFile(outFile);

    if (!_stopNames.contains(newName))
    {
        _stopNames.add(newName);
        _stopNames.sortNatural();
    }

    _updating = true;
    populateStopCombo(*_stopBoxes[row], newName);
    _updating = false;
}

void StopEditorComponent::rebuildRows()
{
    const int n = getStopCount();
    constexpr int rowH = 26;
    _list.setSize(_listViewport.getWidth() - 8, n * rowH);

    for (int i = 0; i < 30; ++i)
    {
        const bool vis = i < n;
        _numLabels[i]->setVisible(vis);
        _stopBoxes[i]->setVisible(vis);
        _footageBoxes[i]->setVisible(vis);

        if (!vis)
            continue;

        auto row = juce::Rectangle<int>(0, i * rowH, _list.getWidth(), rowH - 2);
        _numLabels[i]->setBounds(row.removeFromLeft(28));
        _footageBoxes[i]->setBounds(row.removeFromRight(90));
        row.removeFromRight(6);
        _stopBoxes[i]->setBounds(row);
    }
}

void StopEditorComponent::resized()
{
    constexpr int margin = 8;
    auto bounds = getLocalBounds().reduced(margin);

    _titleLabel.setBounds(bounds.removeFromTop(22));
    bounds.removeFromTop(6);

    auto top = bounds.removeFromTop(24);
    _divisionLabel.setBounds(top.removeFromLeft(60));
    _divisionBox.setBounds(top.removeFromLeft(150));
    top.removeFromLeft(12);
    _countLabel.setBounds(top.removeFromLeft(110));
    _countSlider.setBounds(top.removeFromLeft(110));

    bounds.removeFromTop(8);
    _stopsTitle.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(4);

    auto buttons = bounds.removeFromBottom(28);
    _cancelButton.setBounds(buttons.removeFromRight(70));
    buttons.removeFromRight(margin);
    _okButton.setBounds(buttons.removeFromRight(70));

    _listViewport.setBounds(bounds);
    rebuildRows();
}

} // namespace ui