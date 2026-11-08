#pragma once

#include "core/Correlator.h"
#include "core/FrameRecord.h"
#include "core/LatencyStats.h"
#include "core/SpikeDetector.h"

#include <filesystem>
#include <string>
#include <vector>

namespace framescope::core {

// Everything needed to review a capture later or compare it with another run.
// Stored as a small tab-separated text file (.fscope), easy to diff and to
// read from scripts.
struct SavedSession {
    std::string application;
    std::string createdAt;   // ISO 8601, local time
    std::string sensors;     // which sources were active
    std::vector<FrameRecord> frames;  // time, frame time, CPU/GPU busy
    std::vector<Spike> spikes;        // incl. attribution
    std::vector<Finding> findings;
    std::vector<ModuleLatency> drivers;
};

void saveSession(const std::filesystem::path& path, const SavedSession& session);

// Throws std::runtime_error on unreadable or malformed files.
SavedSession loadSession(const std::filesystem::path& path);

// True if the file starts with the .fscope magic line.
bool isSessionFile(const std::filesystem::path& path);

} // namespace framescope::core
