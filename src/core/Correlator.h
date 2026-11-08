#pragma once

#include "core/Attribution.h"
#include "core/EventTimeline.h"
#include "core/FrameRecord.h"
#include "core/SpikeDetector.h"

#include <cstdint>
#include <span>
#include <string>

namespace framescope::core {

struct CorrelatorConfig {
    double preWindowMs = 1.0;      // also look this far before the frame interval

    double dpcMinMs = 0.5;         // ignore DPC/ISR shorter than this as a cause
    double dpcStrongMs = 2.0;      // a single DPC/ISR this long is a cause on its own
    double dpcShare = 0.15;        // ... or DPC/ISR time >= this share of the excess

    double hardFaultShare = 0.20;  // game hard-fault time >= share of excess
    double diskShare = 0.30;       // longest disk I/O >= share of excess
};

// Everything the trace saw during one spike window.
struct WindowEvidence {
    double maxDpcIsrUs = 0.0;
    EventKind maxDpcIsrKind = EventKind::Dpc;
    std::string maxDpcIsrModule;
    double sumDpcIsrUs = 0.0;        // clipped to the window

    std::size_t gameHardFaults = 0;
    double gameHardFaultUs = 0.0;    // clipped to the window
    std::size_t allHardFaults = 0;

    double maxGameDiskUs = 0.0;
    double maxAnyDiskUs = 0.0;
};

WindowEvidence gatherEvidence(const EventTimeline& timeline, std::int64_t from, std::int64_t to,
                              std::uint32_t gamePid);

// Decide the most likely cause of one spike.
Attribution attribute(const Spike& spike, const FrameRecord& frame, const EventTimeline& timeline,
                      const CorrelatorConfig& cfg = {});

// Session-level summary across all attributed spikes.
struct Diagnosis {
    std::size_t attributed = 0;
    std::size_t system = 0;
    std::size_t game = 0;
    std::size_t mixed = 0;
    std::size_t unknown = 0;
    Cause topCause = Cause::NoTraceData;
    std::size_t topCauseCount = 0;
    std::string topModule;          // driver most often behind DriverLatency spikes
    std::string verdict;            // one line for the UI
};

Diagnosis diagnose(std::span<const Spike> spikes);

} // namespace framescope::core
