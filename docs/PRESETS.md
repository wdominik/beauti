# Look presets

The **Preset** menu offers Low, Medium, High, Very High, and Manual. New filters
start at Medium. The preset selector and the processing controls are always
visible; the effect sections appear only in Manual. The decision is
[decisions](DECISIONS.md#controls-and-presets).

## Catalog

Values are percentages and live in `src/core/presets.h`. The ID is what a scene
saves; it is independent of the language and the menu order.

| Preset | ID | Retouch | Skin Tone | Local Color | Micro Dodge & Burn | Smooth | Eyes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Low | 101 | 40 | 40 | 0 | 30 | 20 | 25 |
| Medium | 102 | 70 | 70 | 30 | 55 | 40 | 50 |
| High | 103 | 100 | 100 | 60 | 80 | 60 | 80 |
| Very High | 104 | 100 | 100 | 90 | 100 | 100 | 80 |

| Preset | Smooth reductions, finest to broadest | Structure size | Smooth edges | Color area size | Color edges |
| --- | --- | --- | --- | --- | --- |
| Low | 45 / 85 / 70 / 20 | 100 | 100 | 100 | 100 |
| Medium | 35 / 90 / 80 / 25 | 115 | 110 | 110 | 100 |
| High | 25 / 95 / 90 / 35 | 130 | 115 | 120 | 100 |
| Very High | 100 / 100 / 100 / 70 | 130 | 100 | 160 | 100 |

Low uses the original Smooth profile at reduced strengths. High is the reference
look. Very High removes fine skin texture deliberately: Smooth and its three
finer bands reach 100%, and Local Color and Micro Dodge & Burn increase, while
Eyes stays at 80%. Presets configure the existing stages only; no preset adds a
shader, model, or geometry change.

## Selecting, editing, and saving

- Selecting a preset writes all fourteen effect values as ordinary OBS settings.
  Device, quality, analysis resolution, and target frame rate are never touched.
  The change uses the usual 150 ms strength transitions and needs no new
  analysis.
- Manual (ID 0) only changes visibility. It starts from the values of the last
  selected preset, including the detail controls. Manual edits survive reopening,
  saving, loading, and duplicating. Selecting a preset replaces them.
- If a script or WebSocket request changes an effect value while a preset is
  active, the filter switches to Manual and keeps the new value.
- Settings without a preset, or with an ID that is not in the catalog, are reset
  to Medium; processing settings are kept.

`src/plugin/preset_settings.h` applies these rules in the property callback and
in the source's create and update callbacks.

## Saved settings before 1.0

Until version 1.0 there is no settings migration. When the catalog or the
effect settings change, saved effect values that no longer fit are discarded
and the filter starts at Medium again; device, quality, analysis resolution, and
frame rate are kept. The registered OBS defaults are the Medium values, so the
**Defaults** button in the filter dialog also restores Medium. From 1.0 on,
changes to saved settings come with an explicit migration.

## Validation

With `BEAUTI_FIXTURE_PRESETS=1` the OBS fixture runner drives the real libobs
callbacks: every preset and the Manual handoff after it, section visibility,
independence of the processing settings, save and reload, an external edit, an
unknown ID, and saved settings without a preset or with a retired ID.
`tests/obs/check_presets.py` checks the report. Slider mode 13 cycles through the
catalog while rendering. Static fixture comparisons check High against the
reference image and measure Very High's fine lightness contrast in skin regions.
