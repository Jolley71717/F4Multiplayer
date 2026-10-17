#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifdef F4MP_TRACY
#	include <tracy/TracyC.h>
#endif

// What our own work costs per frame, part by part: the dev channel's `perf` reads the median, the 99th percentile and
// the worst of the last minute or so of frames, and how often a hitch in the game's frames came with a frame of ours
// that took half of it. The parts are marked with PERF_SCOPE("name") on the main thread, inside RunFrame's frame
// (Perf::Frame); a scope anywhere else records nothing. The recording is pure, so it is unit tested.
namespace Perf
{
	using Clock = std::chrono::steady_clock;

	constexpr std::size_t MAX_ZONES = 32;
	constexpr std::size_t FRAMES = 3600;                    // kept per part: a minute at 60 fps
	constexpr auto        HITCH = std::chrono::milliseconds(100);  // as FrameStats counts one

	// Over the frames kept, in milliseconds.
	struct Summary
	{
		std::uint32_t frames = 0;
		float         p50 = 0.0f;
		float         p99 = 0.0f;
		float         worst = 0.0f;
	};

	class Recorder
	{
	public:
		Recorder() :
			samples(MAX_ZONES * FRAMES, 0)
		{
			names[0] = "frame";
		}

		// The part named a_name (a string literal: its pointer is kept), added on first use. Part 0 is the whole frame.
		// Past MAX_ZONES parts, the last one takes the rest.
		std::size_t Zone(const char* a_name)
		{
			std::scoped_lock l{ lock };
			for (std::size_t i = 1; i < count; ++i) {
				if (std::string_view{ names[i] } == a_name) {
					return i;
				}
			}
			if (count == MAX_ZONES) {
				return MAX_ZONES - 1;
			}
			names[count] = a_name;
			return count++;
		}

		void Add(std::size_t a_zone, Clock::duration a_time)
		{
			if (a_zone < MAX_ZONES) {
				current[a_zone] += a_time;
			}
		}

		// A frame ended: a_ours is the whole of our work in it, a_interval the game's time since the frame before (zero
		// for the first).
		void EndFrame(Clock::duration a_ours, Clock::duration a_interval)
		{
			current[0] = a_ours;
			const std::size_t slot = next % FRAMES;
			for (std::size_t i = 0; i < MAX_ZONES; ++i) {
				samples[i * FRAMES + slot] = Micros(current[i]);
				current[i] = {};
			}
			++next;
			if (a_interval >= HITCH) {
				++hitches;
				if (a_ours * 2 >= a_interval) {
					++oursInHitches;
				}
			}
			worstEver = (std::max)(worstEver, a_ours);
		}

		Summary Of(std::size_t a_zone) const
		{
			Summary out;
			const std::size_t n = (std::min)(next, FRAMES);
			if (a_zone >= MAX_ZONES || n == 0) {
				return out;
			}
			scratch.assign(samples.begin() + a_zone * FRAMES, samples.begin() + a_zone * FRAMES + n);
			out.frames = static_cast<std::uint32_t>(n);
			out.p50 = Ms(Nth(n / 2));
			out.p99 = Ms(Nth((n * 99) / 100));
			out.worst = Ms(*std::max_element(scratch.begin(), scratch.end()));
			return out;
		}

		// "frames=N ours p50=0.21 p99=0.80 max=3.10ms worst_ever=12.0ms hitches=H ours_in_hitches=K | Story 0.01/0.02/0.10 | ..."
		// (each part as p50/p99/max in ms; parts that never took a microsecond are left out).
		std::string Describe() const
		{
			const auto frame = Of(0);
			auto       out = std::format("frames={} ours p50={:.2f} p99={:.2f} max={:.2f}ms worst_ever={:.1f}ms hitches={} ours_in_hitches={}", frame.frames, frame.p50,
					  frame.p99, frame.worst, std::chrono::duration<float, std::milli>(worstEver).count(), hitches, oursInHitches);
			for (std::size_t i = 1; i < count; ++i) {
				const auto part = Of(i);
				if (part.worst > 0.0f) {
					out += std::format(" | {} {:.2f}/{:.2f}/{:.2f}", names[i], part.p50, part.p99, part.worst);
				}
			}
			return out;
		}

		// Starts the counts over; the parts stay known.
		void Reset()
		{
			std::fill(samples.begin(), samples.end(), 0u);
			current = {};
			next = 0;
			hitches = 0;
			oursInHitches = 0;
			worstEver = {};
		}

	private:
		static std::uint32_t Micros(Clock::duration a_time)
		{
			const auto us = std::chrono::duration_cast<std::chrono::microseconds>(a_time).count();
			return static_cast<std::uint32_t>(std::clamp<long long>(us, 0, 0xFFFFFFFF));
		}

		static float Ms(std::uint32_t a_micros) { return static_cast<float>(a_micros) / 1000.0f; }

		std::uint32_t Nth(std::size_t a_index) const
		{
			std::nth_element(scratch.begin(), scratch.begin() + a_index, scratch.end());
			return scratch[a_index];
		}

		std::mutex                                 lock;  // Zone only: a part's first use may come from any thread
		std::array<const char*, MAX_ZONES>         names{};
		std::size_t                                count = 1;
		std::array<Clock::duration, MAX_ZONES>     current{};
		std::vector<std::uint32_t>                 samples;  // MAX_ZONES rings of FRAMES microsecond counts
		mutable std::vector<std::uint32_t>         scratch;
		std::size_t                                next = 0;  // frames ended since the last Reset
		std::uint32_t                              hitches = 0;
		std::uint32_t                              oursInHitches = 0;
		Clock::duration                            worstEver{};
	};

	Recorder& Global();

	// Whether this thread is inside a frame of ours (Perf::Frame): only then does a part record.
	inline thread_local bool inFrame = false;

	// A part as a PERF_SCOPE or PERF_NEXT names it: its place in Global(), and in a developer build with Tracy
	// (xmake f --tracy=<its source>), where Tracy shows it.
	struct Part
	{
		std::size_t zone;
#ifdef F4MP_TRACY
		___tracy_source_location_data where;
#endif
	};

#ifdef F4MP_TRACY
	// Called once, at load: Tracy's client starts listening on this machine only (port 8086), and sends only while a
	// capture is connected.
	inline void StartTracy() { ___tracy_startup_profiler(); }
#else
	inline void StartTracy() {}
#endif

	// One part's time, added to this frame's.
	class Scope
	{
	public:
		explicit Scope(const Part& a_part) :
			zone(a_part.zone),
			start(inFrame ? Clock::now() : Clock::time_point{})
		{
#ifdef F4MP_TRACY
			if (inFrame) {
				tracy = ___tracy_emit_zone_begin(&a_part.where, 1);
			}
#endif
		}

		~Scope()
		{
			if (start != Clock::time_point{}) {
				Global().Add(zone, Clock::now() - start);
#ifdef F4MP_TRACY
				___tracy_emit_zone_end(tracy);
#endif
			}
		}

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		std::size_t       zone;
		Clock::time_point start;
#ifdef F4MP_TRACY
		TracyCZoneCtx tracy{};
#endif
	};

	// Consecutive stretches of one function, each a part: Next ends the stretch before (if any) and starts the one
	// named; the last ends with the function.
	class Stretches
	{
	public:
		Stretches() = default;
		~Stretches() { End(); }

		void Next(const Part& a_part)
		{
			End();
			if (inFrame) {
				zone = a_part.zone;
				start = Clock::now();
#ifdef F4MP_TRACY
				tracy = ___tracy_emit_zone_begin(&a_part.where, 1);
#endif
			}
		}

		Stretches(const Stretches&) = delete;
		Stretches& operator=(const Stretches&) = delete;

	private:
		void End()
		{
			if (start != Clock::time_point{}) {
				const auto now = Clock::now();
				Global().Add(zone, now - start);
				start = {};
#ifdef F4MP_TRACY
				___tracy_emit_zone_end(tracy);
#endif
			}
		}

		std::size_t       zone = 0;
		Clock::time_point start{};
#ifdef F4MP_TRACY
		TracyCZoneCtx tracy{};
#endif
	};

	// The whole of one frame of ours, on the main thread (RunFrame): ends it in Global() when it goes.
	class Frame
	{
	public:
		// One run inside another (a window message pumped from within our frame) is part of the outer one.
		Frame() :
			nested(inFrame),
			start(Clock::now())
		{
			inFrame = true;
#ifdef F4MP_TRACY
			if (!nested) {
				static constexpr ___tracy_source_location_data where{ "frame", "Perf::Frame", __FILE__, __LINE__, 0 };
				tracy = ___tracy_emit_zone_begin(&where, 1);
			}
#endif
		}

		~Frame()
		{
			if (nested) {
				return;
			}
			inFrame = false;
			const auto end = Clock::now();
			Global().EndFrame(end - start, last != Clock::time_point{} ? start - last : Clock::duration{});
			last = start;
#ifdef F4MP_TRACY
			___tracy_emit_zone_end(tracy);
			___tracy_emit_frame_mark(nullptr);  // the game's frames, as Tracy's frame bar
#endif
		}

		Frame(const Frame&) = delete;
		Frame& operator=(const Frame&) = delete;

	private:
		static inline Clock::time_point last{};  // main thread only
		bool                            nested;
		Clock::time_point               start;
#ifdef F4MP_TRACY
		TracyCZoneCtx tracy{};
#endif
	};
}

#define PERF_CONCAT_INNER(a, b) a##b
#define PERF_CONCAT(a, b) PERF_CONCAT_INNER(a, b)
#ifdef F4MP_TRACY
#	define PERF_PART(a_name) \
		static const ::Perf::Part PERF_CONCAT(perfPart_, __LINE__){ ::Perf::Global().Zone(a_name), { a_name, __FUNCTION__, __FILE__, __LINE__, 0 } }
#else
#	define PERF_PART(a_name) static const ::Perf::Part PERF_CONCAT(perfPart_, __LINE__){ ::Perf::Global().Zone(a_name) }
#endif
// Times the rest of the enclosing block as the part a_name (a string literal).
#define PERF_SCOPE(a_name) \
	PERF_PART(a_name);     \
	const ::Perf::Scope PERF_CONCAT(perfScope_, __LINE__) { PERF_CONCAT(perfPart_, __LINE__) }
// Ends a_stretches' stretch before and starts the part a_name (a string literal).
#define PERF_NEXT(a_stretches, a_name) \
	PERF_PART(a_name);                 \
	a_stretches.Next(PERF_CONCAT(perfPart_, __LINE__))
