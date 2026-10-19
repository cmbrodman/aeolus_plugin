// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "aeolus/globals.h"
AEOLUS_NAMESPACE_BEGIN
class Engine;
// Linux standalone control endpoint and independent safety-companion lifetime.
class PanicControl final {
public:
    static std::unique_ptr<PanicControl> create(Engine&);
    ~PanicControl();
    bool watchdogReady() const;
    bool watchdogRunning() const;
    void beginShutdown();
private:
    struct Impl;
    explicit PanicControl(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};
AEOLUS_NAMESPACE_END
