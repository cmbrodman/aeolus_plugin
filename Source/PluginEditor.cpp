// ----------------------------------------------------------------------------
//
//  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------

#include "aeolus/engine.h"
#include "ui/CustomLookAndFeel.h"
#include "ui/GlobalTuningComponent.h"
#include "ui/FxComponent.h"
#include "ui/SettingsComponent.h"
#include "ui/MidiSettingsComponent.h"

#include "PluginProcessor.h"
#include "PluginEditor.h"
#if JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

#include "ui/DivisionEditorComponent.h"

#include "ui/StopEditorComponent.h"

#include "ui/CouplerEditorComponent.h"

using namespace juce;

//==============================================================================
AeolusAudioProcessorEditor::AeolusAudioProcessorEditor (AeolusAudioProcessor& p)
    : AudioProcessorEditor (&p)
    , _audioProcessor(p)
    , _divisionsViewport{}
    , _divisionsComponent{}
    , _divisionViews{}
    , _midiKeyboard(p.getEngine().getMidiKeyboardState(), MidiKeyboardComponent::horizontalKeyboard)
    , _overlay{}
    , _sequencerView(p.getEngine().getSequencer())
    , _versionLabel{{}, JucePlugin_VersionString}
    , _reverbLabel{{}, "Reverb:"}
    , _reverbComboBox{}
    , _reverbSlider{*p.getParametersContainer().reverbWet, juce::Slider::LinearHorizontal}
    , _volumeLabel{{}, "Volume:"}
    , _volumeSlider{*p.getParametersContainer().volume, juce::Slider::LinearHorizontal}
    , _volumeLevelL{p.getEngine().getVolumeLevel().left, ui::LevelIndicator::Orientation::Horizontal}
    , _volumeLevelR{p.getEngine().getVolumeLevel().right, ui::LevelIndicator::Orientation::Horizontal}
    , _tuningButton{"tuningButton", DrawableButton::ImageFitted}
    , _settingsButton{"settingsButton", DrawableButton::ImageFitted}
    , _fxButton{"fxButton", DrawableButton::ImageFitted}
    , _mtsConnectedLabel{{}, "connected to MTS master"}
    , _mtsDisconnectedLabel{{}, "no MTS master found"}
    , _cancelButton{"Cancel"}
{
    auto* g = aeolus::EngineGlobal::getInstance();

    setLookAndFeel(&ui::CustomLookAndFeel::getInstance());

    setSize(_audioProcessor.getEngine().getWindowWidth(),
            _audioProcessor.getEngine().getWindowHeight());
        setResizeLimits(640, 480, 4096, 4096);

        _uiScalingPercent = g->getUIScalingFactor();

    addAndMakeVisible(_versionLabel);
    _versionLabel.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 10, Font::plain)));
    _versionLabel.setJustificationType (Justification::right);

    _cpuLoadMeter.setName("Audio processing load");
    _voiceMeter.setName("Pipe voice capacity");
    addAndMakeVisible(_cpuLoadMeter); addAndMakeVisible(_voiceMeter);

#if !AEOLUS_MULTIBUS_OUTPUT

    // Mutlibus configuration does not have a reverb

    addAndMakeVisible(_reverbLabel);
    addAndMakeVisible(_reverbComboBox);

    int id = 0;
    for (const auto& ir : g->getIRs()) {
        _reverbComboBox.addItem(ir.name, ++id);
    }

    _reverbComboBox.setSelectedId(_audioProcessor.getEngine().getReverbIR() + 1);

    _reverbComboBox.onChange = [this]() {
        const auto irNum = _reverbComboBox.getSelectedId() - 1;
        auto& engine = _audioProcessor.getEngine();

        if (engine.getReverbIR() != irNum)
            engine.postReverbIR(irNum);
    };

    addAndMakeVisible(_reverbSlider);

#endif // !AEOLUS_MULTIBUS_OUTPUT

    _volumeLevelL.setSkew(0.5f);
    addAndMakeVisible(_volumeLevelL);
    _volumeLevelR.setSkew(0.5f);
    addAndMakeVisible(_volumeLevelR);

    addAndMakeVisible(_volumeLabel);
    addAndMakeVisible(_volumeSlider);
    _volumeSlider.setSkewFactor(0.5f);
    _volumeSlider.setLookAndFeel(&ui::CustomLookAndFeel::getInstance());

    addAndMakeVisible(_tuningButton);
    addAndMakeVisible(_settingsButton);
    addAndMakeVisible(_fxButton);

    auto loadSVG = [](const char* data, size_t size) -> std::unique_ptr<Drawable> {
        if (auto xml = parseXML(String::fromUTF8(data, (int)size))) {
            return Drawable::createFromSVG(*xml);
        }
        return nullptr;
    };

    {
        auto normalIcon = loadSVG(BinaryData::tuningfork_svg, BinaryData::tuningfork_svgSize);
        auto hoverIcon = loadSVG(BinaryData::tuningforkhover_svg, BinaryData::tuningforkhover_svgSize);
        _tuningButton.setImages(normalIcon.get(), hoverIcon.get());
        _tuningButton.setMouseCursor(MouseCursor::PointingHandCursor);
    }

    _tuningButton.onClick = [this] {
        auto content = std::make_unique<ui::GlobalTuningComponent>();
        content->setSize(240, 182);
        auto* contentPtr = content.get();

        auto& box = CallOutBox::launchAsynchronously(std::move(content), _tuningButton.getBounds(), this);
        contentPtr->onCancel = [&box] { box.dismiss(); };
        contentPtr->onOk = [&box, contentPtr] {
            auto* g = aeolus::EngineGlobal::getInstance();
            const bool mtsWasEnabled{ g->isMTSEnabled() };
            bool mtsChanged{ contentPtr->isMTSTuningEnabled() != mtsWasEnabled };

            const float freq = contentPtr->getTuningFrequency();
            const auto scaleType = contentPtr->getTuningScaleType();

            const bool scaleChanged = (g->getTuningFrequency() != freq) || (g->getScale().getType() != scaleType);

            if (mtsChanged || scaleChanged) {
                g->setMTSEnabled(contentPtr->isMTSTuningEnabled());
                g->setTuningFrequency(freq);
                g->setScaleType(scaleType);
                g->rebuildRankwaves();
                g->saveSettings();
            }

            box.dismiss();
        };
    };

    {
        auto normalIcon = loadSVG(BinaryData::settings_svg, BinaryData::settings_svgSize);
        auto hoverIcon = loadSVG(BinaryData::settingshover_svg, BinaryData::settingshover_svgSize);
        _settingsButton.setImages(normalIcon.get(), hoverIcon.get());
        _settingsButton.setMouseCursor(MouseCursor::PointingHandCursor);
    }

        _settingsButton.onClick = [this] {
        PopupMenu menu;
        menu.addItem(1, "UI Settings");
        menu.addItem(2, "MIDI Settings");
        menu.addItem(3, "Division Editor");
        menu.addItem(4, "Stop Editor");
        menu.addItem(5, "Coupler Editor");
        PopupMenu performance, meters, diagnostics;
        performance.addItem(6, "Voice capacity...");
        meters.addItem(7, "Warning and critical thresholds...");
        performance.addSubMenu("Load & voice meters", meters);
        diagnostics.addItem(8, "Live readings & reset counters...");
        performance.addSubMenu("Diagnostics", diagnostics);
        menu.addSubMenu("Performance & Safety", performance);

        Component::SafePointer<AeolusAudioProcessorEditor> menuOwner(this);
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(_settingsButton),
            [this,menuOwner](int result)
            {
                if(menuOwner==nullptr) return;
                if (result >= 6 && result <= 8) {
                    showPerformanceSettings(result == 6 ? ui::PerformanceSettingsComponent::Page::VoiceCapacity
                        : result == 7 ? ui::PerformanceSettingsComponent::Page::Meters
                                      : ui::PerformanceSettingsComponent::Page::Diagnostics);
                    return;
                }
                if (result == 1)
                {
                    auto content = std::make_unique<ui::SettingsComponent>();
                    content->setSize(280, 160);
                    auto* contentPtr = content.get();

                    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
                    contentPtr->onCancel = [&box] { box.dismiss(); };
                    contentPtr->onOk = [&box, contentPtr, this] {
                            auto* g = aeolus::EngineGlobal::getInstance();
                            const float uiScalingFactor = contentPtr->getUIScalingFactor();
                            if (g->getUIScalingFactor() != uiScalingFactor) {
                                g->setUIScalingFactor(uiScalingFactor);
                            }

                        g->setKeyboardVisible(contentPtr->isKeyboardVisible());
                        _midiKeyboard.setVisible(g->isKeyboardVisible());
                        g->saveSettings();
                        resized();
                        box.dismiss();
                    };
                }
                else if (result == 2)
                {
                                       // Device manager is only available in standalone; pass nullptr in plugin hosts
                    juce::AudioDeviceManager* dm = nullptr;
#if JucePlugin_Build_Standalone
                    if (_audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
                        if (auto* holder = juce::StandalonePluginHolder::getInstance()) dm = &holder->deviceManager;
#endif

                    auto content = std::make_unique<ui::MidiSettingsComponent>(_audioProcessor.getEngine(), dm, this);
                    content->setSize(420, 420);
                    auto* contentPtr = content.get();

                    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
                    contentPtr->onClose = [safeEditor = Component::SafePointer<AeolusAudioProcessorEditor>(this),
                                           safeBox = Component::SafePointer<CallOutBox>(&box)] {
                        if (safeEditor) safeEditor->_audioProcessor.getEngine().saveOrganState();
                        if (safeBox) safeBox->dismiss();
                    };
                }
                else if (result == 3)
                {
                    auto content = std::make_unique<ui::DivisionEditorComponent>(_audioProcessor.getEngine());
                    content->setSize(400, 470);
                    auto* contentPtr = content.get();

                    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
                    contentPtr->onCancel = [&box] { box.dismiss(); };
                    contentPtr->onOk = [&box, contentPtr, this] {
                        const int count = contentPtr->getCount();
                        const auto names = contentPtr->getNames();
                        box.dismiss();

                        Component::SafePointer<AeolusAudioProcessorEditor> editor(this);
                        juce::MessageManager::callAsync([editor, count, names] {
                            if(editor==nullptr) return;
                            auto* self=editor.getComponent();
                            self->_audioProcessor.suspendForEdit(true);

                            self->_divisionViews.clear();
                            self->_divisionsComponent.removeAllChildren();

                            self->_audioProcessor.getEngine().applyDivisionLayout(count, names);
                            self->_audioProcessor.getParametersContainer().rebindDivisionGains();

                            self->populateDivisions();
                            self->resized();

                            self->_audioProcessor.suspendForEdit(false);
                        });
                    };
                }
                else if (result == 4)
                {
                    auto content = std::make_unique<ui::StopEditorComponent>(_audioProcessor.getEngine());
                    content->setSize(540, 560);
                    auto* contentPtr = content.get();

                    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
                    contentPtr->onCancel = [&box] { box.dismiss(); };
                    contentPtr->onOk = [this, contentPtr] {
                                            if (!contentPtr->commitVariants()) return;
                                            auto createdVariants = contentPtr->getCreatedVariantFiles();
                                            auto editsUi = contentPtr->getAllEdits();
                                            juce::Array<aeolus::Engine::DivisionStopEdits> edits;
                                            for (auto& e : editsUi)
                                            {
                                                aeolus::Engine::DivisionStopEdits d;
                                                d.divisionIndex = e.divisionIndex;
                                                d.pipeNames = e.pipes;
                                                edits.add(std::move(d));
                                            }

                                            auto* box = contentPtr->findParentComponentOfClass<CallOutBox>();
                                            Component::SafePointer<CallOutBox> safeBox(box);
                                            Component::SafePointer<AeolusAudioProcessorEditor> safeEd(this);

                                            MessageManager::callAsync([safeEd, safeBox, edits, createdVariants] {
                                                if (safeBox != nullptr)
                                                    safeBox->dismiss();
                                                if (safeEd == nullptr) {
                                                    for (const auto& file : createdVariants) file.deleteFile();
                                                    return;
                                                }
                                                auto* self = safeEd.getComponent();
                                                self->_audioProcessor.suspendForEdit(true);
                                                self->_divisionViews.clear();
                                                self->_divisionsComponent.removeAllChildren();
                                                if (!self->_audioProcessor.getEngine().applyDivisionStopsList(edits))
                                                    for (const auto& file : createdVariants) file.deleteFile();
                                                self->_audioProcessor.getParametersContainer().rebindDivisionGains();
                                                self->populateDivisions();
                                                self->resized();
                                                self->_audioProcessor.suspendForEdit(false);
                                            });
                                        };
                }
                else if (result == 5)
                {
                    auto content = std::make_unique<ui::CouplerEditorComponent>(_audioProcessor.getEngine());
                    content->setSize(560, 420);
                    auto* contentPtr = content.get();

                    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
                    contentPtr->onCancel = [&box] { box.dismiss(); };
                    contentPtr->onOk = [this, contentPtr] {
                                            auto editsUi = contentPtr->getAllEdits();
                                            juce::Array<aeolus::Engine::CouplerEdits> edits;
                                            for (auto& e : editsUi)
                                            {
                                                aeolus::Engine::CouplerEdits ce;
                                                ce.sourceIndex = e.sourceIndex;
                                                ce.hasBassCoupler = e.hasBass;
                                                ce.specs = e.specs;
                                                edits.add(std::move(ce));
                                            }

                                            auto* box = contentPtr->findParentComponentOfClass<CallOutBox>();
                                            Component::SafePointer<CallOutBox> safeBox(box);
                                            Component::SafePointer<AeolusAudioProcessorEditor> safeEd(this);

                                            MessageManager::callAsync([safeEd, safeBox, edits] {
                                                if (safeBox != nullptr)
                                                    safeBox->dismiss();
                                                if (safeEd == nullptr)
                                                    return;
                                                auto* self = safeEd.getComponent();
                                                self->_audioProcessor.suspendForEdit(true);
                                                self->_divisionViews.clear();
                                                self->_divisionsComponent.removeAllChildren();
                                                self->_audioProcessor.getEngine().applyCouplerLayouts(edits);
                                                self->_audioProcessor.getParametersContainer().rebindDivisionGains();
                                                self->populateDivisions();
                                                self->resized();
                                                self->_audioProcessor.suspendForEdit(false);
                                            });
                                        };
                }
            });
    };

    _fxButton.onClick = [this] {
        auto& params{ _audioProcessor.getParametersContainer() };
        auto content = std::make_unique<ui::FxComponent>(params);
        content->setSize(240, 120);
        auto* contentPtr = content.get();
        bool limiterWasEnabled{ _audioProcessor.getParametersContainer().limiterEnabled->get() };

        auto& box = CallOutBox::launchAsynchronously(std::move(content), _fxButton.getBounds(), this);
        contentPtr->onCancel = [&box, &params, limiterWasEnabled] {
            // Return to the original state
            (*params.limiterEnabled) = limiterWasEnabled;
            box.dismiss();
        };
        contentPtr->onOk = [&box, &params, contentPtr] {
            (*params.limiterEnabled) = contentPtr->isLimiterEnabled();
            box.dismiss();
        };
    };

    addAndMakeVisible(_mtsConnectedLabel);
    addAndMakeVisible(_mtsDisconnectedLabel);

    _mtsConnectedLabel.setColour(Label::textColourId, Colour(204, 255, 204));
    _mtsDisconnectedLabel.setColour(Label::textColourId, Colour(255, 204, 204));

        _cancelButton.onClick = [this]() {
        _audioProcessor.getEngine().postUIControl(aeolus::ControlAction::GeneralCancel);
        updateDivisionViews();
    };

    addAndMakeVisible(_cancelButton);
    addAndMakeVisible(_safetyButton);
    addAndMakeVisible(_safetyStatus);
    _safetyButton.setColour(TextButton::buttonColourId,Colours::darkred);
    _safetyButton.onClick=[this] { _audioProcessor.panic(); };
    addAndMakeVisible(_resumeButton);
    _resumeButton.setEnabled(false);
    _resumeButton.onClick=[this] { _audioProcessor.resumeAudio(); };

    addAndMakeVisible(_divisionsViewport);
    _divisionsViewport.setViewedComponent(&_divisionsComponent, false /* don't delete */);
    _divisionsViewport.setScrollBarsShown(true, false);

    populateDivisions();

    _midiKeyboard.setScrollButtonsVisible(false);
    _midiKeyboard.setAvailableRange(21, 108);
    addAndMakeVisible(_midiKeyboard);
    _midiKeyboard.setVisible(g->isKeyboardVisible());


    // Overlay and sequencer must go on the very top

    addChildComponent(_overlay);

    _overlay.onClick = [this]() {
        _sequencerView.cancelProgramMode();
    };

    _overlay.getExcludeRects = [this]() {
        Array<Rectangle<int>> rects;
        for (auto* dv : _divisionViews)
            rects.add(dv->getPresetRowBoundsIn(_overlay));
        return rects;
    };

    _sequencerView.addListener(this);

    addAndMakeVisible(_sequencerView);

    g->addListener(this);

    resized();

        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<AeolusAudioProcessorEditor>(this)]
        {
            if (safe != nullptr)
                safe->resized();
        });

    refresh();
    startTimerHz(10);
}

AeolusAudioProcessorEditor::~AeolusAudioProcessorEditor()
{
    if (auto* w = findParentComponentOfClass<juce::ResizableWindow>())
        if (w->getConstrainer() == &_noCreep)
            w->setConstrainer(nullptr);

    auto* g = aeolus::EngineGlobal::getInstance();
    g->removeListener(this);

    _sequencerView.removeListener(this);
};

//==============================================================================
void AeolusAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(Colour(0x36, 0x35, 0x33));
    g.fillRect(0, 0, getWidth(), 30);
}

void AeolusAudioProcessorEditor::parentHierarchyChanged()
{
}

void AeolusAudioProcessorEditor::resized()
{
    constexpr int safetyHeight=28;
    // Registration programming must never intercept the emergency controls.
    _overlay.setBounds(getLocalBounds().withTrimmedBottom(safetyHeight));

    constexpr int margin = 5;

    _versionLabel.setBounds(getWidth() - 60, getHeight() - 20, 60 - margin, 20);

    const int half = (getWidth()-3*margin)/2;
    _cpuLoadMeter.setBounds(margin, 32, half, 31);
    _voiceMeter.setBounds(2*margin+half, 32, half, 31);
#if !AEOLUS_MULTIBUS_OUTPUT
    _reverbLabel.setBounds(margin, margin, 52, 20);
    _reverbComboBox.setBounds(_reverbLabel.getRight()+margin, margin, jlimit(110,220,(getWidth()-640)/4+110), 20);
    _reverbSlider.setBounds(_reverbComboBox.getRight()+margin, margin, getWidth()<800 ? 75 : 100, 20);
    _volumeLabel.setBounds(_reverbSlider.getRight()+12, margin, 55, 20);
#else
    _volumeLabel.setBounds(margin, margin, 55, 20);
#endif
    _volumeSlider.setBounds(_volumeLabel.getRight()+margin, margin, 90, 20);
    _volumeLevelL.setBounds(_volumeSlider.getX()+5, margin+2, _volumeSlider.getWidth()-10, 2);
    _volumeLevelR.setBounds(_volumeSlider.getX()+5, margin+16, _volumeSlider.getWidth()-10, 2);
    _settingsButton.setBounds(getWidth()-margin-24, margin-2, 24, 24);
    _fxButton.setBounds(_settingsButton.getX()-36, margin-2, 24, 24);
    _tuningButton.setBounds(_fxButton.getX()-36, margin-2, 24, 24);
    _mtsConnectedLabel.setBounds(_volumeSlider.getRight()+15, margin,
                               jmax(0,_tuningButton.getX()-_volumeSlider.getRight()-25),20);
    _mtsDisconnectedLabel.setBounds(_mtsConnectedLabel.getBounds());
    _safetyButton.setBounds(margin,getHeight()-26,110,22);
    _resumeButton.setBounds(_safetyButton.getRight()+5,getHeight()-26,110,22);
    _safetyStatus.setBounds(_resumeButton.getRight()+5,getHeight()-26,240,22);

    constexpr int T = 70;
    constexpr int sequencerHeight = 26;
    constexpr int sequencerPadding = 6;
    constexpr int keyboardHeight = 70;
    const int kbH = _midiKeyboard.isVisible() ? keyboardHeight : 0;
    const int bottomChrome = kbH + sequencerHeight + 2 * sequencerPadding + safetyHeight;

    int y = 0;

    for (auto* divisionView : _divisionViews) {
        const auto h = divisionView->getEstimatedHeightForWidth(getWidth());
        divisionView->setBounds(0, y, getWidth(), h);
        y += h;
    }

    _divisionsComponent.setBounds(0, 0, getWidth(), y);
    _divisionsViewport.setBounds(0, T, getWidth(), getHeight() - T - bottomChrome);

    if (kbH > 0)
    {
        int keyboardWidth = jmin((int)_midiKeyboard.getTotalKeyboardWidth(), getWidth());
        _midiKeyboard.setBounds((getWidth() - keyboardWidth) / 2,
                                getHeight() - keyboardHeight - safetyHeight,
                                keyboardWidth,
                                keyboardHeight);
        _sequencerView.setBounds(_sequencerView.getX(),
                                 _midiKeyboard.getY() - sequencerHeight - sequencerPadding,
                                 _sequencerView.getWidth(),
                                 sequencerHeight);
    }

    const float sequencerWidth{ (float)_sequencerView.getOptimalWidth() };
    const float sequencerX{ 0.5f * (getWidth() - sequencerWidth) + ui::SequencerView::buttonWidth };

    const int sequencerY = (kbH > 0)
        ? _midiKeyboard.getY() - sequencerHeight - sequencerPadding
        : getHeight() - sequencerHeight - sequencerPadding - safetyHeight;

    _sequencerView.setBounds(sequencerX, sequencerY, sequencerWidth, sequencerHeight);

    _cancelButton.setColour(TextButton::buttonColourId, Colour(0x33, 0x33, 0x33));
       _cancelButton.setBounds(sequencerX - 80,
                            _sequencerView.getY(),
                            60,
                            26);

    int x = _midiKeyboard.getRight() + (getWidth() - _midiKeyboard.getRight() - 140) / 2;

    _audioProcessor.getEngine().setWindowSize(getWidth(), getHeight());

}

void AeolusAudioProcessorEditor::timerCallback()
{
    const auto& safety=_audioProcessor.getEngine();
    const bool supervised=safety.hasSafetyWatchdog();
    _safetyStatus.setText(safety.isEmergencyPanicPending()?"Silencing...":supervised?"Independent watchdog active":safety.safetyWatchdogStarting()?"Watchdog waiting for audio":"Local Panic / Resume",dontSendNotification);
    _safetyStatus.setColour(Label::textColourId,supervised?Colours::lightgreen:Colours::orange);
    _resumeButton.setEnabled(safety.isEmergencyMuted() && !safety.isEmergencyPanicPending());
    _safetyButton.setTooltip(safety.hasSafetyWatchdog()?"Independent watchdog active. Panic silences all notes; Resume restarts audio.":"Independent watchdog unavailable. Panic requests audio silence; Resume restarts audio.");
    refresh();
}

void AeolusAudioProcessorEditor::onUIScalingFactorChanged(float scalingPercent)
{
    _uiScalingPercent = scalingPercent;
    // Do not call setScaleFactor — it makes the native window grow when resized.
}

void AeolusAudioProcessorEditor::onSequencerEnterProgramMode()
{
    _overlay.setVisible(true);
    for (auto* dv : _divisionViews)
        dv->setProgramMode(true);
}

void AeolusAudioProcessorEditor::onSequencerLeaveProgramMode()
{
    _overlay.setVisible(false);
    for (auto* dv : _divisionViews)
        dv->setProgramMode(false);
}

void AeolusAudioProcessorEditor::populateDivisions()
{
    for (int i = 0; i < _audioProcessor.getEngine().getDivisionCount(); ++i) {
        auto* div = _audioProcessor.getEngine().getDivisionByIndex(i);

        auto view = std::make_unique<ui::DivisionView>(div);
        _divisionsComponent.addAndMakeVisible(view.get());
        _divisionViews.add(view.release());
        _overlay.getExcludeRects = [this]() {
            Array<Rectangle<int>> rects;
            for (auto* dv : _divisionViews)
                rects.add(dv->getPresetRowBoundsIn(_overlay));
            return rects;
        };
    }
}

void AeolusAudioProcessorEditor::refresh()
{
    updateMTS();
    updateMeters();
    updateDivisionViews();
    updateSequencerView();
    updateMidiKeyboardRange();
    updateMidiKeyboardKeySwitches();
}

void AeolusAudioProcessorEditor::updateMTS()
{
    auto* g = aeolus::EngineGlobal::getInstance();

    const bool mtsEnabled{ g->isMTSEnabled() };
    const bool mtsConnected{ g->isConnectedToMTSMaster() };

    _mtsConnectedLabel.setVisible(mtsEnabled && mtsConnected);
    _mtsDisconnectedLabel.setVisible(mtsEnabled && !mtsConnected);
}

void AeolusAudioProcessorEditor::updateMeters()
{
    const auto m = _audioProcessor.getPerformance();
    auto& e = _audioProcessor.getEngine(); auto* g = aeolus::EngineGlobal::getInstance();
    const int warning = g->getMeterWarningPercent(), critical = g->getMeterCriticalPercent();
    const auto status = [warning,critical](float value) {
        return value*100 >= critical ? String("LIMIT") : value*100 >= warning ? String("NEAR") : String("OK");
    };
    _cpuLoadMeter.update(m.average, m.peak, warning, critical, m.recentOverload,
        "Audio " + String(m.average*100,0) + "% / peak " + String(m.peak*100,0)
            + "% " + (m.recentOverload ? String("OVER") : status(m.average))
            + "  !" + String((int64)m.deadlineMisses),
        "Uncapped elapsed callback time / buffer deadline. Misses: " + String((int64)m.deadlineMisses)
            + ". Native xruns: " + (m.xruns < 0 ? String("unavailable") : String(m.xruns))
            + ". Provisional thresholds; white marker is the recent peak.");
    const int voices = e.getVoiceCount(), cap = e.getVoiceLimit();
    const double now = Time::getMillisecondCounterHiRes();
    if (voices >= _recentVoicePeak || now-_voicePeakTime >= 2000) { _recentVoicePeak = voices; _voicePeakTime = now; }
    const auto rejected = e.getRejectedNoteCount()+e.getRejectedRegistrationCount();
    if (rejected > _lastRejectionCount) _lastRejectionTime = now;
    _lastRejectionCount = rejected;
    const bool recentRejection = now-_lastRejectionTime < 2000;
    _voiceMeter.update(float(voices)/cap, float(_recentVoicePeak)/cap, warning, critical, recentRejection,
        "Voices " + String(voices) + "/" + String(cap) + " "
            + (recentRejection ? String("BLOCKED") : status(float(voices)/cap))
            + "  blocked " + String((int64)rejected),
        "Active pipe voices include release tails. Recent peak: " + String(_recentVoicePeak)
            + ". Rejected notes: " + String((int64)e.getRejectedNoteCount()) + "; registration additions: "
            + String((int64)e.getRejectedRegistrationCount()) + ". Startup pool: " + String(e.getVoicePool().getMaxVoices())
            + ". Voice usage and audio load are separate limits.");
}

void AeolusAudioProcessorEditor::showPerformanceSettings(ui::PerformanceSettingsComponent::Page page)
{
    auto content = std::make_unique<ui::PerformanceSettingsComponent>(_audioProcessor, page, this);
    auto* panel = content.get();
    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
    const Component::SafePointer<CallOutBox> safeBox(&box);
    panel->onClose = [safeBox] { if (safeBox) safeBox->dismiss(); };
}

void AeolusAudioProcessorEditor::updateMidiKeyboardRange()
{
    auto range = _audioProcessor.getEngine().getMidiKeyboardRange();

    if (range.getStart() < 0 || range.getEnd() < 0)
        _midiKeyboard.setPlayableRange(0, 0);
    else
        _midiKeyboard.setPlayableRange(range.getStart(), range.getEnd());
}

void AeolusAudioProcessorEditor::updateMidiKeyboardKeySwitches()
{
    const auto keySwitches{ _audioProcessor.getEngine().getKeySwitches() };
    _midiKeyboard.setKeySwitches(keySwitches);
}

void AeolusAudioProcessorEditor::updateDivisionViews()
{
    for (auto* dv : _divisionViews)
        dv->update();
}

void AeolusAudioProcessorEditor::updateSequencerView()
{
    _sequencerView.update();
}
