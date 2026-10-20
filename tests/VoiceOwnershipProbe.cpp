// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// OFFLINE ONLY. The original team waits for mutable owners. The separate
// nonblocking team uses private snapshots and immediate canonical fallback.
// Neither is connected to live engine resource/control lifetimes.
#include "MulticoreProbe.h"
#include "aeolus/engine.h"
#include "NonblockingVoiceTeam.h"
#include <memory>
#include <iomanip>

namespace {
using Clock = std::chrono::steady_clock;
struct alignas(64) Job {
    std::unique_ptr<aeolus::Voice> voice;
    aeolus::Pipewave* pipe;
    juce::Random pipeRandom, chiffRandom;
    float chiffGain=1.0f;
    alignas(64) std::array<float, 1024> output {};
    Job(aeolus::Engine& engine, aeolus::Pipewave& p, int index)
        : voice(std::make_unique<aeolus::Voice>(engine)), pipe(&p),
          pipeRandom(1234567 + index*17), chiffRandom(7654321 + index*31) { trigger(); }
    void trigger() {
        voice->reset(); auto state = pipe->trigger(); state.chiffGain = chiffGain;
        voice->trigger(state);
    }
    void render(unsigned quanta=1) {
        std::fill_n(output.begin(),quanta*128,0.0f);
        // Recycling stays on the coordinator, after all workers finish.
        for (unsigned q=0; q<quanta; ++q)
            if (!voice->isOver()) voice->process(output.data()+q*128, output.data()+q*128+64, pipeRandom, chiffRandom);
    }
};
struct Fixture {
    std::array<aeolus::Addsynth, 3> models;
    std::vector<std::unique_ptr<aeolus::Pipewave>> pipes;
    Fixture() {
        const char* names[] = {"P_principal_8_ae0", "bassoon_ae0", "gemshorn8_ae0"};
        const int notes[] = {45,47,48,50,52,53,79,81,83,84};
        for (int m=0; m<3; ++m) {
            const auto result=models[m].readFromResource(names[m]);
            if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
            for (int note : notes) {
                if (note < models[m].getNoteMin() || note > models[m].getNoteMax()) continue;
                auto pipe=std::make_unique<aeolus::Pipewave>(models[m], note-models[m].getNoteMin(),
                                      440.0f*std::pow(2.0f, (note-69)/12.0f));
                pipe->prepateToPlay(44100); pipes.push_back(std::move(pipe));
            }
        }
        if (pipes.empty()) throw std::runtime_error("empty pipe fixture");
    }
    std::vector<Job> jobs(aeolus::Engine& engine, int voices) {
        std::vector<Job> jobs; jobs.reserve(voices);
        for (int v=0; v<voices; ++v) jobs.emplace_back(engine, *pipes[v%pipes.size()], v);
        return jobs;
    }
};
class Team {
    struct alignas(64) Completion { std::atomic<unsigned> epoch {0}; };
    std::vector<Job>& jobs;
    std::array<std::vector<int>,4> owned;
    std::array<Completion,4> completed;
    std::vector<std::thread> workers;
    std::atomic<unsigned> epoch {0};
    std::atomic<int> ready {0};
    std::atomic<bool> stop {false}, failed {false};
    int count, delayUs;
    static void relax() {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    void render(int lane) { for (int v : owned[lane]) jobs[v].render(); }
    void shutdown() { stop.store(true); for (auto& t : workers) if (t.joinable()) t.join(); }
public:
    Team(std::vector<Job>& jobs, int count, int firstCpu, int delayUs=0, const std::atomic<bool>* = nullptr)
        : jobs(jobs), count(count), delayUs(delayUs) {
        // Fixed assignments for each voice's entire lifetime. First experiment
        // balances counts; measured-cost assignment is a separate comparison.
        for (int v=0; v<int(jobs.size()); ++v) owned[v%count].push_back(v);
        try {
            for (int lane=1; lane<count; ++lane) workers.emplace_back([this,lane,firstCpu] {
                try {
                    multicore_probe::Affinity affinity(firstCpu<0 ? -1 : firstCpu+lane);
                    juce::ScopedNoDenormals noDenormals;
                    ready.fetch_add(1, std::memory_order_release);
                    unsigned seen=0;
                    while (!stop.load(std::memory_order_relaxed)) {
                        const auto current=epoch.load(std::memory_order_acquire);
                        if (seen==current) { relax(); continue; }
                        seen=current;
                        if (this->delayUs && lane==1 && current%17==1)
                            std::this_thread::sleep_for(std::chrono::microseconds(this->delayUs));
                        render(lane);
                        completed[lane].epoch.store(current,std::memory_order_release);
                    }
                } catch (...) { failed.store(true); }
            });
            while (ready.load(std::memory_order_acquire)!=count-1) {
                if (failed.load()) throw std::runtime_error("voice worker startup failed");
                std::this_thread::yield();
            }
        } catch (...) { shutdown(); throw; }
    }
    ~Team() { shutdown(); }
    void run(std::array<float,128>& mix) {
        const auto current=epoch.load(std::memory_order_relaxed)+1;
        epoch.store(current,std::memory_order_release);
        render(0);
        for (int lane=1; lane<count; ++lane)
            while (completed[lane].epoch.load(std::memory_order_acquire)!=current) {
                if (failed.load()) throw std::runtime_error("voice worker failed");
                relax(); // UNBOUNDED, strictly offline.
            }
        // Preserve the original voice summation order, independent of owners.
        // Four partial sums would change floating-point rounding.
        mix.fill(0);
        for (const auto& job : jobs)
            for (size_t i=0; i<mix.size(); ++i) mix[i]+=job.output[i];
    }
};
void checkSnapshots(Fixture& fixture, aeolus::Engine& engine, unsigned limit=1) {
    auto jobs=fixture.jobs(engine,33), reference=fixture.jobs(engine,33);
    // Keep chiff audible through several complete resonator wraps; ordinary
    // gain/quiet-sustain branches remain covered by the threaded checks.
    for (size_t v=0; v<jobs.size(); ++v) {
        jobs[v].chiffGain=reference[v].chiffGain=64.0f;
        jobs[v].trigger(); reference[v].trigger();
    }
    std::vector<std::unique_ptr<aeolus::VoiceSnapshot>> snapshots;
    for (auto& job : jobs) snapshots.push_back(std::make_unique<aeolus::VoiceSnapshot>(*job.voice));
    if (snapshots[0]->capture(*jobs[1].voice,jobs[1].pipeRandom,jobs[1].chiffRandom))
        throw std::runtime_error("snapshot accepted a different pipe lifetime");
    for (unsigned invalid : {0u,9u})
        if (snapshots[0]->capture(*jobs[0].voice,jobs[0].pipeRandom,jobs[0].chiffRandom,invalid))
            throw std::runtime_error("snapshot accepted an invalid job length");
    size_t adopted=0, rejected=0;
    for (int q=0; q<4096;) {
        unsigned n=std::min(limit,unsigned(8-q%8));
        for (int boundary : {2048,3072,3150,4096}) if (boundary>q) n=std::min(n,unsigned(boundary-q));
        for (size_t v=0; v<jobs.size(); ++v) {
            auto& job=jobs[v]; auto& expected=reference[v];
            if (q==2048 || q==3150) { job.voice->release(); expected.voice->release(); }
            if (q==3072) { job.trigger(); expected.trigger(); }
            bool captured;
            {
                probe_heap::Guard heapGuard;
                captured=snapshots[v]->capture(*job.voice,job.pipeRandom,job.chiffRandom,n);
                if (captured) {
                    snapshots[v]->render();
                    if (q%3) {
                        snapshots[v]->adopt(*job.voice,job.pipeRandom,job.chiffRandom);
                        std::copy_n(snapshots[v]->output().begin(),n*128,job.output.begin()); ++adopted;
                    } else job.render(n); // Finished-but-discarded worker result.
                } else { job.render(n); ++rejected; }
            }
            expected.render(n);
            if (std::memcmp(job.output.data(),expected.output.data(),n*128*sizeof(float))
                || job.pipeRandom.getSeed()!=expected.pipeRandom.getSeed()
                || job.chiffRandom.getSeed()!=expected.chiffRandom.getSeed()
                || job.voice->isOver()!=expected.voice->isOver())
                throw std::runtime_error("snapshot adoption/discard differs from serial");
        }
        q+=n;
    }
    if (!adopted || !rejected || probe_heap::operations.load())
        throw std::runtime_error("snapshot coverage or allocation check failed");
    std::cout<<"SNAPSHOTS quanta="<<limit<<" exact_adopt_discard=yes allocations_and_frees=0 adopted="<<adopted
             <<" inactive_rejected="<<rejected<<'\n'<<std::flush;
}
template<class RenderTeam>
void check(Fixture& fixture, aeolus::Engine& engine, int voices, int count, int cpu) {
    auto jobs=fixture.jobs(engine,voices), reference=fixture.jobs(engine,voices);
    std::atomic<bool> hold {true};
    RenderTeam team(jobs,count,cpu,2000, &hold);
    std::array<float,128> mix, expected;
    size_t samples=0; bool sounded=false;
    for (int quantum=0; quantum<2048; ++quantum) {
        if (quantum==24) hold.store(false, std::memory_order_release);
        for (int v=0; v<voices; ++v) {
            if ((quantum==32 && v%2==0) || quantum==64 || quantum==600) {
                jobs[v].voice->release(); reference[v].voice->release();
            }
            if (quantum==512) { jobs[v].trigger(); reference[v].trigger(); }
        }
        team.run(mix); expected.fill(0);
        for (int v=0; v<voices; ++v) {
            reference[v].render();
            if (std::memcmp(jobs[v].output.data(),reference[v].output.data(),sizeof(mix)))
                throw std::runtime_error("owned voice differs from seeded serial voice");
            if (jobs[v].voice->isOver()!=reference[v].voice->isOver())
                throw std::runtime_error("voice lifetime differs");
            if (jobs[v].pipeRandom.getSeed()!=reference[v].pipeRandom.getSeed()
                || jobs[v].chiffRandom.getSeed()!=reference[v].chiffRandom.getSeed())
                throw std::runtime_error("voice random stream differs");
            for (size_t i=0; i<mix.size(); ++i) expected[i]+=reference[v].output[i];
        }
        if (std::memcmp(mix.data(),expected.data(),sizeof(mix))) throw std::runtime_error("mix differs");
        for (float x : mix) { if (!std::isfinite(x)) throw std::runtime_error("nonfinite sample"); sounded |= x!=0; }
        samples+=size_t(voices)*128;
        if constexpr (std::is_same_v<RenderTeam,voice_probe::NonblockingTeam<Job>>) {
            if (quantum==23 && count>1
                && (team.stats.adopted || !team.stats.busy || !team.stats.fallback))
                throw std::runtime_error("held-worker progress was not verified");
        }
    }
    if (!sounded) throw std::runtime_error("silent fixture");
    for (auto& job : jobs) if (!job.voice->isOver()) throw std::runtime_error("tail did not finish");
    if constexpr (std::is_same_v<RenderTeam,voice_probe::NonblockingTeam<Job>>) {
        if (count>1 && !team.stats.fallback) throw std::runtime_error("held workers did not exercise fallback");
        std::cout<<"NONBLOCKING adopted="<<team.stats.adopted<<" fallback="<<team.stats.fallback
                 <<" busy="<<team.stats.busy<<" rejected="<<team.stats.rejected<<" ";
    }
    std::cout<<"CHECK voices="<<voices<<" participants="<<count<<" channel_samples="<<samples
             <<" exact_voice_and_ordered_mix=yes tails_finished=yes injected_delay_us=2000\n"<<std::flush;
}
void checkLarge(Fixture& fixture, aeolus::Engine& engine) {
    for (unsigned limit : {2u,4u,8u}) checkSnapshots(fixture,engine,limit);
    for (unsigned limit : {2u,4u,8u}) for (int voices : {33,450}) for (int count=1; count<=4; ++count) {
        auto jobs=fixture.jobs(engine,voices), reference=fixture.jobs(engine,voices);
        for (size_t v=0; v<jobs.size(); ++v) {
            jobs[v].chiffGain=reference[v].chiffGain=64;
            jobs[v].trigger(); reference[v].trigger();
        }
        std::atomic<bool> hold {true};
        voice_probe::NonblockingTeam<Job> team(jobs,count,-1,2000,&hold);
        std::array<float,1024> mix, expected;
        // Boundaries deliberately do not align with either a 512-frame block
        // or the maximum job. No job may span a release/reset/retrigger.
        const int boundaries[] = {24,33,67,513,601,2048,3073,3151,4096};
        for (int q=0; q<4096;) {
            if (q==24) hold.store(false,std::memory_order_release);
            if (q==33 || q==67 || q==601 || q==2048 || q==3151)
                for (size_t v=0; v<jobs.size(); ++v) if (q!=33 || v%2==0) {
                    jobs[v].voice->release(); reference[v].voice->release();
                }
            if (q==513 || q==3073) for (size_t v=0; v<jobs.size(); ++v) {
                jobs[v].trigger(); reference[v].trigger();
            }
            unsigned n=std::min(limit,unsigned(8-q%8));
            for (int boundary : boundaries) if (boundary>q) n=std::min(n,unsigned(boundary-q));
            team.run(mix,n); expected.fill(0);
            for (size_t v=0; v<jobs.size(); ++v) {
                reference[v].render(n);
                if (std::memcmp(jobs[v].output.data(),reference[v].output.data(),n*128*sizeof(float))
                    || jobs[v].pipeRandom.getSeed()!=reference[v].pipeRandom.getSeed()
                    || jobs[v].chiffRandom.getSeed()!=reference[v].chiffRandom.getSeed()
                    || jobs[v].voice->isOver()!=reference[v].voice->isOver())
                    throw std::runtime_error("large job differs from seeded serial quanta");
                for (size_t i=0; i<n*128; ++i) expected[i]+=reference[v].output[i];
            }
            if (std::memcmp(mix.data(),expected.data(),sizeof(mix))) throw std::runtime_error("large mix differs");
            q+=n;
            if (q==24 && count>1 && (team.stats.adopted || !team.stats.busy || !team.stats.fallback))
                throw std::runtime_error("large held-worker progress failed");
        }
        for (const auto& job : jobs) if (!job.voice->isOver()) throw std::runtime_error("large tail unfinished");
        std::cout<<"LARGE_CHECK quanta="<<limit<<" voices="<<voices<<" participants="<<count
                 <<" exact=yes boundary_splits=yes held_worker_progress=yes adopted="<<team.stats.adopted
                 <<" fallback="<<team.stats.fallback<<'\n'<<std::flush;
    }
}
template<class RenderTeam>
void benchmark(Fixture& fixture, aeolus::Engine& engine, int cpu, unsigned batch=1) {
    volatile double checksum=0;
    for (int trial=0; trial<2; ++trial) for (int order=0; order<4; ++order) {
        const int count=trial ? 4-order : order+1;
        auto jobs=fixture.jobs(engine,450); RenderTeam team(jobs,count,cpu);
        std::array<float,1024> mix;
        const auto run = [&] {
            if constexpr (std::is_same_v<RenderTeam,voice_probe::NonblockingTeam<Job>>) team.run(mix,batch);
            else { std::array<float,128> small; team.run(small); mix.fill(0); std::copy(small.begin(),small.end(),mix.begin()); }
        };
        const auto warmEnd=Clock::now()+std::chrono::seconds(2);
        while (Clock::now()<warmEnd) {
            run();
            for (unsigned i=0; i<batch*128; ++i) checksum+=mix[i];
        }
        for (int v=0; v<int(jobs.size()); ++v) {
            jobs[v].pipeRandom.setSeed(1234567+v*17);
            jobs[v].chiffRandom.setSeed(7654321+v*31);
            jobs[v].trigger();
        }
        std::cout<<"TIMED_START_NS="<<std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()<<" participants="<<count<<" trial="<<trial<<'\n'<<std::flush;
        std::vector<double> attack, sustain;
        attack.reserve(86); sustain.reserve(256);
        if constexpr (std::is_same_v<RenderTeam,voice_probe::NonblockingTeam<Job>>) team.clearStats();
        for (int block=0; block<342; ++block) {
            const auto start=Clock::now();
            double blockSum=0;
            for (unsigned q=0; q<8; q+=batch) {
                run();
                for (unsigned i=0; i<batch*128; ++i) blockSum+=mix[i];
            }
            const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            (block<86 ? attack : sustain).push_back(ms);
            checksum+=blockSum;
        }
        std::cout<<"TIMED_END_NS="<<std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()<<" participants="<<count<<" trial="<<trial<<'\n';
        if constexpr (std::is_same_v<RenderTeam,voice_probe::NonblockingTeam<Job>>)
            std::cout<<"ADOPTION participants="<<count<<" trial="<<trial<<" adopted="<<team.stats.adopted
                     <<" fallback="<<team.stats.fallback<<" busy="<<team.stats.busy<<" rejected="<<team.stats.rejected<<'\n';
        for (auto* times : {&attack,&sustain}) {
            std::sort(times->begin(),times->end());
            std::cout<<"BENCH trial="<<trial<<" participants="<<count<<" quanta="<<batch<<" voices=450 frames=512 phase="
                     <<(times==&attack ? "attack" : "sustain")<<" p50_ms="<<(*times)[times->size()/2]
                     <<" p99_ms="<<(*times)[times->size()*99/100]<<" worst_ms="<<times->back()
                     <<" over_budget="<<std::count_if(times->begin(),times->end(),[](double x){return x>512000.0/44100;})
                     <<'\n'<<std::flush;
        }
    }
    std::cout<<"checksum="<<checksum<<'\n';
}
}
int main(int argc,char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    int status=0;
    try {
        const bool large=argc>1 && (std::string(argv[1])=="--check-large" || std::string(argv[1])=="--benchmark-large");
        const bool nonblocking=large || (argc>1 && (std::string(argv[1])=="--check-nonblocking" || std::string(argv[1])=="--benchmark-nonblocking"));
        const bool checking=argc==2 && (std::string(argv[1])=="--check" || std::string(argv[1])=="--check-nonblocking" || std::string(argv[1])=="--check-large");
        const bool bench=(argc==4 && std::string(argv[1])=="--benchmark-large") || (argc==3 && (std::string(argv[1])=="--benchmark" || std::string(argv[1])=="--benchmark-nonblocking"));
        if (!checking && !bench) throw std::runtime_error("Usage: aeolus_voice_probe {--check|--check-nonblocking|--check-large} | {--benchmark|--benchmark-nonblocking} FIRST_CPU | --benchmark-large FIRST_CPU QUANTA");
        const int cpu=bench ? std::stoi(argv[2]) : -1;
        if (bench && (cpu<0 || cpu>CPU_SETSIZE-4)) throw std::runtime_error("invalid CPU base");
        const auto dir=aeolus::getTestDataDirectory(); dir.createDirectory();
        dir.getChildFile("organ_config.json").replaceWithData(BinaryData::default_organ_json,BinaryData::default_organ_jsonSize);
        dir.getChildFile("organ_state.json").replaceWithText("{}");
        std::cout<<"OFFLINE ONLY: complete pipe/chiff/spatial voices; fixed ownership; seeded per-voice RNG; "
                   "ordered mixing; "<<(nonblocking ? "private snapshots, local fallback, no completion waits" : "unbounded completion waits")
                 <<"; no device/reverb/engine scheduling.\n"<<std::flush;
        aeolus::Engine engine; Fixture fixture;
        multicore_probe::Affinity affinity(cpu); juce::ScopedNoDenormals noDenormals;
        if (checking && large) checkLarge(fixture,engine);
        else if (checking && nonblocking) checkSnapshots(fixture,engine);
        if (checking && !large) for (int count=1; count<=4; ++count) {
            if (nonblocking) {
                check<voice_probe::NonblockingTeam<Job>>(fixture,engine,33,count,cpu);
                check<voice_probe::NonblockingTeam<Job>>(fixture,engine,450,count,cpu);
            } else {
                check<Team>(fixture,engine,33,count,cpu); check<Team>(fixture,engine,450,count,cpu);
            }
        }
        else if (bench && nonblocking) {
            const unsigned batch=large ? unsigned(std::stoi(argv[3])) : 1;
            if (batch!=1 && batch!=2 && batch!=4 && batch!=8) throw std::runtime_error("invalid batch size");
            benchmark<voice_probe::NonblockingTeam<Job>>(fixture,engine,cpu,batch);
        }
        else if (bench) benchmark<Team>(fixture,engine,cpu);
        if (probe_heap::operations.load()) throw std::runtime_error("render allocation/free detected");
    } catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; status=1; }
    juce::DeletedAtShutdown::deleteAll(); return status;
}
