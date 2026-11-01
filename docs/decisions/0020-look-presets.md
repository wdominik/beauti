# 0020: Look presets with a Manual mode that starts from the last preset

Status: Accepted for development. Date: 2026-10-27.

## Context

With 14 effect parameters (0019), choosing a look by sliders is impractical.
Most users need a few distinct strengths; some need full control. Hardware
settings are unrelated to the look. No version has been published yet, so saved
settings carry no compatibility obligation.

## Decision

- One `preset` setting offers Low (101), Medium (102), High (103), Very High
  (104), and Manual (0). IDs are saved identities, never list indices.
- High is the approved reference look. Very High removes fine texture more
  strongly with the same eye strength. Low is restrained; Medium lies between.
  Presets only configure the existing stages.
- Selecting a preset writes all its effect values as ordinary settings. Manual
  shows the effect sections and starts from exactly those values; there is no
  separate Manual snapshot. Processing settings stay visible and are never part
  of a preset.
- New filters start at Medium, and the registered defaults are the Medium
  values. This supersedes the effect defaults of
  [0008](0008-control-defaults.md); its processing defaults remain.
- An effect value changed from outside (scripting, WebSocket) while a preset is
  active switches the filter to Manual instead of being overwritten.
- No settings migration before 1.0: settings without a preset or with an ID
  outside the catalog reset to Medium; processing settings are kept. From 1.0
  on, a change to saved settings ships with an explicit migration.

## Consequences

Developers and testers may have to choose their look again after an update
during 0.x; the code carries no version-specific mapping. From 1.0 the catalog
and its IDs (`src/core/presets.h`) become a saved-settings contract. Every option is localized in every catalog. Selecting
a preset after Manual edits replaces those edits.

## Evidence

[Look presets](../PRESETS.md); the real libobs callback check
`tests/obs/check_presets.py`; static fixture comparisons of each level.
