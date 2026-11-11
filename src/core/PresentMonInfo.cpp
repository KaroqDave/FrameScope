#include "core/PresentMonInfo.h"

namespace framescope::core {

PresentMonInfo parsePresentMonHelp(std::string_view text)
{
    PresentMonInfo info;

    // First line of the usage text: "PresentMon <version>"
    constexpr std::string_view marker = "PresentMon ";
    if (const auto pos = text.find(marker); pos != std::string_view::npos) {
        auto rest = text.substr(pos + marker.size());
        const auto end = rest.find_first_of(" \t\r\n");
        rest = rest.substr(0, end);
        if (!rest.empty() && ((rest.front() >= '0' && rest.front() <= '9') || rest.front() == 'v'))
            info.version = std::string{rest};
    }
    info.trackFrameType = text.find("--track_frame_type") != std::string_view::npos;
    return info;
}

} // namespace framescope::core
