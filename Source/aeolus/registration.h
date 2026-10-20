// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
namespace aeolus {
// Scalar publication for UI/persistence readers. Shape changes remain setup-only.
template<class T> class PublishedValue {
    std::atomic<T> value;
public:
    PublishedValue(T initial = {}) noexcept : value(initial) {}
    PublishedValue(const PublishedValue& other) noexcept : value(other.load()) {}
    PublishedValue& operator=(const PublishedValue& other) noexcept { return *this = other.load(); }
    PublishedValue& operator=(T v) noexcept { value.store(v); return *this; }
    T load() const noexcept { return value.load(); }
    operator T() const noexcept { return load(); }
};
enum class ControlAction { Stop, Link, Tremulant, Bass, CapturePreset, RecallPreset,
    DisableStops, CancelStops, GeneralCancel, CancelLinks, DivisionMask, ControlMask, SwellMask,
    Step, CaptureStep, Backward, Forward };
}
