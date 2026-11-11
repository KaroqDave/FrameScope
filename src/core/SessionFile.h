#pragma once

#include "core/Correlator.h"
#include "core/EventTimeline.h"
#include "core/FrameRecord.h"
#include "core/LatencyStats.h"
#include "core/SpikeDetector.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace framescope::core {

// Everything needed to review a capture later or compare it with another run.
// Stored as a small tab-separated text file (.fscope), easy to diff and to
// read from scripts.
// A shader cache write, on the session's time axis (seconds, like frames).
struct ShaderWriteRecord {
    double timeSeconds = 0.0;
    std::uint32_t processId = 0;
    std::uint32_t bytes = 0;
    bool disk = false; // flushed to disk by the system (timing lags)
};

struct SavedSession {
    std::string application;
    std::string createdAt;   // ISO 8601, local time
    std::string sensors;     // which sources were active
    std::vector<FrameRecord> frames;  // time, frame time, CPU/GPU busy
    std::vector<Spike> spikes;        // incl. attribution
    std::vector<Finding> findings;
    std::vector<ModuleLatency> drivers;
    std::uint32_t gamePid = 0;
    std::string frameGeneration; // "AMD AFMF", "AMD FSR-FG" ...; empty if none seen
    std::vector<ShaderWriteRecord> shaderWrites;
};

// Maps shader cache write events from the timeline onto the session's time
// axis, using the first frame with QPC times as reference.
std::vector<ShaderWriteRecord> shaderWritesForSession(const EventTimeline& timeline,
                                                     std::span<const FrameRecord> frames);

void saveSession(const std::filesystem::path& path, const SavedSession& session);

// Throws std::runtime_error on unreadable or malformed files.
SavedSession loadSession(const std::filesystem::path& path);

// True if the file starts with the .fscope magic line.
bool isSessionFile(const std::filesystem::path& path);

} // namespace framescope::core
