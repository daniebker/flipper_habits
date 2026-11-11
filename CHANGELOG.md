# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-11-11

### Added
- Track habits straight from the habit list: OK logs one completion for today
  and holding OK removes one. The LED blinks green on each log and the Flipper
  vibrates when a habit is completed for the day.
- Right arrow on a habit opens its year tracker page.
- NFC tags: "Write NFC Tag" (under Edit Habits) writes a habit to an NTAG21x,
  MIFARE Ultralight or MIFARE Classic tag, and scanning that tag while the
  habit list is showing logs one completion. Vibration and LED colour tell you
  whether it was logged, already complete, or not recognised. The write screen
  reports "Tag Written", "Write Failed" or "Unsupported Tag".
- Each habit now has a stable ID so a tag keeps working after the habit is
  renamed. Existing habits get an ID the first time this version runs; the
  save file format is unchanged.

### Changed
- The habit list is now a custom view with a checkbox per habit (empty,
  half-filled or filled) in place of `[ ]`/`[x]` text.
- Progress is saved to the SD card immediately after every change (logging or
  removing a completion, changing an interval) instead of only when leaving a
  screen, so it survives crashes and reboots.
- Saves are written to a temporary file and then renamed over `habits.dat`, so
  an interrupted save can no longer corrupt existing data. If the main file is
  missing, the app recovers from the temporary file on start.

## [1.0.0] - 2026-11-11

### Added
- Habit list showing each habit's progress for today (e.g. `[x] Read 1/1`).
- Add habits with a name and an interval: daily, or 2–12 times a day.
- Edit habits: change the interval, rename, or delete with confirmation.
- GitHub-style year grid per habit covering the last 53 weeks, split into
  two stacked rows to fit the 128×64 screen.
  - Dot = not done, hollow square = partly done, filled square = done.
  - Header shows the selected day's count and days completed in the past 365 days.
- Tracking controls: OK logs a completion, hold OK removes one, arrows move
  between days (Up/Down) and weeks (Left/Right) to fill in past days.
- Up to 16 habits, each keeping a year of history, saved to
  `/ext/apps_data/flipper_habit/habits.dat`.
- README with usage, build instructions and an AI-generated code disclaimer.
- MIT License.
