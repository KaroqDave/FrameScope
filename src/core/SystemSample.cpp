#include "core/SystemSample.h"

#include <algorithm>

namespace framescope::core {

CpuTopology makeTopology(const std::vector<std::uint8_t>& ccdOfCpu, const std::vector<std::uint32_t>& l3KBOfCcd)
{
    CpuTopology t;
    t.ccdOfCpu = ccdOfCpu;
    t.l3KBOfCcd = l3KBOfCcd;
    if (l3KBOfCcd.size() > 1) {
        const auto [mn, mx] = std::ranges::minmax_element(l3KBOfCcd);
        if (*mx > *mn)
            t.vcacheCcd = static_cast<int>(mx - l3KBOfCcd.begin());
    }
    return t;
}

} // namespace framescope::core
