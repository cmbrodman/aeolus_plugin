// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string_view>

// Isolated diagnostic build only. Counters never reset during a process lifetime.
// Snapshots are independently sampled; adjacent stages may briefly disagree.
namespace aeolus_midi_diag {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
enum Stage { Device, Enqueue, Dequeue, Processor, Engine, StageCount };
enum Extra { CollectorReset, AgePruned, WindowPruned, EngineOff, DivisionOff,
             ReleaseMatched, VoiceRelease, PoolTrigger, PoolFailure, PoolReturn,
             QueueOverflow, AgePrunedOff, WindowPrunedOff, ResetPrunedOff, ExtraCount };
inline std::atomic<std::uint64_t> notes[StageCount][2] {};
inline std::atomic<std::uint64_t> keys[StageCount][16][128][2] {};
inline std::atomic<std::uint64_t> extra[ExtraCount] {};
inline void bump(Extra e) noexcept { extra[e].fetch_add(1, std::memory_order_relaxed); }
template<class Message> inline void event(Stage s, const Message& m) noexcept {
    if (!m.isNoteOnOrOff()) return;
    const int c=m.getChannel()-1, n=m.getNoteNumber(), off=m.isNoteOff()?1:0;
    if (c<0 || c>=16 || n<0 || n>=128) return;
    notes[s][off].fetch_add(1,std::memory_order_relaxed);
    keys[s][c][n][off].fetch_add(1,std::memory_order_relaxed);
}
inline bool noteRaw(const unsigned char* data,int bytes) noexcept {
    return bytes==3 && ((data[0]&0xf0)==0x80 || (data[0]&0xf0)==0x90);
}
inline bool offRaw(const unsigned char* data,int bytes) noexcept {
    return noteRaw(data,bytes) && ((data[0]&0xf0)==0x80 || data[2]==0);
}
inline void raw(Stage s,const unsigned char* data,int bytes) noexcept {
    if(!noteRaw(data,bytes) || data[1]>=128 || data[2]>=128) return;
    const int c=data[0]&15,n=data[1],off=((data[0]&0xf0)==0x80 || data[2]==0)?1:0;
    notes[s][off].fetch_add(1,std::memory_order_relaxed);
    keys[s][c][n][off].fetch_add(1,std::memory_order_relaxed);
}
inline std::uint64_t load(const std::atomic<std::uint64_t>& a) noexcept {
    return a.load(std::memory_order_relaxed);
}
// Formatting happens only on the independent control receiver, never in DSP.
inline int reply(const char* command,char* out,std::size_t capacity) noexcept {
    if (capacity<1024) return 0;
    if (std::string_view(command)=="DIAG") {
        int used=std::snprintf(out,capacity,"DIAG 1");
        for(int s=0;s<StageCount;++s) for(int o=0;o<2;++o)
            used+=std::snprintf(out+used,capacity-used," %llu",(unsigned long long)load(notes[s][o]));
        for(int e=0;e<ExtraCount;++e)
            used+=std::snprintf(out+used,capacity-used," %llu",(unsigned long long)load(extra[e]));
        return used;
    }
    int c=0,n=0;char tail=0;
    if(std::sscanf(command,"NOTE %d %d %c",&c,&n,&tail)!=2 || c<1 || c>16 || n<0 || n>127) return 0;
    int used=std::snprintf(out,capacity,"NOTE 1 %d %d",c,n);
    for(int s=0;s<StageCount;++s) for(int o=0;o<2;++o)
        used+=std::snprintf(out+used,capacity-used," %llu",(unsigned long long)load(keys[s][c-1][n][o]));
    return used;
}
}
