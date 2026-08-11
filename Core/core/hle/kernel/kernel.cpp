// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm> // xappify fork: LogGuestThreadState
#include <map>       // xappify fork: LogGuestThreadState
#include <utility>   // xappify fork: LogGuestThreadState
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/logging/log.h" // xappify fork: LogGuestThreadState
#include "common/serialization/atomic.h"
#include "common/settings.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/config_mem.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/timer.h"

SERIALIZE_EXPORT_IMPL(Kernel::New3dsHwCapabilities)

namespace Kernel {

/// Initialize the kernel
KernelSystem::KernelSystem(Memory::MemorySystem& memory, Core::Timing& timing,
                           std::function<void()> prepare_reschedule_callback,
                           MemoryMode memory_mode, u32 num_cores, u64 override_init_time)
    : memory(memory), timing(timing),
      prepare_reschedule_callback(std::move(prepare_reschedule_callback)),
      memory_mode(memory_mode) {
    std::generate(memory_regions.begin(), memory_regions.end(),
                  [] { return std::make_shared<MemoryRegionInfo>(); });
    MemoryInit(memory_mode, override_init_time);

    resource_limits = std::make_unique<ResourceLimitList>(*this);
    for (u32 core_id = 0; core_id < num_cores; ++core_id) {
        thread_managers.push_back(std::make_unique<ThreadManager>(*this, core_id));
    }
    timer_manager = std::make_unique<TimerManager>(timing);
    ipc_recorder = std::make_unique<IPCDebugger::Recorder>();
    stored_processes.assign(num_cores, nullptr);

    next_thread_id = 1;
}

/// Shutdown the kernel
KernelSystem::~KernelSystem() {
    ResetThreadIDs();
};

ResourceLimitList& KernelSystem::ResourceLimit() {
    return *resource_limits;
}

const ResourceLimitList& KernelSystem::ResourceLimit() const {
    return *resource_limits;
}

u32 KernelSystem::GenerateObjectID() {
    return next_object_id++;
}

std::shared_ptr<Process> KernelSystem::GetCurrentProcess() const {
    return current_process;
}

void KernelSystem::SetCurrentProcess(std::shared_ptr<Process> process) {
    current_process = process;
    SetCurrentMemoryPageTable(process->vm_manager.page_table);
}

void KernelSystem::SetCurrentProcessForCPU(std::shared_ptr<Process> process, u32 core_id) {
    if (current_cpu->GetID() == core_id) {
        current_process = process;
        SetCurrentMemoryPageTable(process->vm_manager.page_table);
    } else {
        stored_processes[core_id] = process;
        thread_managers[core_id]->cpu->SetPageTable(process->vm_manager.page_table);
    }
}

void KernelSystem::SetCurrentMemoryPageTable(std::shared_ptr<Memory::PageTable> page_table) {
    memory.SetCurrentPageTable(page_table);
    if (current_cpu != nullptr) {
        current_cpu->SetPageTable(page_table);
    }
}

void KernelSystem::SetCPUs(std::vector<std::shared_ptr<Core::ARM_Interface>> cpus) {
    ASSERT(cpus.size() == thread_managers.size());
    for (u32 i = 0; i < cpus.size(); i++) {
        thread_managers[i]->SetCPU(*cpus[i]);
    }
}

void KernelSystem::SetRunningCPU(Core::ARM_Interface* cpu) {
    if (current_process) {
        stored_processes[current_cpu->GetID()] = current_process;
    }
    current_cpu = cpu;
    timing.SetCurrentTimer(cpu->GetID());
    if (stored_processes[current_cpu->GetID()]) {
        SetCurrentProcess(stored_processes[current_cpu->GetID()]);
    }
}

ThreadManager& KernelSystem::GetThreadManager(u32 core_id) {
    return *thread_managers[core_id];
}

const ThreadManager& KernelSystem::GetThreadManager(u32 core_id) const {
    return *thread_managers[core_id];
}

ThreadManager& KernelSystem::GetCurrentThreadManager() {
    return *thread_managers[current_cpu->GetID()];
}

const ThreadManager& KernelSystem::GetCurrentThreadManager() const {
    return *thread_managers[current_cpu->GetID()];
}

namespace {
const char* ThreadStatusName(ThreadStatus status) {
    switch (status) {
    case ThreadStatus::Running:
        return "Running";
    case ThreadStatus::Ready:
        return "Ready";
    case ThreadStatus::WaitArb:
        return "WaitArb";
    case ThreadStatus::WaitSleep:
        return "WaitSleep";
    case ThreadStatus::WaitIPC:
        return "WaitIPC";
    case ThreadStatus::WaitSynchAny:
        return "WaitSynchAny";
    case ThreadStatus::WaitSynchAll:
        return "WaitSynchAll";
    case ThreadStatus::WaitHleEvent:
        return "WaitHleEvent";
    case ThreadStatus::Dormant:
        return "Dormant";
    case ThreadStatus::Dead:
        return "Dead";
    }
    return "Unknown";
}
} // Anonymous namespace

// xappify fork — see the header.
void KernelSystem::SamplePc() {
    for (const auto& manager : thread_managers) {
        const Thread* current = manager->GetCurrentThread();
        if (current == nullptr) {
            continue;
        }
        pc_samples[pc_sample_head] = {current->thread_id,
                                      static_cast<u32>(current->context.GetProgramCounter())};
        pc_sample_head = (pc_sample_head + 1) % PcSampleCount;
        ++pc_samples_taken;
    }
}

/// Renders the PC ring as a frequency histogram: how many samples, over how many distinct PCs, and
/// the hottest few. Kept local to the dump — nothing else has a use for it.
static std::string SummarisePcSamples(const std::vector<std::pair<u32, u32>>& samples) {
    if (samples.empty()) {
        return "no samples";
    }

    std::map<u32, u32> counts; // pc -> hits
    for (const auto& [pc, hits] : samples) {
        counts[pc] += hits;
    }

    std::vector<std::pair<u32, u32>> ranked{counts.begin(), counts.end()};
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::size_t total = 0;
    for (const auto& [pc, hits] : ranked) {
        total += hits;
    }

    std::string hottest;
    for (std::size_t i = 0; i < ranked.size() && i < 6; ++i) {
        if (!hottest.empty()) {
            hottest += " ";
        }
        hottest += fmt::format("{:#010x}x{}", ranked[i].first, ranked[i].second);
    }

    return fmt::format("samples={} distinctPCs={} hottest=[{}]", total, ranked.size(), hottest);
}

/// Distinct PCs from a thread's sample bucket, hottest first — the addresses worth disassembling.
static std::vector<u32> HotPcs(const std::vector<std::pair<u32, u32>>& samples, std::size_t limit) {
    std::map<u32, u32> counts;
    for (const auto& [pc, hits] : samples) {
        counts[pc] += hits;
    }
    std::vector<std::pair<u32, u32>> ranked{counts.begin(), counts.end()};
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<u32> pcs;
    for (std::size_t i = 0; i < ranked.size() && i < limit; ++i) {
        pcs.push_back(ranked[i].first);
    }
    return pcs;
}

// xappify fork — see the header for why this exists and why it is emulation-thread only.
void KernelSystem::LogGuestThreadState(const char* marker) {
    LOG_CRITICAL(Kernel, "[guest-state] {} BEGIN cores={} pcSamplesTaken={}", marker,
                 thread_managers.size(), pc_samples_taken);

    // Bucket the ring by thread so each thread's line below can carry its own spin signature.
    // Only the filled part of the ring is valid before it has wrapped once.
    const std::size_t valid = std::min(pc_samples_taken, PcSampleCount);
    std::map<u32, std::vector<std::pair<u32, u32>>> samples_by_thread;
    for (std::size_t i = 0; i < valid; ++i) {
        const auto& sample = pc_samples[(pc_sample_head + PcSampleCount - 1 - i) % PcSampleCount];
        samples_by_thread[sample.thread_id].emplace_back(sample.pc, 1u);
    }

    for (u32 core_id = 0; core_id < thread_managers.size(); ++core_id) {
        const ThreadManager& manager = *thread_managers[core_id];
        const Thread* current = manager.GetCurrentThread();

        for (const auto& thread : manager.GetThreadList()) {
            if (!thread) {
                continue;
            }

            // The wait list is the payload: a deadlocked title shows one or more threads parked on
            // an object that will never be signalled, and the object's NAME is what identifies the
            // subsystem at fault (a service session, a GSP/DSP interrupt event, a mutex).
            std::string waiting_on;
            for (const auto& object : thread->wait_objects) {
                if (!waiting_on.empty()) {
                    waiting_on += ", ";
                }
                if (object) {
                    waiting_on +=
                        fmt::format("{} \"{}\"", object->GetTypeName(), object->GetName());
                } else {
                    waiting_on += "<null>";
                }
            }
            if (thread->status == ThreadStatus::WaitArb) {
                // WaitArb parks on an address rather than an object, so the list above is empty.
                waiting_on = fmt::format("AddressArbiter @ {:#010x}", thread->wait_address);
            }

            // The PC histogram is what separates "spinning in a poll loop" from "running normally"
            // — the single `pc=` below is one instant and cannot. Only threads that were actually
            // scheduled recently have samples; the rest report none, which is itself informative.
            const auto samples = samples_by_thread.find(thread->thread_id);
            const std::string pc_summary = samples == samples_by_thread.end()
                                               ? std::string{"no samples"}
                                               : SummarisePcSamples(samples->second);

            LOG_CRITICAL(Kernel,
                         "[guest-state] {} core={} tid={} name=\"{}\" status={} prio={} "
                         "pc={:#010x} current={} heldMutexes={} pendingMutexes={} waiting-on=[{}] "
                         "{}",
                         marker, core_id, thread->thread_id, thread->name,
                         ThreadStatusName(thread->status), thread->current_priority,
                         thread->context.GetProgramCounter(), thread.get() == current ? 1 : 0,
                         thread->held_mutexes.size(), thread->pending_mutexes.size(), waiting_on,
                         pc_summary);

            // For the RUNNING thread only, dump enough to identify what it is polling.
            //
            // A wedged title shows up here as a handful of PCs looping forever with no SVC — but the
            // histogram cannot say WHICH memory location the loop is waiting on, and that is the
            // whole question. Registers plus the raw instruction words answer it: decode the `ldr`
            // at the loop head, take its base register from the dump, and the address falls out.
            // Two dumps ten seconds apart then say whether the value ever moves.
            //
            // Deliberately NOT disassembled here — four raw words are trivial to decode by hand, and
            // a decoder is code that can be wrong in a way the raw words cannot be.
            if (thread.get() != current || samples == samples_by_thread.end()) {
                continue;
            }

            // r13/r14/r15 are sp/lr/pc and are printed by name below, so stop at r12.
            std::string registers;
            for (std::size_t i = 0; i < 13; ++i) {
                registers += fmt::format("r{}={:08x} ", i, thread->context.cpu_registers[i]);
            }
            LOG_CRITICAL(Kernel, "[guest-state] {} tid={} SPIN {}sp={:08x} lr={:08x} pc={:08x} cpsr={:08x}",
                         marker, thread->thread_id, registers, thread->context.GetStackPointer(),
                         thread->context.GetLinkRegister(), thread->context.GetProgramCounter(),
                         thread->context.cpsr);

            for (const u32 pc : HotPcs(samples->second, 8)) {
                // Guest memory, read through the same MemorySystem the arbiter reads values from.
                // An unmapped PC cannot happen for an address we just sampled executing.
                LOG_CRITICAL(Kernel, "[guest-state] {} tid={} code {:#010x}: {:08x}", marker,
                             thread->thread_id, pc, memory.Read32(pc));
            }

            // The words around whatever the loop is dereferencing. The device capture that motivated
            // this showed a list node whose `next` pointed at itself; seeing the neighbouring fields
            // says whether that is one bad pointer in an otherwise live structure (someone wrote a
            // single field) or wholesale garbage (a stale or freed allocation). Those need different
            // culprits, and it is four lines to tell them apart.
            //
            // Anchored on r0/r1 because the ARM `ldr rX, [rY, #imm]` idiom that dominates these loops
            // keeps the object pointer in a low register; both are printed since which one holds it
            // depends on where in the loop the dump landed.
            for (const u32 base : {thread->context.cpu_registers[0], thread->context.cpu_registers[1]}) {
                if (base < 0x1000) {
                    continue; // Not a pointer.
                }
                std::string words;
                for (u32 offset = 0; offset < 0x40; offset += 4) {
                    words += fmt::format("{:08x} ", memory.Read32(base - 0x20 + offset));
                }
                LOG_CRITICAL(Kernel, "[guest-state] {} tid={} mem {:#010x}-0x20: {}", marker,
                             thread->thread_id, base, words);
            }
        }
    }

    LOG_CRITICAL(Kernel, "[guest-state] {} END", marker);
}

TimerManager& KernelSystem::GetTimerManager() {
    return *timer_manager;
}

const TimerManager& KernelSystem::GetTimerManager() const {
    return *timer_manager;
}

SharedPage::Handler& KernelSystem::GetSharedPageHandler() {
    return *shared_page_handler;
}

const SharedPage::Handler& KernelSystem::GetSharedPageHandler() const {
    return *shared_page_handler;
}

ConfigMem::Handler& KernelSystem::GetConfigMemHandler() {
    return *config_mem_handler;
}

IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() {
    return *ipc_recorder;
}

const IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() const {
    return *ipc_recorder;
}

std::unique_ptr<IPCDebugger::Recorder> KernelSystem::BackupIPCRecorder() {
    return std::move(ipc_recorder);
}

void KernelSystem::RestoreIPCRecorder(std::unique_ptr<IPCDebugger::Recorder> recorder) {
    ipc_recorder = std::move(recorder);
}

void KernelSystem::AddNamedPort(std::string name, std::shared_ptr<ClientPort> port) {
    named_ports.emplace(std::move(name), std::move(port));
}

u32 KernelSystem::NewThreadId() {
    return next_thread_id++;
}

void KernelSystem::ResetThreadIDs() {
    next_thread_id = 0;
}

void KernelSystem::UpdateCPUAndMemoryState(u64 title_id, MemoryMode memory_mode,
                                           New3dsHwCapabilities n3ds_hw_cap) {
    if (Settings::values.is_new_3ds) {
        SetRunning804MHz(n3ds_hw_cap.enable_804MHz_cpu);
    }

    u32 tid_high = static_cast<u32>(title_id >> 32);

    constexpr u32 TID_HIGH_APPLET = 0x00040030;
    constexpr u32 TID_HIGH_SYSMODULE = 0x00040130;

    // PM only updates the reported memory for normal applications.
    // TODO(PabloMK7): Using the title ID is not correct, but close enough.
    if (tid_high != TID_HIGH_APPLET && tid_high != TID_HIGH_SYSMODULE) {
        UpdateReportedMemory(memory_mode, n3ds_hw_cap.memory_mode);
    }
}

void KernelSystem::RestoreMemoryState(u64 title_id) {
    u32 tid_high = static_cast<u32>(title_id >> 32);

    constexpr u32 TID_HIGH_APPLET = 0x00040030;
    constexpr u32 TID_HIGH_SYSMODULE = 0x00040130;

    // PM only updates the reported memory for normal applications.
    // TODO(PabloMK7): Using the title ID is not correct, but close enough.
    if (tid_high != TID_HIGH_APPLET && tid_high != TID_HIGH_SYSMODULE) {
        RestoreReportedMemory();
    }
}

template <class Archive>
void KernelSystem::serialize(Archive& ar, const unsigned int) {
    ar & memory_regions;
    ar & named_ports;
    // current_cpu set externally
    // NB: subsystem references and prepare_reschedule_callback are constant
    ar&* resource_limits.get();
    ar & next_object_id;
    ar&* timer_manager.get();
    ar & next_process_id;
    ar & process_list;
    ar & current_process;
    // NB: core count checked in 'core'
    for (auto& thread_manager : thread_managers) {
        ar&* thread_manager.get();
    }
    ar & config_mem_handler;
    ar & shared_page_handler;
    ar & stored_processes;
    ar & next_thread_id;
    ar & memory_mode;
    ar & running_804MHz;
    ar & main_thread_extended_sleep;
    // Deliberately don't include debugger info to allow debugging through loads

    if (Archive::is_loading::value) {
        for (auto& memory_region : memory_regions) {
            memory_region->Unlock();
        }
        for (auto& process : process_list) {
            process->vm_manager.Unlock();
        }
    }
}
SERIALIZE_IMPL(KernelSystem)

template <class Archive>
void New3dsHwCapabilities::serialize(Archive& ar, const unsigned int) {
    ar & enable_l2_cache;
    ar & enable_804MHz_cpu;
    ar & memory_mode;
}
SERIALIZE_IMPL(New3dsHwCapabilities)

} // namespace Kernel
