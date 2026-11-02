# SPDX-License-Identifier: GPL-3.0-or-later
"""The log reader for BEAUTI_STATS=1."""

import live_stats

LOG = """
12:00:00.000: [beauti] 0.10.0 loaded
12:00:10.000: [beauti-stats] 0.10.0 id=0x1 seconds=10.0 ticks=300 losses=0 obs_frame_ms=20.5 lagged=0
12:00:10.000: [beauti-stats] 0.10.0 id=0x2 seconds=10.0 ticks=299 losses=1 loss_stall=1 obs_frame_ms=20.5 lagged=2
12:00:20.000: [beauti-stats] 0.10.0 id=0x1 seconds=20.0 ticks=600 losses=2 loss_stall=2 obs_frame_ms=21 lagged=0
"""


def test_the_last_summary_of_each_filter_counts():
    filters = live_stats.parse(LOG)
    assert set(filters) == {'0x1', '0x2'}
    assert filters['0x1']['ticks'] == 600
    assert filters['0x1']['loss_stall'] == 2
    assert filters['0x2']['lagged'] == 2


def test_a_log_without_summaries_is_empty():
    assert live_stats.parse('12:00:00.000: [beauti] 0.10.0 loaded\n') == {}
