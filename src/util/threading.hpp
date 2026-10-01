#pragma once

#include <vector>

namespace mfa
{
void applyThreadScheduling(int rtPriority, std::vector<int> const& cpus);
} // namespace mfa
