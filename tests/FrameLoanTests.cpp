#include "Test.h"
#include "game/FrameLoan.h"
#include "game/FriendFrame.h"

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

TEST("frame loan: the owner wears down an item that came back lower, and keeps every other as it is, one not found too")
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
	// Not found by the borrower's game: only a find that failed (on the rig all 6 of a worn kit, every return), never word
	// that the borrower kept it. The owner's stays: taken out on it, every lent kit was lost at its return.
	const ReturnedItem lost{ .slot = 0, .found = false, .health = -1.0f };
	CHECK((AfterReturn(0.8f, &lost) == AfterBack{}));
	CHECK((AfterReturn(-1.0f, &lost) == AfterBack{}));
	const ReturnedItem lostWithHealth{ .slot = 0, .found = false, .health = 0.1f };  // a health with it changes nothing
	CHECK((AfterReturn(0.8f, &lostWithHealth) == AfterBack{}));
	// A whole kit of 5 pieces and a core, none found: nothing of the owner's changes.
	const std::vector<FrameLoan::Tag> kit{ { .slot = 0, .base = 1, .name = "a" }, { .slot = 1, .base = 2, .name = "b" }, { .slot = 2, .base = 3, .name = "c" },
		{ .slot = 3, .base = 4, .name = "d" }, { .slot = 4, .base = 5, .name = "e" }, { .slot = 5, .base = 0x00075FE4, .name = "f", .core = true } };
	const auto none = FrameLoan::ComposeReturn(kit, {});
	REQUIRE(none.size() == 6);
	for (const auto& item : none) {
		CHECK(!item.found);
		CHECK(AfterReturn(0.6f, &item).what == Back::kKeep);
	}
}

TEST("frame loan: a lent item whose name did not hold is taken from our copy by its base, never our player's own core")
{
	using FrameLoan::Found;
	using FrameLoan::Tag;
	using FrameLoan::TakeUnnamed;
	using FrameLoan::NotFound;
	const std::vector<Tag> tags{ { .slot = 0, .base = 0x00140C58, .name = "Sam's T-60 Helmet" }, { .slot = 3, .base = 0x00140C5A, .name = "Sam's T-60 Left Arm" },
		{ .slot = 4, .base = 0x00140C5A, .name = "Sam's T-60 Right Arm" }, { .slot = 7, .base = 0x00075FE4, .name = "Sam's Fusion Core", .core = true } };
	// Found by name: the helmet. The rest are looked for by base, in the order they were made.
	const auto missing = NotFound(tags, { Found{ .slot = 0, .health = 0.5f } });
	REQUIRE(missing.size() == 3);
	CHECK(missing[0].slot == 3);
	CHECK(missing[1].slot == 4);
	CHECK(missing[2].slot == 7);
	CHECK(NotFound(tags, { { .slot = 0 }, { .slot = 3 }, { .slot = 4 }, { .slot = 7 } }).empty());
	CHECK(NotFound({}, {}).empty());
	// Two missing of a base, both in the copy: both taken. One in it: one (the other is somewhere else in our world).
	CHECK(TakeUnnamed(2, 2, 0) == 2);
	CHECK(TakeUnnamed(2, 1, 0) == 1);
	CHECK(TakeUnnamed(1, 3, 0) == 1);  // never more than are missing: an own piece of that base stays for our player
	CHECK(TakeUnnamed(1, 0, 0) == 0);
	// A core: what our player lost of their own to the frame goes back to them, never taken as the lent one.
	CHECK(TakeUnnamed(1, 1, 1) == 0);  // the lent core ran down and one of theirs went in: it is theirs
	CHECK(TakeUnnamed(1, 2, 1) == 1);
	CHECK(TakeUnnamed(1, 1, -2) == 1);  // more of their own than as they got in: nothing held back
	CHECK(TakeUnnamed(1, 5, (std::numeric_limits<std::int32_t>::max)()) == 0);  // after a load, no count of theirs: cores stay
	CHECK(TakeUnnamed(0, 4, 0) == 0);
}

TEST("frame loan: a lent piece taken by its base is one with the mods its copy was made with, never our player's own with others")
{
	using FrameLoan::FitsUnnamed;
	const std::optional<std::vector<std::uint32_t>> made{ { 0x0001F2A1, 0x0001F2B0 } };
	CHECK(FitsUnnamed(made, { 0x0001F2A1, 0x0001F2B0 }));
	CHECK(FitsUnnamed(made, { 0x0001F2B0, 0x0001F2A1 }));  // any order
	// Our player's own piece of the base, put on from the Pip-Boy in place of the lent one: other mods, or fewer, or none.
	CHECK(!FitsUnnamed(made, { 0x0001F2A1, 0x0001F2C3 }));
	CHECK(!FitsUnnamed(made, { 0x0001F2A1 }));
	CHECK(!FitsUnnamed(made, {}));
	// A core (no instance): none made, none found.
	CHECK(FitsUnnamed(std::optional<std::vector<std::uint32_t>>{ std::vector<std::uint32_t>{} }, {}));
	CHECK(!FitsUnnamed(std::optional<std::vector<std::uint32_t>>{ std::vector<std::uint32_t>{} }, { 0x0001F2A1 }));
	// Not known (after a load, the co-save keeps no mods): any stack of the base.
	CHECK(FitsUnnamed(std::nullopt, { 0x0001F2C3 }));
}

TEST("frame loan: a frame our player wears all the way in is theirs, so nothing buries a copy worn on loan from under them")
{
	using FriendFrame::ChooseLeave;
	using FriendFrame::Leave;
	using FriendFrame::Ours;
	// All the way in, the engine has cleared the seat and has no last frame for them yet: only its worn frame names it. On the
	// rig the copy was buried as its owner's game shared it again after a load (or the session ended), and E then did
	// nothing: the engine's exit never finished.
	CHECK(Ours({ .wearing = true }));
	CHECK(ChooseLeave({ .ours = Ours({ .wearing = true }) }) == Leave::kRelease);
	CHECK(Ours({ .seated = true }));    // getting in
	CHECK(Ours({ .lastUsed = true }));  // got out of it last
	CHECK(!Ours({}));
	CHECK(ChooseLeave({ .ours = Ours({}) }) == Leave::kBury);
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

TEST("frame loan: a worn loan's end forfeits the lent items not found, one forfeit each")
{
	using FrameLoan::Tag;
	const std::vector<Tag> tags{ { .slot = 0, .base = 0x10, .name = "Sam's T-60 Helmet" }, { .slot = 1, .base = 0x11, .name = "Sam's T-60 Left Arm" },
		{ .slot = 2, .base = 0x75FE4, .name = "Sam's Fusion Core", .core = true } };
	const std::vector<FrameLoan::Found> found{ { .slot = 1, .health = 0.5f } };
	const auto lost = FrameLoan::NotFound(tags, found);
	CHECK(lost.size() == 2 && lost[0].slot == 0 && lost[1].slot == 2);
	CHECK(FrameLoan::NotFound(tags, { { .slot = 0 }, { .slot = 1 }, { .slot = 2 } }).empty());
	std::vector<Tag> forfeited;
	FrameLoan::Forfeit(forfeited, lost);
	CHECK(forfeited.size() == 2 && forfeited[0].slot == 0 && forfeited[1].slot == 2);
	// A later loan of the same frame leaving the helmet behind again: a second forfeit of that name (two copies of it are
	// in our world, each taken once), the newest last.
	FrameLoan::Forfeit(forfeited, { tags[0] });
	CHECK(forfeited.size() == 3 && forfeited[2] == tags[0]);
	// A tag with no name is never forfeit (no item is named so; it would match nothing anyway).
	FrameLoan::Forfeit(forfeited, { { .slot = 3, .base = 0x12 } });
	CHECK(forfeited.size() == 3);
	// At most MAX_FORFEITS, the oldest going first.
	std::vector<Tag> many;
	for (std::uint32_t i = 0; i < FrameLoan::MAX_FORFEITS + 6; ++i) {
		many.push_back({ .slot = 0, .base = 0x1000 + i, .name = "Sam's piece" });
	}
	FrameLoan::Forfeit(forfeited, many);
	CHECK(forfeited.size() == FrameLoan::MAX_FORFEITS);
	CHECK(forfeited.front().base == 0x1000 + 6 && forfeited.back().base == 0x1000 + FrameLoan::MAX_FORFEITS + 5);
}

TEST("frame loan: only a lent item seen while worn is forfeit; one never in our copy is not")
{
	using FrameLoan::Tag;
	const std::vector<Tag> tags{ { .slot = 0, .base = 0x75FE4, .name = "Sam's Fusion Core", .core = true }, { .slot = 1, .base = 0x154ABD, .name = "Sam's T-60 Helmet" },
		{ .slot = 2, .base = 0x154ABE, .name = "Sam's T-60 Left Arm" } };
	// The rig's batch 12: the kit gone from our copy before our player got in, nothing seen while worn, nothing found at the
	// return. Nothing is forfeit (it was six a loan, eight loans filling the list with 48).
	CHECK(FrameLoan::Forfeits(tags, {}, {}).empty());
	CHECK(FrameLoan::NotFound(tags, {}).size() == 3);
	// Seen while worn and found at the return: nothing forfeit.
	const std::vector<std::uint8_t> all{ 0, 1, 2 };
	CHECK(FrameLoan::Forfeits(tags, { { .slot = 0 }, { .slot = 1 }, { .slot = 2 } }, all).empty());
	// Seen while worn and not found at the return (put in a chest): forfeit, one each.
	const auto lost = FrameLoan::Forfeits(tags, { { .slot = 1, .health = 0.4f } }, all);
	CHECK(lost.size() == 2 && lost[0] == tags[0] && lost[1] == tags[2]);
	// Only the ones seen: the arm never was, so it is not forfeit though not found.
	const auto some = FrameLoan::Forfeits(tags, {}, { 1 });
	CHECK(some.size() == 1 && some[0] == tags[1]);
}

TEST("frame loan: the lent items seen while worn are held once each, and only lent ones")
{
	using FrameLoan::Tag;
	const std::vector<Tag> tags{ { .slot = 0, .base = 0x10, .name = "Sam's T-60 Helmet" }, { .slot = 3, .base = 0x11, .name = "Sam's T-60 Torso" } };
	std::vector<std::uint8_t> held;
	FrameLoan::Hold(held, tags, {});
	CHECK(held.empty());
	FrameLoan::Hold(held, tags, { 3 });
	CHECK(held == std::vector<std::uint8_t>{ 3 });
	// Seen again (in the copy as they got in, then on our player): still once.
	FrameLoan::Hold(held, tags, { 3, 0, 3 });
	CHECK(held == (std::vector<std::uint8_t>{ 3, 0 }));
	// A slot no lent item has is never held.
	FrameLoan::Hold(held, tags, { 7 });
	CHECK(held.size() == 2);
	// A save made in the frame notes only those, in the kit's order: its load forfeits no more than the loan's end would.
	CHECK(FrameLoan::Held(tags, held) == tags);
	CHECK(FrameLoan::Held(tags, { 3 }) == std::vector<Tag>{ tags[1] });
	CHECK(FrameLoan::Held(tags, {}).empty());
	CHECK(FrameLoan::Held({}, { 0, 3 }).empty());
}

TEST("frame loan: the LOAN record keeps the forfeits, reads an older build's, and forfeits nothing of a damaged one")
{
	using FrameLoan::Decode;
	using FrameLoan::Encode;
	FrameLoan::Record record{ .copy = 0xFF0012AB, .base = 0x0019DED9, .spawnId = 0xFF800005, .owner = 3 };
	record.tags.push_back({ .slot = 0, .base = 0x00140C58, .name = "Sam's T-60 Helmet" });
	record.forfeited.push_back({ .slot = 2, .base = 0x00140C5A, .name = "Sam's T-60 Left Arm" });
	record.forfeited.push_back({ .slot = 7, .base = 0x00075FE4, .name = "Sam's Fusion Core", .core = true });
	const auto bytes = Encode(record);
	CHECK(Decode(bytes) == record);
	CHECK(Decode(bytes, FrameLoan::RECORD_VERSION) == record);
	for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
		CHECK(!Decode({ bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut) }));
	}
	auto longer = bytes;
	longer.push_back(0);
	CHECK(!Decode(longer));
	// The forfeits alone, with nothing of a loan (a save made after the loan ended): kept, and nothing to clean up.
	FrameLoan::Record alone;
	alone.forfeited = record.forfeited;
	const auto aloneRead = Decode(Encode(alone));
	CHECK(aloneRead == alone);
	CHECK(aloneRead && FrameLoan::ChooseAfterLoad(*aloneRead, false, 0, false) == FrameLoan::AfterLoad::kNothing);
	CHECK(!Decode(Encode({})));  // nothing at all
	// A loan's with an empty list.
	auto none = record;
	none.forfeited.clear();
	CHECK(Decode(Encode(none)) == none);
	// An older build's (version 1: the loan alone, with no count of forfeits after it) reads with none forfeited; read as
	// the other version, neither layout is taken for the other.
	auto older = Encode(none);
	CHECK(older.back() == 0);
	older.pop_back();
	CHECK(Decode(older, 1) == none);
	CHECK(!Decode(older, 2));
	CHECK(!Decode(bytes, 1));
	CHECK(!Decode(bytes, 0));
	CHECK(!Decode(bytes, FrameLoan::RECORD_VERSION + 1));
	// Damaged: a count over the most, a core flag that is no flag, a name longer than what is left.
	const std::size_t forfeitsAt = 17 + 1 + 4 + 1 + 1 + record.tags[0].name.size();
	CHECK(bytes[forfeitsAt] == 2);
	auto tooMany = bytes;
	tooMany[forfeitsAt] = static_cast<std::uint8_t>(FrameLoan::MAX_FORFEITS + 1);
	CHECK(!Decode(tooMany));
	auto badCore = bytes;
	badCore[forfeitsAt + 1 + 1 + 4] = 2;
	CHECK(!Decode(badCore));
	auto longName = bytes;
	longName[forfeitsAt + 1 + 1 + 4 + 1] = 255;
	CHECK(!Decode(longName));
	// Whatever any byte is, the record reads within its bytes (and a loan with no copy is never read).
	for (std::size_t i = 0; i < bytes.size(); ++i) {
		for (const std::uint8_t value : { std::uint8_t{ 0 }, std::uint8_t{ 1 }, std::uint8_t{ 0x7F }, std::uint8_t{ 0xFF } }) {
			auto changed = bytes;
			changed[i] = value;
			const auto read = Decode(changed);
			CHECK(!read || read->copy != 0 || read->tags.empty());
		}
	}
	// Of more forfeits than the record keeps, the newest.
	FrameLoan::Record full;
	for (std::uint32_t i = 0; i < FrameLoan::MAX_FORFEITS + 3; ++i) {
		full.forfeited.push_back({ .slot = 0, .base = 0x2000 + i, .name = "Sam's piece" });
	}
	const auto fullRead = Decode(Encode(full));
	CHECK(fullRead && fullRead->forfeited.size() == FrameLoan::MAX_FORFEITS && fullRead->forfeited.front().base == 0x2000 + 3);
}

TEST("frame loan: the spawn lent to us is held from the kit coming until the loan ends, and spawn id 0 is never held")
{
	using FrameLoan::HoldsSpawn;
	CHECK(!HoldsSpawn(Stage::kNone, 7, 7));
	CHECK(!HoldsSpawn(Stage::kAsked, 7, 7));
	CHECK(HoldsSpawn(Stage::kLent, 7, 7));
	CHECK(HoldsSpawn(Stage::kWorn, 7, 7));
	CHECK(HoldsSpawn(Stage::kOut, 7, 7));
	CHECK(!HoldsSpawn(Stage::kWorn, 7, 8));
	// A loan worn on past its session lets go of its id: no spawn of the next session is taken for it.
	CHECK(!HoldsSpawn(Stage::kWorn, 0, 0));
	CHECK(!HoldsSpawn(Stage::kWorn, 0, 7));
}

TEST("frame loan: the frame we got out of is told as parked unless it is lent out or the copy lent to us")
{
	using FrameLoan::TellsParked;
	CHECK(TellsParked(false, false));
	CHECK(!TellsParked(true, false));
	CHECK(!TellsParked(false, true));
	CHECK(!TellsParked(true, true));
}

TEST("frame loan: the clean-up after a load waits the exit settle once our player is out")
{
	using FrameLoan::CleanupDue;
	CHECK(!CleanupDue(true, std::nullopt));
	CHECK(!CleanupDue(true, 1s));
	CHECK(CleanupDue(true, 2s));
	CHECK(CleanupDue(true, 5s));
	CHECK(CleanupDue(false, std::nullopt));  // no copy: nothing to settle
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
