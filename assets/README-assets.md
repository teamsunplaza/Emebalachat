# Assets inventory — what the app actually references

This app (**Emebala Chat**) loads image assets at runtime by filename search
(see [`FindLogoPath()`](../src/ui/asset_loader.cpp) /
`FindAppIconPath()` — probe order: exe directory, then up to two levels up,
under `assets/`). There is no build-time embedding of this folder.

## Referenced by Emebala Chat

| File | Referenced from |
|------|-----------------|
| `Emebala_Chat_Logo_small.png` | `asset_loader.cpp` (logo + icon fallback), About window, tooltip, badge, drag icon |
| `Emebala_Chat_Appicon.png` / `Emebala_Chat_Appicon_small.png` | `asset_loader.cpp` (tray / badge / drag icon / About) |
| `Emebala_Chat_Appicon.ico` | `src/app_icon.rc` (exe icon resource) |
| `Emebala_Chat_poster*.png`, `Emebala_Brand_Logo*.png` | Not loaded at runtime; kept for marketing/branding use |
| `logo.png` / `logo.svg` | `logo.png` is a runtime fallback in `asset_loader.cpp`; `logo.svg` is the source artwork |

## NOT referenced anywhere — separate product assets

The six `Emebala_Reader_*` files below are **not referenced by any source,
resource, build, or installer file** in this repository (verified 2026-09-06,
Phase 2 audit — full-tree search of `src/`, `CMakeLists.txt`,
`installer/setup.iss`, `src/app_icon.rc` returned zero hits):

- `Emebala_Reader_Appicon.png`
- `Emebala_Reader_Appicon_small.png`
- `Emebala_Reader_Logo.png`
- `Emebala_Reader_Logo_small.png`
- `Emebala_Reader_Poster.png`
- `Emebala_Reader_Poster_small.png`

They most likely belong to a **separate product (Emebala Reader)**. They are
deliberately **kept** in this folder (do not delete — asset-loss risk for the
other product) and have **zero distribution impact**: the installer
(`installer/setup.iss` `[Files]`) installs only the exe + LICENSE, and
`CMakeLists.txt` never copies `assets/` into the build output, so these files
are never part of any shipped artifact.
