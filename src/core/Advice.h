#pragma once

#include "core/Attribution.h"
#include "core/Correlator.h"

#include <string>
#include <string_view>

namespace framescope::core {

// What to do about a cause of spikes. One or two short sentences; empty when
// there is nothing useful to say.
std::string_view adviceFor(Cause cause) noexcept;

// What to do about a finding (looked up by its title). Empty for findings
// that are fine or purely informational.
std::string adviceFor(const Finding& finding);

} // namespace framescope::core
