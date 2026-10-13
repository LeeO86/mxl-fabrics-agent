#include "mirror/readers.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace mfa
{
namespace fs = std::filesystem;

std::optional<FileKey> parseMapsLine(std::string const& line)
{
    std::istringstream in(line);
    std::string addr;
    std::string perms;
    std::string offset;
    std::string dev;
    std::uint64_t inode = 0;
    if (!(in >> addr >> perms >> offset >> dev >> inode) || inode == 0)
    {
        return std::nullopt;
    }
    auto const colon = dev.find(':');
    if (colon == std::string::npos)
    {
        return std::nullopt;
    }
    char* end = nullptr;
    auto const major = std::strtoul(dev.substr(0, colon).c_str(), &end, 16);
    if (end == nullptr || *end != '\0')
    {
        return std::nullopt;
    }
    auto const minor = std::strtoul(dev.substr(colon + 1).c_str(), &end, 16);
    if (end == nullptr || *end != '\0')
    {
        return std::nullopt;
    }
    return FileKey{static_cast<unsigned>(major), static_cast<unsigned>(minor), inode};
}

bool nestedPidNamespace(std::string const& statusText)
{
    std::istringstream in(statusText);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.rfind("NSpid:", 0) == 0)
        {
            std::istringstream ids(line.substr(6));
            int count = 0;
            std::string id;
            while (ids >> id)
            {
                ++count;
            }
            return count > 1;
        }
    }
    return false;
}

void collectFiles(std::string const& dir, std::string const& owner, std::map<FileKey, std::string>& files)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        struct stat st{};
        if (::lstat(it->path().c_str(), &st) == 0 && S_ISREG(st.st_mode))
        {
            files[FileKey{major(st.st_dev), minor(st.st_dev), static_cast<std::uint64_t>(st.st_ino)}] = owner;
        }
    }
}

ReaderScan scanReaders(std::map<FileKey, std::string> const& files, std::string const& procRoot, int selfPid)
{
    ReaderScan scan;
    {
        std::ifstream status(fs::path(procRoot) / "self" / "status");
        std::stringstream text;
        text << status.rdbuf();
        if (nestedPidNamespace(text.str()))
        {
            scan.complete = false;
            scan.error = "the agent runs in its own PID namespace (hostPID is not set): other processes are not visible";
        }
    }
    if (files.empty())
    {
        return scan;
    }
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(procRoot, ec))
    {
        auto const name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
        {
            continue;
        }
        int const pid = std::atoi(name.c_str());
        if (pid == selfPid)
        {
            continue;
        }
        std::ifstream maps(entry.path() / "maps");
        if (!maps)
        {
            if (errno == EACCES || errno == EPERM)
            {
                scan.complete = false;
                if (scan.error.empty())
                {
                    scan.error = "maps of pid " + name + " not readable";
                }
            }
            continue; // gone meanwhile
        }
        std::set<std::string> owners;
        std::string line;
        while (std::getline(maps, line))
        {
            if (auto const key = parseMapsLine(line))
            {
                if (auto const it = files.find(*key); it != files.end())
                {
                    owners.insert(it->second);
                }
            }
        }
        if (owners.empty())
        {
            continue;
        }
        std::string comm;
        std::ifstream commFile(entry.path() / "comm");
        std::getline(commFile, comm);
        for (auto const& owner : owners)
        {
            scan.readers[owner].push_back(ReaderProcess{pid, comm});
        }
    }
    return scan;
}

std::string removalBlocker(ReaderScan const& scan, std::string const& owner, std::vector<ReaderProcess>* readers)
{
    if (auto const it = scan.readers.find(owner); it != scan.readers.end() && !it->second.empty())
    {
        *readers = it->second;
        std::string pids;
        for (auto const& reader : it->second)
        {
            pids += (pids.empty() ? "" : ",") + std::to_string(reader.pid) + "(" + reader.comm + ")";
        }
        return "mapped by " + pids;
    }
    if (!scan.complete)
    {
        return "reader check incomplete: " + scan.error;
    }
    return {};
}
} // namespace mfa
