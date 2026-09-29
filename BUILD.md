# Building FFXV VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` locates it
  automatically via `vswhere.exe`. The project does not pin a Windows SDK version; whichever
  is installed is used.
- The **NVIDIA DLSS SDK**, which is not included in this repository (it is published by NVIDIA under
  its own license). Clone or download it from <https://github.com/NVIDIA/DLSS> (tested with tag
  `v310.9.1`) so that these paths exist:
  - `third_party\DLSS\include\nvsdk_ngx.h`
  - `third_party\DLSS\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib`

  For example: `git clone --branch v310.9.1 https://github.com/NVIDIA/DLSS third_party\DLSS`
- Nothing else. MinHook, Dear ImGui and the OpenXR headers are vendored in `third_party/`.

## Build

```
scripts\build.bat
```

Output: `builds\dxgi.dll`.

The build runs MSBuild on `src\FFXVVR.vcxproj` (Release|x64) with the mod's own sources plus the
vendored MinHook and Dear ImGui sources, linked against the static MSVC runtime, so no
redistributable is needed at run time. You can also open the project in Visual Studio directly.

## Deploying

Copy these files into the game folder, next to `ffxv_s.exe`
(`<Steam library>\steamapps\common\FINAL FANTASY XV`):

- `builds\dxgi.dll` (just built)
- `openxr_loader.dll` - an x64 build of the official
  [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo
  (a copy is in `release/`)
- `ffxv-vr.ini` (see `runtime/ffxv-vr.ini` for a starting point; the mod also creates it)

For DLSS 4, the game folder needs NVIDIA's `nvngx_dlss.dll` (310.x). The release installer
(`release\FFXV-VR.bat`) installs it from a file named `ffxv_vr_dlss.dll` placed beside it, and keeps
the game's original DLSS file for uninstall. See `release/README.txt` for the end-user steps.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| Dear ImGui | `third_party/imgui` | MIT (see its `LICENSE.txt`) |
| OpenXR headers | `third_party/openxr` | Apache 2.0 OR MIT |
| OpenXR loader | `release/openxr_loader.dll`, obtained separately | Apache 2.0 |
| NVIDIA DLSS SDK | `third_party/DLSS`, downloaded separately | NVIDIA RTX SDKs License |
