#include "Test.h"
#include "game/FrameLoan.h"

#include <limits>

using namespace std::chrono_literals;
using FrameLoan::Press;
using FrameLoan::Stage;
using FrameLoan::Step;

TEST("frame loan: a press on a friend's frame asks for it, or says why not, and gets our player in once it is lent and filled")
{
	using FrameLoan::ChoosePress;
	using FrameLoan::PressFacts;
	const PressFacts here{ .ownerHere = true, .ownerLends = true };
	CHECK(ChoosePress(here) == Press::kAsk);
	// Asked: told again, never asked twice.
	auto facts = here;
	facts.asked = true;
	CHECK(ChoosePress(facts) == Press::kWait);
	// Lent to us: in once the kit is in our copy, told to wait until then.
	facts = here;
	facts.ours = true;
	CHECK(ChoosePress(facts) == Press::kWait);
	facts.filled = true;
	CHECK(ChoosePress(facts) == Press::kEnter);
	// Lent to us even should their status say they stopped lending since, or not have come in yet.
	facts.ownerLends = false;
	facts.ownerHere = false;
	CHECK(ChoosePress(facts) == Press::kEnter);
	// Why not, in that order.
	CHECK(ChoosePress({ .ownerHere = false, .ownerLends = true }) == Press::kOwnerGone);
	CHECK(ChoosePress({ .ownerHere = true, .ownerLends = true, .lentToOther = true }) == Press::kLent);
	CHECK(ChoosePress({ .ownerHere = true, .ownerLends = false }) == Press::kNotLending);
	CHECK(ChoosePress({ .ownerHere = true, .ownerLends = true, .otherLoan = true }) == Press::kBusy);
}

TEST("frame loan: an ask times out, a frame never got into goes back, and the loan ends a moment after our player is out")
{
	using FrameLoan::NextStep;
	using FrameLoan::WearFacts;
	const WearFacts standing{ .copyThere = true, .in = false };
	const WearFacts in{ .copyThere = true, .in = true };
	CHECK(NextStep(Stage::kAsked, 1s, standing) == Step::kNone);
	CHECK(NextStep(Stage::kAsked, FrameLoan::ASK_TIMEOUT, standing) == Step::kGiveUp);
	// Lent: waits for our player, then gives it back.
	CHECK(NextStep(Stage::kLent, 5s, standing) == Step::kNone);
	CHECK(NextStep(Stage::kLent, 5s, in) == Step::kWorn);
	CHECK(NextStep(Stage::kLent, FrameLoan::ENTER_TIMEOUT, standing) == Step::kReturn);
	CHECK(NextStep(Stage::kLent, 1s, {}) == Step::kReturn);  // our copy went (a cell unloaded, say)
	// Worn for as long as they are in it; out, and back in within the settle, still the same wear.
	CHECK(NextStep(Stage::kWorn, 10min, in) == Step::kNone);
	CHECK(NextStep(Stage::kWorn, 10min, standing) == Step::kOut);
	CHECK(NextStep(Stage::kOut, 500ms, standing) == Step::kNone);
	CHECK(NextStep(Stage::kOut, 500ms, in) == Step::kWorn);
	CHECK(NextStep(Stage::kOut, FrameLoan::EXIT_SETTLE, standing) == Step::kReturn);
	// Our copy gone as they got out: still the settle first.
	CHECK(NextStep(Stage::kOut, 500ms, {}) == Step::kNone);
	CHECK(NextStep(Stage::kOut, FrameLoan::EXIT_SETTLE, {}) == Step::kReturn);
	// In power armor while our copy does not resolve (a cell changing under them): still in it, whatever the stage.
	const WearFacts unsure{ .copyThere = false, .in = false, .inArmor = true };
	CHECK(NextStep(Stage::kWorn, 10min, unsure) == Step::kNone);
	CHECK(NextStep(Stage::kOut, 1h, unsure) == Step::kNone);
	CHECK(NextStep(Stage::kLent, 1h, unsure) == Step::kNone);
	// In power armor, our copy there and they not in it (another frame): out, but given back only once out of that too.
	const WearFacts elsewhere{ .copyThere = true, .in = false, .inArmor = true };
	CHECK(NextStep(Stage::kWorn, 1s, elsewhere) == Step::kOut);
	CHECK(NextStep(Stage::kOut, 1h, elsewhere) == Step::kNone);
	CHECK(NextStep(Stage::kOut, FrameLoan::EXIT_SETTLE, standing) == Step::kReturn);
	CHECK(NextStep(Stage::kNone, 1h, standing) == Step::kNone);
}

TEST("frame loan: lent items are found again by their base and the name they were given")
{
	const FrameLoan::Tag arm{ .slot = 2, .base = 0x00140C5A, .name = "Sam's T-60 Left Arm" };
	CHECK(FrameLoan::IsTagged(arm, 0x00140C5A, "Sam's T-60 Left Arm"));
	CHECK(!FrameLoan::IsTagged(arm, 0x00140C5A, "T-60 Left Arm"));  // our own
	CHECK(!FrameLoan::IsTagged(arm, 0x00140C5B, "Sam's T-60 Left Arm"));
	const FrameLoan::Tag unnamed{ .slot = 3, .base = 0x00140C5A };
	CHECK(!FrameLoan::IsTagged(unnamed, 0x00140C5A, ""));
}

TEST("frame loan: the return says of each lent item whether it was found and at what condition, in slot order")
{
	using FrameLoan::ComposeReturn;
	using FrameLoan::Found;
	using FrameLoan::Tag;
	const std::vector<Tag> tags{ { .slot = 7, .base = 0x00075FE4, .name = "Sam's Fusion Core", .core = true }, { .slot = 0, .base = 0x00140C58, .name = "Sam's T-60 Helmet" },
		{ .slot = 3, .base = 0x00140C5A, .name = "Sam's T-60 Left Arm" } };
	const auto back = ComposeReturn(tags, { { .slot = 0, .health = 0.4f }, { .slot = 7, .health = 0.75f } });
	CHECK(back.size() == 3);
	CHECK((back[0] == Protocol::ReturnedItem{ .slot = 0, .found = true, .health = 0.4f }));
	CHECK((back[1] == Protocol::ReturnedItem{ .slot = 3, .found = false, .health = -1.0f }));
	CHECK((back[2] == Protocol::ReturnedItem{ .slot = 7, .found = true, .health = 0.75f }));
	// Found twice (on our player and in the copy): the lower condition. A condition out of range is none.
	const auto twice = ComposeReturn(tags, { { .slot = 3, .health = 0.9f }, { .slot = 3, .health = 0.2f }, { .slot = 0, .health = std::numeric_limits<float>::quiet_NaN() } });
	CHECK((twice[0] == Protocol::ReturnedItem{ .slot = 0, .found = true, .health = -1.0f }));
	CHECK((twice[1] == Protocol::ReturnedItem{ .slot = 3, .found = true, .health = 0.2f }));
	CHECK(FrameLoan::Health(1.5f) == 1.0f);
	CHECK(FrameLoan::Health(-0.5f) == -1.0f);
	// A slot named twice is answered once.
	const auto doubled = ComposeReturn({ { .slot = 1, .base = 1 }, { .slot = 1, .base = 2 } }, {});
	CHECK(doubled.size() == 1);
	// Our copy gone (unloaded with its cell, what it held with it): one not found went with it, and is left out.
	const auto gone = ComposeReturn(tags, { { .slot = 3, .health = 0.5f } }, true);
	REQUIRE(gone.size() == 1);
	CHECK((gone[0] == Protocol::ReturnedItem{ .slot = 3, .found = true, .health = 0.5f }));
}

TEST("frame loan: the owner keeps an item the return says nothing of, wears down one that came back, and takes out one that did not")
{
	using FrameLoan::AfterBack;
	using FrameLoan::AfterReturn;
	using FrameLoan::Back;
	using Protocol::ReturnedItem;
	CHECK((AfterReturn(0.8f, nullptr) == AfterBack{}));  // its copy never made (a mod the borrower lacks)
	const ReturnedItem worn{ .slot = 0, .found = true, .health = 0.5f };
	CHECK((AfterReturn(0.8f, &worn) == AfterBack{ .what = Back::kWear, .health = 0.5f }));
	const ReturnedItem better{ .slot = 0, .found = true, .health = 0.9f };
	CHECK((AfterReturn(0.8f, &better) == AfterBack{}));
	// Dropped, sold, stored or used up in the borrower's world: the owner keeping theirs would make it two.
	const ReturnedItem lost{ .slot = 0, .found = false, .health = -1.0f };
	CHECK(AfterReturn(0.8f, &lost).what == Back::kRemove);
	CHECK(AfterReturn(-1.0f, &lost).what == Back::kRemove);
}

TEST("frame loan: the owner lends while they lend, the frame is theirs here, nobody is in it and nobody has it")
{
	using FrameLoan::ChooseLend;
	using Protocol::LoanRefusal;
	CHECK(ChooseLend({ .lends = true, .here = true }) == LoanRefusal::kNone);
	CHECK(ChooseLend({ .lends = false, .here = true }) == LoanRefusal::kNotLending);
	CHECK(ChooseLend({ .lends = true, .here = false }) == LoanRefusal::kUnavailable);
	CHECK(ChooseLend({ .lends = true, .here = true, .inUse = true }) == LoanRefusal::kUnavailable);
	CHECK(ChooseLend({ .lends = true, .here = true, .lentOut = true }) == LoanRefusal::kLent);
}

TEST("frame loan: the kit is every piece and one fusion core, nothing else a frame holds")
{
	using FrameLoan::KitStack;
	using FrameLoan::PickKit;
	const KitStack piece{ .piece = true, .fits = true };
	const KitStack core{ .core = true, .fits = true };
	const KitStack other{ .fits = true };
	const KitStack strange{ .piece = true, .fits = false };  // a runtime mod on it: no other game names it
	const auto kit = PickKit({ piece, core, other, piece, core, strange, piece });
	CHECK((kit == std::vector<std::size_t>{ 0, 1, 3, 6 }));
	std::vector<KitStack> many(40, piece);
	CHECK(PickKit(many).size() == Protocol::MAX_LENT_ITEMS);
	CHECK(PickKit({}).empty());
}

TEST("frame loan: a return only ever lowers a condition")
{
	using FrameLoan::HealthAfter;
	using Protocol::ReturnedItem;
	CHECK(HealthAfter(0.8f, { .slot = 0, .found = true, .health = 0.5f }) == 0.5f);
	CHECK(!HealthAfter(0.5f, { .slot = 0, .found = true, .health = 0.8f }));  // repaired on the way: no
	CHECK(!HealthAfter(0.5f, { .slot = 0, .found = false, .health = -1.0f }));
	CHECK(!HealthAfter(0.5f, { .slot = 0, .found = true, .health = -1.0f }));
	CHECK(!HealthAfter(0.5f, { .slot = 0, .found = true, .health = 1.5f }));
	// One lent at its full condition, with none noted on it.
	CHECK(HealthAfter(-1.0f, { .slot = 0, .found = true, .health = 0.9f }) == 0.9f);
	CHECK(!HealthAfter(-1.0f, { .slot = 0, .found = true, .health = 1.0f }));
}

TEST("frame loan: the owner's frame goes where the borrower left it, or stays where it stood")
{
	using FrameLoan::ChoosePlace;
	using FrameLoan::Place;
	Protocol::FrameReturn back{ .borrowerId = 2, .ownerId = 1, .spawnId = 0xFF800001, .ended = false, .cell = 0, .worldspace = 0x0000003C, .x = 1, .y = 2, .z = 3 };
	CHECK(ChoosePlace(back) == Place::kMove);
	back.worldspace = 0;
	back.cell = 0x0001F278;
	CHECK(ChoosePlace(back) == Place::kMove);
	back.cell = 0;  // a place our game has no form for
	CHECK(ChoosePlace(back) == Place::kStay);
	back.cell = 0x0001F278;
	back.ended = true;
	CHECK(ChoosePlace(back) == Place::kStay);
}

TEST("frame loan: the LOAN record reads back as written, and nothing of a damaged one")
{
	FrameLoan::Record record{ .copy = 0xFF0012AB, .base = 0x0019DED9, .spawnId = 0xFF800005, .owner = 3 };
	record.tags.push_back({ .slot = 0, .base = 0x00140C58, .name = "Sam's T-60 Helmet" });
	record.tags.push_back({ .slot = 7, .base = 0x00075FE4, .name = "Sam's Fusion Core", .core = true });
	const auto bytes = FrameLoan::Encode(record);
	CHECK(FrameLoan::Decode(bytes) == record);
	for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
		CHECK(!FrameLoan::Decode({ bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut) }));
	}
	auto longer = bytes;
	longer.push_back(0);
	CHECK(!FrameLoan::Decode(longer));
	auto badCore = bytes;
	badCore[17 + 1 + 4] = 2;  // the first tag's core flag
	CHECK(!FrameLoan::Decode(badCore));
	CHECK(!FrameLoan::Decode(FrameLoan::Encode({})));  // no copy
	// A name longer than the record keeps is cut, not lost.
	FrameLoan::Record longName{ .copy = 1, .base = 2 };
	longName.tags.push_back({ .slot = 1, .base = 3, .name = std::string(300, 'x') });
	const auto cut = FrameLoan::Decode(FrameLoan::Encode(longName));
	CHECK(cut && cut->tags.size() == 1 && cut->tags[0].name.size() == FrameLoan::MAX_TAG_NAME);
}

TEST("frame loan: after loading a save made in a borrowed frame, our player is sent out and the copy and the lent items go")
{
	using FrameLoan::AfterLoad;
	using FrameLoan::ChooseAfterLoad;
	FrameLoan::Record record{ .copy = 0xFF0012AB, .base = 0x0019DED9, .spawnId = 0xFF800005, .owner = 3 };
	record.tags.push_back({ .slot = 0, .base = 0x00140C58, .name = "Sam's T-60 Helmet" });
	CHECK(ChooseAfterLoad(record, true, 0x0019DED9, true) == AfterLoad::kAll);
	// The id names something else now: only the named items go.
	CHECK(ChooseAfterLoad(record, true, 0x0019DEDA, true) == AfterLoad::kItems);
	CHECK(ChooseAfterLoad(record, true, 0x0019DED9, false) == AfterLoad::kItems);
	CHECK(ChooseAfterLoad(record, false, 0, false) == AfterLoad::kItems);
	record.tags.clear();
	CHECK(ChooseAfterLoad(record, false, 0, false) == AfterLoad::kNothing);
	CHECK(ChooseAfterLoad({}, true, 0x0019DED9, true) == AfterLoad::kNothing);
}
