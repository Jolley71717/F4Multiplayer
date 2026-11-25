// Friends' stages whose script acts on a loaded reference (src/game/StageNeedsRef.h): Vault 81's intercom talk ends in
// V81_00_Intro 55, which opens the gear door, and a game caught up to it with the cave not loaded was left with the stage
// done and the door shut for good. The stage waits until the door is loaded for a moment.
#include "Test.h"

#include "game/StageNeedsRef.h"

namespace
{
	using namespace std::chrono_literals;
	using StageNeedsRef::Settling;
	using Clock = StageNeedsRef::Clock;

	const Clock::time_point T0 = Clock::time_point{} + 1h;
	constexpr std::uint64_t KEY = 0x000B8464'0037ull;
}

TEST("stage needs a loaded ref: V81_00_Intro's 55 is the row for Vault 81's gear door, and no other stage is")
{
	const auto row = StageNeedsRef::Find(StageRules::File::kFallout4, 0x000B8464, 55);
	REQUIRE(row != nullptr);
	CHECK(row->ref == 0x000A9B71);
	CHECK(StageNeedsRef::Find(StageRules::File::kFallout4, 0x000B8464, 51) == nullptr);  // the talk's other stages need nothing
	CHECK(StageNeedsRef::Find(StageRules::File::kFallout4, 0x000B8464, 400) == nullptr);
	CHECK(StageNeedsRef::Find(StageRules::File::kCoast, 0x000B8464, 55) == nullptr);  // another plugin's quest of that number
	CHECK(StageNeedsRef::Find(StageRules::File::kFallout4, 0x000B8465, 55) == nullptr);
}

TEST("stage needs a loaded ref: a door not loaded holds the stage, one loaded for the settle time lets it go")
{
	Settling settling;
	CHECK(!settling.Ready(KEY, false, T0));  // the catch-up came with the cave unloaded
	CHECK(!settling.Ready(KEY, false, T0 + 30s));
	CHECK(!settling.Ready(KEY, true, T0 + 31s));  // the player comes in: the door loads, the settle time starts
	CHECK(!settling.Ready(KEY, true, T0 + 31s + StageNeedsRef::SETTLE - 1ms));
	CHECK(settling.Ready(KEY, true, T0 + 31s + StageNeedsRef::SETTLE));
}

TEST("stage needs a loaded ref: a door that unloads again starts its settle time over")
{
	Settling settling;
	CHECK(!settling.Ready(KEY, true, T0));
	CHECK(!settling.Ready(KEY, true, T0 + 1s));
	CHECK(!settling.Ready(KEY, false, T0 + 1500ms));  // the player stepped out
	CHECK(!settling.Ready(KEY, true, T0 + 5s));
	CHECK(!settling.Ready(KEY, true, T0 + 5s + StageNeedsRef::SETTLE - 1ms));
	CHECK(settling.Ready(KEY, true, T0 + 5s + StageNeedsRef::SETTLE));
}

TEST("stage needs a loaded ref: a stage set is forgotten, and each stage settles on its own")
{
	Settling settling;
	CHECK(!settling.Ready(KEY, true, T0));
	CHECK(!settling.Ready(KEY + 1, true, T0 + 1s));
	CHECK(settling.Size() == 2);
	CHECK(settling.Ready(KEY, true, T0 + StageNeedsRef::SETTLE));
	CHECK(!settling.Ready(KEY + 1, true, T0 + StageNeedsRef::SETTLE));  // the other one's time started later
	settling.Done(KEY);
	CHECK(settling.Size() == 1);
	settling.Clear();  // a load or the end of the session
	CHECK(settling.Size() == 0);
}
