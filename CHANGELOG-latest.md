## Breaker 1.1.0

A refinements and performance update, with a few new features.

### New
- **PC Checkup:** one page that lists every setting where Breaker's recommendation differs from yours, across all categories. Fix them one at a time or with **Apply all**. Home tells you how many are left.
- **Restore point before big changes:** Debloat's "Apply recommended", Checkup's "Apply all" and applying a snapshot now make a System Restore point first (one admin prompt; on by default, in Breaker settings → General). There's also **Create a restore point now** under Advanced → System tweaks.
- **Profiles by time of day:** a profile can switch on during set hours, like 22:00–07:00, alongside the game, battery and app triggers.
- **Search forgives typos:** "blutooth", "telemtry" and "wallpapr" still find the right settings.
- **Quick actions:** Restart File Explorer, Empty the Recycle Bin and Battery report, all searchable.
- **13 more settings:** File Explorer's side pane (Home, Gallery, OneDrive), virtual desktops on the taskbar and in Alt+Tab, skipping the lock screen, the sign-in blur, starting startup apps without the 10-second delay, plus new Debloat switches (Ask Copilot in the right-click menu, Click to Do, Copilot and shopping in Edge, online tips in Settings).
- **Hangar** shows up in My Apps.

### Faster
- Pages you've visited open instantly from memory and refresh behind the scenes; a revisit within 20 seconds doesn't re-read anything.
- Long lists (services, installed apps) no longer re-measure every row when the window redraws.
- The list of installed Store apps is read once and shared by Debloat, the watchdog and undo, instead of a separate PowerShell call each time.
- Switching back to Breaker refreshes values at most every few seconds.
- In the tray, Breaker now lets go of its pages and trims its memory.

### Fixed
- Profile triggers read naturally in messages ("between 09:00 and 17:30") instead of showing raw values.
- Text prompts take keyboard focus straight away when they open from a menu.
- PC Checkup groups follow the sidebar order.
