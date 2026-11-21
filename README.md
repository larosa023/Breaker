# Breaker

A fast, modern replacement for the Windows 11 Settings app and the old Control Panel.
Part of the larosa023 suite of lightweight Windows apps (Foyer, Strata, Faderly, Tonely, Ember, Mantel, Marq, Lumora…), and the control center for all of them.

**Status: v1.2.0.** Everything in the original plan, plus Screen time, PC Checkup, restore points, time-of-day profiles and a performance pass. See [CHANGELOG-latest.md](CHANGELOG-latest.md).

- **One place:** System, Devices, Network, Personalization, Apps, Gaming, Accounts, Privacy & Security, Windows Update, Debloat and Advanced. About 300 settings and Control Panel items, each with a one-sentence explanation.
- **Search:** Ctrl+K, or just start typing. Results update on every keystroke, understand synonyms ("dark", "theme" and "night" find the same switches) and forgive typos ("blutooth").
- **Undo:** every change is recorded. Ctrl+Z undoes the last one, the history page undoes any of them, and "restore everything to how it was on…" rolls back to a date.
- **Profiles:** saved groups of settings (Gaming, Work, Battery Saver, Presentation, or your own), applied in one click or switched automatically: while a game runs, on battery, while certain apps run, or during set hours.
- **Update watchdog:** notices when Windows changes a setting you chose and offers to put it back.
- **Snapshots:** every setting in one file, to apply after a fresh install or on another PC.
- **PC Checkup:** every recommendation you haven't applied yet, across all categories, with Apply all.
- **Restore points:** big batches of changes (Debloat, Checkup, snapshots) make a System Restore point first.
- **Screen time:** how long you spend in each app and game, today, this week and this month, with categories, daily limits, a weekly summary and CSV export. A tiny background tracker keeps counting when Breaker is closed.
- **Themes and My Apps:** the suite's shared theme format, plus settings, launch and updates for the other suite apps.

---

## Run it

1. Download `Breaker.exe` from [Releases](https://github.com/larosa023/Breaker/releases) and double-click it. It runs from anywhere.
   To install it properly, open **Breaker settings → About → Install**. That copies it to `%LOCALAPPDATA%\Programs\Breaker`,
   adds a Start menu entry and lists it under installed apps. No administrator rights needed.
2. Windows Settings is untouched. Breaker's title bar always has a **Windows Settings ↗** button that opens the matching page there.

Windows may show a SmartScreen warning the first time because the exe isn't code-signed yet: **More info → Run anyway**.

**Background use:** by default Breaker runs only while its window is open. Three options in **Breaker settings → General** keep it
in the tray, and only then: the tray icon itself (quick toggles and profiles), **Win+I opens Breaker** (Shift+Win+I still opens
Windows Settings), and automatic profile switching.

**Updates:** Breaker checks GitHub Releases on startup and every few hours (configurable, can be turned off). Downloads are verified
with SHA-256. If a new version doesn't start, Breaker rolls back by itself. It never updates while a game or full-screen app is
running or while a setting is being changed. Choose Stable or Beta in **Breaker settings → Updates**.

**Portable mode:** **Breaker settings → About → Portable mode**, or create an empty file named `portable` next to `Breaker.exe`.
Settings then go in a `data` folder beside it.

Command line: `Breaker.exe system/display` opens a page · `breaker://privacy/telemetry#privacy.adId` links to one setting ·
`--tray` starts in the tray · `--tracker` runs the screen time tracker · `--watchdog` checks your settings at sign-in and exits if nothing changed · `--quit` · `--uninstall`.

## Screen time

**Screen time** in the sidebar has three tabs, **Today**, **This week** and **This month**, each with totals (screen time, gaming,
most used app), a bar chart per day, time by category and a ranked list of apps. Click an app for its last 30 days, to change its
category, give it a daily limit, stop tracking it or delete its history.

**How it counts:** a small background process, `Breaker.exe --tracker`, notes which app is in front every 5 seconds. It's the same
exe started in a headless mode: one hidden window, no graphics, below-normal priority with Windows' power throttling on, and it
writes to disk every few minutes. It starts with Windows (the `BreakerTracker` entry in your Run key) while tracking is on, and
restarts itself on the new copy when Breaker is updated, by Breaker's own updater or by Hangar.

- Not counted: the lock screen, sleep, and idle time (no input for 2 minutes, adjustable), unless a game or video is keeping the screen on.
- **Games:** an app is tagged as a game when it lives in a Steam, Epic, GOG or Xbox games folder, was launched by Steam, Epic,
  GOG Galaxy or RetroArch, or runs full screen for a while. The name comes from Steam's `appmanifest` files, Epic's install
  manifests or GOG's registry entries, so you see "Hollow Knight", not `hollow_knight.exe`. Each RetroArch game is counted separately.
- **Categories:** Games, Work, Browsing, Media, Chat and Other, picked from a list of well-known apps; change any app from its page.
- **Limits:** per app or per category. You get a notification at 80% and at 100%. Breaker never closes or blocks anything.
- **Breaker settings → Screen time:** tracking on/off, the idle time, limit and weekly-summary notifications, category limits,
  the never-track list, export to CSV and clearing history (today, last 7 days, a date range or everything).

The data lives in `%APPDATA%\Breaker\screentime` (or `data\screentime` in portable mode): one small text file per month
(`2026-11.tsv`: day, app, seconds), plus `apps.tsv` (names and which apps are games) and `state.tsv` (notifications already shown).
Nothing is sent anywhere.

## Using Breaker

| | |
|---|---|
| **Search** | Ctrl+K or start typing. Change settings right in the results; click a result to jump to where it lives |
| **Move around** | Arrows and Enter · Tab switches between the sidebar and the page · Ctrl+Tab next tab · Esc or Backspace back · Alt+← / Alt+→ |
| **Change things** | Enter or Space flips a switch or opens a list · ← / → move a slider · Ctrl+Z undoes the last change |
| **Details** | The ⓘ button (or F1) shows what a setting changes under the hood, and offers Pin to Home, Add to profile, Open in Windows Settings and Copy link |
| **Lists** | Ctrl+F filters long pages (installed apps, services, startup apps, variables) · F5 refreshes |

### What Windows doesn't let other apps change

A few things are locked to the real Settings app: default apps, pairing Bluetooth devices, account and sign-in options, night light,
HDR, display scaling, accent color and Defender's real-time protection. Breaker shows these as links that open the exact Windows
page, and the watchdog can still tell you when your default apps change.

---

## Why this tech stack

Breaker is **C++17 + Win32 + Direct2D/DirectWrite**, the same stack as Foyer and Strata, cross-compiled with Zig.

- **Speed and memory:** one ~1.5 MB native exe with no runtime. Pages are custom-drawn and only the rows on screen are painted.
  When idle it uses no CPU: there are no polling loops, and animation timers stop when nothing is moving.
- **Windows access:** the registry, `SystemParametersInfo`, power, audio, display, service and network APIs are called directly.
  PowerShell is used only where Windows offers nothing else (Defender status, Windows Update history, Store apps), always off the UI thread.
- **Matches the suite:** the theme engine (`gfx.cpp`), JSON settings, logging, portable mode and the updater module are the same
  files Foyer and Strata use, so theme files and update behavior are identical.

| Option | Why not |
|---|---|
| C# + WinUI 3 / WPF | Easiest UI tooling, but needs the .NET / Windows App SDK runtime, starts slower and uses 80–150 MB of RAM, which is the thing Breaker is meant to beat. |
| Electron / Tauri | Easy to theme, but heavy (Electron) or still a web view (Tauri), and every system call would cross a bridge. |
| Rust + windows-rs | Comparable speed, but no code shared with Foyer and Strata. |

The cost of native C++ is that every control is hand-drawn, which is also what gives the suite its look.

## How Breaker reads and changes settings

Every setting is an **id with a string value** (`colors.darkApps` = `"1"`). Three rules keep it safe:

1. **Nothing slow runs on the window thread.** Pages paint immediately from a cache of last-known values, then background tasks
   re-read the real values and correct the page. Slow sources (PowerShell, Windows Update) get their own task. Lists found on the
   PC (services, apps, drives) are kept per page, so a revisit shows the last result instantly while a fresh copy is built.
2. **One function changes things.** `ApplyChanges()` in `engine.cpp` captures the exact previous state, writes the new value,
   reads it back and records a history entry. Profiles, snapshots, Debloat, the watchdog and undo all go through it.
3. **Admin rights only for the write that needs them.** Breaker runs as a normal user. If Windows refuses a write, only that
   change is handed to an elevated copy of Breaker (`Breaker.exe --elevated`), so a batch of fifty changes asks once, and a
   change that doesn't need admin never asks.

**Undo** stores the exact prior state, not just "the other value". For registry settings that includes "the value didn't exist",
so undoing puts Windows back precisely as it was. Risky settings (firewall, services, UAC) ask first, and display mode changes
revert by themselves after 15 seconds unless you click Keep.

## Project structure

```
src/
  main.cpp           entry point, single instance, command line, admin helper mode
  window.cpp/.h      the window: sidebar, search, header and tabs, rows, toasts, prompts, keyboard and mouse
  engine.cpp/.h      the settings engine: registry specs, handlers, elevation, undo history, value cache
  catalog.cpp        the settings catalog: one line per setting
  handlers.cpp       settings that need an API call (display, sound, power, mouse, firewall, services, DNS...)
  pages.cpp/.h       the page list and the builders that find things on this PC (drives, apps, services...)
  features.cpp/.h    Home, Debloat, Profiles, Undo history, Update watchdog, Snapshots, PC Checkup, search
  tally.cpp/.h       Screen time data: month files, dates, categories, talking to the tracker
  tracker.cpp        the Screen time tracker ("--tracker"): sampling, idle/lock/sleep, game names, limits, weekly summary
  tallyui.cpp        the Screen time pages, charts and settings tab
  appsettings.cpp    Breaker's own options and settings pages
  suite.cpp/.h       My Apps: the other suite apps' settings, updates and shared theme
  system.cpp/.h      install/uninstall, portable, start with Windows, tray, Win+I, breaker: links
  a11y.cpp/.h        screen reader support (MSAA)
  updater.cpp/.h     GitHub Releases updater (shared with Foyer and Strata)
  autoupdate.*       Breaker's updater glue (defers during games and setting changes)
  gfx.cpp/.h         Direct2D/DirectWrite helpers + themes (shared with Foyer and Strata)
  config.cpp/.h      Breaker's settings, profiles, theme files
  textinput.*, json.h, log.cpp, util.cpp   shared helpers
res/                 icon, manifest, version resource
.github/workflows    build + automatic GitHub Release when BREAKER_VERSION changes
```

### Adding a setting

Most settings are one line in `src/catalog.cpp`: name, description, how to read it, how to change it. Undo comes for free.

```cpp
{L"explorer.extensions", L"advanced/explorer", L"Files", L"Show file extensions",
 L"Show the .jpg, .exe and so on at the end of file names.", L"file extensions known types",
 CtlToggle, L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced!HideFileExt=d:0/1?1",
 nullptr, L"1", 0, L"control.exe folders"},
```

The spec reads as: this registry value, a DWORD, **on = 0, off = 1**, and a missing value means 1. `~` means "value removed",
`;;` joins several values written together, and choices or sliders store the value directly. Settings in `HKLM` are detected as
needing admin automatically. The info panel's "what it changes" text is generated from the same spec, so it can't drift.

For a setting that needs code, use `L"@"` as the spec and add a handler with the same id in `src/handlers.cpp`: a `read` function,
a `write` function that returns `W_DENIED` when it needs admin, and optionally a `choices` function. Everything else (search,
undo, profiles, snapshots, the watchdog, pinning) works without further changes.

## Themes

**Breaker settings → Appearance** has Dark (default), Light, Midnight (pure black), Glass, Ember and Ocean, plus Follow Windows.
You can set the accent (or follow Windows), the backdrop (solid, Mica, Acrylic) and its tint strength, the font and size,
density, corner radius and animation speed including none. **Layout** chooses which categories appear in the sidebar and their
order. Themes export to the same JSON file Foyer and Strata import, and **My Apps** can apply one theme to every installed suite app.

## Build

On Linux/macOS: `pip install ziglang && ./build.sh` → `dist/Breaker.exe`.
On Windows with Visual Studio 2022: `cmake -S . -B build -A x64 && cmake --build build --config Release`
(the Zig build is the one CI uses; the CMake file is provided for convenience).

Every push to `main` builds the exe in GitHub Actions. Bumping `BREAKER_VERSION` in `src/common.h` publishes a release
(versions with a `-`, like `1.1.0-beta.1`, are published as pre-releases for the Beta channel).

Files: settings in `%APPDATA%\Breaker\config.json`, undo history in `history.json`, the watchdog's list in `desired.json`,
screen time in `screentime\`,
and a local log in `breaker.log` (**Breaker settings → About → Open log**). Nothing leaves the PC except the update check to GitHub.
