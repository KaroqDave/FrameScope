#pragma once

#include "core/Correlator.h"
#include "core/EventTimeline.h"
#include "core/FrameRecord.h"
#include "core/SessionStats.h"
#include "core/SpikeDetector.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace framescope::core {

// All frames and spikes of one capture, in time order.
// This is the in-memory timeline M2/M3 will attach system events to.
// A burst of spikes that runs straight into the game losing focus belongs to
// that switch (pausing, the task switcher, stopping the capture), not to play:
// marks the spikes leading up to a "not in foreground" spike the same way.
// A spike counts as part of the burst while the gap to the next one is at most
// maxGapS, up to maxReachS before the focus loss. Spikes with a system-side
// cause keep it and end the walk. Returns how many spikes were changed.
std::size_t markLeadUpToFocusLoss(std::span<Spike> spikes, double maxGapS = 1.0, double maxReachS = 5.0);

// The same for a burst that runs right up to the end of the capture: stopping
// it means pausing or leaving the game, and the capture may end before the
// focus loss is seen.
std::size_t markSlowdownAtCaptureEnd(std::span<Spike> spikes, double captureEndS, double maxGapS = 1.0,
                                     double maxReachS = 5.0);

// Loading screens and level changes: a stretch of slow frames (slowFrameMs or
// longer) nearly back to back, at most maxGapS between one ending and the next
// starting, that together took at least minStalledS. Ordinary stutter is a
// long frame between normal ones; here the game all but stopped presenting.
// Small spikes within maxGapS around the stretch go with it. Spikes with a
// system-side cause keep it. Returns how many spikes were changed.
std::size_t markLoading(std::span<Spike> spikes, double minStalledS = 1.5, double maxGapS = 0.25,
                        double slowFrameMs = 50.0);

class CaptureSession {
public:
    explicit CaptureSession(SpikeConfig cfg = {});

    std::optional<Spike> append(FrameRecord frame);
    void clear();

    // Replace everything with a saved session (frames + attributed spikes).
    // Sessions saved before frame generation was recognised from the present
    // pattern still hold the generated frames as frames of their own, and the
    // false spikes that came with them: the filter runs again over the frames.
    // If it folds any, spikes are detected anew and take over the cause of the
    // saved spike in the same frame. frameGeneration is the name stored with
    // the session; when empty, the detected pattern names it.
    void restore(std::vector<FrameRecord> frames, std::vector<Spike> spikes, std::string frameGeneration = {},
                 bool endedInPlay = false);

    // Generated frames the last restore() folded into their rendered frames
    // (0 = the saved session was used as it is), and the spikes it had before.
    [[nodiscard]] std::size_t refilteredFrames() const noexcept { return m_refilteredFrames; }
    [[nodiscard]] std::size_t spikesBeforeRefilter() const noexcept { return m_spikesBeforeRefilter; }

    [[nodiscard]] const std::vector<FrameRecord>& frames() const noexcept { return m_frames; }
    [[nodiscard]] const std::vector<Spike>& spikes() const noexcept { return m_spikes; }
    [[nodiscard]] SessionStats stats() const;

    // Frame generation seen in the capture ("AMD AFMF" ...), empty if none.
    void setFrameGeneration(std::string name) { m_frameGeneration = std::move(name); }
    [[nodiscard]] const std::string& frameGeneration() const noexcept { return m_frameGeneration; }

    // Attribute spikes whose frame the trace has fully covered, waiting
    // settleMs for late ETW buffers. Does nothing while no trace has started.
    // Returns how many spikes got a new attribution.
    std::size_t attributePending(const EventTimeline& timeline, double settleMs = 1500.0,
                                 const CorrelatorConfig& cfg = {});

    // The capture was stopped with the global hotkey while the game kept
    // running in front: nobody paused or left the game to stop it, so a
    // slowdown right at the end is stutter like any other.
    void setEndedInPlay(bool on) noexcept { m_endedInPlay = on; }
    [[nodiscard]] bool endedInPlay() const noexcept { return m_endedInPlay; }

    // The capture has ended: applies the rules above (the one about the
    // capture's end only if it didn't end in play). Returns the spikes changed.
    std::size_t closeOut();

    [[nodiscard]] Diagnosis diagnosis() const { return diagnose(m_spikes); }

private:
    SpikeDetector m_detector;
    std::vector<FrameRecord> m_frames;
    std::vector<Spike> m_spikes;
    std::string m_frameGeneration;
    bool m_endedInPlay = false;
    std::size_t m_refilteredFrames = 0;
    std::size_t m_spikesBeforeRefilter = 0;
};

} // namespace framescope::core
