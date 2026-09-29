# Final Fantasy XV VR

FINAL FANTASY XV WINDOWS EDITION is now playable in VR.

Cruise across Eos in the Regalia. Camp beneath the stars with the Chocobros. Hunt daemons after dark.
Stand before the Astrals. Explore Altissia and the ruins of Insomnia at full scale. Experience Noctis's
journey from inside its world.

The mod includes three modes: Stereo, AER and Mono. First-person gameplay is included, along with VR
cutscenes and the original third-person experience.

Works with Virtual Desktop, SteamVR and Meta Quest Link.

It's free to download, and any future updates will be free too.

**Questions and problem reports:** [the Patreon post](https://www.patreon.com/dhalcyon/posts/walk-tall-170916549)

## Installation

1. Download **FFXV-VR-v1.0.zip** from [Releases](../../releases) (not the green "Code" button, that is
   the source code).
2. Extract ALL the files into your game folder, the one with `ffxv_s.exe`
   (Steam: right-click FINAL FANTASY XV > Manage > Browse local files).
3. Double-click `FFXV-VR.bat` in that folder and pick an image quality. Balanced (2560x1440) is
   recommended.

If the installer says the game has no settings file yet, start the game once, reach the title screen,
quit, and run `FFXV-VR.bat` again.

To play: start your VR runtime (it must be the active OpenXR runtime), launch the game, load your save,
and once you are in the game press **F9**. Wait for the beep: head tracking takes a few seconds.

To change quality or uninstall, run `FFXV-VR.bat` again.

## Keys

All keys can be changed in the Insert menu.

| Key | Action |
|---|---|
| F9 | start VR and head tracking; press again if tracking is lost |
| Insert | VR menu |
| R | recenter |
| End | switch to Mono (safety key) |
| K | first person (experimental; set it up in the Insert menu first) |

## If the game seems stuck

If the game is still running but a conversation or scene seems frozen (a character not moving, dialogue
not moving on), press **End** to switch to Mono, or pick another mode in the Insert menu (VR tab). That
usually gets it moving again. Switch back to Stereo in the Insert menu once you are past it.

## Performance

FINAL FANTASY XV is already a demanding game in its original form, and VR increases the rendering cost
considerably.

If you need more performance, begin by lowering the resolution and disabling the NVIDIA GameWorks
settings. VXAO, HairWorks, TurfEffects, ShadowLibs and NVIDIA Flow can have a considerable performance
cost.

Use the refresh rate and resolution that provide the most stable experience on your system.

## Recommended settings

- **Anti-aliasing: TAA.** The mod replaces it with DLSS 4 (DLAA) in both eyes. Do not pick the game's
  own DLSS option.
- **Model LOD: default.** Raising it makes grass pop in and out as you turn your head.
- **Sharpening and Texture detail** are in the Insert menu (VR tab > Graphics). Higher looks crisper but
  makes grass shimmer more.

## Reporting a problem

Send `ffxv-vr.log` from the game folder (`ffxv-vr.prev.log` is the session before), say what you were
doing and which VR mode you were in, and include a save file so the problem can be reproduced. Post it in the comments on
[the Patreon post](https://www.patreon.com/dhalcyon/posts/walk-tall-170916549).

## Support

If you'd like to support the work: [patreon.com/dhalcyon](https://www.patreon.com/dhalcyon)

---

## Source code

A `dxgi.dll` proxy loaded next to `ffxv_s.exe`. It hooks Direct3D 11 and the game's own renderer and
submits to any OpenXR runtime.

- **Building:** see [BUILD.md](BUILD.md). Short version: Visual Studio 2022 with the C++ workload, the
  NVIDIA DLSS SDK in `third_party/DLSS`, then `scripts\build.bat`.
- **Engine reference:** [docs/FFXV_ENGINE_REFERENCE.md](docs/FFXV_ENGINE_REFERENCE.md) lists every game
  address, structure offset and engine mechanism the mod uses.

| Path | What's there |
|---|---|
| `src/` | Mod source (DXGI proxy, D3D11 hooks, engine hooks, OpenXR presenter, DLSS, ImGui menu) and its project file |
| `scripts/` | `build.bat` |
| `third_party/` | Vendored MinHook, Dear ImGui and OpenXR headers, used by the build |
| `docs/` | The engine reference |
| `release/` | The install/uninstall script and shipped package layout |
| `runtime/` | A starting `ffxv-vr.ini` |
| `builds/` | The prebuilt `dxgi.dll` shipped in the release |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon. The additional permission for linking the
NVIDIA DLSS SDK and the third-party notices are in [NOTICE.md](NOTICE.md).

Experimental community mod. Not affiliated with Square Enix or NVIDIA. Use at your own risk.
