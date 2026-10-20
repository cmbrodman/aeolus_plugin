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
    _divisionBox.onChange = [this] {
        if (_reloading)
            return;
        storeCurrent();
        _reloading = true;
        loadDivisionIntoRows();
        _reloading = false;
    };

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

void StopEditorComponent::storeCurrent()
{
    if (_shownDivision < 0)
        return;

    DivisionStopEdits e;
    e.divisionIndex = _shownDivision;
    e.stopCount = getStopCount();
    e.pipes = getPipeNames();
    _pending[_shownDivision] = std::move(e);
}

juce::Array<StopEditorComponent::DivisionStopEdits> StopEditorComponent::getAllEdits()
{
    storeCurrent();
    juce::Array<DivisionStopEdits> out;
    for (auto& kv : _pending)
        out.add(kv.second);
    return out;
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
    _shownDivision = getDivisionIndex();
    auto* div = _engine.getDivisionByIndex(_shownDivision);
    if (div == nullptr)
        return;

    const bool fromPending = _pending.count(_shownDivision) > 0;

    int n = 1;
    StringArray pipes;
    if (fromPending)
    {
        n = _pending[_shownDivision].stopCount;
        pipes = _pending[_shownDivision].pipes;
    }
    else
    {
        n = jmax(1, div->getStopsCount());
        for (int i = 0; i < div->getStopsCount(); ++i)
            pipes.add(currentPipeOfStop(div->getStopByIndex(i)));
    }

    n = jlimit(1, 30, n);
    _countSlider.setValue(n, dontSendNotification);

    _updating = true;
    for (int i = 0; i < 30; ++i)
    {
        String pipe = (i < pipes.size()) ? pipes[i] : String();
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
        int id = 0;
        if (auto found = _variants.find(name); found != _variants.end())
            if (auto* f = footageFromFnFd(int(found->second.getProperty("fn",0)), int(found->second.getProperty("fd",0)))) id = int(f-kFootages)+1;
        _footageBoxes[row]->setSelectedId(id, dontSendNotification);
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
    var value;
    if (src) value = src->toVar();
    else if (auto found = _variants.find(pipe); found != _variants.end()) value = found->second.clone();
    else return;
    auto* object = value.getDynamicObject();
    if (!object || (int(object->getProperty("fn")) == f.fn && int(object->getProperty("fd")) == f.fd)) return;
    const auto base = _variantBases.count(pipe) ? _variantBases[pipe] : pipe;
    auto* original = model->getStopByName(base);
    const bool originalFootage = original && original->getFn() == f.fn && original->getFd() == f.fd;
    const String newName = originalFootage ? base : base + "__" + footageSlug(f);
    object->setProperty("fn", f.fn); object->setProperty("fd", f.fd); object->setProperty("name", newName);
    if (!originalFootage) { _variants[newName] = value; _variantBases[newName] = base; }

    if (!_stopNames.contains(newName))
    {
        _stopNames.add(newName);
        _stopNames.sortNatural();
    }

    _updating = true;
    populateStopCombo(*_stopBoxes[row], newName);
    _updating = false;
}

bool StopEditorComponent::commitVariants()
{
    const auto directory = aeolus::getOrganDataDirectory().getChildFile("Stops");
    const auto config=JSON::parse(aeolus::getCustomOrganConfigFile().loadFileAsString());
    const auto* divisions=config.getProperty("divisions",{}).getArray();
    bool valid=divisions && divisions->size()>=1 && divisions->size()<=10;
    if (divisions) for (const auto& division : *divisions) valid &= division.getDynamicObject()!=nullptr;
    if (!valid) {
        aeolus::reportUserError( "Stop Editor",
            "Cannot read a supported organ_config.json; no stop variants were saved.");
        return false;
    }
    storeCurrent();
    juce::StringArray referenced;
    for (const auto& edit : _pending) for (const auto& name : edit.second.pipes) referenced.addIfNotAlreadyThere(name);
    for (auto it = _variants.begin(); it != _variants.end();)
        if (!referenced.contains(it->first)) it = _variants.erase(it); else ++it;
    if (_variants.empty()) { _okButton.setEnabled(false); return true; }
    auto result = directory.createDirectory();
    juce::Array<juce::File> created, sources;
    if (result.wasOk()) for (const auto& entry : _variants) {
        auto file = directory.getChildFile(entry.first + ".json");
        // A variant name may already belong to a custom model in another
        // supported folder or binary file. Reuse it instead of shadowing it.
        bool found = false;
        for (const auto& search : aeolus::getStopSearchDirectories()) {
            for (const auto& candidate : RangedDirectoryIterator(search, false)) {
                const auto source = candidate.getFile();
                const auto extension = source.getFileExtension().toLowerCase();
                if (source.existsAsFile() && source.getFileNameWithoutExtension() == entry.first
                    && (extension == ".json" || extension == ".ae0")) {
                    file = source; found = true; break;
                }
            }
            if (found) break;
        }
        if (!file.existsAsFile()) {
            result = aeolus::saveTextFile(file, JSON::toString(entry.second, true));
            if (result.failed()) break;
            created.add(file);
        }
        aeolus::Addsynth existing;
        result = existing.readFromFile(file);
        if (result.wasOk() && (existing.getFn() != int(entry.second.getProperty("fn",0))
            || existing.getFd() != int(entry.second.getProperty("fd",0))))
            result = Result::fail("A different footage model already exists: " + file.getFullPathName());
        if (result.failed()) break; // preserve a user's invalid/conflicting file
        existing.setStopName(entry.first);
        const auto* cached = aeolus::Model::getInstance()->getStopByName(entry.first);
        if (!cached || !aeolus::EngineGlobal::getInstance()->getStopByName(entry.first)
            || JSON::toString(cached->toVar()) != JSON::toString(existing.toVar()))
            sources.add(file); // unchanged reused ranks keep their existing waveform
    }
    if (result.failed()) {
        for (const auto& file : created) file.deleteFile();
        aeolus::reportUserError( "Stop Editor", result.getErrorMessage());
        return false;
    }
    for (const auto& source : sources) {
        if (!aeolus::EngineGlobal::getInstance()->reloadStopFile(source)) {
            for (const auto& file : created) file.deleteFile();
            aeolus::reportUserError("Stop Editor", "The model change is not ready. Release all notes and try again; no organ edit was applied.");
            return false;
        }
    }
    _createdVariants=created;
    _variants.clear();
    _okButton.setEnabled(false);
    return true;
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