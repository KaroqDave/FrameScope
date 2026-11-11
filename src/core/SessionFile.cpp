#include "core/SessionFile.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace framescope::core {
namespace {

constexpr std::string_view kMagic = "FRAMESCOPE";
constexpr int kVersion = 1;

// Tabs and newlines are the separators; keep them out of free text.
std::string clean(std::string_view s)
{
    std::string out{s};
    for (char& c : out) {
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    }
    return out;
}

std::vector<std::string_view> splitTabs(std::string_view line, std::size_t maxFields = 64)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (out.size() + 1 < maxFields) {
        const auto tab = line.find('\t', start);
        if (tab == std::string_view::npos)
            break;
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    out.push_back(line.substr(start));
    return out;
}

double num(std::string_view s)
{
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{})
        throw std::runtime_error(std::format("Bad number '{}' in session file", s));
    return v;
}

std::optional<double> optNum(std::string_view s)
{
    return s.empty() ? std::nullopt : std::optional<double>(num(s));
}

std::string optStr(const std::optional<double>& v)
{
    return v ? std::format("{}", *v) : std::string{};
}

template <typename E>
E enumOf(std::string_view s, int maxValue)
{
    const int v = static_cast<int>(num(s));
    if (v < 0 || v > maxValue)
        throw std::runtime_error("Bad enum value in session file");
    return static_cast<E>(v);
}

} // namespace

void saveSession(const std::filesystem::path& path, const SavedSession& s)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot write " + path.string());

    out << kMagic << '\t' << kVersion << '\n';
    out << "app\t" << clean(s.application) << '\n';
    out << "created\t" << clean(s.createdAt) << '\n';
    out << "sensors\t" << clean(s.sensors) << '\n';
    out << "gamepid\t" << s.gamePid << '\n';
    if (!s.frameGeneration.empty())
        out << "framegen\t" << clean(s.frameGeneration) << '\n';

    out << "[frames]\n";
    for (const auto& f : s.frames)
    {
        out << std::format("{}\t{}\t{}\t{}", f.timeSeconds, f.frameTimeMs, optStr(f.cpuBusyMs), optStr(f.gpuBusyMs));
        // Display columns only when tracked: time on screen ("x" = never shown),
        // generated frames shown with it, their total time on screen.
        if (f.displayTracked) {
            out << '\t' << (f.displayedMs ? std::format("{}", *f.displayedMs) : std::string{"x"});
            if (f.generatedFrames > 0)
                out << std::format("\t{}\t{}", f.generatedFrames, f.generatedDisplayedMs);
        }
        out << '\n';
    }

    out << "[spikes]\n";
    for (const auto& sp : s.spikes) {
        out << std::format("{}\t{}\t{}", sp.frameIndex, static_cast<int>(sp.bound), sp.medianMs);
        if (sp.attribution) {
            const auto& a = *sp.attribution;
            out << std::format("\t1\t{}\t{}\t{}\t{}\t{}", static_cast<int>(a.cause), static_cast<int>(a.side),
                               a.confidence, clean(a.module), clean(a.detail));
        } else {
            out << "\t0";
        }
        out << '\n';
    }

    out << "[findings]\n";
    for (const auto& f : s.findings)
        out << std::format("{}\t{}\t{}\n", static_cast<int>(f.level), clean(f.title), clean(f.detail));

    out << "[drivers]\n";
    for (const auto& d : s.drivers) {
        out << std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", clean(d.module), d.dpcCount, d.isrCount, d.maxDpcUs,
                           d.maxIsrUs, d.totalUs, d.over500us, d.over1ms);
    }
    out << "[shader]\n";
    for (const auto& w : s.shaderWrites)
        out << std::format("{}\t{}\t{}\t{}\n", w.timeSeconds, w.processId, w.bytes, w.disk ? 1 : 0);

    if (!out)
        throw std::runtime_error("Write failed: " + path.string());
}

std::vector<ShaderWriteRecord> shaderWritesForSession(const EventTimeline& tl, std::span<const FrameRecord> frames)
{
    std::vector<ShaderWriteRecord> out;
    const auto ref = std::ranges::find_if(frames, [](const FrameRecord& f) { return f.qpcStart.has_value(); });
    if (ref == frames.end())
        return out;
    // Session time of the reference frame's start
    const double refStartS = ref->timeSeconds - ref->frameTimeMs / 1000.0;
    for (const auto& e : tl.events()) {
        if (e.kind != EventKind::ShaderCacheWrite)
            continue;
        const double t = refStartS + tl.toMicroseconds(e.startQpc - *ref->qpcStart) / 1e6;
        out.push_back({t, e.processId, e.bytes, e.module == "disk"});
    }
    return out;
}

bool isSessionFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string line;
    return in && std::getline(in, line) && line.starts_with(kMagic);
}

SavedSession loadSession(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open " + path.string());

    std::string line;
    if (!std::getline(in, line) || !line.starts_with(kMagic))
        throw std::runtime_error(path.string() + " is not a FrameScope session");
    if (const auto f = splitTabs(line); f.size() < 2 || num(f[1]) > kVersion)
        throw std::runtime_error("Session file is from a newer FrameScope version");

    SavedSession s;
    std::string section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line.front() == '[') {
            section = line;
            continue;
        }

        if (section.empty()) {
            const auto f = splitTabs(line, 2);
            const auto value = f.size() > 1 ? std::string{f[1]} : std::string{};
            if (f[0] == "app")
                s.application = value;
            else if (f[0] == "created")
                s.createdAt = value;
            else if (f[0] == "sensors")
                s.sensors = value;
            else if (f[0] == "gamepid")
                s.gamePid = static_cast<std::uint32_t>(num(value));
            else if (f[0] == "framegen")
                s.frameGeneration = value;
        } else if (section == "[frames]") {
            const auto f = splitTabs(line);
            if (f.size() < 4)
                throw std::runtime_error("Bad frame line in session file");
            FrameRecord r;
            r.timeSeconds = num(f[0]);
            r.frameTimeMs = num(f[1]);
            r.cpuBusyMs = optNum(f[2]);
            r.gpuBusyMs = optNum(f[3]);
            if (f.size() > 4 && !f[4].empty()) {
                r.displayTracked = true;
                if (f[4] != "x")
                    r.displayedMs = num(f[4]);
                if (f.size() > 6) {
                    r.generatedFrames = static_cast<std::uint16_t>(num(f[5]));
                    r.generatedDisplayedMs = num(f[6]);
                }
            }
            r.application = s.application;
            s.frames.push_back(std::move(r));
        } else if (section == "[spikes]") {
            const auto f = splitTabs(line, 9); // detail is last and may contain anything but tabs
            if (f.size() < 4)
                throw std::runtime_error("Bad spike line in session file");
            Spike sp;
            sp.frameIndex = static_cast<std::size_t>(num(f[0]));
            if (sp.frameIndex >= s.frames.size())
                throw std::runtime_error("Spike refers to a missing frame");
            sp.bound = enumOf<Bound>(f[1], static_cast<int>(Bound::Stall));
            sp.medianMs = num(f[2]);
            sp.timeSeconds = s.frames[sp.frameIndex].timeSeconds;
            sp.frameTimeMs = s.frames[sp.frameIndex].frameTimeMs;
            if (f[3] == "1" && f.size() >= 9) {
                sp.attribution = Attribution{enumOf<Cause>(f[4], static_cast<int>(Cause::NotInForeground)),
                                             enumOf<Side>(f[5], static_cast<int>(Side::Mixed)), num(f[6]),
                                             std::string{f[8]}, std::string{f[7]}};
            }
            s.spikes.push_back(std::move(sp));
        } else if (section == "[findings]") {
            const auto f = splitTabs(line, 3);
            if (f.size() < 3)
                throw std::runtime_error("Bad finding line in session file");
            s.findings.push_back({enumOf<Finding::Level>(f[0], static_cast<int>(Finding::Level::Problem)),
                                  std::string{f[1]}, std::string{f[2]}});
        } else if (section == "[shader]") {
            const auto f = splitTabs(line);
            if (f.size() < 4)
                throw std::runtime_error("Bad shader line in session file");
            s.shaderWrites.push_back({num(f[0]), static_cast<std::uint32_t>(num(f[1])),
                                      static_cast<std::uint32_t>(num(f[2])), f[3] == "1"});
        } else if (section == "[drivers]") {
            const auto f = splitTabs(line);
            if (f.size() < 8)
                throw std::runtime_error("Bad driver line in session file");
            ModuleLatency d;
            d.module = std::string{f[0]};
            d.dpcCount = static_cast<std::uint64_t>(num(f[1]));
            d.isrCount = static_cast<std::uint64_t>(num(f[2]));
            d.maxDpcUs = num(f[3]);
            d.maxIsrUs = num(f[4]);
            d.totalUs = num(f[5]);
            d.over500us = static_cast<std::uint64_t>(num(f[6]));
            d.over1ms = static_cast<std::uint64_t>(num(f[7]));
            s.drivers.push_back(std::move(d));
        }
    }
    return s;
}

} // namespace framescope::core
