## Breaker 1.2.0

### New: Screen time
A new **Screen time** section in the sidebar shows where your time goes, app by app and game by game.

- **Today, This week and This month:** totals at the top (screen time, gaming time, most used app), a bar chart per day, time by category, and a ranked list of apps with a bar each. Click any app for its own page with the last 30 days.
- **Counts only real use:** it notes which app is in front every 5 seconds. Time doesn't count when there's no input for 2 minutes (unless a game or video is playing), or while the PC is locked or asleep.
- **Keeps counting when Breaker is closed:** a tiny background tracker (`Breaker.exe --tracker`) starts with Windows. It has no window, runs at low priority, wakes once every 5 seconds and saves every few minutes.
- **Knows your games:** anything from Steam, Epic, GOG, the Xbox app or RetroArch, or running full screen, is tagged as a game and shown under its real name (from Steam's and Epic's own records and GOG's registry entries), not the exe name.
- **Categories:** Games, Work, Browsing, Media, Chat and Other are picked automatically; change any app's category from its page.
- **Daily limits:** for an app or a whole category, with a gentle notification at 80% and 100%. Nothing is ever closed or blocked.
- **Weekly summary** notification (on or off), and **export to CSV**.
- **Privacy:** a never-track list, and "Clear history" for today, the last 7 days, a date range or everything. Everything stays in plain files in Breaker's data folder, and nothing leaves the PC.
- All of it is under **Breaker settings → Screen time**, including turning tracking off (which also stops the tracker and removes it from startup).

### Also
- A failed file dialog is now written to Breaker's log.
- Uninstalling Breaker also stops the tracker and removes it from startup.
