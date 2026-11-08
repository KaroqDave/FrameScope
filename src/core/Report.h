#pragma once

#include "core/Comparison.h"
#include "core/Correlator.h"
#include "core/FrameRecord.h"
#include "core/LatencyStats.h"
#include "core/SessionStats.h"
#include "core/SpikeDetector.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace framescope::core {

struct ReportData {
    std::string title = "FrameScope report";
    std::string application;
    std::string createdAt;
    std::string sensors;
    std::span<const FrameRecord> frames;
    std::span<const Spike> spikes;
    std::vector<Finding> findings;
    std::vector<ModuleLatency> drivers;
    std::vector<Finding> baseline;            // idle baseline findings, if one was run
    std::optional<RunComparison> comparison;  // vs. an earlier run, if compared
    std::size_t maxSpikeRows = 100;           // worst spikes listed in full
};

// One self-contained HTML file (inline CSS + SVG chart, no scripts).
std::string renderHtmlReport(const ReportData& data);

std::string htmlEscape(std::string_view text);

} // namespace framescope::core
