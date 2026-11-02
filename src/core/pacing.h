// SPDX-License-Identifier: GPL-3.0-or-later
// Pure timing policy of the OBS filter: the analysis capture schedule and the retry
// delay after a failed analysis. Independent of OBS and of the inference provider.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
namespace beauti {
// Decides on which video ticks a new capture starts. The schedule advances by the
// requested interval instead of restarting at the last tick, so a target that is no
// divisor of the canvas rate keeps its average rate (25 FPS on a 30 FPS canvas skips
// every sixth tick, rather than falling to 15 FPS). Times are ns.
struct CapturePacer {
	// Ticks and intervals come from integer ns; this absorbs their rounding.
	static constexpr uint64_t tolerance = 1000;
	uint64_t last = 0, next = 0;
	// A shortened interval takes effect at once; a clock running backward restarts.
	bool due(uint64_t now, uint64_t interval) const
	{
		return !last || now < last || now + tolerance >= std::min(next, last + interval);
	}
	void taken(uint64_t now, uint64_t interval)
	{
		const uint64_t scheduled = last ? std::min(next, last + interval) : now;
		const uint64_t behind = now > scheduled ? now - scheduled : 0;
		// After a pause or on a canvas slower than the target there is nothing to catch up.
		next = now >= last && behind < interval ? scheduled + interval : now + interval;
		last = now;
	}
};
inline constexpr std::chrono::seconds kFirstRetryDelay{2}, kMaxRetryDelay{60};
inline constexpr std::chrono::seconds next_retry_delay(std::chrono::seconds delay)
{
	return std::min(delay * 2, kMaxRetryDelay);
}
}
