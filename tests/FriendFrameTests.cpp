// Which power armor frames our player (or a companion of theirs) is kept out of (FriendFrame): every copy the session
// placed of a friend's frame, never our own, a plugin's or a quest's.

#include "Test.h"
#include "game/FriendFrame.h"

namespace
{
	constexpr std::uint32_t FRIEND = 2;
	constexpr std::uint32_t OTHER_FRIEND = 3;

	// A copy the session placed: made at runtime, nobody's here.
	constexpr FriendFrame::Facts COPY{ .created = true };
}

TEST("friend frame: our copy of a friend's parked frame, or of the one their stand-in got out of, is theirs")
{
	using FriendFrame::KeptOutFor;
	auto parked = COPY;
	parked.parkedFor = FRIEND;
	CHECK(KeptOutFor(parked) == FRIEND);
	auto left = COPY;
	left.leftFor = OTHER_FRIEND;
	CHECK(KeptOutFor(left) == OTHER_FRIEND);
}

TEST("friend frame: every copy of a frame a friend's game shares is theirs, whoever used it last")
{
	using FriendFrame::KeptOutFor;
	auto shared = COPY;
	shared.sharedBy = FRIEND;  // the one they got out of last, a raider's they never used, or a 0.9.1 leftover their game shares
	CHECK(KeptOutFor(shared) == FRIEND);
}

TEST("friend frame: our player's own frame, an unowned frame, a plugin's frame and a quest's frame are never kept from them")
{
	using FriendFrame::KeptOutFor;
	CHECK(KeptOutFor(COPY) == 0);  // a runtime frame that is no copy: our own world made it
	CHECK(KeptOutFor({}) == 0);    // a plugin's frame (the museum's suit in Concord), whatever FrameShare did with it
	auto with = [](auto a_change) {
		auto facts = COPY;
		facts.parkedFor = FRIEND;
		facts.leftFor = FRIEND;
		facts.sharedBy = FRIEND;
		a_change(facts);
		return KeptOutFor(facts);
	};
	CHECK(with([](auto&) {}) == FRIEND);
	CHECK(with([](auto& f) { f.ours = true; }) == 0);      // our player is in it or got out of it last: a copy taken before stays theirs
	CHECK(with([](auto& f) { f.quest = true; }) == 0);     // a quest alias of ours holds it
	CHECK(with([](auto& f) { f.created = false; }) == 0);  // every game has its own reference of it
}

TEST("friend frame: a companion ordered into a friend's frame copy is sent out again; our player, a stand-in and a friend's NPC are not")
{
	using FriendFrame::Occupant;
	using FriendFrame::SendsOut;
	constexpr Occupant COMPANION{ .runsHere = true };
	CHECK(SendsOut(COMPANION, FRIEND));
	CHECK(!SendsOut(COMPANION, 0));                                          // a frame our player may use: theirs to be in
	CHECK(!SendsOut({ .standIn = true, .runsHere = true }, FRIEND));         // a friend's stand-in in its own frame copy
	CHECK(!SendsOut({ .player = true, .runsHere = true }, FRIEND));          // our player is kept out as they get in
	CHECK(!SendsOut({}, FRIEND));                                            // an NPC a friend's game runs: their game says
}

TEST("friend frame: a copy got out of is left as it is for 10 s, and while its 3D is not back for 30 s at most")
{
	using FriendFrame::Settling;
	using namespace std::chrono_literals;
	CHECK(Settling(false, 0s));
	CHECK(Settling(true, 2s));     // the rig's companion: its frame's 3D, seen as they got out, went a second or two later
	CHECK(Settling(true, 10s));
	CHECK(!Settling(true, 11s));   // back: Spawns and the parked copy see its 3D again
	CHECK(Settling(false, 20s));   // still not back
	CHECK(Settling(false, 30s));
	CHECK(!Settling(false, 31s));  // never back: placed again in the end
}

TEST("friend frame: a copy let go of while a companion is being sent out of it stays the friend's until they are out")
{
	using FriendFrame::KeptOutFor;
	auto waiting = COPY;
	waiting.waitingFor = FRIEND;  // the friend left, or their game shared the frame again, with the companion still in it
	CHECK(KeptOutFor(waiting) == FRIEND);  // our player is still kept out, and the companion still sent out
	waiting.ours = true;
	CHECK(KeptOutFor(waiting) == 0);
}

TEST("friend frame: nothing buries a copy with anyone in it; a save keeps one a companion is being sent out of, with them")
{
	using FriendFrame::ChooseLeave;
	using FriendFrame::Leave;
	CHECK(ChooseLeave({}) == Leave::kBury);  // nobody in it: it goes, and no save has it
	// A companion getting in, all the way in (the engine disabled the frame and cleared the seat), or asked out last frame:
	// it stays ours and as it is. Buried, a save had them wear a frame the save did not have.
	CHECK(ChooseLeave({ .sendingOut = true }) == Leave::kWait);
	CHECK(ChooseLeave({ .sendingOut = true, .inUse = true }) == Leave::kWait);  // never "theirs now": the friend's frame stays theirs
	CHECK(ChooseLeave({ .inUse = true }) == Leave::kRelease);                   // someone not ours to send out (a friend's NPC)
	CHECK(ChooseLeave({ .ours = true }) == Leave::kRelease);                    // our player's own suit, from before this build
	CHECK(ChooseLeave({ .ours = true, .sendingOut = true }) == Leave::kRelease);
}

TEST("friend frame: a copy kept through a save is waited on again after loading it only while its id still names that frame")
{
	using FriendFrame::SameCopy;
	CHECK(SameCopy(0x1A2B, 0x1A2B, true, false));
	CHECK(!SameCopy(0x1A2B, 0x3C4D, true, false));  // the id names another form now
	CHECK(!SameCopy(0x1A2B, 0, true, false));       // or none
	CHECK(!SameCopy(0x1A2B, 0x1A2B, false, false));  // not made at runtime: the game's own
	CHECK(!SameCopy(0x1A2B, 0x1A2B, true, true));    // gone
	CHECK(!SameCopy(0, 0, true, false));             // nothing was noted
	// Waited on again, our player is kept out of it and the companion in it sent out, as before the save.
	auto copy = COPY;
	copy.waitingFor = FRIEND;
	CHECK(FriendFrame::SendsOut({ .runsHere = true }, FriendFrame::KeptOutFor(copy)));
}

TEST("friend frame: the copies a save kept are written into its co-save and read back exactly, or not at all")
{
	using FriendFrame::Decode;
	using FriendFrame::Encode;
	using FriendFrame::KeptCopy;
	const std::vector<KeptCopy> kept{ { 0xFF0013BB, 0x1A2B, 2 }, { 0xFF001276, 0x3C4D, 3 } };
	CHECK(Decode(Encode(kept)) == kept);
	CHECK(Encode({}).empty());  // no record for a save that kept none: a load of it reads none back
	CHECK(Decode({}).empty());
	auto bytes = Encode(kept);
	bytes.pop_back();
	CHECK(Decode(bytes).empty());  // cut short: nothing is guessed
	CHECK(Decode(std::vector<std::uint8_t>(12 * (FriendFrame::MAX_KEPT_COPIES + 1))).empty());
	CHECK(Encode(std::vector<KeptCopy>(FriendFrame::MAX_KEPT_COPIES + 5)).size() == 12 * FriendFrame::MAX_KEPT_COPIES);
}

TEST("friend frame: the gate's word on the engine's seating thread keeps our player out of our frame lent out, and a friend's, and lets them borrow a shared one")
{
	using FriendFrame::ChooseEnter;
	using FriendFrame::Enter;
	using FriendFrame::Find;
	using FriendFrame::Kept;
	using FriendFrame::Looked;
	constexpr std::uint32_t OUR_FRAME = 0x0001A2B3;
	constexpr std::uint32_t PARKED = 0xFF000101;
	constexpr std::uint32_t SHARED = 0xFF000102;
	constexpr std::uint32_t SPAWN = 0x00C00011;
	const std::vector<Kept> kept{
		{ .frame = OUR_FRAME, .lentTo = FRIEND },
		{ .frame = PARKED, .owner = FRIEND, .kind = "parked" },
		{ .frame = SHARED, .owner = OTHER_FRIEND, .kind = "their game's", .spawnId = SPAWN },
	};
	CHECK(ChooseEnter(Find(kept, OUR_FRAME)) == Enter::kLent);
	CHECK(Find(kept, OUR_FRAME)->lentTo == FRIEND);
	CHECK(ChooseEnter(Find(kept, PARKED)) == Enter::kKeptOut);
	CHECK(Find(kept, PARKED)->owner == FRIEND);
	CHECK(ChooseEnter(Find(kept, SHARED)) == Enter::kBorrow);
	CHECK(Find(kept, SHARED)->spawnId == SPAWN);
	// Any other frame, or none, lets them in, and so does a word from before anything was put down.
	CHECK(ChooseEnter(Find(kept, 0xFF000999)) == Enter::kIn);
	CHECK(Find(kept, 0) == nullptr);
	CHECK(ChooseEnter(Find({}, PARKED)) == Enter::kIn);
	// The look says a friend's frame only, as the main thread's own answer: not ours lent out.
	CHECK(!Looked(Find(kept, OUR_FRAME)));
	CHECK(Looked(Find(kept, PARKED)));
	CHECK(Looked(Find(kept, SHARED)));
	CHECK(!Looked(nullptr));
}

TEST("friend frame: our frame lent out is asked about first, as the main thread asks it, whatever else the gate's word has of it")
{
	using FriendFrame::ChooseEnter;
	using FriendFrame::Enter;
	using FriendFrame::Find;
	using FriendFrame::Kept;
	constexpr std::uint32_t FRAME = 0xFF000201;
	const std::vector<Kept> kept{ { .frame = FRAME, .lentTo = FRIEND }, { .frame = FRAME, .owner = OTHER_FRIEND, .spawnId = 7 } };
	CHECK(ChooseEnter(Find(kept, FRAME)) == Enter::kLent);
	CHECK(ChooseEnter(&kept[1]) == Enter::kBorrow);
	constexpr Kept nobodys{ .frame = FRAME };
	CHECK(ChooseEnter(&nobodys) == Enter::kIn);
}
