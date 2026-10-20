// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "aeolus/voice.h"
#include <memory>
namespace aeolus {
// Opt-in Linux stereo test renderer. Prepared outside the audio callback.
// Canonical voices stay on the coordinator; workers only render private copies.
class VoiceBatch final {
public:
    static std::unique_ptr<VoiceBatch> fromEnvironment(VoicePool&);
    VoiceBatch(VoicePool&, int participants, int firstCpu, int delayUs, unsigned maxQuanta);
    ~VoiceBatch();
    void begin() noexcept;
    void include(Voice*) noexcept;
    void requireQuantum() noexcept;
    void render(unsigned quanta) noexcept;
    bool hasPrepared() const noexcept;
    void copy(Voice*,float*,float*) const noexcept;
    bool isOver(Voice*) const noexcept;
    void advance() noexcept;
    struct Stats { size_t adopted=0, fallback=0, local=0, busy=0, rejected=0; };
    Stats stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
