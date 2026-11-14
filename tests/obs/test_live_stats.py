# SPDX-License-Identifier: GPL-3.0-or-later
"""The log reader for BEAUTI_STATS=1."""

import live_stats

LOG = """
12:00:00.000: [beauti] 0.10.0 loaded
12:00:10.000: [beauti-stats] 0.10.0 id=0x1 seconds=10.0 ticks=300 losses=0 obs_frame_ms=20.5 lagged=0
12:00:10.000: [beauti-stats] 0.10.0 id=0x2 seconds=10.0 ticks=299 losses=1 loss_stall=1 obs_frame_ms=20.5 lagged=2
12:00:20.000: [beauti-stats] 0.10.0 id=0x1 seconds=20.0 ticks=600 losses=2 loss_stall=2 obs_frame_ms=21 lagged=0
"""


def summary(segment, quality, target_fps, eyes, values):
    """One summary line of filter 0x1 at 1080p on a 60 FPS canvas with Core AI."""
    configuration = 'source_w=1920 source_h=1080 analysis_w=1280 analysis_h=720 canvas_fps=60.00 backend=0'
    return (
        f'12:00:00.000: [beauti-stats] 0.0.0 id=0x1 segment={segment} {configuration} quality={quality} '
        f'target_fps={target_fps} retouch=1 eyes={eyes} {values} capture_fps=30.0\n'
    )


SEGMENTED = (
    summary(1, 0, 0, 1, 'seconds=10.0 accepted=300 startup_ms=120 age_p50=34 age_p95=50 lagged=0')
    + summary(1, 0, 0, 1, 'seconds=14.0 accepted=420 startup_ms=120 recovery_max=95 age_p50=34 age_p95=51 lagged=1')
    + summary(2, 1, 30, 0, 'seconds=2.0 accepted=60 age_p50=40 age_p95=60 lagged=0')
    + summary(3, 1, 30, 1, 'seconds=24.0 accepted=600 age_p50=41 age_p95=62 lagged=3')
)


def test_the_last_summary_of_each_filter_counts():
    filters = live_stats.parse(LOG)
    assert set(filters) == {'0x1', '0x2'}
    assert filters['0x1']['ticks'] == 600
    assert filters['0x1']['loss_stall'] == 2
    assert filters['0x2']['lagged'] == 2


def test_a_log_without_summaries_is_empty():
    assert live_stats.parse('12:00:00.000: [beauti] 0.10.0 loaded\n') == {}
    assert live_stats.segments('12:00:00.000: [beauti] 0.10.0 loaded\n') == []


def test_every_segment_keeps_its_last_summary():
    rows = live_stats.segments(SEGMENTED)
    assert [(row['id'], row['segment'], row['seconds']) for row in rows] == [
        ('0x1', 1, 14),
        ('0x1', 2, 2),
        ('0x1', 3, 24),
    ]
    assert rows[0]['lagged'] == 1 and rows[2]['quality'] == 1
    assert live_stats.parse(SEGMENTED)['0x1']['segment'] == 3


def test_the_table_names_the_configuration_and_leaves_out_short_segments():
    lines = live_stats.table(live_stats.segments(SEGMENTED)).splitlines()
    assert len(lines) == 4
    first = [cell.strip() for cell in lines[2].strip('|').split('|')]
    assert first[:9] == ['1', '1', '1920x1080', '1280x720', '60', 'Core AI', 'Performance', 'Automatic', '1/1']
    assert first[10] == '30.0' and first[13] == '34/51'
    assert first[-2:] == ['120', '95']
    assert 'High Quality | 30 | 1/1' in lines[3]
    assert len(live_stats.table(live_stats.segments(SEGMENTED), min_seconds=0).splitlines()) == 5


def test_a_log_from_before_segments_still_makes_a_table():
    lines = live_stats.table(live_stats.segments(LOG)).splitlines()
    assert len(lines) == 4
    assert '| 1 | 1 | - | - | - | - | - | - | - | 20 |' in lines[2]
