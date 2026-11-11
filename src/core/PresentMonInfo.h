#pragma once

#include <string>
#include <string_view>

namespace framescope::core {

// What a PresentMon executable supports, read from its `--help` output.
// PresentMon exits with an error on any option it doesn't know, so optional
// flags are only passed when the help text lists them.
struct PresentMonInfo {
    std::string version;            // "2.3.1"; empty if not recognised
    bool trackFrameType = false;    // --track_frame_type (frame generation)

    [[nodiscard]] bool recognised() const noexcept { return !version.empty(); }
};

PresentMonInfo parsePresentMonHelp(std::string_view helpText);

} // namespace framescope::core
