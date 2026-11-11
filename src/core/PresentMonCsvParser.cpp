#include "core/PresentMonCsvParser.h"

#include <algorithm>
#include <cctype>
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

std::optional<std::int64_t> toInt64(std::string_view s)
{
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size())
        return std::nullopt;
    return value;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

// Column names changed case between PresentMon versions (MsBetweenPresents vs
// msBetweenPresents), so match without regard to case.
int findColumn(const std::vector<std::string_view>& cols,
               std::initializer_list<std::string_view> names)
{
    for (const auto name : names) {
        const auto it = std::ranges::find_if(cols, [name](std::string_view c) { return equalsIgnoreCase(c, name); });
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
    const auto freq = m_qpcFrequency;
    *this = PresentMonCsvParser{};
    m_qpcFrequency = freq;
}

bool PresentMonCsvParser::parseHeader(std::string_view line)
{
    const auto cols = splitCsv(line);

    const int frameTime = findColumn(cols, {"FrameTime", "MsBetweenPresents"});
    if (frameTime < 0)
        return false;

    m_frameTimeCol = frameTime;
    m_header = std::string{line};
    m_cpuBusyCol = findColumn(cols, {"CPUBusy", "MsCPUBusy"});
    m_gpuBusyCol = findColumn(cols, {"GPUBusy", "MsGPUBusy", "MsGPUActive"});
    m_pidCol = findColumn(cols, {"ProcessID"});
    m_appCol = findColumn(cols, {"Application"});
    m_swapChainCol = findColumn(cols, {"SwapChainAddress"});
    m_frameTypeCol = findColumn(cols, {"FrameType"});
    m_displayedCol = findColumn(cols, {"DisplayedTime", "MsBetweenDisplayChange"});
    m_droppedCol = findColumn(cols, {"Dropped"});

    // QPC time column (PresentMon --qpc_time). Names differ between versions.
    m_qpcCol = -1;
    m_qpcIsFrameEnd = false;
    m_qpcHeuristic = false;
    if (const int c = findColumn(cols, {"CPUStartQPC"}); c >= 0) {
        m_qpcCol = c;
    } else if (const int v1 = findColumn(cols, {"QPCTime"}); v1 >= 0) {
        m_qpcCol = v1;
        m_qpcIsFrameEnd = true;
    } else if (const int t = findColumn(cols, {"CPUStartTime"}); t >= 0) {
        // Some versions keep the name and just switch the unit to ticks.
        m_qpcCol = t;
        m_qpcHeuristic = true;
    }

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

    std::string key{column(cols, m_pidCol)};
    key += ':';
    key += column(cols, m_swapChainCol);
    if (m_lockedKey.empty())
        m_lockedKey = key;
    else if (key != m_lockedKey) {
        ++m_skippedRows;
        return std::nullopt;
    }

    // With frame generation (and --track_frame_type), generated frames come as
    // rows of their own. Only application frames say something about how the
    // game runs; generated ones are counted on the next application frame.
    const auto displayed = toDouble(column(cols, m_displayedCol));
    if (const auto type = column(cols, m_frameTypeCol);
        !type.empty() && type != "Application" && type != "NA" && type != "Unknown") {
        ++m_generatedRows;
        if (m_frameGenName.empty())
            m_frameGenName = std::string{type};
        if (displayed && *displayed > 0.0 && m_pendingGenerated < 1000) {
            ++m_pendingGenerated;
            m_pendingGeneratedMs += *displayed;
        }
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
    if (m_displayedCol >= 0) {
        rec.displayTracked = true;
        const bool droppedFlag = column(cols, m_droppedCol) == "1";
        if (displayed && *displayed > 0.0 && !droppedFlag)
            rec.displayedMs = *displayed;
        rec.generatedFrames = m_pendingGenerated;
        rec.generatedDisplayedMs = m_pendingGeneratedMs;
    }
    m_pendingGenerated = 0;
    m_pendingGeneratedMs = 0.0;
    if (const auto qpc = toInt64(column(cols, m_qpcCol))) {
        // Seconds-since-start fit easily below 1e7; raw QPC ticks never do.
        if (!m_qpcHeuristic || *qpc > 10'000'000) {
            const auto ticks = static_cast<std::int64_t>(*frameTime * static_cast<double>(m_qpcFrequency) / 1000.0);
            rec.qpcStart = m_qpcIsFrameEnd ? *qpc - ticks : *qpc;
            rec.qpcEnd = *rec.qpcStart + ticks;
        }
    }
    if (const auto pid = toDouble(column(cols, m_pidCol)))
        rec.processId = static_cast<std::uint32_t>(*pid);
    rec.application = std::string{column(cols, m_appCol)};
    return rec;
}

std::vector<FrameRecord> loadPresentMonCsv(const std::filesystem::path& path, std::string* frameGenerationName)
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
    if (frameGenerationName)
        *frameGenerationName = parser.frameGenerationName();
    return frames;
}

} // namespace framescope::core
