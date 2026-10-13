#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace mfa
{
// A file by device (major, minor) and inode: the same in every mount namespace.
using FileKey = std::tuple<unsigned, unsigned, std::uint64_t>;

struct ReaderProcess
{
    int pid = 0;
    std::string comm;
};

struct ReaderScan
{
    bool complete = true; // false: some process could not be checked (other PID namespace, no access)
    std::string error;
    std::map<std::string, std::vector<ReaderProcess>> readers; // per owner (mirror path) of a matched file
};

// The device and inode of one /proc/<pid>/maps line ("addr perms offset 00:2a 123 /path").
std::optional<FileKey> parseMapsLine(std::string const& line);
// True when /proc/self/status shows a nested PID namespace (NSpid with more than one id): processes
// outside it are not visible.
bool nestedPidNamespace(std::string const& statusText);
// Every regular file below a directory, by device and inode, mapped to owner.
void collectFiles(std::string const& dir, std::string const& owner, std::map<FileKey, std::string>& files);
// The processes other than selfPid that map one of the files.
ReaderScan scanReaders(std::map<FileKey, std::string> const& files, std::string const& procRoot, int selfPid);
} // namespace mfa
