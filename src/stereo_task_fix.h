#pragma once
#include <cstdint>

namespace StereoTaskFix
{
using LogFn = void (*)(const char*, ...);
// Installs once at engine initialization; detours remain for process lifetime.
bool Install(std::uintptr_t exeBase, bool (*stereoActive)(), LogFn log);
void Tick(std::uint64_t present);
}
