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
#include "ui/DivisionView.h"

using namespace juce;

namespace ui {

constexpr int paddingTop = 30;
constexpr int paddingBottom = 5;
constexpr int buttonSize = 86;

DivisionView::DivisionView(aeolus::Division* division)
    : onStateChanged{}
    , _division(division)
    , _nameLabel{{}, division->getName()}
    , _cancelButton{"All OFF"}
    , _controlPanel(division)
    , _stopButtons{}

{
    _nameLabel.setJustificationType(Justification::centred);
    _nameLabel.setColour(Label::textColourId, Colour(0xCC, 0xCC, 0x99));
    auto font = CustomLookAndFeel::getManualLabelFont();
    font.setHeight(22);
    _nameLabel.setFont(font);

    addAndMakeVisible(_nameLabel);
    addAndMakeVisible(_cancelButton);
    _cancelButton.setColour(TextButton::buttonColourId, Colour(0x66, 0x66, 0x33));
    _cancelButton.onClick = [this]() {
        cancelAllStops();
    };

    addAndMakeVisible(_controlPanel);

    populateStopButtons();
    populateLinkButtons();
    populatePresetButtons();

            // Shade swell-enabled divisions differently
    if (_division->hasSwell()) {
        _gradientColour[0] = Colour(0x40, 0x31, 0x2F);
        _gradientColour[1] = Colour(0x24, 0x1F, 0x1F);
    } else {
        _gradientColour[0] = Colour(0x31, 0x2F, 0x2F);
        _gradientColour[1] = Colour(0x1F, 0x1F, 0x1F);
    }

            // Bass Coupler button (only for manuals, not for Pedal)
    if (_division->getName() != "Pedal")
    {
        _bassCouplerButton.setButtonText("Bass");
        _bassCouplerButton.setClickingTogglesState(true);
        _bassCouplerButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0x33, 0x33, 0x33));
        _bassCouplerButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::darkorange);
        _bassCouplerButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0x99, 0x99, 0x99));
        _bassCouplerButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);

        _bassCouplerButton.onClick = [this]()
        {
            _division->setBassCouplerEnabled(_bassCouplerButton.getToggleState());
        };

        addAndMakeVisible(_bassCouplerButton);
    }
            // Panic button only on Pedal division
    if (_division->getName() == "Pedal")
    {
        _panicButton.setButtonText("PANIC");
        _panicButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
        _panicButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        _panicButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);

        _panicButton.onClick = [this]()
        {
            _division->getEngine().allNotesOff();
            // If your Engine uses a different panic API, use that instead
        };

        addAndMakeVisible(_panicButton);
    }
}

void DivisionView::update()
{
    _controlPanel.update();

    for (auto* b : _stopButtons)
        b->update();

    int linkIdx = 0;

    for (auto* b : _linkButtons) {
        const bool enabled { _division->isLinkEnabled(linkIdx++) };
        b->setToggleState(enabled, juce::dontSendNotification);
    }

    if (_division->getName() != "Pedal")
        _bassCouplerButton.setToggleState(_division->isBassCouplerEnabled(), juce::dontSendNotification);
}

void DivisionView::cancelAllStops()
{
    jassert(_division != nullptr);

    for (int i = 0; i < _division->getStopsCount(); ++i) {
        auto& stop = _division->getStopByIndex(i);

        auto* button = _stopButtons.getUnchecked(i);

        stop.setEnabled(false);
        button->update();
    }

    updatePresetButtons();
}

void DivisionView::cancelAllLinks()
{
    jassert(_division != nullptr);

    _division->cancelAllLinks();

    for (auto& button : _linkButtons)
        button->setToggleState(false, dontSendNotification);
}

void DivisionView::cancelTremulant()
{
    jassert(_division != nullptr);

    _division->setTremulantEnabled(false);
}

constexpr int controlPanelWidth = 130;

int DivisionView::getEstimatedHeightForWidth(int width) const
{
    constexpr int leftCouplerStrip = 110;
    const int usable = width - controlPanelWidth - leftCouplerStrip;
    const int nButtonsInRow = juce::jmax(1, usable / buttonSize);
    const int nRows = _stopButtons.size() / nButtonsInRow
                    + (_stopButtons.size() % nButtonsInRow > 0 ? 1 : 0);
    return nRows * buttonSize + paddingTop + paddingBottom + 28;
}

void DivisionView::resized()
{
    constexpr int controlPanelWidth = 130;
    constexpr int leftCouplerStrip  = 110;
    constexpr int margin = 10;
    constexpr int couplerBtnW = leftCouplerStrip - 2 * margin;
    constexpr int couplerBtnH = 25;

    // Division name across the top (between the two side strips)
    _nameLabel.setBounds(leftCouplerStrip, 0,
                         getWidth() - controlPanelWidth - leftCouplerStrip,
                         paddingTop);

    // Cancel button near the right panel
    _cancelButton.setBounds(getWidth() - controlPanelWidth - 50, 10, 40, 15);

    // --- Left coupler buttons ---
    {
        int y = 35;
        for (auto* linkButton : _linkButtons)
        {
            linkButton->setBounds(margin, y, couplerBtnW, couplerBtnH);
            y += couplerBtnH + 4;
        }
    }

    // --- Right control panel ---
    _controlPanel.setBounds(getWidth() - controlPanelWidth, 0, controlPanelWidth, getHeight());

    // --- Bass button (manuals only) ---
    if (_division->getName() != "Pedal")
    {
        const int buttonWidth = controlPanelWidth - 3 * margin - 15;
        _bassCouplerButton.setBounds(
            getWidth() - controlPanelWidth + margin + 25,
            55,
            buttonWidth,
            25
        );
    }

    if (_division->getName() == "Pedal")
    {
        const int margin = 10;
        const int buttonWidth = controlPanelWidth - 3 * margin - 15; // same as Bass/Tremulant
        const int buttonHeight = 50; // double the Bass height (25)

        _panicButton.setBounds(
            getWidth() - controlPanelWidth + margin + 25,
            55,                 // same vertical band Bass used on manuals
            buttonWidth,
            buttonHeight
        );
    }

    // --- Stop buttons: middle strip only (no FlexBox) ---
    const int midLeft  = leftCouplerStrip;
    const int midRight = getWidth() - controlPanelWidth;
    const int midWidth = midRight - midLeft;
    const int midTop   = paddingTop;

    if (midWidth > buttonSize && _stopButtons.size() > 0)
    {
        const int nButtonsInRow = juce::jmax(1, midWidth / buttonSize);
        int index = 0;

        for (auto* button : _stopButtons)
        {
            const int row = index / nButtonsInRow;
            const int col = index % nButtonsInRow;

            const int buttonsInThisRow = juce::jmin(nButtonsInRow,
                                                    _stopButtons.size() - row * nButtonsInRow);
            const int rowWidth  = buttonsInThisRow * buttonSize;
            const int rowStartX = midLeft + (midWidth - rowWidth) / 2;

            button->setBounds(
                rowStartX + col * buttonSize,
                midTop + row * buttonSize,
                buttonSize,
                buttonSize
            );
            ++index;
        }
    }
    layoutPresetButtons();
}

void DivisionView::paint(Graphics& g)
{
    const auto grad{
        ColourGradient::vertical(
            _gradientColour[0], 0,
            _gradientColour[1], (float)getHeight()
        )
    };
    g.setGradientFill(grad);
    g.fillRect(getLocalBounds());
    g.setColour(Colour(0x1F, 0x1F, 0x1F));
    g.fillRect(0, 0, 110, getHeight());   // left coupler strip
}

void DivisionView::populateStopButtons()
{
    _stopButtons.clear();

    if (_division == nullptr)
        return;

    for (int i = 0; i < _division->getStopsCount(); ++i) {
        auto button = std::make_unique<StopButton>(*_division, i);
        auto* ptr = button.get();

        _stopButtons.add(button.release());
        addAndMakeVisible(ptr);
    }
}

void DivisionView::populateLinkButtons()
{
    jassert(_division != nullptr);

    for (int i = 0; i < _division->getLinksCount(); ++i) {
        auto& link = _division->getLinkByIndex(i);

        const String caption = _division->getMnemonic() + " + " + link.division->getMnemonic();
        auto button = std::make_unique<TextButton>(caption);
        auto* ptr = button.get();
        ptr->setColour(TextButton::textColourOffId, Colour(0x99, 0x99, 0x99));
        ptr->setColour(TextButton::textColourOnId, Colour(0xFF, 0xFF, 0xFF));
        ptr->setColour(TextButton::buttonColourId, Colour(0x33, 0x33, 0x33));
        ptr->setColour(TextButton::buttonOnColourId, Colours::darkgreen);

        ptr->setClickingTogglesState(true);
        ptr->setToggleState(link.enabled, juce::dontSendNotification);

        button->onClick = [division=_division, i, ptr] {
            division->enableLink(i, ptr->getToggleState());
        };

        _linkButtons.add(button.release());
        addAndMakeVisible(ptr);
    }

    auto& engine = _division->getEngine();

    for (int i = 0; i < engine.getDivisionCount(); ++i) {
        auto* div = engine.getDivisionByIndex(i);

        if (div == _division)
            break;
    }
}

void DivisionView::populatePresetButtons()
{
    _presetButtons.clear();

    for (int i = 0; i < aeolus::DIVISION_PRESET_COUNT; ++i)
    {
        auto button = std::make_unique<TextButton>(String(i + 1));
        auto* ptr = button.get();

        ptr->setColour(TextButton::buttonColourId, Colour(0x40, 0x33, 0x33));
        ptr->setColour(TextButton::buttonOnColourId, Colour(0xDF, 0xC0, 0x36));
        ptr->setColour(TextButton::textColourOffId, Colour(0x99, 0x99, 0x99));
        ptr->setColour(TextButton::textColourOnId, Colours::white);

        ptr->onClick = [this, i]()
        {
            if (_programMode)
            {
                _division->capturePreset(i);
                if (onPresetStored)
                    onPresetStored();
            }
            else
            {
                _division->recallPreset(i);
            }
            updatePresetButtons();
        };

        addAndMakeVisible(ptr);
        _presetButtons.add(button.release());
    }
}

void DivisionView::layoutPresetButtons()
{
    constexpr int controlPanelWidth = 130;
    constexpr int leftCouplerStrip = 110;
    constexpr int presetH = 22;
    constexpr int gap = 4;

    const int midLeft = leftCouplerStrip;
    const int midWidth = getWidth() - controlPanelWidth - leftCouplerStrip;
    const int n = _presetButtons.size();
    if (n == 0 || midWidth < 20)
        return;

    const int btnW = 28;
    const int totalW = n * btnW + (n - 1) * gap;
    const int startX = midLeft + jmax(0, (midWidth - totalW) / 2);
    const int y = getHeight() - presetH - 4;

    _presetRowBounds = { midLeft, y - 2, midWidth, presetH + 6 };

    for (int i = 0; i < n; ++i)
        _presetButtons[i]->setBounds(startX + i * (btnW + gap), y, btnW, presetH);
}

void DivisionView::updatePresetButtons()
{
    for (int i = 0; i < _presetButtons.size(); ++i)
    {
        auto* b = _presetButtons[i];
        const bool captured = _division->isPresetCaptured(i);
        const bool active = (_division->getActivePreset() == i);

        b->setToggleState(active, dontSendNotification);
        b->setColour(TextButton::textColourOffId,
                     captured ? Colour(0xCC, 0xCC, 0x99) : Colour(0x66, 0x66, 0x66));
    }
}

void DivisionView::setProgramMode(bool on)
{
    _programMode = on;
}

juce::Rectangle<int> DivisionView::getPresetRowBoundsIn(Component& target) const
{
    return target.getLocalArea(this, _presetRowBounds);
}

} // namespace ui