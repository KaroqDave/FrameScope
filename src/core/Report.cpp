#include "core/Report.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>

namespace framescope::core {

std::string htmlEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

namespace {

std::string_view sideClass(Side s)
{
    switch (s) {
    case Side::System:  return "system";
    case Side::Game:    return "game";
    case Side::Mixed:   return "mixed";
    case Side::Unknown: break;
    }
    return "unknown";
}

std::string_view levelClass(Finding::Level l)
{
    switch (l) {
    case Finding::Level::Good:    return "good";
    case Finding::Level::Info:    return "info";
    case Finding::Level::Warning: return "warning";
    case Finding::Level::Problem: return "problem";
    }
    return "info";
}

constexpr std::string_view kStyle = R"(
:root { --bg:#fff; --fg:#1d2026; --muted:#6b7280; --line:#e5e7eb; --card:#f7f8fa; --plot:#1b1d22; --trace:#4fc3f7;
        --system:#e53935; --game:#5c6bc0; --mixed:#ef8f00; --unknown:#9e9e9e; --good:#43a047; --info:#607d8b; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#14161a; --fg:#e6e8ec; --muted:#9aa1ad; --line:#2a2e36; --card:#1c1f25; } }
* { box-sizing: border-box; }
body { margin:0; padding:32px; background:var(--bg); color:var(--fg);
       font:15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; max-width:1100px; margin-inline:auto; }
h1 { font-size:26px; margin:0 0 4px; } h2 { font-size:18px; margin:32px 0 10px; }
.meta { color:var(--muted); font-size:13px; }
.verdict { margin:20px 0; padding:16px 20px; border-radius:10px; background:var(--card); border-left:5px solid var(--info);
           font-size:17px; font-weight:600; }
.verdict.system { border-color:var(--system); } .verdict.game { border-color:var(--game); } .verdict.mixed { border-color:var(--mixed); }
table { border-collapse:collapse; width:100%; font-size:14px; }
th, td { text-align:left; padding:7px 10px; border-bottom:1px solid var(--line); vertical-align:top; }
th { font-weight:600; color:var(--muted); font-size:12px; text-transform:uppercase; letter-spacing:.03em; }
td.num, th.num { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap; }
.stats { display:grid; grid-template-columns:repeat(auto-fit, minmax(150px, 1fr)); gap:12px; }
.stat { background:var(--card); border-radius:10px; padding:12px 14px; }
.stat b { display:block; font-size:22px; } .stat span { color:var(--muted); font-size:12px; }
.tag { display:inline-block; padding:1px 8px; border-radius:99px; font-size:12px; font-weight:600; color:#fff; }
.tag.system { background:var(--system); } .tag.game { background:var(--game); } .tag.mixed { background:var(--mixed); }
.tag.unknown { background:var(--unknown); } .tag.good { background:var(--good); } .tag.info { background:var(--info); }
.tag.warning { background:var(--mixed); } .tag.problem { background:var(--system); }
svg.chart { width:100%; height:auto; border-radius:10px; display:block; }
.legend { color:var(--muted); font-size:12px; margin-top:6px; }
.legend i { display:inline-block; width:10px; height:10px; border-radius:50%; margin:0 4px 0 12px; vertical-align:-1px; }
.note { color:var(--muted); font-size:13px; }
footer { margin-top:40px; color:var(--muted); font-size:12px; }
)";

std::string spikeColor(const Spike& s)
{
    if (!s.attribution)
        return "#e0e0e0";
    switch (s.attribution->side) {
    case Side::System:  return "#ff5252";
    case Side::Game:    return "#9ca3ff";
    case Side::Mixed:   return "#ffb300";
    case Side::Unknown: break;
    }
    return "#e0e0e0";
}

std::string renderChart(std::span<const FrameRecord> frames, std::span<const Spike> spikes)
{
    if (frames.empty())
        return "<p class=\"note\">No frames.</p>";

    constexpr double W = 1000, H = 260, L = 52, R = 12, T = 12, B = 26;
    const double pw = W - L - R, ph = H - T - B;
    const double t0 = 0.0, t1 = std::max(1.0, frames.back().timeSeconds);

    double maxMs = 0.0;
    for (const auto& f : frames)
        maxMs = std::max(maxMs, f.frameTimeMs);
    const double yMax = std::clamp(maxMs * 1.1, 35.0, 250.0);
    const auto x = [&](double t) { return L + (t - t0) / (t1 - t0) * pw; };
    const auto y = [&](double ms) { return T + ph - std::min(ms, yMax) / yMax * ph; };

    std::string svg = std::format(R"(<svg class="chart" viewBox="0 0 {} {}" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Frame times">)", W, H);
    svg += std::format(R"(<rect width="{}" height="{}" fill="#1b1d22"/>)", W, H);

    // Reference lines
    double lastY = 1e9;
    for (const double ms : {8.33, 16.67, 33.33, 50.0, 100.0, 200.0}) {
        if (ms > yMax)
            break;
        const double yy = y(ms);
        if (lastY - yy < 14)
            continue;
        lastY = yy;
        svg += std::format(R"(<line x1="{}" y1="{:.1f}" x2="{}" y2="{:.1f}" stroke="#333740" stroke-dasharray="4 4"/>)", L, yy, W - R, yy);
        svg += std::format(R"(<text x="{}" y="{:.1f}" fill="#a0a6b2" font-size="11" text-anchor="end" font-family="sans-serif">{:.0f} ms</text>)",
                           L - 6, yy + 4, ms);
    }
    // Time axis
    double step = 1;
    for (const double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0}) {
        step = s;
        if (t1 / s <= 10)
            break;
    }
    for (double t = 0; t <= t1; t += step) {
        svg += std::format(R"(<text x="{:.1f}" y="{}" fill="#a0a6b2" font-size="11" text-anchor="middle" font-family="sans-serif">{:.0f} s</text>)",
                           x(t), H - 8, t);
    }

    // Per-column min/max path
    const int columns = static_cast<int>(pw);
    std::vector<double> lo(columns, std::numeric_limits<double>::quiet_NaN()), hi = lo;
    for (const auto& f : frames) {
        const int c = std::clamp(static_cast<int>(x(f.timeSeconds) - L), 0, columns - 1);
        if (std::isnan(hi[c])) {
            lo[c] = hi[c] = f.frameTimeMs;
        } else {
            lo[c] = std::min(lo[c], f.frameTimeMs);
            hi[c] = std::max(hi[c], f.frameTimeMs);
        }
    }
    std::string path;
    for (int c = 0; c < columns; ++c) {
        if (std::isnan(hi[c]))
            continue;
        path += std::format("{}{:.1f} {:.1f}", path.empty() ? "M" : " L", L + c, y(lo[c]));
        if (hi[c] != lo[c])
            path += std::format(" L{:.1f} {:.1f}", L + c, y(hi[c]));
    }
    svg += std::format(R"(<path d="{}" fill="none" stroke="#4fc3f7" stroke-width="1.2"/>)", path);

    for (const auto& s : spikes) {
        svg += std::format(R"(<circle cx="{:.1f}" cy="{:.1f}" r="3.5" fill="{}"><title>{:.2f} s: {:.1f} ms</title></circle>)",
                           x(s.timeSeconds), y(s.frameTimeMs), spikeColor(s), s.timeSeconds, s.frameTimeMs);
    }
    svg += "</svg>";
    svg += R"(<div class="legend">Spikes:<i style="background:#ff5252"></i>system<i style="background:#9ca3ff"></i>game<i style="background:#ffb300"></i>mixed<i style="background:#e0e0e0"></i>unknown / no trace</div>)";
    return svg;
}

std::string renderFindings(const std::vector<Finding>& findings)
{
    const bool linked = std::ranges::any_of(findings, [](const Finding& f) { return f.spikes.has_value(); });
    std::string h = "<table><tr><th>Status</th><th>Check</th><th>Details</th>";
    h += linked ? "<th class=\"num\">Spikes</th></tr>" : "</tr>";
    for (const auto& f : findings) {
        h += std::format(R"(<tr><td><span class="tag {}">{}</span></td><td>{}</td><td>{}</td>)",
                         levelClass(f.level), toString(f.level), htmlEscape(f.title), htmlEscape(f.detail));
        if (linked)
            h += std::format(R"(<td class="num">{}</td>)", f.spikes ? std::format("{}", *f.spikes) : std::string{"—"});
        h += "</tr>";
    }
    return h + "</table>";
}

std::string stat(std::string_view label, std::string value)
{
    return std::format(R"(<div class="stat"><b>{}</b><span>{}</span></div>)", value, label);
}

} // namespace

std::string renderHtmlReport(const ReportData& d)
{
    const auto stats = computeStats(d.frames, d.spikes.size());
    const auto diag = diagnose(d.spikes);

    std::string h;
    h += "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    h += std::format("<title>{}</title><style>{}</style></head><body>", htmlEscape(d.title), kStyle);

    h += std::format("<h1>{}</h1>", htmlEscape(d.title));
    h += std::format(R"(<div class="meta">{}{}{}</div>)",
                     d.application.empty() ? "" : htmlEscape(d.application),
                     d.createdAt.empty() ? "" : " · " + htmlEscape(d.createdAt),
                     d.sensors.empty() ? "" : " · " + htmlEscape(d.sensors));

    // ---- Verdict
    const std::string verdictClass = diag.attributed == 0 ? "unknown"
                                     : 2 * diag.system >= diag.attributed ? "system"
                                     : 2 * diag.game >= diag.attributed   ? "game"
                                                                          : "mixed";
    const std::string verdict = d.spikes.empty()            ? "No stutter detected."
                                : diag.attributed == 0      ? std::format("{} spikes, no system trace (causes unknown).", d.spikes.size())
                                                            : diag.verdict;
    h += std::format(R"(<div class="verdict {}">{}</div>)", verdictClass, htmlEscape(verdict));

    // ---- Stats
    h += "<div class=\"stats\">";
    h += stat("Average FPS", std::format("{:.0f}", stats.averageFps));
    h += stat("1% low FPS", std::format("{:.0f}", stats.onePercentLowFps));
    h += stat("Worst frame", std::format("{:.0f} ms", stats.worstFrameMs));
    h += stat("Spikes", std::format("{}", stats.spikes));
    h += stat("Spikes per minute", std::format("{:.1f}", stats.spikesPerMinute));
    h += stat("Duration", std::format("{:.0f} s", stats.durationSeconds));
    h += "</div>";

    h += "<h2>Frame times</h2>" + renderChart(d.frames, d.spikes);

    // ---- Causes
    if (diag.attributed > 0) {
        std::map<std::pair<Cause, Side>, std::size_t> counts;
        for (const auto& s : d.spikes) {
            if (s.attribution && s.attribution->cause != Cause::NoTraceData)
                ++counts[{s.attribution->cause, s.attribution->side}];
        }
        std::vector<std::pair<std::pair<Cause, Side>, std::size_t>> rows(counts.begin(), counts.end());
        std::ranges::sort(rows, std::greater{}, &decltype(rows)::value_type::second);
        h += "<h2>Causes</h2><table><tr><th>Cause</th><th>Side</th><th class=\"num\">Spikes</th><th class=\"num\">Share</th></tr>";
        for (const auto& [key, n] : rows) {
            h += std::format(R"(<tr><td>{}</td><td><span class="tag {}">{}</span></td><td class="num">{}</td><td class="num">{:.0f}%</td></tr>)",
                             toString(key.first), sideClass(key.second), toString(key.second), n,
                             100.0 * static_cast<double>(n) / static_cast<double>(diag.attributed));
        }
        h += "</table>";
    }

    if (!d.findings.empty())
        h += "<h2>System checks</h2>" + renderFindings(d.findings);
    if (!d.baseline.empty())
        h += "<h2>Idle baseline</h2>" + renderFindings(d.baseline);

    // ---- Comparison
    if (d.comparison) {
        const auto& c = *d.comparison;
        h += "<h2>Run comparison</h2>";
        h += std::format(R"(<div class="verdict">{}</div>)", htmlEscape(c.verdict));
        h += "<table><tr><th></th><th class=\"num\">Run 1</th><th class=\"num\">Run 2</th></tr>";
        const auto row = [&](std::string_view label, std::string a, std::string b) {
            h += std::format(R"(<tr><td>{}</td><td class="num">{}</td><td class="num">{}</td></tr>)", label, a, b);
        };
        row("Duration", std::format("{:.0f} s", c.first.stats.durationSeconds), std::format("{:.0f} s", c.second.stats.durationSeconds));
        row("Average FPS", std::format("{:.0f}", c.first.stats.averageFps), std::format("{:.0f}", c.second.stats.averageFps));
        row("1% low FPS", std::format("{:.0f}", c.first.stats.onePercentLowFps), std::format("{:.0f}", c.second.stats.onePercentLowFps));
        row("Spikes", std::format("{}", c.first.stats.spikes), std::format("{}", c.second.stats.spikes));
        row("Spikes per minute", std::format("{:.1f}", c.first.stats.spikesPerMinute), std::format("{:.1f}", c.second.stats.spikesPerMinute));
        row("CPU-bound spikes", std::format("{}", c.first.cpuSpikes), std::format("{}", c.second.cpuSpikes));
        row("GPU-bound spikes", std::format("{}", c.first.gpuSpikes), std::format("{}", c.second.gpuSpikes));
        row("Stall spikes", std::format("{}", c.first.stallSpikes), std::format("{}", c.second.stallSpikes));
        h += "</table>";
        h += std::format(R"(<p class="note">{} spikes line up at the same moments ({:.0f}%), best alignment offset {:+.1f} s.</p>)",
                         c.matchedSpikes, c.repeatShare * 100.0, c.offsetSeconds);
        for (const auto& n : c.notes)
            h += std::format(R"(<p class="note">{}</p>)", htmlEscape(n));
    }

    // ---- Spikes
    if (!d.spikes.empty()) {
        std::vector<const Spike*> worst;
        for (const auto& s : d.spikes)
            worst.push_back(&s);
        std::ranges::sort(worst, [](const Spike* a, const Spike* b) { return a->excessMs() > b->excessMs(); });
        const auto shown = std::min(worst.size(), d.maxSpikeRows);
        h += std::format("<h2>Spikes{}</h2>", shown < worst.size() ? std::format(" (worst {} of {})", shown, worst.size()) : "");
        h += "<table><tr><th class=\"num\">Time</th><th class=\"num\">Frame</th><th class=\"num\">Excess</th><th>Bound</th>"
             "<th>Side</th><th>Cause</th><th class=\"num\">Conf.</th><th>Evidence</th></tr>";
        for (std::size_t i = 0; i < shown; ++i) {
            const auto& s = *worst[i];
            h += std::format(R"(<tr><td class="num">{:.2f} s</td><td class="num">{:.1f} ms</td><td class="num">{:.1f} ms</td><td>{}</td>)",
                             s.timeSeconds, s.frameTimeMs, s.excessMs(), toString(s.bound));
            if (s.attribution) {
                const auto& a = *s.attribution;
                h += std::format(R"(<td><span class="tag {}">{}</span></td><td>{}</td><td class="num">{:.0f}%</td><td>{}</td></tr>)",
                                 sideClass(a.side), toString(a.side), toString(a.cause), a.confidence * 100.0,
                                 htmlEscape(a.detail));
            } else {
                h += "<td>—</td><td>—</td><td class=\"num\">—</td><td>—</td></tr>";
            }
        }
        h += "</table>";
    }

    // ---- Drivers
    if (!d.drivers.empty()) {
        h += "<h2>Driver latency</h2><table><tr><th>Driver</th><th class=\"num\">DPCs</th><th class=\"num\">ISRs</th>"
             "<th class=\"num\">Max DPC</th><th class=\"num\">Max ISR</th><th class=\"num\">&gt; 0.5 ms</th><th class=\"num\">&gt; 1 ms</th></tr>";
        for (const auto& m : d.drivers) {
            h += std::format(R"(<tr><td>{}</td><td class="num">{}</td><td class="num">{}</td><td class="num">{:.2f} ms</td><td class="num">{:.2f} ms</td><td class="num">{}</td><td class="num">{}</td></tr>)",
                             htmlEscape(m.module), m.dpcCount, m.isrCount, m.maxDpcUs / 1000.0, m.maxIsrUs / 1000.0,
                             m.over500us, m.over1ms);
        }
        h += "</table>";
    }

    h += "<footer>Created with FrameScope " FRAMESCOPE_VERSION ". Spike = frame more than 2× the rolling median and at least 8 ms above it. "
         "Causes are likely explanations from system data during that frame, not proof.</footer>";
    h += "</body></html>\n";
    return h;
}

} // namespace framescope::core
