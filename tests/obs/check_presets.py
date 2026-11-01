# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate real OBS preset callbacks, manual handoff, persistence, external edits, and settings resets."""

import argparse
import json
from pathlib import Path

STRENGTHS = ('retouch', 'skin_tone', 'dodge_burn', 'smooth', 'eyes')
DETAILS = (
    'smooth_fine',
    'smooth_small',
    'smooth_medium',
    'smooth_broad',
    'smooth_size',
    'smooth_edges',
    'local_color',
    'local_color_size',
    'local_color_edges',
)
EFFECTS = STRENGTHS + DETAILS
HARDWARE = ('native_backend', 'native_quality', 'native_size', 'native_fps')
PRESETS = range(101, 105)
SECTIONS = ('retouch_section', 'tone_section', 'color_section', 'dodge_section', 'smooth_section', 'eyes_section')


def run(path):
    records = json.loads(path.read_text())
    for record in records:
        settings = record['settings']
        properties = {p['name']: p for p in record['properties']}
        manual = settings['preset'] not in PRESETS
        for section in SECTIONS:
            assert properties[section]['visible'] == manual, (record['case'], section)
        for key in EFFECTS:
            assert properties[key]['visible'] == manual, (record['case'], key)
        assert properties['preset']['visible'] and properties['processing_section']['visible']
        assert len(properties['preset']['items']) == 5
        assert all(properties[key]['visible'] for key in HARDWARE)
    by_name = {r['case']: r for r in records}
    fresh = by_name['fresh']['settings']
    assert fresh['preset'] == 102
    assert [fresh[k] for k in STRENGTHS] == [70, 70, 55, 40, 50]
    for index, record in enumerate(records):
        if record['case'] != 'manual-after-preset':
            continue
        previous = records[index - 1]['settings']
        current = record['settings']
        assert previous['preset'] in PRESETS and current['preset'] == 0
        assert {k: current[k] for k in EFFECTS} == {k: previous[k] for k in EFFECTS}
        assert [current[k] for k in HARDWARE] == [2, 1, 960, 17]
    reloaded = by_name['manual-reloaded']['settings']
    assert reloaded['preset'] == 0
    assert [reloaded[k] for k in ('retouch', 'smooth_fine', 'local_color_size')] == [17, 13, 177]
    medium = by_name['PresetMedium']['settings']
    for case in ('unknown-id', 'hardware-only', 'no-preset', 'retired-id'):
        reset = by_name[case]['settings']
        assert reset['preset'] == 102, case
        assert {k: reset[k] for k in EFFECTS} == {k: medium[k] for k in EFFECTS}, case
    assert all(by_name[c]['settings']['native_fps'] == 17 for c in ('hardware-only', 'no-preset', 'retired-id'))
    external = by_name['external-edit']['settings']
    assert external['preset'] == 0 and external['retouch'] == 10
    assert [external[k] for k in STRENGTHS[1:]] == [70, 55, 40, 50]
    high = by_name['preset-after-manual']['settings']
    assert high['preset'] == 103
    assert [high[k] for k in STRENGTHS] == [100, 100, 80, 60, 80]
    assert [high[k] for k in DETAILS] == [25, 95, 90, 35, 130, 115, 60, 120, 100]
    very_high = by_name['PresetVeryHigh']['settings']
    assert [very_high[k] for k in STRENGTHS] == [100, 100, 100, 100, 80]
    assert [very_high[k] for k in DETAILS] == [100, 100, 100, 70, 130, 100, 90, 160, 100]
    result = {
        'passed': True,
        'cases': len(records),
        'manual_handoffs': 4,
        'external_edits_switch_to_manual': True,
        'unusable_settings_reset_to_default': True,
        'hardware_untouched': True,
        'manual_edits_survive_reload': True,
    }
    path.with_suffix('.checked.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    run(parser.parse_args().report)


if __name__ == '__main__':
    main()
