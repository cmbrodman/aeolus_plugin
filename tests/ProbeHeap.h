// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
namespace probe_heap {
thread_local bool enabled=false;
std::atomic<size_t> operations {0};
void record(void* p=reinterpret_cast<void*>(1)) noexcept {
    if (enabled && p) operations.fetch_add(1,std::memory_order_relaxed);
}
struct Guard {
    bool previous=enabled;
    Guard() noexcept { enabled=true; }
    ~Guard() { enabled=previous; }
};
}
void* operator new(std::size_t size) {
    probe_heap::record();
    if (auto* p=std::malloc(std::max(size,size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { probe_heap::record(p); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t size,std::align_val_t alignment) {
    probe_heap::record(); void* p=nullptr;
    if (!posix_memalign(&p,size_t(alignment),std::max(size,size_t(1)))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size,std::align_val_t alignment) { return ::operator new(size,alignment); }
void operator delete(void* p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { ::operator delete(p); }
