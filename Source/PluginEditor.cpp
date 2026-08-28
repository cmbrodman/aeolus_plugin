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
    , _cpuLoadLabel{{}, "CPU Load:"}
    , _cpuLoadValueLabel{}
    , _voiceCountLabel{{}, "Voices:"}
    , _voiceCountValueLabel{}
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

    setResizable(true, false);   // resize from the frame, not a second constrainer
        setSize(1420, 640);

        _uiScalingPercent = g->getUIScalingFactor();
        const float scale = 1e-2f * _uiScalingPercent;
        if (std::abs(scale - 1.0f) > 0.001f)
            setScaleFactor(scale);

    addAndMakeVisible(_versionLabel);
    _versionLabel.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 10, Font::plain)));
    _versionLabel.setJustificationType (Justification::right);

    addAndMakeVisible(_cpuLoadLabel);
    _cpuLoadValueLabel.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 12, Font::plain)));
    _cpuLoadValueLabel.setJustificationType (Justification::right);
    _cpuLoadValueLabel.setColour(Label::textColourId, Colours::lightyellow);
    addAndMakeVisible(_cpuLoadValueLabel);

    addAndMakeVisible(_voiceCountLabel);
    _voiceCountValueLabel.setFont(Font(FontOptions(Font::getDefaultMonospacedFontName(), 12, Font::plain)));
    _voiceCountValueLabel.setJustificationType (Justification::right);
    _voiceCountValueLabel.setColour(Label::textColourId, Colours::lightyellow);
    addAndMakeVisible(_voiceCountValueLabel);

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

        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(_settingsButton),
            [this](int result)
            {
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

                    auto content = std::make_unique<ui::MidiSettingsComponent>(_audioProcessor.getEngine(), dm);
                    content->setSize(420, 420);
                    auto* contentPtr = content.get();

                    auto& box = CallOutBox::launchAsynchronously(std::move(content), _settingsButton.getBounds(), this);
                    contentPtr->onClose = [&box, this] {
                        _audioProcessor.getEngine().saveOrganState();
                         box.dismiss();
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
        for (auto* divisionView : _divisionViews) {
            divisionView->cancelAllStops();
            divisionView->cancelAllLinks();
            divisionView->cancelTremulant();
        }
        updateDivisionViews();
    };

    addAndMakeVisible(_cancelButton);

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

    startTimerHz(10);
}

AeolusAudioProcessorEditor::~AeolusAudioProcessorEditor()
{
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

void AeolusAudioProcessorEditor::resized()
{
    _overlay.setBounds(getLocalBounds());

    constexpr int margin = 5;

    _versionLabel.setBounds(getWidth() - 60, getHeight() - 20, 60 - margin, 20);

    _cpuLoadLabel.setBounds(margin, margin, 70, 20);
    _cpuLoadValueLabel.setBounds(_cpuLoadLabel.getRight() + margin, margin, 36, 20);
    _voiceCountLabel.setBounds(150, margin, 56, 20);
    _voiceCountValueLabel.setBounds(_voiceCountLabel.getRight() + margin, margin, 30, 20);

#if !AEOLUS_MULTIBUS_OUTPUT
    _reverbLabel.setBounds(_voiceCountValueLabel.getRight() + 40, margin, 60, 20);
    _reverbComboBox.setBounds(_reverbLabel.getRight() + margin, margin, 220, 20);
    _reverbSlider.setBounds(_reverbComboBox.getRight() + margin, margin, 100, 20);

    _volumeLabel.setBounds(_reverbSlider.getRight() + 40, margin, 60, 20);

#else

    _volumeLabel.setBounds(_voiceCountValueLabel.getRight() + 430, margin, 60, 20);

#endif

    _volumeSlider.setBounds(_volumeLabel.getRight() + margin, margin, 100, 20);

    _volumeLevelL.setBounds(_volumeSlider.getX() + 5, _volumeSlider.getY() + 2, _volumeSlider.getWidth() - 10, 2);
    _volumeLevelR.setBounds(_volumeSlider.getX() + 5, _volumeSlider.getY() + _volumeSlider.getHeight() - 4, _volumeSlider.getWidth() - 10, 2);

    _tuningButton.setBounds(_volumeSlider.getRight() + 40, margin - 2, 24, 24);
    _fxButton.setBounds(_tuningButton.getRight() + 20, margin - 2, 24, 24);
    _settingsButton.setBounds(_fxButton.getRight() + 20, margin - 2, 24, 24);

    _mtsConnectedLabel.setBounds(_settingsButton.getRight() + 40, margin, 160, 20);
    _mtsDisconnectedLabel.setBounds(_mtsConnectedLabel.getBounds());

        constexpr int T = margin * 2 + 20;
    constexpr int sequencerHeight = 26;
    constexpr int sequencerPadding = 6;
    constexpr int keyboardHeight = 70;
    const int kbH = _midiKeyboard.isVisible() ? keyboardHeight : 0;
    const int bottomChrome = kbH + sequencerHeight + 2 * sequencerPadding;

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
                                getHeight() - keyboardHeight,
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
        : getHeight() - sequencerHeight - sequencerPadding;

    _sequencerView.setBounds(sequencerX, sequencerY, sequencerWidth, sequencerHeight);

    _cancelButton.setColour(TextButton::buttonColourId, Colour(0x33, 0x33, 0x33));
       _cancelButton.setBounds(sequencerX - 80,
                            _sequencerView.getY(),
                            60,
                            26);

    int x = _midiKeyboard.getRight() + (getWidth() - _midiKeyboard.getRight() - 140) / 2;

}

void AeolusAudioProcessorEditor::timerCallback()
{
    refresh();
}

void AeolusAudioProcessorEditor::onUIScalingFactorChanged(float scalingPercent)
{
    if (std::abs(scalingPercent - _uiScalingPercent) < 0.01f)
        return;

    const float scaling = 1e-2f * scalingPercent;
    setScaleFactor(scaling);
    _uiScalingPercent = scalingPercent;
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
    auto strLoad = String (int (_audioProcessor.getProcessLoad() * 100.0f)) + "%";
    auto strVoices = String (_audioProcessor.getActiveVoiceCount());

    _cpuLoadValueLabel.setText (strLoad, dontSendNotification);
    _voiceCountValueLabel.setText (strVoices, dontSendNotification);
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
