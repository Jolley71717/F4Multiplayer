#include "Test.h"
#include "game/WorkshopDamage.h"

#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <vector>

// Settlement damage and repair: a piece's damage is its builder's game's say, copies show it and nothing else, and a
// friend's repair is the host's game's, paid from the settlement's workbench.
namespace
{
	using namespace WorkshopDamage;
	using namespace std::chrono_literals;

	constexpr std::uint32_t GENERATOR = 0xFF000A01;
	constexpr std::uint32_t PURIFIER = 0xFF000A02;
	constexpr std::uint32_t CROP = 0xFF000A03;
	constexpr std::uint32_t HOST = 1;
	constexpr std::uint32_t OTHER = 2;

	constexpr State WHOLE{};
	constexpr State HALF{ 50.0f, false };
	constexpr State WRECKED{ 0.0f, true };
	constexpr State FLAGGED{ Protocol::NO_PIECE_HEALTH, true };  // destroyed with no health kept: a crop

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	// The builder's game: each piece's damage, none once it is gone.
	struct Pieces
	{
		std::map<std::uint32_t, State> pieces;
		int                            reads = 0;

		std::optional<State> Read(std::uint32_t a_ref)
		{
			++reads;
			const auto it = pieces.find(a_ref);
			return it != pieces.end() ? std::optional<State>{ it->second } : std::nullopt;
		}
	};

	// A game's copies: what each shows, and what was done to them. A show does what the engine does: a mend makes whole,
	// a wreck lowers to the wanted.
	struct Copies
	{
		std::map<Key, State>              copies;
		std::vector<std::pair<Key, Step>> shows;
		bool                              refuses = false;
		bool                              stubborn = false;  // the copy takes the show and stays as it was (a fire it stands in)

		std::optional<State> Shown(const Key& a_key)
		{
			const auto it = copies.find(a_key);
			return it != copies.end() ? std::optional<State>{ it->second } : std::nullopt;
		}

		bool Show(const Key& a_key, Step a_step, const State& a_wanted)
		{
			shows.emplace_back(a_key, a_step);
			if (refuses) {
				return false;
			}
			if (!stubborn) {
				copies[a_key] = a_step == Step::kMend ? State{} : a_wanted;
			}
			return true;
		}
	};

	std::vector<std::uint32_t> RefsOf(const std::vector<Entry>& a_entries)
	{
		std::vector<std::uint32_t> out;
		for (const auto& entry : a_entries) {
			out.push_back(entry.refId);
		}
		return out;
	}
}

TEST("workshop damage: a piece is whole with no health and no destroyed flag, and two healths half a point apart are one")
{
	CHECK(Whole(WHOLE));
	CHECK(!Whole(HALF));
	CHECK(!Whole(WRECKED));
	CHECK(!Whole(FLAGGED));
	CHECK(Same(HALF, State{ 50.4f, false }));
	CHECK(!Same(HALF, State{ 51.0f, false }));
	CHECK(!Same(HALF, State{ 50.0f, true }));
	CHECK(!Same(WHOLE, State{ 0.0f, false }));  // no health is not a health of nothing
	CHECK(Same(FLAGGED, FLAGGED));
}

TEST("workshop damage: a state crosses a message and comes back as it was")
{
	for (const auto& state : { WHOLE, HALF, WRECKED, FLAGGED }) {
		const auto entry = EntryOf(GENERATOR, state);
		CHECK(entry.refId == GENERATOR);
		CHECK(StateOf(entry) == state);
	}
	CHECK(EntryOf(GENERATOR, WHOLE).health == Protocol::NO_PIECE_HEALTH);
	CHECK(StateOf(Entry{ GENERATOR, -7.0f, false }) == WHOLE);  // any negative health is none
}

TEST("workshop damage: the reporter tracks only pieces a message can name")
{
	Reporter reporter;
	CHECK(!reporter.Track(0x0001F2A3));  // placed by a plugin: the destructibles sync's, not ours
	CHECK(!reporter.Track(0));
	CHECK(reporter.Track(GENERATOR));
	CHECK(reporter.Track(GENERATOR));  // twice is once
	CHECK(reporter.Size() == 1);
	CHECK(reporter.Tracks(GENERATOR));
	CHECK(!reporter.Said(GENERATOR));
}

TEST("workshop damage: the reporter says every piece once, then only what changed")
{
	Reporter reporter;
	Pieces   game{ { { GENERATOR, WHOLE }, { PURIFIER, WHOLE } } };
	reporter.Track(GENERATOR);
	reporter.Track(PURIFIER);
	auto now = T0;
	CHECK((RefsOf(reporter.Poll(game, now)) == std::vector<std::uint32_t>{ GENERATOR, PURIFIER }));
	REQUIRE(reporter.Said(GENERATOR).has_value());
	CHECK(*reporter.Said(GENERATOR) == WHOLE);
	now += POLL;
	CHECK(reporter.Poll(game, now).empty());
	game.pieces[GENERATOR] = WRECKED;
	now += POLL;
	const auto out = reporter.Poll(game, now);
	REQUIRE(out.size() == 1);
	CHECK(out[0].refId == GENERATOR);
	CHECK(out[0].destroyed);
	CHECK(out[0].health == 0.0f);
	// Repaired by the host's game: whole again, and said so.
	game.pieces[GENERATOR] = WHOLE;
	now += POLL;
	const auto mended = reporter.Poll(game, now);
	REQUIRE(mended.size() == 1);
	CHECK(StateOf(mended[0]) == WHOLE);
}

TEST("workshop damage: the reporter looks at the game once a poll, not every frame")
{
	Reporter reporter;
	Pieces   game{ { { GENERATOR, WHOLE } } };
	reporter.Track(GENERATOR);
	CHECK(reporter.Poll(game, T0).size() == 1);
	game.pieces[GENERATOR] = WRECKED;
	const int reads = game.reads;
	CHECK(reporter.Poll(game, T0 + 100ms).empty());
	CHECK(reporter.Poll(game, T0 + POLL - 1ms).empty());
	CHECK(game.reads == reads);
	CHECK(reporter.Poll(game, T0 + POLL).size() == 1);
}

TEST("workshop damage: a health that moved less than the slack is not said again")
{
	Reporter reporter;
	Pieces   game{ { { GENERATOR, HALF } } };
	reporter.Track(GENERATOR);
	CHECK(reporter.Poll(game, T0).size() == 1);
	game.pieces[GENERATOR] = State{ 50.3f, false };
	CHECK(reporter.Poll(game, T0 + POLL).empty());
	game.pieces[GENERATOR] = State{ 40.0f, false };
	CHECK(reporter.Poll(game, T0 + 2 * POLL).size() == 1);
}

TEST("workshop damage: every piece is said again now and then, and when a player joins or a save loads")
{
	Reporter reporter;
	Pieces   game{ { { GENERATOR, WRECKED }, { PURIFIER, WHOLE } } };
	reporter.Track(GENERATOR);
	reporter.Track(PURIFIER);
	CHECK(reporter.Poll(game, T0).size() == 2);
	CHECK(reporter.Poll(game, T0 + POLL).empty());
	// A friend who arrives after the damage: all of it at the next poll.
	reporter.SendAll();
	CHECK(reporter.Poll(game, T0 + 2 * POLL).size() == 2);
	CHECK(reporter.Poll(game, T0 + 3 * POLL).empty());
	// And unasked, each snapshot.
	CHECK(reporter.Poll(game, T0 + 2 * POLL + SNAPSHOT).size() == 2);
}

TEST("workshop damage: a piece that is gone is forgotten, not reported")
{
	Reporter reporter;
	Pieces   game{ { { GENERATOR, WRECKED }, { PURIFIER, WHOLE } } };
	reporter.Track(GENERATOR);
	reporter.Track(PURIFIER);
	CHECK(reporter.Poll(game, T0).size() == 2);
	game.pieces.erase(GENERATOR);  // scrapped: the workshop stream tells that
	CHECK(reporter.Poll(game, T0 + POLL).empty());
	CHECK(!reporter.Tracks(GENERATOR));
	CHECK(reporter.Size() == 1);
	reporter.Forget(PURIFIER);
	CHECK(reporter.Size() == 0);
}

TEST("workshop damage: a piece that went away damaged is remembered, so its copies hear of it when it is back whole")
{
	Reporter reporter;
	Pieces   game{ { { CROP, FLAGGED }, { PURIFIER, WHOLE } } };
	reporter.Track(CROP);
	reporter.Track(PURIFIER);
	CHECK(reporter.Poll(game, T0).size() == 2);
	game.pieces.erase(CROP);  // off for a moment
	game.pieces.erase(PURIFIER);
	CHECK(reporter.Poll(game, T0 + POLL).empty());
	CHECK(reporter.LostDamaged(CROP));
	CHECK(!reporter.LostDamaged(PURIFIER));  // it was whole: nothing a copy of it shows is stale
	// Back, and repaired meanwhile: tracked again, it is said once, whole.
	game.pieces[CROP] = WHOLE;
	CHECK(reporter.Track(CROP));
	CHECK(!reporter.LostDamaged(CROP));
	const auto said = reporter.Poll(game, T0 + 2 * POLL);
	REQUIRE(said.size() == 1);
	CHECK(said[0].refId == CROP);
	CHECK(Whole(StateOf(said[0])));
	// A piece we were told is gone for good is not remembered; nor is anything after a clear.
	game.pieces[CROP] = FLAGGED;
	CHECK(reporter.Poll(game, T0 + 3 * POLL).size() == 1);
	game.pieces.erase(CROP);
	CHECK(reporter.Poll(game, T0 + 4 * POLL).empty());
	CHECK(reporter.LostDamaged(CROP));
	reporter.Forget(CROP);
	CHECK(!reporter.LostDamaged(CROP));
	game.pieces[GENERATOR] = WRECKED;
	reporter.Track(GENERATOR);
	CHECK(reporter.Poll(game, T0 + 5 * POLL).size() == 1);
	game.pieces.erase(GENERATOR);
	CHECK(reporter.Poll(game, T0 + 6 * POLL).empty());
	reporter.Clear();
	CHECK(!reporter.LostDamaged(GENERATOR));
}

TEST("workshop damage: a cleared reporter starts over")
{
	Reporter reporter;
	Pieces   game{ { { GENERATOR, WRECKED } } };
	reporter.Track(GENERATOR);
	CHECK(reporter.Poll(game, T0).size() == 1);
	reporter.Clear();
	CHECK(reporter.Size() == 0);
	CHECK(reporter.Poll(game, T0 + 1ms).empty());  // nothing tracked; and the poll is due at once
	reporter.Track(GENERATOR);
	CHECK(reporter.Poll(game, T0 + 2ms + POLL).size() == 1);
}

TEST("workshop damage: the reporter keeps a bounded number of pieces")
{
	Reporter reporter;
	for (std::uint32_t i = 0; i < MAX_TRACKED; ++i) {
		CHECK(reporter.Track(0xFF100000 + i));
	}
	CHECK(!reporter.Track(0xFF200000));
	CHECK(reporter.Track(0xFF100000));  // one it has
	CHECK(reporter.Size() == MAX_TRACKED);
}

TEST("workshop damage: entries go out a message's worth at a time")
{
	std::vector<Entry> entries;
	for (std::uint32_t i = 0; i < Protocol::MAX_DAMAGE_ENTRIES * 2 + 3; ++i) {
		entries.push_back({ 0xFF100000 + i, 1.0f, false });
	}
	const auto chunks = Chunks(entries);
	REQUIRE(chunks.size() == 3);
	CHECK(chunks[0].size() == Protocol::MAX_DAMAGE_ENTRIES);
	CHECK(chunks[1].size() == Protocol::MAX_DAMAGE_ENTRIES);
	CHECK(chunks[2].size() == 3);
	CHECK(chunks[2].back().refId == entries.back().refId);
	CHECK(Chunks({}).empty());
	CHECK(Chunks(entries, 0).size() == entries.size());  // never an endless loop
	// Each chunk is a message that decodes.
	for (const auto& chunk : chunks) {
		const auto packet = Protocol::Encode(Protocol::PieceDamages{ 0, chunk }, Protocol::MessageType::kReportWorkshopDamage);
		CHECK(Protocol::DecodePieceDamages(packet).has_value());
	}
}

TEST("workshop damage: a copy is wrecked to its builder's, mended to it, or mended first when it is further gone")
{
	CHECK(PlanFor(WHOLE, WHOLE) == Step::kNothing);
	CHECK(PlanFor(WRECKED, WRECKED) == Step::kNothing);
	CHECK(PlanFor(WHOLE, HALF) == Step::kLower);
	CHECK(PlanFor(WHOLE, WRECKED) == Step::kLower);
	CHECK(PlanFor(HALF, WRECKED) == Step::kLower);
	CHECK(PlanFor(WHOLE, FLAGGED) == Step::kLower);
	CHECK(PlanFor(HALF, WHOLE) == Step::kMend);
	CHECK(PlanFor(WRECKED, WHOLE) == Step::kMend);
	CHECK(PlanFor(FLAGGED, WHOLE) == Step::kMend);
	// Our own game hit the copy harder than the builder's game hit the piece: the engine can't raise a health.
	CHECK(PlanFor(WRECKED, HALF) == Step::kMendThenLower);
	CHECK(PlanFor(State{ 20.0f, false }, HALF) == Step::kMendThenLower);
	CHECK(PlanFor(State{ 50.0f, true }, HALF) == Step::kMendThenLower);
	CHECK(PlanFor(HALF, FLAGGED) == Step::kMendThenLower);  // a health where the builder's has none
	CHECK(std::string_view{ Name(Step::kLower) } == "wrecked");
	CHECK(std::string_view{ Name(Step::kMend) } == "mended");
}

TEST("workshop damage: the mirror wrecks a copy to what the host's game says, and mends it when the host's is repaired")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WHOLE } } };
	CHECK(mirror.Set(HOST, EntryOf(GENERATOR, WRECKED)));
	CHECK(mirror.Refresh(game, T0) == 1);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WRECKED));
	CHECK(mirror.Lowered() == 1);
	CHECK(mirror.Refresh(game, T0 + REFRESH) == 0);  // it shows it now
	CHECK(mirror.Set(HOST, EntryOf(GENERATOR, WHOLE)));
	CHECK(mirror.Refresh(game, T0 + 2 * REFRESH) == 1);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WHOLE));
	CHECK(mirror.Mended() == 1);
	REQUIRE(game.shows.size() == 2);
	CHECK(game.shows[0].second == Step::kLower);
	CHECK(game.shows[1].second == Step::kMend);
}

TEST("workshop damage: a copy our own game damaged is put back to its builder's")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WHOLE }, { Key{ HOST, PURIFIER }, HALF } } };
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	mirror.Set(HOST, EntryOf(PURIFIER, HALF));
	CHECK(mirror.Refresh(game, T0) == 0);
	// A raider of this game's own, a stray shot of this player's: never the host's say.
	game.copies[Key{ HOST, GENERATOR }] = WRECKED;
	game.copies[Key{ HOST, PURIFIER }] = WRECKED;
	CHECK(mirror.Refresh(game, T0 + REFRESH) == 2);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WHOLE));
	CHECK((game.copies[Key{ HOST, PURIFIER }] == HALF));
	CHECK(mirror.Mended() == 2);  // a mend, with or without the wreck after
	CHECK(mirror.Lowered() == 0);
}

TEST("workshop damage: the mirror looks twice a second, and a copy that isn't there costs nothing and is shown when it comes")
{
	Mirror mirror;
	Copies game;
	mirror.Set(HOST, EntryOf(GENERATOR, WRECKED));
	CHECK(mirror.Refresh(game, T0) == 0);
	CHECK(game.shows.empty());
	// The friend arrives after the damage: the copy is made, and the next look wrecks it.
	game.copies[Key{ HOST, GENERATOR }] = WHOLE;
	CHECK(mirror.Refresh(game, T0 + 100ms) == 0);  // between looks
	CHECK(mirror.Refresh(game, T0 + REFRESH) == 1);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WRECKED));
}

TEST("workshop damage: each builder's pieces are their own")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WHOLE }, { Key{ OTHER, GENERATOR }, WHOLE } } };
	mirror.Set(HOST, EntryOf(GENERATOR, WRECKED));
	CHECK(mirror.Refresh(game, T0) == 1);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WRECKED));
	CHECK((game.copies[Key{ OTHER, GENERATOR }] == WHOLE));
	CHECK(mirror.Wanted(Key{ HOST, GENERATOR }).has_value());
	CHECK(!mirror.Wanted(Key{ OTHER, GENERATOR }).has_value());
}

TEST("workshop damage: a show the game couldn't do is counted and tried again")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WHOLE } } };
	game.refuses = true;
	mirror.Set(HOST, EntryOf(GENERATOR, WRECKED));
	CHECK(mirror.Refresh(game, T0) == 0);
	CHECK(mirror.Failed() == 1);
	game.refuses = false;
	CHECK(mirror.Refresh(game, T0 + REFRESH) == 1);
	CHECK(mirror.Lowered() == 1);
}

TEST("workshop damage: a copy our game keeps wrecking is changed a few times a minute, not twice a second")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WRECKED } } };
	game.stubborn = true;
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	auto now = T0;
	for (int i = 0; i < 40; ++i) {
		mirror.Refresh(game, now);
		now += REFRESH;
	}
	CHECK(game.shows.size() == MAX_SHOWS);
	CHECK(mirror.Capped() == 1);  // once a window
	// The next window: again.
	now = T0 + SHOW_WINDOW;
	mirror.Refresh(game, now);
	CHECK(game.shows.size() == MAX_SHOWS + 1);
}

TEST("workshop damage: a new word from the builder lifts a copy's cap at once, the same word said again does not")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WRECKED } } };
	game.stubborn = true;  // this game's raiders keep wrecking the copy
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	auto now = T0;
	for (int i = 0; i < 20; ++i) {
		mirror.Refresh(game, now);
		now += REFRESH;
	}
	CHECK(game.shows.size() == MAX_SHOWS);
	CHECK(mirror.Capped() == 1);
	// The snapshot says the same again: still capped.
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	mirror.Refresh(game, now);
	now += REFRESH;
	CHECK(game.shows.size() == MAX_SHOWS);
	// The host's piece is wrecked (the copy shows that already), then repaired by the host: the copy is mended at the
	// next look, well inside the window.
	game.stubborn = false;
	mirror.Set(HOST, EntryOf(GENERATOR, WRECKED));
	CHECK(mirror.Refresh(game, now) == 0);
	now += REFRESH;
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	CHECK(now - T0 < SHOW_WINDOW);
	CHECK(mirror.Refresh(game, now) == 1);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WHOLE));
	CHECK(game.shows.size() == MAX_SHOWS + 1);
}

TEST("workshop damage: only a better word lifts the cap: falling health in an attack in both games keeps the copy capped")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, State{ 10.0f, false } } } };
	game.stubborn = true;  // this game's raiders keep it lower than the host's
	auto  now = T0;
	float health = 50.0f;
	for (int i = 0; i < 20; ++i) {
		mirror.Set(HOST, EntryOf(GENERATOR, State{ health, false }));  // the host's piece takes a hit every poll
		mirror.Refresh(game, now);
		health -= 1.0f;
		now += REFRESH;
	}
	CHECK(game.shows.size() == MAX_SHOWS);
	CHECK(mirror.Capped() == 1);
	// The host's piece repaired a little (health raised): shown at once.
	mirror.Set(HOST, EntryOf(GENERATOR, State{ health + 20.0f, false }));
	CHECK(mirror.Refresh(game, now) == 1);
	CHECK(game.shows.size() == MAX_SHOWS + 1);
}

TEST("workshop damage: a word is better when no longer destroyed, repaired, or its health raised past the slack")
{
	CHECK(Better(WRECKED, WHOLE));                            // repaired
	CHECK(Better(WRECKED, HALF));                             // no longer destroyed
	CHECK(Better(FLAGGED, WHOLE));                            // a crop repaired
	CHECK(Better(HALF, WHOLE));                               // the health gone: repaired
	CHECK(Better(HALF, State{ 60.0f, false }));               // raised
	CHECK(!Better(HALF, State{ 50.4f, false }));              // within the slack
	CHECK(!Better(HALF, State{ 40.0f, false }));              // lower
	CHECK(!Better(HALF, WRECKED));                            // destroyed
	CHECK(!Better(WHOLE, HALF));
	CHECK(!Better(WHOLE, WHOLE));
	CHECK(!Better(State{ 10.0f, true }, State{ 5.0f, true }));
	CHECK(Better(State{ 5.0f, true }, State{ 10.0f, true }));
}

TEST("workshop damage: a frame shows a few copies at most, and the rest go on at the next frames")
{
	Mirror mirror;
	Copies game;
	constexpr std::uint32_t COUNT = 10;
	for (std::uint32_t i = 0; i < COUNT; ++i) {
		game.copies[Key{ HOST, 0xFF200000 + i }] = WHOLE;
		mirror.Set(HOST, EntryOf(0xFF200000 + i, WRECKED));  // one explosion in the host's game
	}
	const auto frame = 16ms;
	auto       now = T0;
	CHECK(mirror.Refresh(game, now) == MAX_SHOWS_PER_FRAME);
	now += frame;
	CHECK(mirror.Refresh(game, now) == MAX_SHOWS_PER_FRAME);
	now += frame;
	CHECK(mirror.Refresh(game, now) == COUNT - 2 * MAX_SHOWS_PER_FRAME);
	now += frame;
	CHECK(mirror.Refresh(game, now) == 0);  // the look is done: the next is REFRESH after its last frame
	CHECK(mirror.Deferred() == 2);
	// Each copy shown once, none twice, none left out.
	REQUIRE(game.shows.size() == COUNT);
	std::set<Key> shown;
	for (const auto& show : game.shows) {
		shown.insert(show.first);
	}
	CHECK(shown.size() == COUNT);
	for (const auto& [key, state] : game.copies) {
		CHECK((state == WRECKED));
	}
	// A copy forgotten where a look stopped: the look goes on from the next one.
	for (std::uint32_t i = 0; i < COUNT; ++i) {
		mirror.Set(HOST, EntryOf(0xFF200000 + i, WHOLE));
	}
	now = T0 + 10 * REFRESH;
	CHECK(mirror.Refresh(game, now) == MAX_SHOWS_PER_FRAME);
	mirror.Forget(Key{ HOST, 0xFF200000 + static_cast<std::uint32_t>(MAX_SHOWS_PER_FRAME) });
	game.copies.erase(Key{ HOST, 0xFF200000 + static_cast<std::uint32_t>(MAX_SHOWS_PER_FRAME) });
	now += frame;
	CHECK(mirror.Refresh(game, now) == MAX_SHOWS_PER_FRAME);
	now += frame;
	CHECK(mirror.Refresh(game, now) == COUNT - 1 - 2 * MAX_SHOWS_PER_FRAME);
	for (const auto& [key, state] : game.copies) {
		CHECK((state == WHOLE));
	}
}

TEST("workshop damage: a copy its own damage took away comes back once mended, unless its builder's word takes it away too")
{
	CHECK(BringsBack(true, Step::kMend, false));
	CHECK(BringsBack(true, Step::kMendThenLower, false));
	CHECK(!BringsBack(true, Step::kMendThenLower, true));
	CHECK(!BringsBack(true, Step::kLower, false));    // not mended: it stays as it is
	CHECK(!BringsBack(false, Step::kMend, false));    // never switched on that wasn't taken away by its damage
	CHECK(!BringsBack(true, Step::kNothing, false));
}

TEST("workshop damage: copies made again after a load are shown afresh")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WRECKED } } };
	game.stubborn = true;
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	auto now = T0;
	for (int i = 0; i < 20; ++i) {
		mirror.Refresh(game, now);
		now += REFRESH;
	}
	CHECK(game.shows.size() == MAX_SHOWS);
	mirror.Invalidate();
	game.stubborn = false;
	CHECK(mirror.Refresh(game, now) == 1);
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WHOLE));
	CHECK(mirror.Size() == 1);  // what the host said is kept
}

TEST("workshop damage: a builder who left says nothing more, and a scrapped piece is forgotten")
{
	Mirror mirror;
	Copies game{ { { Key{ HOST, GENERATOR }, WHOLE }, { Key{ OTHER, CROP }, WHOLE } } };
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	mirror.Set(HOST, EntryOf(PURIFIER, WRECKED));
	mirror.Set(OTHER, EntryOf(CROP, FLAGGED));
	CHECK(mirror.Size() == 3);
	mirror.ForgetPlayer(HOST);
	CHECK(mirror.Size() == 1);
	game.copies[Key{ HOST, GENERATOR }] = WRECKED;  // stays as this game has it
	CHECK(mirror.Refresh(game, T0) == 1);           // the other builder's crop alone
	CHECK((game.copies[Key{ HOST, GENERATOR }] == WRECKED));
	CHECK((game.copies[Key{ OTHER, CROP }] == FLAGGED));
	mirror.Forget(Key{ OTHER, CROP });
	CHECK(mirror.Size() == 0);
	mirror.Set(HOST, EntryOf(GENERATOR, WHOLE));
	mirror.Clear();
	CHECK(mirror.Size() == 0);
}

TEST("workshop damage: the mirror keeps a bounded number of pieces")
{
	Mirror mirror;
	for (std::uint32_t i = 0; i < MAX_MIRRORED; ++i) {
		CHECK(mirror.Set(HOST, Entry{ 0xFF100000 + i, 1.0f, false }));
	}
	CHECK(!mirror.Set(HOST, Entry{ 0xFF300000, 1.0f, false }));
	CHECK(mirror.Dropped() == 1);
	CHECK(mirror.Set(HOST, Entry{ 0xFF100000, 2.0f, true }));  // one it has is still told
	REQUIRE(mirror.Wanted(Key{ HOST, 0xFF100000 }).has_value());
	CHECK(mirror.Wanted(Key{ HOST, 0xFF100000 })->destroyed);
	CHECK(mirror.Size() == MAX_MIRRORED);
}

TEST("workshop damage: Accept on a copy asks the host only when the copy is wrecked, nothing waits, and friends may edit")
{
	CHECK(OnAccept(false, false, true) == Press::kNotDamaged);  // the grab: refused as ever
	CHECK(OnAccept(false, true, true) == Press::kNotDamaged);
	CHECK(OnAccept(true, true, true) == Press::kPending);
	CHECK(OnAccept(true, true, false) == Press::kPending);
	CHECK(OnAccept(true, false, false) == Press::kOff);
	CHECK(OnAccept(true, false, true) == Press::kAsk);
}

TEST("workshop damage: every answer to a repair has a notice, and only 'not the host's' teaches the workbench")
{
	for (std::uint8_t why = 0; why <= static_cast<std::uint8_t>(Protocol::WorkshopAnswer::LAST_WHY); ++why) {
		const auto verdict = Judge(static_cast<Why>(why));
		CHECK(verdict.notice != nullptr);
		CHECK(verdict.benchNotOwned == (static_cast<Why>(why) == Why::kNotHostOwned));
	}
	CHECK(std::string_view{ Judge(Why::kApplied).notice } == "workshop.shared.repaired");
	CHECK(std::string_view{ Judge(Why::kNoMaterials).notice } == "workshop.shared.repair_no_materials");
	CHECK(std::string_view{ Judge(Why::kNotAllowed).notice } == OFF_NOTICE);
	CHECK(std::string_view{ Judge(Why::kNotDamaged).notice } == "workshop.shared.repair_not_damaged");
	CHECK(std::string_view{ Judge(Why::kNoSuchPiece).notice } == "workshop.shared.repair_no_piece");
	CHECK(std::string_view{ Judge(Why::kNoAnswer).notice } == REFUSED_NOTICE);
	CHECK(std::string_view{ Judge(Why::kHostBusy).notice } == REFUSED_NOTICE);
	CHECK(std::string_view{ Judge(Why::kBenchUnopened).notice } == "workshop.shared.repair_bench_unopened");  // the host must open it once
}

TEST("workshop damage: the host's game refuses a repair for the first thing wrong, and takes one with nothing wrong")
{
	Facts ok;
	ok.benchKnown = ok.hostOwned = ok.benchLoaded = ok.benchOpened = ok.editAllowed = true;
	ok.targetKnown = ok.targetAtBench = ok.targetRepairable = ok.targetLoaded = ok.damaged = true;
	CHECK(!Refusal(ok).has_value());

	const auto with = [&](auto a_change) {
		Facts facts = ok;
		a_change(facts);
		return Refusal(facts);
	};
	CHECK(with([](Facts& f) { f.benchKnown = false; }) == Why::kNotHostOwned);
	CHECK(with([](Facts& f) { f.hostOwned = false; }) == Why::kNotHostOwned);
	CHECK(with([](Facts& f) { f.benchLoaded = false; }) == Why::kBenchNotLoaded);
	CHECK(with([](Facts& f) { f.benchOpened = false; }) == Why::kBenchUnopened);
	CHECK(with([](Facts& f) { f.editAllowed = false; }) == Why::kNotAllowed);  // the host's bSharedWorkshopEdit off
	CHECK(with([](Facts& f) { f.otherBenchOpen = true; }) == Why::kHostBusy);
	CHECK(with([](Facts& f) { f.targetKnown = false; }) == Why::kNoSuchPiece);  // the piece is gone
	CHECK(with([](Facts& f) { f.targetAtBench = false; }) == Why::kNoSuchPiece);
	CHECK(with([](Facts& f) { f.targetRepairable = false; }) == Why::kNotWorkshopItem);
	CHECK(with([](Facts& f) { f.targetLoaded = false; }) == Why::kBenchNotLoaded);
	CHECK(with([](Facts& f) { f.targetHeld = true; }) == Why::kPieceBusy);
	CHECK(with([](Facts& f) { f.damaged = false; }) == Why::kNotDamaged);
	// The setting is asked before the piece: a friend who may not edit learns nothing of the host's pieces.
	CHECK(with([](Facts& f) { f.editAllowed = false; f.targetKnown = false; }) == Why::kNotAllowed);
	// Nothing known at all: not the host's.
	CHECK(Refusal(Facts{}) == Why::kNotHostOwned);
}

namespace
{
	// Components, their loose items, and junk.
	constexpr std::uint32_t STEEL = 0x0001A001;
	constexpr std::uint32_t GEAR = 0x0001A002;
	constexpr std::uint32_t COPPER = 0x0001A003;
	constexpr std::uint32_t LOOSE_STEEL = 0x0002B001;
	constexpr std::uint32_t LOOSE_GEAR = 0x0002B002;
	constexpr std::uint32_t LOOSE_COPPER = 0x0002B003;
	constexpr std::uint32_t FAN = 0x0002B010;       // 2 steel, 2 gears
	constexpr std::uint32_t TYPEWRITER = 0x0002B011; // 2 steel, 2 gears, 1 copper: more parts than a fan
	constexpr std::uint32_t FUSION_CORE = 0x0003C001;

	const std::map<std::uint32_t, std::uint32_t> LOOSE{ { STEEL, LOOSE_STEEL }, { GEAR, LOOSE_GEAR }, { COPPER, LOOSE_COPPER } };

	Held Loose(std::uint32_t a_item, std::uint32_t a_component, std::uint32_t a_count) { return { a_item, a_count, { { a_component, 1 } } }; }
	Held Fans(std::uint32_t a_count) { return { FAN, a_count, { { STEEL, 2 }, { GEAR, 2 } } }; }
	Held Typewriters(std::uint32_t a_count) { return { TYPEWRITER, a_count, { { STEEL, 2 }, { GEAR, 2 }, { COPPER, 1 } } }; }

	using Changes = std::vector<std::pair<std::uint32_t, std::int64_t>>;
}

TEST("workshop damage: a repair costs a tenth of each recipe line, cut to a whole number, one at least")
{
	CHECK(RepairCount(10, 0.1f) == 1);
	CHECK(RepairCount(30, 0.1f) == 3);
	CHECK(RepairCount(25, 0.1f) == 2);
	CHECK(RepairCount(3, 0.1f) == 1);
	CHECK(RepairCount(0, 0.1f) == 1);
	CHECK(RepairCount(8, 0.5f) == 4);
	CHECK(RepairCount(8, 0.0f) == 1);
	CHECK(RepairCount(8, -1.0f) == 1);
	CHECK(RepairCount(8, std::numeric_limits<float>::quiet_NaN()) == 1);
	CHECK(RepairCount(0xFFFFFFFFu, 4.0f) == 0xFFFFFFFFu);

	const auto cost = RepairCost({ { STEEL, 40 }, { GEAR, 4 }, { 0, 9 }, { STEEL, 10 } }, 0.1f);
	CHECK((cost == std::vector<Cost>{ { STEEL, 5 }, { GEAR, 1 } }));  // a form named twice is one line; no form, no line
	CHECK(RepairCost({}, 0.1f).empty());
}

TEST("workshop damage: the workbench pays in loose components first")
{
	const auto payment = Pay({ Loose(LOOSE_STEEL, STEEL, 10), Fans(3) }, { { STEEL, 3 } }, LOOSE);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { LOOSE_STEEL, -3 } }));
	CHECK(payment->lost == 0);
}

TEST("workshop damage: junk is broken up for what the loose components lack, and the parts left over are put back loose")
{
	// 3 gears wanted, 1 loose: one fan gives 2 more, and its 2 steel go back loose.
	const auto payment = Pay({ Loose(LOOSE_GEAR, GEAR, 1), Fans(3) }, { { GEAR, 3 } }, LOOSE);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { LOOSE_GEAR, -1 }, { FAN, -1 }, { LOOSE_STEEL, 2 } }));  // takes first, then what is put back

	// Two fans for 3 gears: 1 gear and 4 steel go back.
	const auto two = Pay({ Fans(2) }, { { GEAR, 3 } }, LOOSE);
	REQUIRE(two.has_value());
	CHECK((two->changes == Changes{ { FAN, -2 }, { LOOSE_STEEL, 4 }, { LOOSE_GEAR, 1 } }));
}

TEST("workshop damage: the parts of junk broken for one line pay the next")
{
	const auto payment = Pay({ Fans(1) }, { { GEAR, 1 }, { STEEL, 2 } }, LOOSE);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { FAN, -1 }, { LOOSE_GEAR, 1 } }));
}

TEST("workshop damage: the junk of fewest parts is broken first")
{
	const auto payment = Pay({ Typewriters(1), Fans(1) }, { { STEEL, 2 } }, LOOSE);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { FAN, -1 }, { LOOSE_GEAR, 2 } }));
}

TEST("workshop damage: a recipe that names an item takes the item")
{
	const auto payment = Pay({ { FUSION_CORE, 2, {} }, Loose(LOOSE_STEEL, STEEL, 5) }, { { FUSION_CORE, 1 }, { STEEL, 1 } }, LOOSE);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { LOOSE_STEEL, -1 }, { FUSION_CORE, -1 } }));
}

TEST("workshop damage: a workbench that can't pay all of it pays nothing")
{
	CHECK(!Pay({}, { { STEEL, 1 } }, LOOSE).has_value());
	CHECK(!Pay({ Loose(LOOSE_STEEL, STEEL, 2), Fans(1) }, { { STEEL, 5 } }, LOOSE).has_value());
	// The first line could be paid, the second not: nothing is taken for either.
	CHECK(!Pay({ Loose(LOOSE_STEEL, STEEL, 9) }, { { STEEL, 1 }, { COPPER, 1 } }, LOOSE).has_value());
	// A cost of nothing is paid with nothing.
	const auto free = Pay({ Fans(1) }, {}, LOOSE);
	REQUIRE(free.has_value());
	CHECK(free->changes.empty());
}

TEST("workshop damage: spare parts with no loose item are counted lost, never put back as something else")
{
	const std::map<std::uint32_t, std::uint32_t> noGear{ { STEEL, LOOSE_STEEL }, { GEAR, 0 } };
	const auto payment = Pay({ Typewriters(1) }, { { STEEL, 1 } }, noGear);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { TYPEWRITER, -1 }, { LOOSE_STEEL, 1 } }));
	CHECK(payment->lost == 3);  // 2 gears (no loose item) and 1 copper (not in the map)
}

TEST("workshop damage: a loose component taken and its spare put back is one change")
{
	// Steel loose and in a fan; the fan is broken for a gear and its steel joins the loose steel.
	const auto payment = Pay({ Loose(LOOSE_STEEL, STEEL, 1), Fans(1) }, { { STEEL, 1 }, { GEAR, 2 } }, LOOSE);
	REQUIRE(payment.has_value());
	CHECK((payment->changes == Changes{ { FAN, -1 }, { LOOSE_STEEL, 1 } }));  // -1 taken, +2 put back
}
