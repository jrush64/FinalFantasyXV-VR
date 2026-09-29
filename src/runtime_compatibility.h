#pragma once

#include <dxgiformat.h>
#include <cstring>

namespace RuntimeCompatibility
{
struct Flags
{
    bool steamVr{};
    bool meta{};
};

inline Flags Detect(const char* runtimeName)
{
    const char* name = runtimeName ? runtimeName : "";
    Flags flags{};
    flags.steamVr = std::strstr(name, "SteamVR") != nullptr;
    // SteamVR can advertise "Meta compatibility mode" while still using the
    // SteamVR compositor and device rules. SteamVR therefore wins this match.
    flags.meta = !flags.steamVr &&
        (std::strstr(name, "Oculus") != nullptr || std::strstr(name, "Meta") != nullptr);
    return flags;
}

inline bool RequiresShaderTransfer(DXGI_FORMAT source, DXGI_FORMAT destination)
{
    return source != destination;
}
}
