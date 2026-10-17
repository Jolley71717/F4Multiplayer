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
				const bool paused = a_setup.pause != Clock::duration{} && now >= start + a_setup.pauseAt && now < start + a_setup.pauseAt + a_setup.pause;
				if (paused || now < nextSend) {
					continue;
				}
				nextSend += 33ms;
				if (nextSend <= now) {
					nextSend = now + 33ms;
				}
				Protocol::PlayerState state;
				state.sequence = ++sequence;
				state.worldspace = 0x3C;
				state.x = RUN * std::chrono::duration<float>(now - start).count();
				state.heading = 1.5708f;
				state.speed = RUN;
				state.sentMs = a_setup.sendsTime ? Protocol::SentMs(now) : 0;
				sent.emplace_back(now, state);
			}
		}

		// The server, in the host's game: each tick the newest state not relayed yet goes out in a batch.
		std::vector<Packet> packets;
		{
			std::size_t   next = 0;
			std::uint32_t tick = 0;
			for (auto now = start; now < end; now += 16ms) {
				std::optional<Protocol::PlayerState> newest;
				while (next < sent.size() && sent[next].first <= now) {
					newest = sent[next++].second;
				}
				if (!newest) {
					continue;
				}
				const auto late = Micro{ static_cast<Micro::rep>(unit(random) * static_cast<float>(a_setup.jitter.count())) };
				packets.push_back({ now + a_setup.trip + late, Protocol::Encode(Protocol::PlayerStates{ ++tick, { { FRIEND_ID, *newest } } }) });
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
			// Uneven only where its path ran out: about as often as the delay leaves out the furthest frames (StateBuffer::LEADS_LEFT_OUT).
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
