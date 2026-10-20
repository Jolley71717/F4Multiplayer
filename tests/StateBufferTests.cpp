#include "Test.h"

#include "Protocol.h"
#include "game/StateBuffer.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <vector>

// A friend's run as another game draws it: the sender's game samples its player on Session's schedule at its
// own frames, the server relays the newest state every tick in a PlayerStates batch (encoded and decoded as on
// the wire), the network delays each batch by its own amount, and the drawing game reads what has come once a
// frame and puts the stand-in where StateBuffer says.
namespace
{
	using namespace std::chrono_literals;
	using StateBuffer::Clock;
	using Micro = std::chrono::microseconds;

	constexpr std::uint32_t FRIEND_ID = 1;
	constexpr float         RUN = 400.0f;  // units a second, along x

	struct Setup
	{
		float           fps = 60.0f;          // the drawing game's
		float           frameJitter = 0.1f;   // each of its frames is up to this fraction longer or shorter
		float           senderFps = 60.0f;
		float           senderFrameJitter = 0.05f;
		bool            sendsTime = true;     // states say when they were sampled
		Micro           trip = 30ms;          // server to the drawing game
		Micro           jitter = 20ms;        // plus up to this much more, at random per batch
		Clock::duration sendEvery = 33ms;     // the sender's schedule (Session's SEND_INTERVAL); a host's game that sends less often is a longer one
		float           lateChance = 0.0f;    // a batch is held up by `lateBy` more this often, at random,
		Clock::duration lateEvery{};          // or the first batch of every `lateEvery`
		Micro           lateBy{};
		bool            queued = false;       // batches come in the order they went: one held up holds up those behind it, which then come together
		float           dropChance = 0.0f;    // a batch is lost on the way this often, at random: the next brings a state a gap later
		Clock::duration senderClock{};        // the sender's steady clock is this far ahead of ours
		// The sender's work (Session sampling its player) runs up to this long into its frame, at random, after the engine
		// moved the player for that frame; its state is stamped with that moment, or with the frame's (FrameClock).
		Micro           senderWork{};
		bool            senderStampsFrame = true;
		Clock::duration length = 20s;
		unsigned        seed = 1;
		Clock::duration stallAt{};  // the drawing game stops for `stall` here
		Clock::duration stall{};
		Clock::duration pauseAt{};  // the sender sends nothing for `pause` here (a loading screen)
		Clock::duration pause{};
		Clock::duration firstLate{};  // the drawing game hitches this long handling the first state (a slow welcome)
		// Our frame's work (reading what has come, sampling the stand-in) runs apart from the engine's update of the
		// stand-in, which draws it halfway through each frame: our work comes early in a frame or late, at random, and
		// in `skipWork` of frames not at all (the window message went to the next frame). Otherwise both are at the
		// frame's start.
		bool  workApart = false;
		float skipWork = 0.1f;
		bool  byPath = true;  // the update puts it where the sampled path is at its moment (Puppets); false: where our work sampled it
	};

	struct Drawn
	{
		Clock::time_point at;
		float             seconds = 0.0f;  // since the frame before
		float             x = 0.0f;
		bool              held = false;
		float             delay = 0.0f;
		float             behind = 0.0f;  // seconds behind where the friend really is
	};

	struct Packet
	{
		Clock::time_point         arrives;
		std::vector<std::uint8_t> bytes;
	};

	const auto start = Clock::time_point{} + 1h;

	// Every frame drawn after the first two seconds.
	std::vector<Drawn> Play(const Setup& a_setup)
	{
		std::mt19937                          random{ a_setup.seed };
		std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
		const auto                            end = start + a_setup.length;
		const auto frameOf = [&](float a_fps, float a_jitter) {
			const float seconds = (1.0f + a_jitter * (2.0f * unit(random) - 1.0f)) / a_fps;
			return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(seconds));
		};

		// The sender's game: Session sends on schedule, at its own frames, where its player is at that frame.
		std::vector<std::pair<Clock::time_point, Protocol::PlayerState>> sent;
		{
			auto          nextSend = start;
			std::uint32_t sequence = 0;
			for (auto now = start; now < end; now += frameOf(a_setup.senderFps, a_setup.senderFrameJitter)) {
				const auto workAt = a_setup.senderWork > Micro{} ? now + Micro{ static_cast<Micro::rep>(unit(random) * static_cast<float>(a_setup.senderWork.count())) } : now;
				const bool paused = a_setup.pause != Clock::duration{} && now >= start + a_setup.pauseAt && now < start + a_setup.pauseAt + a_setup.pause;
				if (paused || workAt < nextSend) {
					continue;
				}
				nextSend += a_setup.sendEvery;
				if (nextSend <= workAt) {
					nextSend = workAt + a_setup.sendEvery;
				}
				Protocol::PlayerState state;
				state.sequence = ++sequence;
				state.worldspace = 0x3C;
				state.x = RUN * std::chrono::duration<float>(now - start).count();
				state.heading = 1.5708f;
				state.speed = RUN;
				state.sentMs = a_setup.sendsTime ? Protocol::SentMs((a_setup.senderStampsFrame ? now : workAt) + a_setup.senderClock) : 0;
				sent.emplace_back(workAt, state);
			}
		}

		// The server, in the host's game: each tick the newest state not relayed yet goes out in a batch.
		std::vector<Packet> packets;
		{
			std::size_t   next = 0;
			std::uint32_t tick = 0;
			auto          nextLate = start + a_setup.lateEvery;
			for (auto now = start; now < end; now += 16ms) {
				std::optional<Protocol::PlayerState> newest;
				while (next < sent.size() && sent[next].first <= now) {
					newest = sent[next++].second;
				}
				if (!newest) {
					continue;
				}
				if (a_setup.dropChance > 0.0f && unit(random) < a_setup.dropChance) {
					continue;  // lost on the way (states go unreliable): the next tick's batch brings a newer one
				}
				auto late = Micro{ static_cast<Micro::rep>(unit(random) * static_cast<float>(a_setup.jitter.count())) };
				if (a_setup.lateChance > 0.0f && unit(random) < a_setup.lateChance) {
					late += a_setup.lateBy;
				}
				if (a_setup.lateEvery != Clock::duration{} && now >= nextLate) {
					nextLate += a_setup.lateEvery;
					late += a_setup.lateBy;
				}
				auto arrives = now + a_setup.trip + late;
				if (a_setup.queued && !packets.empty()) {
					arrives = (std::max)(arrives, packets.back().arrives);
				}
				packets.push_back({ arrives, Protocol::Encode(Protocol::PlayerStates{ ++tick, { { FRIEND_ID, *newest } } }) });
			}
			std::ranges::stable_sort(packets, {}, &Packet::arrives);
		}

		// The drawing game: our work reads what has come and samples the stand-in (RemotePlayers::Update), and the
		// engine's update of the stand-in puts it where that sample says (Puppets), which is where it is drawn.
		std::vector<Drawn>                  drawn;
		StateBuffer::Buffer                 buffer;
		std::optional<StateBuffer::Sampled> target;
		std::size_t                         next = 0;
		bool                                stalled = false;
		bool                                pushed = false;
		auto                                last = start;
		const auto work = [&](Clock::time_point& a_now) {
			while (next < packets.size() && packets[next].arrives <= a_now) {
				const auto batch = Protocol::DecodePlayerStates(packets[next++].bytes);
				REQUIRE(batch);
				for (const auto& entry : batch->players) {
					if (!pushed) {
						pushed = true;
						a_now += a_setup.firstLate;
					}
					buffer.Push(entry.state, a_now);
				}
			}
			buffer.Frame(a_now);
			target = buffer.Sample(a_now);
		};
		const auto draw = [&](Clock::time_point a_now) {
			const float seconds = std::chrono::duration<float>(a_now - last).count();
			last = a_now;
			if (!target || a_now - start < 2s) {
				return;
			}
			float x = target->state.x;
			if (const auto at = a_setup.byPath ? target->path.At(a_now) : std::nullopt) {
				x = at->x;
			}
			const float behind = std::chrono::duration<float>(a_now - start).count() - x / RUN;
			drawn.push_back({ a_now, seconds, x, target->held, buffer.delay, behind });
		};
		for (auto now = start; now < end;) {
			const auto length = frameOf(a_setup.fps, a_setup.frameJitter);
			if (a_setup.stall != Clock::duration{} && !stalled && now >= start + a_setup.stallAt) {
				stalled = true;
				now += a_setup.stall;
			}
			if (!a_setup.workApart) {
				work(now);
				draw(now);
				now += length;
				continue;
			}
			const float roll = unit(random);
			const bool  skipped = roll < a_setup.skipWork;
			const bool  early = roll < (1.0f + a_setup.skipWork) * 0.5f;
			auto        at = now + (early ? length / 10 : length * 8 / 10);
			if (!skipped && early) {
				work(at);
			}
			draw(now + length / 2);
			if (!skipped && !early) {
				work(at);
			}
			now += length;
		}
		return drawn;
	}

	struct Counts
	{
		int frames = 0;
		int held = 0;
		int backwards = 0;  // drawn behind where it was the frame before
		int uneven = 0;     // moved less than 80%, or more than 120%, of what the run covers in that frame
		int leaps = 0;      // moved more than half as far again as the run covers in that frame
		float heldFor = 0.0f;  // seconds of held frames
	};

	// From frame a_from on (the step into a_from counts).
	Counts Count(const std::vector<Drawn>& a_drawn, std::size_t a_from = 1)
	{
		Counts out;
		for (std::size_t i = (std::max)(a_from, std::size_t{ 1 }); i < a_drawn.size(); ++i) {
			const float step = a_drawn[i].x - a_drawn[i - 1].x;
			const float ran = RUN * a_drawn[i].seconds;
			++out.frames;
			out.held += a_drawn[i].held;
			out.backwards += step < -0.5f;
			out.uneven += step < 0.8f * ran || step > 1.2f * ran;
			out.leaps += step > 1.5f * ran;
			out.heldFor += a_drawn[i].held ? a_drawn[i].seconds : 0.0f;
		}
		return out;
	}

	std::size_t FirstAfter(const std::vector<Drawn>& a_drawn, Clock::duration a_since)
	{
		const auto at = std::ranges::find_if(a_drawn, [&](const Drawn& a_frame) { return a_frame.at >= start + a_since; });
		return static_cast<std::size_t>(at - a_drawn.begin());
	}
}

TEST("state buffer: a game at 16 fps draws a friend's run evenly, never held and never backwards")
{
	for (unsigned seed = 1; seed <= 5; ++seed) {
		Setup setup;
		setup.fps = 16.0f;
		setup.frameJitter = 0.3f;
		setup.seed = seed;
		const auto counts = Count(Play(setup));
		CHECK(counts.frames > 250);
		CHECK(counts.backwards == 0);
		CHECK(counts.held <= counts.frames / 100);
		CHECK(counts.uneven <= counts.frames / 100);
	}
}

TEST("state buffer: a game at 60 fps draws a friend's run evenly too, still about 70 ms behind")
{
	for (unsigned seed = 1; seed <= 5; ++seed) {
		Setup setup;
		setup.seed = seed;
		const auto drawn = Play(setup);
		const auto counts = Count(drawn);
		CHECK(counts.frames > 1000);
		CHECK(counts.backwards == 0);
		CHECK(counts.held <= counts.frames / 100);
		CHECK(counts.uneven <= counts.frames / 100);
		CHECK(drawn.back().delay < 0.1f);
	}
}

TEST("state buffer: after our game stalls, a friend's stand-in carries on at their pace instead of slowing and lurching")
{
	for (const float fps : { 16.0f, 60.0f }) {
		Setup setup;
		setup.fps = fps;
		setup.frameJitter = 0.2f;
		setup.length = 10s;
		setup.stallAt = 5s;
		setup.stall = 300ms;
		const auto drawn = Play(setup);
		const auto counts = Count(drawn, FirstAfter(drawn, 5s));  // from the frame after the stall on
		CHECK(counts.frames > 50);
		CHECK(counts.backwards == 0);
		CHECK(counts.held == 0);
		CHECK(counts.uneven == 0);
	}
}

TEST("state buffer: a friend in a slow game, sending at its uneven frames, is drawn evenly")
{
	for (const float fps : { 16.0f, 60.0f }) {
		Setup setup;
		setup.fps = fps;
		setup.senderFps = 16.0f;
		setup.senderFrameJitter = 0.3f;
		const auto counts = Count(Play(setup));
		CHECK(counts.backwards == 0);
		CHECK(counts.held <= counts.frames / 100);
		CHECK(counts.uneven <= counts.frames / 100);
	}
}

TEST("state buffer: on a jittery connection the delay grows until frames are rarely held")
{
	for (const float fps : { 16.0f, 60.0f }) {
		Setup setup;
		setup.fps = fps;
		setup.jitter = 90ms;
		const auto drawn = Play(setup);
		const auto counts = Count(drawn, FirstAfter(drawn, 5s));
		CHECK(counts.backwards == 0);
		CHECK(counts.held <= counts.frames / 50);
		CHECK(drawn.back().delay > 0.09f);  // up from 70 ms
		CHECK(drawn.back().delay < 0.25f);
	}
}

TEST("state buffer: a friend back from a loading screen is not drawn further behind than before")
{
	Setup setup;
	setup.length = 12s;
	setup.pauseAt = 4s;
	setup.pause = 3s;
	const auto drawn = Play(setup);
	const auto before = drawn[FirstAfter(drawn, 4s) - 1].delay;
	const auto back = FirstAfter(drawn, 7s + 500ms);
	const auto counts = Count(drawn, back);
	CHECK(counts.backwards == 0);
	CHECK(counts.held == 0);
	CHECK(counts.uneven == 0);
	CHECK(drawn.back().delay < before + 0.01f);
}

TEST("state buffer: states that don't say when they were sent are still drawn in order")
{
	Setup setup;
	setup.fps = 16.0f;
	setup.frameJitter = 0.3f;
	setup.sendsTime = false;
	const auto counts = Count(Play(setup));
	CHECK(counts.frames > 250);
	CHECK(counts.backwards == 0);
	CHECK(counts.held <= counts.frames / 100);
}

TEST("state buffer: a friend whose first state we handled late is drawn as close behind as usual a second later")
{
	for (const float fps : { 16.0f, 60.0f }) {
		Setup normal;
		normal.fps = fps;
		normal.frameJitter = 0.2f;
		normal.length = 8s;
		float usual = 0.0f;
		for (const auto& frame : Play(normal)) {
			usual = (std::max)(usual, frame.behind);
		}
		for (const auto late : { 300ms, 1000ms }) {
			// Our game hitched over the welcome on joining, so the first state was handled `late`; frames are kept
			// from two seconds in, a second after the hitch.
			auto setup = normal;
			setup.firstLate = late;
			const auto drawn = Play(setup);
			const auto counts = Count(drawn);
			CHECK(counts.frames > 50);
			CHECK(counts.backwards == 0);
			CHECK(counts.held <= counts.frames / 100);
			CHECK(counts.uneven <= counts.frames / 100);
			float worst = 0.0f;
			for (const auto& frame : drawn) {
				worst = (std::max)(worst, frame.behind);
			}
			CHECK(worst < usual + 0.02f);
			CHECK(worst < 0.2f);
		}
	}
}

TEST("state buffer: a stand-in is drawn evenly though our frame's work comes at another moment of each frame than the engine's update")
{
	for (const float fps : { 16.0f, 30.0f, 60.0f }) {
		for (unsigned seed = 1; seed <= 3; ++seed) {
			Setup setup;
			setup.fps = fps;
			setup.frameJitter = 0.2f;
			setup.seed = seed;
			setup.workApart = true;
			const auto drawn = Play(setup);
			const auto counts = Count(drawn, FirstAfter(drawn, 5s));  // once the delay has grown to what this asks for
			CHECK(counts.frames > 200);
			CHECK(counts.backwards == 0);
			// Uneven only where its path ran out: our work came later than it had in the last few seconds.
			CHECK(counts.uneven <= counts.frames / 50);
		}
	}
}

TEST("state buffer: put where our work last sampled it, the same stand-in ran unevenly (what a friend saw)")
{
	for (const float fps : { 16.0f, 60.0f }) {
		Setup setup;
		setup.fps = fps;
		setup.frameJitter = 0.2f;
		setup.workApart = true;
		setup.byPath = false;
		const auto counts = Count(Play(setup));
		CHECK(counts.uneven > counts.frames / 5);
	}
}

TEST("state buffer: a friend raising their gun has their stand-in's pitch swept between their states, not stepped")
{
	// Their states come 50 ms apart as they look up from level to 40 degrees in four steps, then hold there; their
	// game says nothing of when they were sampled, so they are placed as they arrive.
	StateBuffer::Buffer buffer;
	auto                now = start;
	std::uint32_t       sequence = 0;
	for (const int pitch : { 0, 10, 20, 30, 40, 40, 40, 40, 40, 40 }) {
		Protocol::PlayerState state;
		state.sequence = ++sequence;
		state.pitch = static_cast<std::int8_t>(pitch);
		CHECK(buffer.Push(state, now));
		now += 50ms;
	}

	// Drawn every 5 ms from the first state until well past the last, the stand-in's pitch only ever rises, from
	// level to 40, and passes through pitches no state said: the gun sweeps up, it does not jump 10 degrees at a time.
	std::vector<float> pitches;
	for (auto at = start; at < now + 200ms; at += 5ms) {
		buffer.Frame(at);
		const auto sampled = buffer.Sample(at);
		REQUIRE(sampled);
		pitches.push_back(sampled->pitch);
	}
	REQUIRE(pitches.size() > 100);
	CHECK(pitches.front() == 0.0f);
	CHECK(pitches.back() == 40.0f);
	CHECK(std::ranges::is_sorted(pitches));
	const auto between = std::ranges::count_if(pitches, [](float a_pitch) { return std::abs(std::fmod(a_pitch, 10.0f)) > 0.5f && std::abs(std::fmod(a_pitch, 10.0f)) < 9.5f; });
	CHECK(between > 10);
	// Held past the newest state, it keeps that state's pitch.
	buffer.Frame(now + 1s);
	const auto held = buffer.Sample(now + 1s);
	REQUIRE(held && held->held);
	CHECK(held->pitch == 40.0f);
}

TEST("state buffer: a host sending 10 to 20 states a second through a connection that holds some up is drawn going on from where it stood, never leaping ahead")
{
	// The host's game sends less often than Session's 30 a second (a slow or busy game, or a route that drops some); now
	// and then the connection holds a batch up and the ones behind it come together with it; the two games' clocks read
	// differently; and ours runs at 30, 60 or 144 frames a second, unevenly. Run on past the newest state while the next
	// was late, the stand-in stood, then leapt ahead by as long as it had stood when the late ones came: what a friend
	// saw as the host running choppily.
	for (const auto every : { 50ms, 66ms, 100ms }) {
		for (const float fps : { 30.0f, 60.0f, 144.0f }) {
			for (unsigned seed = 1; seed <= 3; ++seed) {
				Setup setup;
				setup.sendEvery = every;
				setup.fps = fps;
				setup.frameJitter = 0.3f;
				setup.jitter = 30ms;
				setup.lateChance = 0.03f;
				setup.lateBy = 80ms;
				setup.queued = true;
				setup.senderClock = 987654321ms;
				setup.seed = seed;
				const auto drawn = Play(setup);
				const auto counts = Count(drawn, FirstAfter(drawn, 5s));
				CHECK(counts.frames > 300);
				CHECK(counts.backwards == 0);
				CHECK(counts.leaps == 0);
				// It stands still only while a held-up state is later than the delay covers: well under 1% of the time.
				CHECK(counts.heldFor < 0.15f);
			}
		}
	}
}

TEST("state buffer: a game at 144 fps remembers a late state for seconds, so the next one does not freeze the stand-in again")
{
	// Every two seconds one batch is held up 100 ms and the ones behind it come with it. Kept frame by frame, the delay
	// remembered the last 64 frames: four seconds at 16 fps but under half a second at 144, so in a fast game each held-up
	// batch was forgotten long before the next, and every one froze the stand-in. Remembered for seconds, the delay
	// covers the next one, whatever our frame rate.
	for (const float fps : { 30.0f, 60.0f, 144.0f }) {
		Setup setup;
		setup.fps = fps;
		setup.sendEvery = 50ms;
		setup.jitter = 10ms;
		setup.lateEvery = 2s;
		setup.lateBy = 100ms;
		setup.queued = true;
		const auto drawn = Play(setup);
		const auto counts = Count(drawn, FirstAfter(drawn, 5s));  // from the third held-up batch on
		CHECK(counts.frames > 300);
		CHECK(counts.backwards == 0);
		CHECK(counts.leaps == 0);
		// Over the seven held-up batches after that, a frame or two at most (one a little later than the one before).
		CHECK(counts.heldFor < 0.02f);
		CHECK(drawn.back().delay < 0.25f);
	}
}

TEST("state buffer: a friend's states lost on the way now and then, at 10 to 30 a second, leave their stand-in going on evenly, drawn no further behind than the longer gap asks")
{
	// Player states go unreliable (Session sends them so), and one batch in twenty never comes: the next brings a state a
	// gap later than usual. Where that outran the delay the stand-in stands a moment and goes on from there, never leaping
	// or running backwards, and the delay settles to cover the longer gap and no more: about 100 ms at 30 states a second,
	// 140 at 20, and the 250 ms most at 10 (a 200 ms gap with the jitter on top). Kept by frames, the delay of a game at
	// 144 fps forgot each lost state within half a second, and the stand-in leapt after the next one at 30 a second too.
	for (const auto every : { 33ms, 50ms, 100ms }) {
		for (const float fps : { 30.0f, 60.0f, 144.0f }) {
			Setup setup;
			setup.sendEvery = every;
			setup.fps = fps;
			setup.frameJitter = 0.2f;
			setup.dropChance = 0.05f;
			const auto drawn = Play(setup);
			const auto counts = Count(drawn, FirstAfter(drawn, 5s));
			CHECK(counts.frames > 300);
			CHECK(counts.backwards == 0);
			CHECK(counts.leaps == 0);
			CHECK(counts.heldFor < 0.1f);
			CHECK(drawn.back().delay <= (std::min)(std::chrono::duration<float>(2 * every).count() + 0.07f, 0.25f));
		}
	}
}

TEST("state buffer: over a Steam relay that holds states up now and then, a running friend's stand-in carries on instead of standing, and never leaps")
{
	// What a host saw as a jittery friend (Nexus, 2026-10-19): states through a relay come with some jitter, one in thirty
	// or so held up 60 to 120 ms with the ones behind it, and one in a hundred lost. Stood at the newest state while the
	// next was late, the stand-in stopped in its run each time, then went on.
	for (const auto late : { 60ms, 120ms }) {
		for (const float fps : { 30.0f, 60.0f, 144.0f }) {
			for (unsigned seed = 1; seed <= 3; ++seed) {
				Setup setup;
				setup.fps = fps;
				setup.frameJitter = 0.2f;
				setup.trip = 40ms;
				setup.jitter = 25ms;
				setup.lateChance = 0.03f;
				setup.lateBy = late;
				setup.queued = true;
				setup.dropChance = 0.01f;
				setup.workApart = true;
				setup.seed = seed;
				const auto drawn = Play(setup);
				const auto counts = Count(drawn, FirstAfter(drawn, 5s));
				CHECK(counts.frames > 300);
				CHECK(counts.backwards == 0);
				CHECK(counts.leaps == 0);
				CHECK(counts.heldFor < 0.02f);
				CHECK(counts.uneven <= counts.frames / 50);
			}
		}
	}
}

namespace
{
	// A friend running along x at RUN, a state every 33 ms, who stops dead at a_stopAt: the states after that say they stand
	// there. The ones from a_lateFrom on come a_lateBy late (together). Drawn every a_frame; where it is drawn each frame.
	std::vector<float> RunAndStop(Clock::duration a_stopAt, Clock::duration a_lateFrom, Clock::duration a_lateBy, Clock::duration a_frame = 7ms)
	{
		StateBuffer::Buffer buffer;
		struct Due
		{
			Clock::time_point     at;
			Protocol::PlayerState state;
		};
		std::vector<Due> dues;
		std::uint32_t    sequence = 0;
		for (auto t = 0ms; t < 6s; t += 33ms) {
			const float           ran = RUN * std::chrono::duration<float>((std::min)(Clock::duration(t), a_stopAt)).count();
			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.worldspace = 0x3C;
			state.x = ran;
			state.speed = t < a_stopAt ? RUN : 0.0f;
			state.sentMs = Protocol::SentMs(start + t);
			auto at = start + t + 30ms;
			if (t >= a_lateFrom && t < a_lateFrom + a_lateBy) {
				at = start + a_lateFrom + a_lateBy + 30ms;
			}
			dues.push_back({ at, state });
		}
		std::vector<float> xs;
		std::size_t        next = 0;
		for (auto now = start; now < start + 6s; now += a_frame) {
			while (next < dues.size() && dues[next].at <= now) {
				buffer.Push(dues[next++].state, now);
			}
			buffer.Frame(now);
			if (const auto sampled = buffer.Sample(now); sampled && now > start + 1s) {
				xs.push_back(sampled->state.x);
			}
		}
		return xs;
	}
}

TEST("state buffer: a friend who stops while their next state is late is drawn going on a little, then easing back to where they stopped")
{
	// Their run stops at 3 s; the states from then on come 150 ms late. Carried on past the last running state, the
	// stand-in went at most EXTRAPOLATE further than where they stopped, and goes back there without a jump.
	const auto  xs = RunAndStop(3s, 3s, 150ms);
	const float stop = RUN * 3.0f;
	REQUIRE(xs.size() > 400);
	const float furthest = *std::ranges::max_element(xs);
	CHECK(furthest <= stop + RUN * 0.1f + 1.0f);
	float worstStep = 0.0f;
	for (std::size_t i = 1; i < xs.size(); ++i) {
		worstStep = (std::max)(worstStep, std::abs(xs[i] - xs[i - 1]));
	}
	CHECK(worstStep < RUN * 0.007f * 1.5f);  // never more than a run's step and a half in a 7 ms frame
	CHECK(std::abs(xs.back() - stop) < 0.5f);
}

TEST("state buffer: a late state that came on time changes nothing; one far off (a teleport) is gone to at once")
{
	// No state late: nothing carried on, nothing to ease out, and the stand-in is exactly where interpolation puts it.
	{
		const auto xs = RunAndStop(3s, 10s, 0ms);
		REQUIRE(xs.size() > 400);
		CHECK(std::abs(xs.back() - RUN * 3.0f) < 0.01f);
		for (std::size_t i = 1; i < xs.size(); ++i) {
			CHECK(xs[i] >= xs[i - 1] - 0.01f);
		}
	}
	// A friend put 2000 units on between two states (setpos, a door in the same space) is not eased there.
	StateBuffer::Buffer   buffer;
	Protocol::PlayerState state;
	state.worldspace = 0x3C;
	state.speed = RUN;
	auto now = start;
	for (std::uint32_t i = 1; i <= 20; ++i) {
		state.sequence = i;
		state.x = RUN * 0.033f * static_cast<float>(i);
		state.sentMs = Protocol::SentMs(now);
		buffer.Push(state, now);
		buffer.Frame(now);
		now += 33ms;
	}
	now += 200ms;  // the next is late: the stand-in has gone on and then stood
	buffer.Frame(now);
	state.sequence = 21;
	state.x += 2000.0f;
	state.sentMs = Protocol::SentMs(now - 30ms);
	buffer.Push(state, now);
	CHECK(buffer.correction.Left(now) == 0.0f);
}

TEST("state buffer: carried on past the newest state, a running friend's path goes on too, for the engine's updates between our frames")
{
	StateBuffer::Buffer   buffer;
	Protocol::PlayerState state;
	state.worldspace = 0x3C;
	state.speed = RUN;
	auto now = start;
	for (std::uint32_t i = 1; i <= 30; ++i) {
		state.sequence = i;
		state.x = RUN * 0.033f * static_cast<float>(i);
		state.sentMs = Protocol::SentMs(now);
		buffer.Push(state, now);
		buffer.Frame(now);
		now += 33ms;
	}
	// The next state is late: our frame comes as the drawn time reaches the newest one.
	now += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(buffer.delay)) - 33ms;
	buffer.Frame(now);
	const auto sampled = buffer.Sample(now);
	REQUIRE(sampled);
	const auto at = [&](Clock::duration a_later) { return sampled->path.At(now + a_later)->x; };
	CHECK(at(20ms) > at(0ms) + RUN * 0.015f);
	CHECK(at(60ms) > at(20ms) + RUN * 0.03f);
	// It stands once EXTRAPOLATE has run out.
	CHECK(std::abs(at(400ms) - at(300ms)) < 0.01f);
	CHECK(at(400ms) <= state.x + RUN * 0.1f + 0.5f);
}

TEST("state buffer: a friend whose game skips frames unevenly is drawn running evenly when their states carry their frames' time")
{
	// The rig's VM (F4Multiplayer-ktmu): about 30 fps, frames half as long again or half as short, and the work that samples
	// the player up to 20 ms into the frame. Stamped with that moment, an even run comes as a stand and a leap.
	for (const bool frames : { false, true }) {
		int uneven = 0, total = 0, leaps = 0;
		for (unsigned seed = 1; seed <= 5; ++seed) {
			Setup setup;
			setup.senderFps = 30.0f;
			setup.senderFrameJitter = 0.5f;
			setup.senderWork = 20ms;
			setup.senderStampsFrame = frames;
			setup.seed = seed;
			const auto drawn = Play(setup);
			const auto counts = Count(drawn, FirstAfter(drawn, 5s));
			CHECK(counts.backwards == 0);
			uneven += counts.uneven;
			leaps += counts.leaps;
			total += counts.frames;
		}
		REQUIRE(total > 4000);
		if (frames) {
			CHECK(uneven <= total / 100);
			CHECK(leaps == 0);
		} else {
			CHECK(uneven > total / 10);  // as it was
		}
	}
}

TEST("state buffer: a stand-in placed a moment before our frame's work sampled it is on its way there, not standing at that moment's place")
{
	StateBuffer::Buffer   buffer;
	Protocol::PlayerState state;
	state.worldspace = 0x3C;
	state.speed = RUN;
	auto now = start;
	for (std::uint32_t i = 1; i <= 30; ++i) {
		state.sequence = i;
		state.x = RUN * 0.033f * static_cast<float>(i);
		state.sentMs = Protocol::SentMs(now);
		buffer.Push(state, now);
		buffer.Frame(now);
		now += 33ms;
	}
	now -= 16ms;  // between two states
	buffer.Frame(now);
	const auto sampled = buffer.Sample(now);
	REQUIRE(sampled);
	const auto at = [&](Clock::duration a_off) { return sampled->path.At(now + a_off)->x; };
	// The engine's frame a few ms before our work: drawn that much further back, on the same run.
	CHECK(at(-10ms) < at(0ms) - RUN * 0.008f);
	CHECK(at(-10ms) > at(0ms) - RUN * 0.012f);
	CHECK(at(-5ms) < at(0ms));
	CHECK(at(10ms) > at(0ms) + RUN * 0.008f);
}

TEST("state buffer: played behind by a lag, a friend is drawn where their run was that long ago, with the history kept for it")
{
	// Slowed friends in VATS (game/RealTimeVats.h, Playback): 4 seconds of the game's slow motion at 0.04 after a second of
	// normal play, in 30 fps frames. The states are not bent: the run is drawn at the pace it was run, from further back.
	StateBuffer::Buffer   buffer;
	StateBuffer::Buffer   control;  // the same run, never lagging
	Protocol::PlayerState state;
	state.worldspace = 0x3C;
	state.speed = RUN;
	auto  now = start;
	float lag = 0.0f;
	for (std::uint32_t i = 1; i <= 150; ++i) {
		state.sequence = i;
		state.x = RUN * 0.033f * static_cast<float>(i);
		state.sentMs = Protocol::SentMs(now);
		buffer.Push(state, now);
		control.Push(state, now);
		lag = i > 30 ? (std::min)(lag + 0.96f * 0.033f, 4.0f) : 0.0f;
		buffer.lag = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(lag));
		buffer.Frame(now);
		control.Frame(now);
		now += 33ms;
	}
	now -= 33ms;
	REQUIRE(lag > 3.5f);

	// The history is kept as far back as the lag reaches; the one that never lags keeps its second.
	CHECK(buffer.snapshots.size() > 120);
	CHECK(control.snapshots.size() < 40);

	const auto sampled = buffer.Sample(now);
	const auto present = control.Sample(now);
	REQUIRE(sampled);
	REQUIRE(present);
	// Where it is drawn is the lag behind where the one that does not lag has it (the same delay: they saw the same states).
	CHECK(present->state.x - sampled->state.x > RUN * (lag - 0.05f));
	CHECK(present->state.x - sampled->state.x < RUN * (lag + 0.05f));
	CHECK(sampled->state.speed == RUN);  // it is running there, not held

	// Its path is in the same time: the engine's update, placed that far behind the present, is on it.
	const auto lagged = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(lag));
	const auto onPath = sampled->path.At(now - lagged);
	REQUIRE(onPath);
	CHECK(std::abs(onPath->x - sampled->state.x) < RUN * 0.01f);
	// ... and a step later on goes forward at the pace of the run.
	const auto later = sampled->path.At(now - lagged + 20ms);
	REQUIRE(later);
	CHECK(later->x - onPath->x > RUN * 0.015f);
	CHECK(later->x - onPath->x < RUN * 0.025f);

	// Back to none: drawn as the one that never lagged.
	buffer.lag = {};
	const auto back = buffer.Sample(now);
	REQUIRE(back);
	CHECK(std::abs(back->state.x - present->state.x) < 1.0f);
}
