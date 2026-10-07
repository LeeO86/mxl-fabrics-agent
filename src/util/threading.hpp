#pragma once

#include <string>
#include <vector>

namespace mfa
{
// Raises the process's permitted capabilities into its effective set. The image gives the
// binary SYS_NICE and IPC_LOCK as permitted file capabilities only; a non-root process gets
// no effective ones otherwise. Call before any thread starts (threads inherit the set).
// Returns the effective capabilities as text, for the startup log.
std::string raisePermittedCapabilities();

void applyThreadScheduling(int rtPriority, std::vector<int> const& cpus);
} // namespace mfa
