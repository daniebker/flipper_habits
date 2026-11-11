# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
