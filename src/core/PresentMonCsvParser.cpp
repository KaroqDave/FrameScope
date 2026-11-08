#include "core/PresentMonCsvParser.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <stdexcept>

namespace framescope::core {
namespace {

std::string_view trim(std::string_view s)
{
    constexpr std::string_view ws = " \t\r\n";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string_view::npos)
        return {};
    const auto last = s.find_last_not_of(ws);
    s = s.substr(first, last - first + 1);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}

// PresentMon doesn't emit quoted commas, so a plain split is enough.
std::vector<std::string_view> splitCsv(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const auto comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            out.push_back(trim(line.substr(start)));
            break;
        }
        out.push_back(trim(line.substr(start, comma - start)));
        start = comma + 1;
    }
    return out;
}

std::optional<double> toDouble(std::string_view s)
{
    if (s.empty() || s == "NA")
        return std::nullopt;
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        return std::nullopt;
    return value;
}

int findColumn(const std::vector<std::string_view>& cols,
               std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        const auto it = std::ranges::find(cols, name);
        if (it != cols.end())
            return static_cast<int>(it - cols.begin());
    }
    return -1;
}

std::string_view column(const std::vector<std::string_view>& cols, int idx)
{
    return (idx >= 0 && idx < static_cast<int>(cols.size())) ? cols[idx] : std::string_view{};
}

} // namespace

void PresentMonCsvParser::reset()
{
    *this = PresentMonCsvParser{};
}

bool PresentMonCsvParser::parseHeader(std::string_view line)
{
    const auto cols = splitCsv(line);

    const int frameTime = findColumn(cols, {"FrameTime", "MsBetweenPresents"});
    if (frameTime < 0)
        return false;

    m_frameTimeCol = frameTime;
    m_cpuBusyCol = findColumn(cols, {"CPUBusy", "MsCPUBusy"});
    m_gpuBusyCol = findColumn(cols, {"GPUBusy", "MsGPUBusy", "MsGPUActive"});
    m_pidCol = findColumn(cols, {"ProcessID"});
    m_appCol = findColumn(cols, {"Application"});
    m_swapChainCol = findColumn(cols, {"SwapChainAddress"});
    m_frameTypeCol = findColumn(cols, {"FrameType"});

    m_requiredColumns = 1 + std::max({m_frameTimeCol, m_pidCol, m_swapChainCol});
    return true;
}

std::optional<FrameRecord> PresentMonCsvParser::feedLine(std::string_view line)
{
    line = trim(line);
    if (line.empty())
        return std::nullopt;

    // A (repeated) header line starts a new block; re-read the column layout.
    if (!hasHeader() || line.starts_with("Application,")) {
        parseHeader(line);
        return std::nullopt;
    }

    const auto cols = splitCsv(line);
    if (static_cast<int>(cols.size()) < m_requiredColumns) {
        ++m_skippedRows;
        return std::nullopt;
    }

    // With frame generation, PresentMon also lists generated frames.
    // Only real application frames say something about stutter.
    if (const auto type = column(cols, m_frameTypeCol); !type.empty() && type != "Application") {
        ++m_skippedRows;
        return std::nullopt;
    }

    std::string key{column(cols, m_pidCol)};
    key += ':';
    key += column(cols, m_swapChainCol);
    if (m_lockedKey.empty())
        m_lockedKey = key;
    else if (key != m_lockedKey) {
        ++m_skippedRows;
        return std::nullopt;
    }

    const auto frameTime = toDouble(column(cols, m_frameTimeCol));
    if (!frameTime || *frameTime <= 0.0) {
        ++m_skippedRows;
        return std::nullopt;
    }

    m_elapsedMs += *frameTime;

    FrameRecord rec;
    rec.timeSeconds = m_elapsedMs / 1000.0;
    rec.frameTimeMs = *frameTime;
    rec.cpuBusyMs = toDouble(column(cols, m_cpuBusyCol));
    rec.gpuBusyMs = toDouble(column(cols, m_gpuBusyCol));
    if (const auto pid = toDouble(column(cols, m_pidCol)))
        rec.processId = static_cast<std::uint32_t>(*pid);
    rec.application = std::string{column(cols, m_appCol)};
    return rec;
}

std::vector<FrameRecord> loadPresentMonCsv(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Cannot open " + path.string());

    PresentMonCsvParser parser;
    std::vector<FrameRecord> frames;
    std::string line;
    while (std::getline(in, line)) {
        if (auto rec = parser.feedLine(line))
            frames.push_back(std::move(*rec));
    }

    if (!parser.hasHeader())
        throw std::runtime_error("No PresentMon header found in " + path.string());
    return frames;
}

} // namespace framescope::core
