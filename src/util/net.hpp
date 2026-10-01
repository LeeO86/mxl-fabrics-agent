#pragma once

#include <set>
#include <string>

namespace mfa
{
std::string localHostname();
std::set<std::string> localAddresses();
int taiOffsetSeconds();
bool pathIsTmpfs(std::string const& path);
} // namespace mfa
