// VATS and the time scale in a session: the wire (kReportTimeScale, kTimeScales), the server's table
// (common/TimeScaleBook.h), each game's report and its friends' rows (src/game/VatsTime.h), the group's slow motion as
// the game's time multiplier is set for it (src/game/RealTimeVats.h), and a friend drawn through a slowed game of theirs
// (src/game/StateBuffer.h, src/game/EngineClock.h).
#include "Test.h"

#include "Protocol.h"
#include "TimeScaleBook.h"
#include "game/EngineClock.h"
#include "game/RealTimeVats.h"
#include "game/StateBuffer.h"
#include "game/VatsTime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using MT = Protocol::MessageType;

	std::vector<std::uint8_t> Packet(MT a_type, auto a_write)
	{
		Protocol::Writer w{ a_type };
		a_write(w);
		return w.Data();
	}
}

TEST("vats time: a time scale report and the table go over the wire and come back the same")
{
	const Protocol::TimeScale report{ 0, Protocol::kTimeVats | Protocol::kTimeSlowed, 0.04f };
	const auto                back = Protocol::DecodeTimeScale(Protocol::Encode(report));
	REQUIRE(back);
	CHECK(*back == report);
	CHECK(Protocol::Encode(report)[0] == static_cast<std::uint8_t>(MT::kReportTimeScale));

	const Protocol::TimeScales table{ 0.3f, { { 2, Protocol::kTimeVats | Protocol::kTimeSlowed, 0.3f }, { 3, Protocol::kTimeSlowed, 0.3f }, { 4, 0, 1.0f } } };
	const auto                 tableBack = Protocol::DecodeTimeScales(Protocol::Encode(table));
	REQUIRE(tableBack);
	CHECK(*tableBack == table);
	CHECK(Protocol::Encode(table)[0] == static_cast<std::uint8_t>(MT::kTimeScales));

	const auto off = Protocol::DecodeTimeScales(Protocol::Encode(Protocol::TimeScales{}));
	REQUIRE(off);
	CHECK(off->groupSpeed == 0.0f);
	CHECK(off->players.empty());
}

TEST("vats time: a report or a table that says nonsense is dropped")
{
	const auto report = [](std::uint8_t a_flags, float a_multiplier) {
		return Packet(MT::kReportTimeScale, [&](Protocol::Writer& w) {
			w.U32(0);
			w.U8(a_flags);
			w.F32(a_multiplier);
		});
	};
	CHECK(Protocol::DecodeTimeScale(report(Protocol::kTimeVats, 0.5f)));
	CHECK(!Protocol::DecodeTimeScale(report(4, 0.5f)));  // an unknown flag
	CHECK(!Protocol::DecodeTimeScale(report(0, std::numeric_limits<float>::quiet_NaN())));
	CHECK(!Protocol::DecodeTimeScale(report(0, std::numeric_limits<float>::infinity())));
	CHECK(!Protocol::DecodeTimeScale(report(0, -0.5f)));
	CHECK(!Protocol::DecodeTimeScale(report(0, Protocol::MAX_TIME_MULTIPLIER * 2.0f)));
	auto trailing = report(0, 1.0f);
	trailing.push_back(0);
	CHECK(!Protocol::DecodeTimeScale(trailing));

	const auto table = [](float a_speed, std::uint8_t a_count, std::uint32_t a_player) {
		return Packet(MT::kTimeScales, [&](Protocol::Writer& w) {
			w.F32(a_speed);
			w.U8(a_count);
			w.U32(a_player);
			w.U8(Protocol::kTimeVats);
			w.F32(0.3f);
		});
	};
	CHECK(Protocol::DecodeTimeScales(table(0.3f, 1, 2)));
	CHECK(Protocol::DecodeTimeScales(table(0.0f, 1, 2)));
	CHECK(!Protocol::DecodeTimeScales(table(0.01f, 1, 2)));  // all but a stop for a friend's own game
	CHECK(!Protocol::DecodeTimeScales(table(1.0f, 1, 2)));   // slows nothing: said as 0
	CHECK(!Protocol::DecodeTimeScales(table(-0.3f, 1, 2)));
	CHECK(!Protocol::DecodeTimeScales(table(std::numeric_limits<float>::quiet_NaN(), 1, 2)));
	CHECK(!Protocol::DecodeTimeScales(table(0.3f, 1, 0)));  // a row names a player
	CHECK(!Protocol::DecodeTimeScales(table(0.3f, 2, 2)));  // says two, holds one
	CHECK(!Protocol::DecodeTimeScales(table(0.3f, 0, 2)));  // says none, holds one
}

TEST("vats time: the host's group speed is off unless on and slowing, and never all but a stop")
{
	CHECK(TimeScaleBook::GroupSpeed(false, 0.3f) == 0.0f);
	CHECK(TimeScaleBook::GroupSpeed(true, 0.3f) == 0.3f);
	CHECK(TimeScaleBook::GroupSpeed(true, 1.0f) == 0.0f);
	CHECK(TimeScaleBook::GroupSpeed(true, 2.0f) == 0.0f);
	CHECK(TimeScaleBook::GroupSpeed(true, 0.001f) == Protocol::MIN_GROUP_VATS_SPEED);
	CHECK(TimeScaleBook::GroupSpeed(true, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
	// Whatever the book gives decodes.
	for (const float speed : { 0.0f, 0.001f, 0.05f, 0.3f, 0.99f, 1.0f, 5.0f }) {
		CHECK(Protocol::DecodeTimeScales(Protocol::Encode(Protocol::TimeScales{ TimeScaleBook::GroupSpeed(true, speed), {} })));
	}
}

TEST("vats time: the server's table changes only with a new word, and a player who leaves is gone from it")
{
	TimeScaleBook::Book book;
	CHECK(book.Report(2, Protocol::kTimeVats | Protocol::kTimeSlowed, 0.04f));
	CHECK(!book.Report(2, Protocol::kTimeVats | Protocol::kTimeSlowed, 0.04f));  // the same again: nobody is told
	CHECK(book.Report(3, 0, 1.0f));
	CHECK(book.InVats() == 1);
	CHECK(book.Report(2, 0, 1.0f));
	CHECK(book.InVats() == 0);
	auto table = book.Table(0.3f);
	CHECK(table.groupSpeed == 0.3f);
	REQUIRE(table.players.size() == 2);
	CHECK(table.players[0].playerId == 2);
	CHECK(table.players[1].playerId == 3);
	CHECK(book.Leave(2));
	CHECK(!book.Leave(2));
	table = book.Table(0.0f);
	REQUIRE(table.players.size() == 1);
	CHECK(table.players[0].playerId == 3);
	// Full: nobody more is kept (a session has far fewer players).
	for (std::uint32_t id = 100; id < 100 + Protocol::MAX_TIME_SCALES; ++id) {
		(void)book.Report(id, 0, 1.0f);
	}
	CHECK(book.Table(0.0f).players.size() == Protocol::MAX_TIME_SCALES);
	CHECK(Protocol::DecodeTimeScales(Protocol::Encode(book.Table(0.3f))));
}

TEST("vats time: our report goes at once when VATS comes up or goes down, a moving multiplier ten times a second at most")
{
	using VatsTime::Clock;
	VatsTime::Reporter reporter;
	Clock::time_point  t{ 1s };

	CHECK(!reporter.Frame(t, false, { false, 1.0f }));  // not in a session: nothing
	auto first = reporter.Frame(t, true, { false, 1.0f });  // welcomed: said once
	REQUIRE(first);
	CHECK(first->flags == 0);
	CHECK(first->multiplier == 1.0f);
	CHECK(!reporter.Frame(t + 16ms, true, { false, 1.0f }));

	// VATS up (real time keeps the game at full speed): at once, the same frame.
	auto up = reporter.Frame(t + 20ms, true, { true, 1.0f });
	REQUIRE(up);
	CHECK(up->flags == Protocol::kTimeVats);
	// Down again a frame later: at once too.
	auto down = reporter.Frame(t + 36ms, true, { false, 1.0f });
	REQUIRE(down);
	CHECK(down->flags == 0);

	// Jet: slowed is a flag, so at once.
	t += 1s;
	auto jet = reporter.Frame(t, true, { false, 0.3f });
	REQUIRE(jet);
	CHECK(jet->flags == Protocol::kTimeSlowed);
	// Small moves are not said; a bigger one waits for the gap.
	CHECK(!reporter.Frame(t + 16ms, true, { false, 0.31f }));
	CHECK(!reporter.Frame(t + 32ms, true, { false, 0.5f }));
	auto paced = reporter.Frame(t + VatsTime::MIN_GAP, true, { false, 0.5f });
	REQUIRE(paced);
	CHECK(paced->multiplier == 0.5f);
	CHECK(paced->flags == Protocol::kTimeSlowed);

	// Nonsense from the game goes out as something the server takes.
	auto odd = reporter.Frame(t + 1s, true, { false, std::numeric_limits<float>::quiet_NaN() });
	REQUIRE(odd);
	CHECK(Protocol::DecodeTimeScale(Protocol::Encode(*odd)));

	// Out of the session and welcomed again: said again, though nothing changed.
	CHECK(!reporter.Frame(t + 2s, false, { false, 1.0f }));
	CHECK(reporter.Frame(t + 2s + 16ms, true, { false, 1.0f }));
}

TEST("vats time: friends' rows leave our own out, and say who came into or out of VATS and who left")
{
	VatsTime::Friends friends;
	auto              changes = friends.Set({ 0.3f, { { 1, 0, 1.0f }, { 2, Protocol::kTimeVats | Protocol::kTimeSlowed, 0.3f } } }, 1);
	CHECK(friends.GroupSpeed() == 0.3f);
	REQUIRE(friends.Rows().size() == 1);  // our own (player 1) is ours to know
	CHECK(friends.InVats() == 1);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].playerId == 2);
	CHECK(changes[0].vats);
	CHECK(!changes[0].wasVats);

	changes = friends.Set({ 0.3f, { { 2, Protocol::kTimeVats | Protocol::kTimeSlowed, 0.3f } } }, 1);
	CHECK(changes.empty());  // nothing new

	changes = friends.Set({ 0.3f, { { 2, 0, 1.0f }, { 3, 0, 1.0f } } }, 1);
	CHECK(friends.InVats() == 0);
	REQUIRE(changes.size() == 2);
	CHECK(changes[0].playerId == 2);
	CHECK(!changes[0].vats);
	CHECK(changes[0].wasVats);

	changes = friends.Set({ 0.0f, { { 3, 0, 1.0f } } }, 1);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].playerId == 2);
	CHECK(changes[0].left);
	CHECK(friends.GroupSpeed() == 0.0f);

	friends.Clear();
	CHECK(friends.Rows().empty());
}

TEST("vats time: the group's slow motion is on while the host has it and anyone is in VATS, us at once")
{
	CHECK(VatsTime::Group(0.0f, true, 1) == 0.0f);   // the host has it off
	CHECK(VatsTime::Group(0.3f, false, 0) == 0.0f);  // nobody in VATS
	CHECK(VatsTime::Group(0.3f, true, 0) == 0.3f);   // us: before the server's table says so
	CHECK(VatsTime::Group(0.3f, false, 2) == 0.3f);  // a friend
	CHECK(VatsTime::Group(1.0f, true, 0) == 0.0f);   // slows nothing
}

TEST("vats time: in the group's slow motion every game runs at its speed, and is set back to full speed after")
{
	constexpr float     GROUP = 0.3f;
	RealTimeVats::Clock clock;
	// A friend is in VATS: our game, walking about at full speed, slows to the group's speed, each frame it is not there.
	CHECK(clock.Frame(true, true, false, 1.0f, 0.0f, GROUP) == GROUP);
	CHECK(clock.Grouped());
	CHECK(clock.Frame(true, true, false, GROUP, 0.0f, GROUP) < 0.0f);  // there already
	// We come into VATS too: the game's own 0.04 is set to the group's, not to real time.
	CHECK(clock.Frame(true, true, true, 0.04f, 0.04f, GROUP) == GROUP);
	CHECK(clock.Frame(true, true, true, GROUP, 0.04f, GROUP) < 0.0f);
	// Out of VATS while the friend still is: the group's speed holds.
	CHECK(clock.Frame(true, true, false, GROUP, 0.0f, GROUP) < 0.0f);
	// The friend is out: full speed again, once.
	CHECK(clock.Frame(true, true, false, GROUP, 0.0f, 0.0f) == 1.0f);
	CHECK(!clock.Grouped());
	CHECK(clock.Frame(true, true, false, 1.0f, 0.0f, 0.0f) < 0.0f);

	// On Jet, slower than the group already: left as it is, and Jet is not ended when the group's slow motion is.
	CHECK(clock.Frame(true, true, false, 0.2f, 0.2f, GROUP) < 0.0f);
	CHECK(clock.Frame(true, true, false, 0.2f, 0.2f, 0.0f) < 0.0f);
	// Jet slower than nothing the group asks: a gentle Jet (0.5) is slowed to the group's.
	CHECK(clock.Frame(true, true, false, 0.5f, 0.5f, GROUP) == GROUP);

	// The group ends while we are in VATS: real time VATS takes it from there (bRealTimeVats) ...
	RealTimeVats::Clock inVats;
	CHECK(inVats.Frame(true, true, true, 0.04f, 0.04f, GROUP) == GROUP);
	CHECK(inVats.Frame(true, true, true, 0.04f, 0.04f, 0.0f) == 1.0f);
	// ... or the game's own slow VATS stays, with real time off.
	RealTimeVats::Clock slowVats;
	CHECK(slowVats.Frame(true, false, true, 0.04f, 0.04f, GROUP) == GROUP);
	CHECK(slowVats.Frame(true, false, true, 0.04f, 0.04f, 0.0f) < 0.0f);

	// The session ends in it: set back to full speed.
	RealTimeVats::Clock leaving;
	CHECK(leaving.Frame(true, true, false, 1.0f, 0.0f, GROUP) == GROUP);
	CHECK(leaving.Frame(false, true, false, GROUP, 0.0f, GROUP) == 1.0f);
	// Alone, a group speed is nothing.
	RealTimeVats::Clock alone;
	CHECK(alone.Frame(false, true, false, 1.0f, 0.0f, GROUP) < 0.0f);

	// Without it, real time VATS is as it was.
	RealTimeVats::Clock plain;
	CHECK(plain.Frame(true, true, true, 0.04f, 0.04f) == 1.0f);
	CHECK(plain.Frame(true, true, false, 1.0f, 0.0f) < 0.0f);
}

// A friend whose game slows to VATS's 0.04 for five seconds (bRealTimeVats off, or Jet) and then runs at full speed
// again, drawn by ours: their states are stamped on their engine's real frame time (EngineClock::FrameSeconds), so on
// our side they keep to the wall clock. The friend is drawn where they really were a short delay ago all through, the
// buffer never holds more than its limit, the delay never grows, and nothing jumps when their time scale changes.
TEST("vats time: a friend whose game slows and speeds up again is drawn where they were, with no buffer growing and no jump")
{
	using StateBuffer::Clock;
	constexpr float RUN = 300.0f;            // units a second of their game's time
	constexpr auto  FRAME = 16667us;         // both games at 60 fps
	constexpr auto  SEND = 33ms;             // the sender's schedule
	constexpr auto  TRIP = 50ms;
	const auto      start = Clock::time_point{ 10s };
	const auto      multiplier = [&](Clock::time_point a_at) { return a_at >= start + 2s && a_at < start + 7s ? 0.04f : 1.0f; };

	struct Sent
	{
		Clock::time_point at;
		float             x;
		Protocol::PlayerState state;
	};
	std::vector<Sent> sent;  // where they really were (by their stamps) and what went out
	EngineClock::Frames senderClock;
	float               x = 0.0f;
	std::uint32_t       sequence = 0;
	std::uint64_t       mark = 0;
	auto                nextSend = start;
	for (auto t = start; t < start + 10s; t += FRAME) {
		const float real = std::chrono::duration<float>(FRAME).count();
		const float mult = multiplier(t);
		x += RUN * mult * real;  // the engine moves them by the slowed frame time
		const auto stamp = senderClock.Now(t, ++mark, EngineClock::FrameSeconds(real * mult, real));
		if (t >= nextSend) {
			nextSend += SEND;
			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.worldspace = 0x3C;
			state.x = x;
			state.speed = RUN * mult;
			state.sentMs = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(stamp.time_since_epoch()).count());
			sent.push_back({ stamp, x, state });
		}
	}

	// Where they really were at a_at (on their clock, which keeps to ours), between the states.
	const auto truth = [&](Clock::time_point a_at) {
		for (std::size_t i = 1; i < sent.size(); ++i) {
			if (sent[i].at >= a_at) {
				const float span = std::chrono::duration<float>(sent[i].at - sent[i - 1].at).count();
				const float k = span > 0.0f ? std::chrono::duration<float>(a_at - sent[i - 1].at).count() / span : 1.0f;
				return sent[i - 1].x + (sent[i].x - sent[i - 1].x) * std::clamp(k, 0.0f, 1.0f);
			}
		}
		return sent.back().x;
	};

	StateBuffer::Buffer buffer;
	std::size_t         next = 0;
	std::size_t         most = 0;
	float               worst = 0.0f;
	float               maxDelay = 0.0f;
	float               maxStep = 0.0f;
	std::optional<float> lastX;
	for (auto now = start + TRIP; now < start + 10s; now += FRAME) {
		while (next < sent.size() && sent[next].at + TRIP <= now) {
			buffer.Push(sent[next].state, sent[next].at + TRIP);
			++next;
		}
		buffer.Frame(now);
		most = (std::max)(most, buffer.snapshots.size());
		const auto drawn = buffer.Sample(now);
		if (!drawn || now < start + 1s) {
			continue;  // the first second: the delay settles
		}
		maxDelay = (std::max)(maxDelay, buffer.delay);
		// Their states are laid on our clock as far apart as theirs says, from the first one's arrival (SenderClock): a
		// steady trip behind when they were sampled.
		const auto drawnAt = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(buffer.delay)) - TRIP;
		worst = (std::max)(worst, std::abs(drawn->state.x - truth(drawnAt)));
		if (lastX) {
			maxStep = (std::max)(maxStep, std::abs(drawn->state.x - *lastX));
		}
		lastX = drawn->state.x;
	}
	CHECK(most <= StateBuffer::MAX_SNAPSHOTS);
	CHECK(maxDelay <= std::chrono::duration<float>(StateBuffer::MAX_DELAY).count() + 1e-4f);
	CHECK(maxDelay < 0.15f);  // their slow motion is no late state: the delay stays where the trip puts it
	if (worst >= 3.0f) {
		std::printf("    worst %.2f units, delay up to %.3f s, biggest step %.2f\n", worst, maxDelay, maxStep);
	}
	CHECK(worst < 3.0f);     // units: where they were, all through, the changes of speed included
	CHECK(maxStep < RUN * std::chrono::duration<float>(FRAME).count() * 1.2f);  // no frame steps further than they ran
}
