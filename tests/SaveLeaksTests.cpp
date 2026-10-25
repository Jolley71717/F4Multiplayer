// What a load does with references of ours on their way out as it comes (SaveStrays::Remains, LingerOf, Handover,
// LostOnWayOut; SaveGuard::BeforeLoad and AfterLoad): the engine's load frees only what has a record of changes, and every
// one of ours on its way out has had its record dropped before it. A friend's stand-in so kept through a load points at its
// base, a face copy the load frees, and a save that held one crashed the second load of it in a run.
// A save an earlier version wrote with one in it is still not safe to load again and again in one run on the rig (the third
// load crashed, on a settler): this models what a load does with ours, not all the engine does with the save's record.

#include "Test.h"
#include "game/SaveStrays.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace
{
	constexpr std::uint32_t FRAME = 0x0017F6E4;  // a power armor frame every game has
	constexpr std::uint32_t LIGHT = 0x0001ED2D;  // the friend's Pip-Boy light
	constexpr std::uint32_t FACE = 0xFF0008E4;   // a friend's face copy: a created base, with a record

	// The engine as far as a load goes (BGSSaveLoadGame::RevertAllChanges and TESForm::AddChange, read from disk, 1.11.240):
	// a load frees each created form with a record, and nothing else; a record is refused to a created reference that is
	// deleted and not persistent. An actor is freed some frames after the load lets go of it, and reads its base as it goes:
	// one whose created base the same load freed (at once) wrote into freed memory, and the next load crashed. One kept
	// through a load whose base the load freed is broken: touching it after that load crashes.
	struct Engine
	{
		struct Form
		{
			std::uint32_t base = 0;
			bool          record = true;
			bool          deleted = false;
			bool          persistent = false;
			bool          actor = false;
		};
		std::map<std::uint32_t, Form> forms;
		bool                          freedUnderIt = false;  // an actor freed after its base (see above)

		bool AddChange(std::uint32_t a_id)
		{
			auto& form = forms.at(a_id);
			if (form.deleted && !form.persistent) {
				return false;
			}
			form.record = true;
			return true;
		}

		static bool Created(std::uint32_t a_id) { return (a_id >> 24) == 0xFF; }

		void Load()
		{
			for (const auto& [id, form] : forms) {
				if (form.record && form.actor && Created(form.base) && forms.contains(form.base) && forms.at(form.base).record) {
					freedUnderIt = true;
				}
			}
			std::erase_if(forms, [](const auto& a_entry) { return a_entry.second.record; });
		}

		[[nodiscard]] bool Broken(std::uint32_t a_id) const
		{
			const auto it = forms.find(a_id);
			return it != forms.end() && Created(it->second.base) && !forms.contains(it->second.base);
		}
	};
	// SaveGuard::BeforeLoad's hand-over (SaveGuard::HandToLoad), on the model: what each one came to. a_keepBase: an actor's
	// created base has its record dropped, so the load keeps it until the actor is gone (as HandToLoad does).
	SaveStrays::Handover HandOver(Engine& a_engine, std::uint32_t a_id, bool a_keepBase = true)
	{
		auto& form = a_engine.forms.at(a_id);
		if (a_keepBase && form.actor && Engine::Created(form.base) && a_engine.forms.contains(form.base)) {
			a_engine.forms.at(form.base).record = false;
		}
		const bool had = form.record;
		const auto handover = SaveStrays::HandoverOf(had, !had && a_engine.AddChange(a_id));
		if (handover == SaveStrays::Handover::kRelease) {
			a_engine.forms.erase(a_id);  // the engine's own letting go, as the load does it
		}
		return handover;
	}

	enum class Build
	{
		k091,       // nothing handed over
		kNoBase,    // handed over, the base left with its record
		kHandOver,  // as now
	};
}

TEST("save leaks: a load frees only what has a record, so a stand-in buried for it stays, its face copy freed, unless handed to the load")
{
	for (const auto build : { Build::k091, Build::kNoBase, Build::kHandOver }) {
		Engine engine;
		engine.forms[FACE] = { .base = 0x0002A3F5 };                      // the face copy (from a settler), with a record
		engine.forms[0xFF0010AA] = { .base = FACE, .actor = true };        // the friend's stand-in, buried as the load comes
		engine.forms[0xFF0010B0] = { .base = FRAME };                      // its frame copy
		engine.forms[0xFF0010B1] = { .base = LIGHT };                      // its light
		engine.forms[0xFF0010B2] = { .base = FRAME, .deleted = true };     // a copy DespawnAll had deleted already
		// DespawnAll: none of them may be written, so each has its record dropped (SaveGuard::Exclude).
		for (const auto id : { 0xFF0010AAu, 0xFF0010B0u, 0xFF0010B1u, 0xFF0010B2u }) {
			engine.forms[id].record = false;
		}
		if (build != Build::k091) {
			const bool keepBase = build == Build::kHandOver;
			CHECK(HandOver(engine, 0xFF0010AA, keepBase) == SaveStrays::Handover::kGiveRecord);
			CHECK(HandOver(engine, 0xFF0010B0, keepBase) == SaveStrays::Handover::kGiveRecord);
			CHECK(HandOver(engine, 0xFF0010B1, keepBase) == SaveStrays::Handover::kGiveRecord);
			CHECK(HandOver(engine, 0xFF0010B2, keepBase) == SaveStrays::Handover::kRelease);  // deleted, not persistent: takes no record
		}
		engine.Load();
		switch (build) {
		case Build::k091:
			CHECK(engine.forms.size() == 4);   // all of it kept through the load
			CHECK(engine.Broken(0xFF0010AA));  // its base went with the load: the next one crashed on it
			break;
		case Build::kNoBase:
			CHECK(engine.forms.empty());
			CHECK(engine.freedUnderIt);  // the stand-in freed after its face copy
			break;
		case Build::kHandOver:
			CHECK(engine.forms.size() == 1);  // only the face copy, nobody's, with no record: no save writes it
			CHECK(engine.forms.contains(FACE));
			CHECK(!engine.forms.at(FACE).record);
			CHECK(!engine.freedUnderIt);
			break;
		}
	}
}

TEST("save leaks: a stand-in an earlier version wrote into a save is handed to each next load, so none is kept through one with its base gone")
{
	for (const auto build : { Build::k091, Build::kNoBase, Build::kHandOver }) {
		Engine engine;
		bool   broken = false;
		std::optional<std::uint32_t> broughtBack;  // SaveGuard's list of strays the last load brought back
		for (std::uint32_t load = 0; load < 4; ++load) {
			if (build != Build::k091 && broughtBack && engine.forms.contains(*broughtBack)) {
				CHECK(HandOver(engine, *broughtBack, build == Build::kHandOver) == SaveStrays::Handover::kGiveRecord);
			}
			broughtBack.reset();
			engine.Load();
			broken = broken || engine.Broken(0xFF0010AA);
			// The save brings back the stand-in, in no cell, and its face copy (under a new id each load), each with its
			// record; one still in memory under the stand-in's id takes the save's record (the form is not made again).
			const auto face = FACE + 0x100 * (load + 1);
			engine.forms[face] = { .base = 0x0002A3F5 };
			auto& standIn = engine.forms[0xFF0010AA];
			standIn.base = face;
			standIn.actor = true;
			standIn.persistent = true;
			// AfterLoad: a stray of ours; its record is dropped, so no save writes it (and it is noted, to go with the next load).
			standIn.record = false;
			broughtBack = 0xFF0010AA;
		}
		CHECK(broken == (build == Build::k091));
		CHECK(engine.freedUnderIt == (build == Build::kNoBase));
	}
}

TEST("save leaks: what a load came to with each of ours on its way out")
{
	using SaveStrays::Handover;
	using SaveStrays::HandoverOf;
	CHECK(HandoverOf(true, false) == Handover::kHasRecord);  // left to the load as it is (a copy kept for a send-out, a held one)
	CHECK(HandoverOf(true, true) == Handover::kHasRecord);
	CHECK(HandoverOf(false, true) == Handover::kGiveRecord);
	CHECK(HandoverOf(false, false) == Handover::kRelease);
}

TEST("save leaks: a stand-in or a light kept through a load all the same lingers as a frame copy does, by kind")
{
	using SaveStrays::StillOurs;
	// A friend's stand-in, buried before the load: the same form, disabled or deleted, and no puppet of ours.
	const SaveStrays::Remains standIn{ .same = true, .actor = true, .off = true };
	CHECK(StillOurs(standIn));
	auto with = [](SaveStrays::Remains a_remains, auto a_change) {
		a_change(a_remains);
		return StillOurs(a_remains);
	};
	CHECK(!with(standIn, [](auto& r) { r.same = false; }));   // an actor the loaded game made since under the id
	CHECK(!with(standIn, [](auto& r) { r.off = false; }));    // enabled
	CHECK(!with(standIn, [](auto& r) { r.puppet = true; }));  // one of our puppets again (a stand-in in use, a mirrored NPC)
	// What a frame copy asks does not apply to an actor: a stand-in carries its friend's gear.
	CHECK(with(standIn, [](auto& r) { r.holdsItems = true; }));
	CHECK(with(standIn, [](auto& r) { r.newOwner = true; }));

	const SaveStrays::Remains light{ .same = true, .off = true };
	CHECK(StillOurs(light));
	CHECK(!with(light, [](auto& r) { r.same = false; }));
	CHECK(!with(light, [](auto& r) { r.off = false; }));

	// The book keeps each kind lingering alike.
	SaveStrays::Book book;
	book.Track(0xFF0010AA, FACE);
	book.Track(0xFF0010B1, LIGHT);
	book.Track(0xFF0010B0, FRAME);
	const std::map<std::uint32_t, SaveStrays::Remains> now{
		{ 0xFF0010AA, standIn },
		{ 0xFF0010B1, light },
		{ 0xFF0010B0, { .same = true, .frame = true, .off = true, .worn = true } },  // the player's suit now
	};
	book.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; }, [&](std::uint32_t a_id, std::uint32_t) { return StillOurs(now.at(a_id)); });
	CHECK(book.LingeringCount() == 2);
	CHECK(book.IsLingering(0xFF0010AA, FACE));
	CHECK(book.IsLingering(0xFF0010B1, LIGHT));
	CHECK(!book.IsLingering(0xFF0010B0, FRAME));
}

TEST("save leaks: a copy buried before its 3D came in, never disabled, is disabled after the load and lingers")
{
	using SaveStrays::Linger;
	using SaveStrays::LingerOf;
	// Placed as a friend's parked frame came into view; the player quickloads before its 3D is in, so it was buried enabled.
	const SaveStrays::Remains waiting{ .same = true, .frame = true, .off = false, .notedOn = true };
	CHECK(LingerOf(waiting) == Linger::kTurnOff);
	auto with = [&](auto a_change) {
		auto remains = waiting;
		a_change(remains);
		return LingerOf(remains);
	};
	CHECK(with([](auto& r) { r.off = true; }) == Linger::kYes);             // off already: lingers as it is
	CHECK(with([](auto& r) { r.notedOn = false; }) == Linger::kNo);         // noted off, and enabled since: not ours to turn off
	CHECK(with([](auto& r) { r.same = false; }) == Linger::kNo);            // the loaded game's frame under the id
	CHECK(with([](auto& r) { r.worn = true; }) == Linger::kNo);             // somebody is in it
	CHECK(with([](auto& r) { r.holdsItems = true; }) == Linger::kNo);
	CHECK(with([](auto& r) { r.newOwner = true; }) == Linger::kNo);
	// A stand-in buried so: the same, unless it is a puppet again.
	const SaveStrays::Remains standIn{ .same = true, .actor = true, .notedOn = true };
	CHECK(LingerOf(standIn) == Linger::kTurnOff);
	auto puppet = standIn;
	puppet.puppet = true;
	CHECK(LingerOf(puppet) == Linger::kNo);
	// Once disabled it is noted off, and asked as any other from then on (StillOurs needs it off).
	auto turnedOff = waiting;
	turnedOff.off = true;
	turnedOff.notedOn = false;
	CHECK(LingerOf(turnedOff) == Linger::kYes);
	turnedOff.off = false;  // enabled by the game since: left to it
	CHECK(LingerOf(turnedOff) == Linger::kNo);
}

TEST("save leaks: a graveyard entry whose handle failed, its form still there, is still on its way out at the load")
{
	SaveStrays::LostOnWayOut lost;
	int copy = 0, other = 0;  // two addresses
	lost.Add(0xFF001200, &copy);
	CHECK(lost.Has(0xFF001200, &copy));
	CHECK(!lost.Has(0xFF001200, &other));  // another form under the id now
	CHECK(!lost.Has(0xFF001201, &copy));
	CHECK(!lost.Has(0xFF001200, nullptr));
	lost.Add(0xFF001200, &copy);  // once only
	lost.Add(0, &copy);
	lost.Add(0xFF001202, nullptr);
	CHECK(lost.Size() == 1);

	// Let go of once its form is gone, or the id names another form.
	lost.Add(0xFF001203, &other);
	std::map<std::uint32_t, const void*> now{ { 0xFF001200, &copy }, { 0xFF001203, &copy } };
	lost.Keep([&](std::uint32_t a_id) -> const void* {
		const auto it = now.find(a_id);
		return it != now.end() ? it->second : nullptr;
	});
	CHECK(lost.Size() == 1);
	CHECK(lost.Has(0xFF001200, &copy));
	now.clear();
	lost.Keep([&](std::uint32_t) -> const void* { return nullptr; });
	CHECK(lost.Size() == 0);

	// Past MAX the oldest go; a load clears them all.
	std::vector<int> many(SaveStrays::LostOnWayOut::MAX + 3);
	for (std::size_t i = 0; i < many.size(); ++i) {
		lost.Add(0xFF002000 + static_cast<std::uint32_t>(i), &many[i]);
	}
	CHECK(lost.Size() == SaveStrays::LostOnWayOut::MAX);
	CHECK(!lost.Has(0xFF002000, &many[0]));
	CHECK(lost.Has(0xFF002000 + static_cast<std::uint32_t>(many.size() - 1), &many.back()));
	lost.Clear();
	CHECK(lost.Size() == 0);
}

TEST("save leaks: a forgotten form of ours still there keeps its id while its record is dropped each second")
{
	// TESDataHandler::GetNextID passes over an id while a form is under it, before it asks about a record: dropping the record
	// of a forgotten form still there (KeepForgotten's a_kept) never frees the id. Only its form going does (a_orphaned, or
	// gone under another form), and then the book lets go of it.
	SaveStrays::Book book;
	book.Track(0xFF000D00, FRAME);
	CHECK(book.Forget(0xFF000D00));
	std::vector<std::uint32_t> kept, orphaned;
	const auto keep = [&](std::optional<std::uint32_t> a_base) {
		book.KeepForgotten([&](std::uint32_t) { return a_base; }, [&](std::uint32_t a_id) { kept.push_back(a_id); }, [&](std::uint32_t a_id) { orphaned.push_back(a_id); });
	};
	keep(FRAME);
	keep(FRAME);
	CHECK(kept.size() == 2);
	CHECK(book.ForgottenCount() == 1);
	keep(std::nullopt);  // the form is gone: its record dropped once more, and off the books
	CHECK(orphaned == std::vector<std::uint32_t>{ 0xFF000D00 });
	CHECK(book.ForgottenCount() == 0);
	CHECK(!book.IsPlaced(0xFF000D00, FRAME));
}
