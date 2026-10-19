// The game-free half of giving items by dropping them: what may be given, when a dropped item has come to rest, our
// player's claims, and the co-save record of what our world holds of the session's transfers.

#include "Test.h"

#include "game/DropBook.h"

using namespace std::chrono_literals;

namespace
{
	using DropBook::Givable;
	using DropBook::ItemFacts;
	using LootScope::Item;

	ItemFacts Facts(Item a_kind, std::int32_t a_count = 1)
	{
		ItemFacts facts;
		facts.kind = a_kind;
		facts.count = a_count;
		return facts;
	}
}

TEST("drop book: weapons, armor, ammo, aid and junk are given; keys, notes, books and featured items are everyone's own")
{
	for (const auto kind : { Item::kWeapon, Item::kArmor, Item::kAmmo, Item::kAid, Item::kMisc, Item::kComponent, Item::kIngredient }) {
		CHECK(DropBook::Judge(Facts(kind)) == Givable::kGive);
	}
	for (const auto kind : { Item::kKey, Item::kNote, Item::kBook }) {
		CHECK(DropBook::Judge(Facts(kind)) == Givable::kPersonal);
	}
	auto bobblehead = Facts(Item::kMisc);
	bobblehead.featured = true;
	CHECK(DropBook::Judge(bobblehead) == Givable::kPersonal);
	CHECK(DropBook::Judge(Facts(Item::kOther)) == Givable::kNotItem);
}

TEST("drop book: a quest item, or one whose base or a mod exists in our game only, stays ours")
{
	auto quest = Facts(Item::kWeapon);
	quest.questItem = true;
	CHECK(DropBook::Judge(quest) == Givable::kQuest);
	auto made = Facts(Item::kArmor);
	made.runtimeBase = true;
	CHECK(DropBook::Judge(made) == Givable::kNotEverywhere);
	auto modded = Facts(Item::kWeapon);
	modded.runtimeMod = true;
	CHECK(DropBook::Judge(modded) == Givable::kNotEverywhere);
}

TEST("drop book: a stack is given up to what one dropped reference holds")
{
	CHECK(DropBook::Judge(Facts(Item::kAmmo, Protocol::MAX_DROP_COUNT)) == Givable::kGive);
	CHECK(DropBook::Judge(Facts(Item::kAmmo, Protocol::MAX_DROP_COUNT + 1)) == Givable::kTooMany);
	CHECK(DropBook::Judge(Facts(Item::kAmmo, 0)) == Givable::kTooMany);
}

TEST("drop book: a dropped item is reported once it has rested, never sooner than the minimum, and by the maximum")
{
	CHECK(!DropBook::Settled(1s, 1s));             // still for long enough, but too soon after the drop
	CHECK(!DropBook::Settled(2s, 100ms));          // moving
	CHECK(DropBook::Settled(2s, 600ms));           // came to rest
	CHECK(DropBook::Settled(DropBook::SETTLE_MAX, 0ms));  // still rolling: reported where it is
}

TEST("drop book: one claim per item until answered, again after the retry time, and only so many at once")
{
	DropBook::Claims claims;
	const auto       t0 = DropBook::Clock::time_point{} + 1h;
	CHECK(claims.Start(7, t0));
	CHECK(!claims.Start(7, t0 + 1s));  // a held key: not a claim a frame
	CHECK(claims.Waiting(7));
	CHECK(claims.Start(7, t0 + DropBook::Claims::CLAIM_RETRY));  // an answer lost: asked again
	CHECK(claims.Answered(7));
	CHECK(!claims.Answered(7));  // a second answer (the item went to someone) is no claim of ours
	for (std::uint32_t id = 100; id < 108; ++id) {
		CHECK(claims.Start(id, t0));
	}
	CHECK(!claims.Start(200, t0 + 1s));  // eight out
	CHECK(claims.Start(200, t0 + DropBook::Claims::CLAIM_RETRY));  // the old ones expired
	claims.Clear();
	CHECK(claims.Size() == 0u);
}

TEST("drop book: the record holds the session we are in, and tells the server only that session's transfers")
{
	DropBook::Record record;
	record.Made(1);  // in no session yet: nothing to record it under
	CHECK(record.Empty());
	record.Bind(0xA);
	record.Made(11);
	record.Made(11);
	record.Took(5);
	record.Bind(0xB);
	record.Took(9);
	auto b = record.ForServer();
	CHECK(b.sessionId == 0xB);
	CHECK(b.made.empty());
	CHECK(b.taken == std::vector<std::uint32_t>{ 9 });
	record.Bind(0xA);
	const auto a = record.ForServer();
	CHECK(a.made == std::vector<std::uint64_t>{ 11 });
	CHECK(a.taken == std::vector<std::uint32_t>{ 5 });
	CHECK(record.HasMade(11));
	CHECK(record.HasTaken(5));
	CHECK(!record.HasTaken(9));
	record.Bind(0);  // out of the session: nothing sent, nothing recorded
	CHECK(record.ForServer().sessionId == 0);
	record.Took(77);
	record.Bind(0xA);
	CHECK(!record.HasTaken(77));
}

TEST("drop book: the record round-trips through the co-save, and a loaded one keeps the session we are in")
{
	DropBook::Record record;
	record.Bind(0xA);
	record.Made(0x1122334455667788);
	record.Took(3);
	record.Bind(0xB);
	record.Took(4);
	const auto bytes = record.Save();
	auto       loaded = DropBook::Record::Load(bytes);
	REQUIRE(loaded);
	CHECK(loaded->Save() == bytes);

	DropBook::Record now;
	now.Bind(0xA);
	now.Took(99);  // after the save: the loaded world doesn't have it
	now.Replace(std::move(*loaded));
	CHECK(now.Current() == 0xA);
	const auto sent = now.ForServer();
	CHECK(sent.made == std::vector<std::uint64_t>{ 0x1122334455667788 });
	CHECK(sent.taken == std::vector<std::uint32_t>{ 3 });
}

TEST("drop book: a damaged record is not read")
{
	DropBook::Record record;
	record.Bind(0xA);
	record.Took(3);
	auto bytes = record.Save();
	CHECK(!DropBook::Record::Load(std::span{ bytes }.first(bytes.size() - 1)));
	bytes.push_back(0);
	CHECK(!DropBook::Record::Load(bytes));
	bytes.pop_back();
	bytes[0] = 9;  // a version to come
	CHECK(!DropBook::Record::Load(bytes));
	CHECK(!DropBook::Record::Load({}));
}

TEST("drop book: the record keeps the latest sessions and, in each, the latest transfers")
{
	DropBook::Record record;
	for (std::uint64_t session = 1; session <= DropBook::Record::MAX_SESSIONS + 1; ++session) {
		record.Bind(session);
		record.Took(static_cast<std::uint32_t>(session));
	}
	record.Bind(1);  // the oldest went: a fresh entry (in place of the next oldest, 2)
	CHECK(!record.HasTaken(1));
	record.Bind(3);
	CHECK(record.HasTaken(3));
	for (std::uint32_t id = 0; id < Protocol::MAX_DROP_RECORD + 5; ++id) {
		record.Took(1000 + id);
	}
	const auto sent = record.ForServer();
	CHECK(sent.taken.size() == Protocol::MAX_DROP_RECORD);
	CHECK(sent.taken.back() == 1000 + Protocol::MAX_DROP_RECORD + 4);
	const auto loaded = DropBook::Record::Load(record.Save());
	CHECK(loaded.has_value());
}
