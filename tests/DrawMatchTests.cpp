#include "Test.h"

#include "game/DrawMatch.h"

#include <cstring>
#include <utility>

using namespace std::chrono_literals;
using DrawMatch::Do;
using DrawMatch::Next;
using DrawMatch::Track;

namespace
{
	const auto START = DrawMatch::Clock::time_point{} + 1h;
	const auto NONE = DrawMatch::Clock::time_point{};
}

TEST("DrawMatch: a stand-in that matches its friend is asked nothing")
{
	Track track;
	CHECK(Next(track, false, false, false, NONE, START) == Do::kNothing);
	CHECK(Next(track, true, true, false, NONE, START + 1s) == Do::kNothing);
	CHECK(track.asks == 0);
}

TEST("DrawMatch: a draw the stand-in never takes is asked a few times a second apart, then let be")
{
	Track track;
	int   draws = 0;
	for (int ms = 0; ms < 60000; ms += 16) {  // a minute of frames
		draws += Next(track, true, false, false, NONE, START + std::chrono::milliseconds(ms)) == Do::kDraw;
	}
	CHECK(draws == DrawMatch::MAX_TRIES);
	CHECK(track.gaveUp);
	CHECK(track.giveUps == 1);
	// Asks a second apart.
	Track spaced;
	CHECK(Next(spaced, true, false, false, NONE, START) == Do::kDraw);
	CHECK(Next(spaced, true, false, false, NONE, START + 500ms) == Do::kNothing);
	CHECK(Next(spaced, true, false, false, NONE, START + 1s) == Do::kDraw);
}

TEST("DrawMatch: a stand-in put away again after each draw stops being drawn")
{
	Track track;
	int   draws = 0;
	auto  now = START;
	for (int i = 0; i < 30; ++i) {
		draws += Next(track, true, false, false, NONE, now) == Do::kDraw;
		now += 500ms;
		CHECK(Next(track, true, true, false, NONE, now) == Do::kNothing);  // the draw took, for a moment
		now += 600ms;
	}
	CHECK(draws == DrawMatch::MAX_TRIES);
	CHECK(track.giveUps == 1);
}

TEST("DrawMatch: the friend drawing or holstering again, or a gear change, gives the asks back")
{
	Track track;
	auto  now = START;
	for (int i = 0; i < 10; ++i, now += 1s) {
		(void)Next(track, true, false, false, NONE, now);
	}
	CHECK(track.gaveUp);
	// They holster: the stand-in's weapon is away already, nothing to ask; then they draw again.
	CHECK(Next(track, false, false, false, NONE, now) == Do::kNothing);
	CHECK(!track.gaveUp);
	now += 1s;
	CHECK(Next(track, true, false, false, NONE, now) == Do::kDraw);
	for (int i = 0; i < 10; ++i) {
		now += 1s;
		(void)Next(track, true, false, false, NONE, now);
	}
	CHECK(track.gaveUp);
	// Its gear changed: a redraw, which holsters first when drawn, and draws when not.
	now += 1s;
	CHECK(Next(track, true, false, true, NONE, now) == Do::kDraw);
	now += 1s;
	CHECK(Next(track, true, true, true, NONE, now) == Do::kHolster);
}

TEST("DrawMatch: no ask before the caller's wait is over (an equip or a get-up playing)")
{
	Track track;
	CHECK(Next(track, true, false, true, START + 1s, START) == Do::kNothing);
	CHECK(Next(track, true, false, true, START + 1s, START + 1s) == Do::kDraw);
}

TEST("DrawMatch: a holster the stand-in never takes is still asked now and then, a draw is not")
{
	Track track;
	auto  now = START;
	int   holsters = 0;
	for (int i = 0; i < 5; ++i, now += 1s) {
		holsters += Next(track, false, true, false, NONE, now) == Do::kHolster;
	}
	CHECK(holsters == DrawMatch::MAX_TRIES);
	CHECK(track.gaveUp);
	// A minute of frames: one ask every SLOW_HOLSTER, not one a second.
	holsters = 0;
	for (int ms = 0; ms < 60000; ms += 16) {
		holsters += Next(track, false, true, false, NONE, now + std::chrono::milliseconds(ms)) == Do::kHolster;
	}
	CHECK(holsters >= 5 && holsters <= 7);
	// A draw it never takes stays let be.
	Track draw;
	for (int i = 0; i < 10; ++i, now += 1s) {
		(void)Next(draw, true, false, false, NONE, now);
	}
	CHECK(draw.gaveUp);
	for (int i = 0; i < 100; ++i, now += 1s) {
		CHECK(Next(draw, true, false, false, NONE, now) == Do::kNothing);
	}
}

TEST("DrawMatch: a state that held a while gives the next mismatch its asks back")
{
	Track track;
	auto  now = START;
	for (int i = 0; i < 3; ++i, now += 1s) {
		CHECK(Next(track, true, false, false, NONE, now) == Do::kDraw);
	}
	CHECK(Next(track, true, true, false, NONE, now) == Do::kNothing);
	now += DrawMatch::STABLE;
	CHECK(Next(track, true, true, false, NONE, now) == Do::kNothing);
	CHECK(track.tries == 0);
	// Put away by something (sitting down, say): drawn again, with every ask.
	int draws = 0;
	for (int i = 0; i < 10; ++i) {
		now += 1s;
		draws += Next(track, true, false, false, NONE, now) == Do::kDraw;
	}
	CHECK(draws == DrawMatch::MAX_TRIES);
}

using DrawMatch::State;

namespace
{
	// The engine's actor state word as CommonLib declares it (ActorState at 0C): a bit, then the weapon state in three
	// bits of a signed enum type.
	enum class WeaponStateField : std::int32_t
	{
		kSheathed,
		kWantToDraw,
		kDrawing,
		kDrawn,
		kWantToSheathe,
		kSheathing,
	};

	struct StateWord
	{
		std::uint32_t    reanimating: 1;
		WeaponStateField weaponState: 3;
		std::uint32_t    rest: 28;
	};
	static_assert(sizeof(StateWord) == 4);

	// The field read as the plugin reads it, with the engine's value in its bits.
	std::int32_t ReadField(std::uint32_t a_engineValue)
	{
		const std::uint32_t word = 1u | (a_engineValue << 1) | (0x5u << 4);
		StateWord           out{};
		std::memcpy(&out, &word, sizeof(out));
		return std::to_underlying(out.weaponState);
	}
}

TEST("DrawMatch: the weapon state is read as the engine keeps it, however the compiler reads the bit-field")
{
	for (std::uint32_t value = 0; value <= 5; ++value) {
		CHECK(DrawMatch::StateOf(ReadField(value)) == static_cast<State>(value));
	}
	// Read signed, the holster's two states come back negative (the way MSVC reads them): the same states.
	CHECK(DrawMatch::StateOf(-4) == State::kWantToSheathe);
	CHECK(DrawMatch::StateOf(-3) == State::kSheathing);
	CHECK(DrawMatch::Drawn(DrawMatch::StateOf(ReadField(4))));
	CHECK(DrawMatch::Drawn(DrawMatch::StateOf(ReadField(5))));
	// The two values the engine has no name for count as drawn.
	CHECK(DrawMatch::StateOf(6) == State::kDrawn);
	CHECK(DrawMatch::StateOf(-1) == State::kDrawn);
}

TEST("DrawMatch: a weapon state on its way counts as the engine counts it, and starts where the weapon still is")
{
	CHECK(!DrawMatch::Drawn(State::kSheathed));
	CHECK(!DrawMatch::Drawn(State::kWantToDraw));
	CHECK(!DrawMatch::Drawn(State::kDrawing));
	CHECK(DrawMatch::Drawn(State::kDrawn));
	CHECK(DrawMatch::Drawn(State::kWantToSheathe));
	CHECK(DrawMatch::Drawn(State::kSheathing));
	CHECK(DrawMatch::From(State::kWantToDraw) == State::kSheathed);
	CHECK(DrawMatch::From(State::kDrawing) == State::kSheathed);
	CHECK(DrawMatch::From(State::kWantToSheathe) == State::kDrawn);
	CHECK(DrawMatch::From(State::kSheathing) == State::kDrawn);
	CHECK(DrawMatch::From(State::kDrawn) == State::kDrawn);
	CHECK(DrawMatch::From(State::kSheathed) == State::kSheathed);
}

TEST("DrawMatch: a holster on its way is let finish, and asked once")
{
	Track track;
	CHECK(Next(track, false, State::kDrawn, false, NONE, START) == Do::kHolster);
	int asks = 0;
	for (int ms = 16; ms < 2000; ms += 16) {
		const auto state = ms < 600 ? State::kWantToSheathe : ms < 1300 ? State::kSheathing : State::kSheathed;
		asks += Next(track, false, state, false, NONE, START + std::chrono::milliseconds(ms)) != Do::kNothing;
	}
	CHECK(asks == 0);
	CHECK(track.asks == 1);
	CHECK(track.restarts == 0);
}

TEST("DrawMatch: a holster that stalls half-way is set back and asked again, not asked on top of itself")
{
	Track track;
	CHECK(Next(track, false, State::kDrawn, false, NONE, START) == Do::kHolster);
	// The graph never takes it: the engine wants to sheathe, the gun still in the hand. Set back and asked again (the
	// caller), it lands in the same state at once.
	int  holsters = 0;
	int  restarts = 0;
	auto last = START;
	bool spaced = true;
	for (int ms = 16; ms < 60000; ms += 16) {
		const auto now = START + std::chrono::milliseconds(ms);
		const auto what = Next(track, false, State::kWantToSheathe, false, NONE, now);
		holsters += what == Do::kHolster;
		if (what == Do::kRestart) {
			++restarts;
			spaced = spaced && now - last >= DrawMatch::STUCK;
			last = now;
		}
	}
	CHECK(holsters == 0);
	// Three more within the tries (STUCK apart), then one each SLOW_HOLSTER: never let be with the gun out.
	CHECK(restarts >= 6 && restarts <= 8);
	CHECK(spaced);
	CHECK(track.restarts == static_cast<std::uint32_t>(restarts));
	CHECK(track.gaveUp);
}

TEST("DrawMatch: a holster set back and asked again that the graph takes then is left alone")
{
	Track track;
	auto  now = START;
	CHECK(Next(track, false, State::kDrawn, false, NONE, now) == Do::kHolster);
	now += 16ms;
	while (Next(track, false, State::kWantToSheathe, false, NONE, now) != Do::kRestart) {
		now += 16ms;
		REQUIRE(now < START + 10s);
	}
	CHECK(now - START >= DrawMatch::STUCK);
	for (int i = 0; i < 1000; ++i) {
		now += 16ms;
		const auto state = i < 40 ? State::kSheathing : State::kSheathed;
		CHECK(Next(track, false, state, false, NONE, now) == Do::kNothing);
	}
	CHECK(track.restarts == 1);
}

TEST("DrawMatch: a draw that stalls half-way is set back and asked again a few times, then let be")
{
	Track track;
	CHECK(Next(track, true, State::kSheathed, false, NONE, START) == Do::kDraw);
	int restarts = 0;
	for (int ms = 16; ms < 60000; ms += 16) {
		restarts += Next(track, true, State::kWantToDraw, false, NONE, START + std::chrono::milliseconds(ms)) == Do::kRestart;
	}
	CHECK(restarts == DrawMatch::MAX_TRIES - 1);
	CHECK(track.gaveUp);
}

TEST("DrawMatch: a weapon on its way out when the friend wants it away gets there, then is put away")
{
	Track track;
	// They drew and put it away again while the stand-in's draw played: nothing asked on top of the draw.
	CHECK(Next(track, false, State::kDrawing, false, NONE, START) == Do::kNothing);
	CHECK(Next(track, false, State::kDrawing, false, NONE, START + 1s) == Do::kNothing);
	CHECK(Next(track, false, State::kDrawn, false, NONE, START + 1s + 16ms) == Do::kHolster);
	CHECK(track.restarts == 0);
}

TEST("DrawMatch: a redraw's holster that stalls half-way is set back to drawn, where the gun and the friend's are")
{
	Track track;
	auto  now = START;
	// Their gear changed: the stand-in holsters to draw again, and its graph never takes the holster.
	CHECK(Next(track, true, State::kDrawn, true, NONE, now) == Do::kHolster);
	int restarts = 0;
	for (int i = 0; i < 400; ++i) {
		now += 16ms;
		const auto what = Next(track, true, State::kWantToSheathe, false, NONE, now);
		CHECK(what != Do::kDraw);
		CHECK(what != Do::kHolster);
		if (what == Do::kRestart) {
			++restarts;
			CHECK(now - START >= DrawMatch::STUCK);
			break;
		}
	}
	CHECK(restarts == 1);
	CHECK(DrawMatch::From(State::kWantToSheathe) == State::kDrawn);
	// Set back, it is drawn as they are: nothing more.
	for (int i = 0; i < 1000; ++i) {
		now += 16ms;
		CHECK(Next(track, true, State::kDrawn, false, NONE, now) == Do::kNothing);
	}
}

TEST("DrawMatch: nothing is asked while a replayed throw plays, and a holster it stalled is set back after")
{
	Track track;
	CHECK(Next(track, false, State::kDrawn, false, NONE, START) == Do::kHolster);
	// The friend throws: the stand-in's throw plays (THROW_HOLD), and its holster, not taken, wants to sheathe meanwhile.
	const auto thrown = START + 100ms;
	const auto hold = thrown + DrawMatch::THROW_HOLD;
	for (auto now = thrown; now < hold; now += 16ms) {
		CHECK(Next(track, false, State::kWantToSheathe, false, hold, now) == Do::kNothing);
	}
	auto now = hold;
	while (Next(track, false, State::kWantToSheathe, false, hold, now) != Do::kRestart) {
		now += 16ms;
		REQUIRE(now < START + 10s);
	}
	CHECK(now - thrown >= DrawMatch::STUCK);
	CHECK(now - thrown < DrawMatch::STUCK + 100ms);
	// One held off by the throw is asked as soon as it has played.
	Track draw;
	CHECK(Next(draw, true, State::kSheathed, false, hold, thrown) == Do::kNothing);
	CHECK(Next(draw, true, State::kSheathed, false, hold, hold) == Do::kDraw);
}
