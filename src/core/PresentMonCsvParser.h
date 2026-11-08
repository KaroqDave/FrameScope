#pragma once

#include "core/FrameRecord.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace framescope::core {

// Streaming parser for PresentMon CSV output (file or --output_stdout).
//
// Understands both the PresentMon 2.x column names (FrameTime, CPUBusy,
// GPUBusy) and the 1.x / --v1_metrics names (MsBetweenPresents, MsCPUBusy,
// MsGPUBusy / MsGPUActive).
//
// Lines before the header (status text) are ignored. The parser locks onto
// the first swap chain it sees, so frames from overlays or launcher windows
// in the same process don't get mixed in. The timeline is the running sum of
// frame times, which avoids depending on PresentMon's time-column format.
class PresentMonCsvParser {
public:
    // Feed one line (with or without the trailing '\r').
    // Returns a frame if the line was a valid data row of the tracked swap chain.
    std::optional<FrameRecord> feedLine(std::string_view line);

    void reset();

    [[nodiscard]] bool hasHeader() const noexcept { return m_frameTimeCol >= 0; }
    [[nodiscard]] std::size_t skippedRows() const noexcept { return m_skippedRows; }
    [[nodiscard]] const std::string& lockedSwapChain() const noexcept { return m_lockedKey; }

private:
    bool parseHeader(std::string_view line);

    int m_frameTimeCol = -1;
    int m_cpuBusyCol = -1;
    int m_gpuBusyCol = -1;
    int m_pidCol = -1;
    int m_appCol = -1;
    int m_swapChainCol = -1;
    int m_frameTypeCol = -1;
    int m_requiredColumns = 0;

    std::string m_lockedKey;
    double m_elapsedMs = 0.0;
    std::size_t m_skippedRows = 0;
};

// Convenience: load a whole CSV file. Throws std::runtime_error if the file
// can't be opened or contains no PresentMon header.
std::vector<FrameRecord> loadPresentMonCsv(const std::filesystem::path& path);

} // namespace framescope::core
