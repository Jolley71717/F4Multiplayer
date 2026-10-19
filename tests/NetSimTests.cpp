#include "Test.h"

#include "game/NetSim.h"

#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using NetSim::Clock;
	const auto start = Clock::time_point{} + 1h;
}

TEST("netsim: off, nothing is held; on, every state comes the lag later, in order, and a held-up one holds up those behind it")
{
	NetSim::Line<int> line{ 7 };
	CHECK(!line.settings.Active());
	line.settings.lagMs = 40;
	line.settings.jitterMs = 20;
	line.settings.spikeMs = 100;
	line.settings.spikePercent = 10.0f;
	REQUIRE(line.settings.Active());

	std::vector<std::pair<Clock::time_point, int>> out;
	for (int i = 0; i < 300; ++i) {
		const auto now = start + i * 33ms;
		CHECK(line.Push(i, now));
		line.Release(now, [&](int a_item) { out.emplace_back(now, a_item); });
	}
	line.Release(start + 1h, [&](int a_item) { out.emplace_back(start + 1h, a_item); });
	REQUIRE(out.size() == 300);
	for (std::size_t i = 0; i < out.size(); ++i) {
		CHECK(out[i].second == static_cast<int>(i));  // in order
		CHECK(out[i].first - (start + static_cast<int>(i) * 33ms) >= 40ms);
	}
	CHECK(line.spikes > 10);
	CHECK(line.spikes < 60);
}

TEST("netsim: some are lost, and turned off what waits comes at once")
{
	NetSim::Line<int> line{ 3 };
	line.settings.lagMs = 1000;
	line.settings.lossPercent = 20.0f;
	int kept = 0;
	for (int i = 0; i < 1000; ++i) {
		kept += line.Push(i, start);
	}
	CHECK(kept > 700);
	CHECK(kept < 900);
	CHECK(line.lost == static_cast<std::uint64_t>(1000 - kept));
	int released = 0;
	line.Release(start + 999ms, [&](int) { ++released; });
	CHECK(released == 0);
	line.Flush([&](int) { ++released; });
	CHECK(released == kept);
	CHECK(line.Waiting() == 0);
}
