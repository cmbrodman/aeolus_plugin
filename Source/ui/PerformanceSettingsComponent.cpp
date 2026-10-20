// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PerformanceSettingsComponent.h"

namespace ui {
using namespace juce;
PerformanceSettingsComponent::PerformanceSettingsComponent(AeolusAudioProcessor& processor, Page page, Component* lifetimeOwner)
    : _processor(processor), _page(page), _owner(lifetimeOwner)
{
    if (_owner) _owner->addComponentListener(this);
    for (auto* label : {&_title, &_description, &_firstLabel, &_secondLabel}) addAndMakeVisible(label);
    _title.setFont(Font(FontOptions(16.0f, Font::bold)));
    _description.setJustificationType(Justification::topLeft);
    _description.setMinimumHorizontalScale(1.0f);
    auto* g = aeolus::EngineGlobal::getInstance();
    for (auto* slider : {&_first, &_second}) {
        slider->setSliderStyle(Slider::LinearHorizontal);
        slider->setTextBoxStyle(Slider::TextBoxRight, false, 70, 24);
    }
    addAndMakeVisible(_cancel);
    _cancel.onClick = [this] { if (onClose) onClose(); };
    if (page == Page::VoiceCapacity) {
        _title.setText("Performance & Safety: Voice capacity", dontSendNotification);
        const int pool = processor.getEngine().getVoicePool().getMaxVoices();
        _description.setText("Pipe voices, including stops, mixtures, couplers and release tails. "
            "A complete new note is ignored if it cannot fit. Lowering the limit lets existing voices finish. "
            "Rejected notes are never queued. Rejected registration additions wait for key release and replay.\n"
            "Startup pool: " + String(pool) + ". This control does not resize it. A voice limit does not guarantee clean audio.", dontSendNotification);
        _firstLabel.setText("Maximum pipe voices", dontSendNotification);
        _first.setName("Maximum pipe voices"); _first.setRange(1, pool, 1);
        _first.setValue(g->getVoiceLimitSetting() == 0 ? pool : jmin(pool, g->getVoiceLimitSetting()), dontSendNotification);
        addAndMakeVisible(_first); addAndMakeVisible(_poolDefault);
        _poolDefault.setToggleState(g->getVoiceLimitSetting() == 0, dontSendNotification);
        _first.setEnabled(!_poolDefault.getToggleState());
        _poolDefault.onClick = [this] { _first.setEnabled(!_poolDefault.getToggleState()); };
        if (g->getVoiceLimitSetting() > pool)
            _description.setText(_description.getText() + " Saved limit " + String(g->getVoiceLimitSetting()) + " is clamped to this pool.", dontSendNotification);
    } else if (page == Page::Meters) {
        _title.setText("Performance & Safety: Meter thresholds", dontSendNotification);
        _description.setText("Provisional display thresholds; these are not calibrated hardware safety limits. "
            "Both bars use green below Warning, orange from Warning, and red from Critical. "
            "Missed callback deadlines or native xruns turn the audio meter red for two seconds. "
            "Recent peaks are held for two seconds. Counts remain until reset or application exit. "
            "Changing these thresholds does not change audio processing or the voice cap.", dontSendNotification);
        _firstLabel.setText("Warning (%)", dontSendNotification);
        _secondLabel.setText("Critical (%)", dontSendNotification);
        _first.setName("Warning percent"); _second.setName("Critical percent");
        _first.setRange(50, 89, 1); _second.setRange(90, 100, 1);
        _first.setValue(g->getMeterWarningPercent(), dontSendNotification);
        _second.setValue(g->getMeterCriticalPercent(), dontSendNotification);
        addAndMakeVisible(_first); addAndMakeVisible(_second);
    } else {
        _title.setText("Performance & Safety: Diagnostics", dontSendNotification);
        _cancel.setButtonText("Close"); addAndMakeVisible(_reset);
        _reset.onClick = [this] { if (!_ownerDeleted) { _processor.resetPerformanceCounters(); timerCallback(); } };
        startTimerHz(5); timerCallback();
    }
    if (page != Page::Diagnostics) {
        addAndMakeVisible(_apply); _apply.onClick = [this] { apply(); };
    }
    setSize(440, 345);
}
PerformanceSettingsComponent::~PerformanceSettingsComponent()
{
    stopTimer();
    if (_owner && !_ownerDeleted) _owner->removeComponentListener(this);
}
void PerformanceSettingsComponent::componentBeingDeleted(Component&)
{
    _ownerDeleted = true;
    stopTimer();
    if (onClose) onClose();
}
void PerformanceSettingsComponent::apply()
{
    if (_ownerDeleted) return;
    auto* g = aeolus::EngineGlobal::getInstance();
    if (_page == Page::VoiceCapacity)
        g->setVoiceLimitSetting(_poolDefault.getToggleState() ? 0 : int(_first.getValue()));
    else {
        g->setMeterWarningPercent(int(_first.getValue()));
        g->setMeterCriticalPercent(int(_second.getValue()));
    }
    g->saveSettings();
    if (onClose) onClose();
}
void PerformanceSettingsComponent::timerCallback()
{
    if (_ownerDeleted) return;
    const auto m = _processor.getPerformance(); auto& e = _processor.getEngine();
    _description.setText("Callback load: " + String(m.latest * 100, 1) + "%\nSmoothed load: "
        + String(m.average * 100, 1) + "%    Recent peak: " + String(m.peak * 100, 1)
        + "%\nMissed processing deadlines: " + String((int64)m.deadlineMisses)
        + "\nNative device xruns: " + (m.xruns < 0 ? String("unavailable") : String(m.xruns))
        + "\nPipe voices: " + String(e.getVoiceCount()) + " / " + String(e.getVoiceLimit())
        + " (pool " + String(e.getVoicePool().getMaxVoices()) + ")\nRejected incoming notes: "
        + String((int64)e.getRejectedNoteCount()) + "\nRejected registration additions: "
        + String((int64)e.getRejectedRegistrationCount())
        + "\nProcessing load is elapsed callback time / buffer deadline, not total system CPU. "
          "Native xruns and missed processing deadlines are separate measurements.", dontSendNotification);
}
void PerformanceSettingsComponent::resized()
{
    _title.setBounds(10, 8, getWidth()-20, 28);
    _description.setBounds(10, 42, getWidth()-20, _page == Page::Diagnostics ? 240 : 146);
    _poolDefault.setBounds(10, 190, getWidth()-20, 26);
    _firstLabel.setBounds(10, 220, 158, 28); _first.setBounds(170, 220, getWidth()-180, 28);
    _secondLabel.setBounds(10, 256, 158, 28); _second.setBounds(170, 256, getWidth()-180, 28);
    _reset.setBounds(10, getHeight()-42, 145, 30);
    _cancel.setBounds(getWidth()-210, getHeight()-42, 95, 30);
    _apply.setBounds(getWidth()-105, getHeight()-42, 95, 30);
}
}
