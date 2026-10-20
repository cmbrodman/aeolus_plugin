// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/globals.h"
#include <optional>
namespace aeolus {
// Setup/persistence only. Avoid JUCE 8's redundant MessageManager::Lock path,
// which leaves its entry mutex acquired when the caller already owns the MM.
class SetupMessageLock {
    std::optional<juce::MessageManagerLock> lock;
public:
    SetupMessageLock() {
        auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        if (manager && !manager->currentThreadHasLockedMessageManager()) lock.emplace();
    }
};
}
