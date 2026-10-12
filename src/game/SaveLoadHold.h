#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// The hold on the frame's game work while a save or a game load is under way, from F4SE's message
// before it to the one after (main.cpp). A save runs on the main thread, between frames. A load runs
// on a thread of its own while the main thread goes on drawing and running frames, so a flag the frame
// checked was not enough: a frame that had checked it just before the load began went on working
// while the load tore the world down. Here a frame starts its game work (Enter) under the same lock
// that starts a hold, and the load's thread can wait for a frame already working to finish. The game
// work a save or load needs (taking the stand-ins out before it, starting the session over after it)
// is handed to the frame's thread (Post), which runs it first thing, held or not. Free of the game,
// so it is unit tested.
namespace SaveLoadHold
{
	using Clock = std::chrono::steady_clock;

	class Hold
	{
	public:
		using Work = std::function<void()>;

		// a_limit: a hold whose end never came stops game work only this long.
		explicit Hold(Clock::duration a_limit) :
			limit(a_limit)
		{}

		// The save's or load's side (F4SE's messages, any thread).

		// No frame's game work starts from now on. Returns this hold's number, for End.
		std::uint64_t Begin(Clock::time_point a_now)
		{
			std::scoped_lock l{ lock };
			held = true;
			since = a_now;
			return ++number;
		}

		// The newest hold's number (0 before the first).
		[[nodiscard]] std::uint64_t Newest() const
		{
			std::scoped_lock l{ lock };
			return number;
		}

		// Ends hold a_number. A newer hold begun since (a game loaded again before the last load's own
		// work had run) stays on. Returns how many frames it held, or nothing when no hold ended.
		std::optional<std::uint32_t> End(std::uint64_t a_number)
		{
			std::scoped_lock l{ lock };
			if (!held || a_number != number) {
				return std::nullopt;
			}
			held = false;
			return std::exchange(heldFrames, 0);
		}

		// Whether a hold is on (and not past the limit).
		[[nodiscard]] bool Held(Clock::time_point a_now) const
		{
			std::scoped_lock l{ lock };
			return HeldNow(a_now);
		}

		// Hands a_work to the frame's thread, which runs it before its next frame's game work, held or not.
		void Post(Work a_work)
		{
			std::scoped_lock l{ lock };
			posted.push_back(std::move(a_work));
		}

		// Waits until the work handed over has run and no frame is doing game work, at most a_timeout.
		// False when it timed out.
		bool WaitForFrames(Clock::duration a_timeout)
		{
			std::unique_lock l{ lock };
			return changed.wait_for(l, a_timeout, [&] { return posted.empty() && working == 0; });
		}

		// Takes back the work handed over that the frame's thread has not started (it did not come in
		// time), for the caller to run itself. Work already started is left to finish there.
		std::vector<Work> TakeBack()
		{
			std::scoped_lock l{ lock };
			return std::exchange(posted, {});
		}

		// The frame's side.

		// Runs the work handed over (Post), in the order it came. Returns how much there was. A piece that
		// throws doesn't stop the rest (the end of a hold may be among it); the first exception is thrown
		// on once all of it has run.
		std::size_t RunPosted()
		{
			std::vector<Work> work;
			{
				std::scoped_lock l{ lock };
				if (posted.empty()) {
					return 0;
				}
				work.swap(posted);
				++working;
			}
			const Done         done{ *this };  // counted as working until it has all run
			std::exception_ptr failed;
			for (auto& fn : work) {
				try {
					fn();
				} catch (...) {
					if (!failed) {
						failed = std::current_exception();
					}
				}
			}
			if (failed) {
				std::rethrow_exception(failed);
			}
			return work.size();
		}

		// Whether this frame's game work may start: not while a hold is on (the frame is counted, for
		// End). After true, Leave follows when the work is done (see Frame).
		bool Enter(Clock::time_point a_now)
		{
			std::scoped_lock l{ lock };
			if (HeldNow(a_now)) {
				++heldFrames;
				return false;
			}
			++working;
			return true;
		}

		void Leave()
		{
			{
				std::scoped_lock l{ lock };
				--working;
			}
			changed.notify_all();
		}

	private:
		struct Done
		{
			Hold& hold;
			~Done() { hold.Leave(); }
		};

		bool HeldNow(Clock::time_point a_now) const { return held && a_now - since < limit; }

		mutable std::mutex      lock;
		std::condition_variable changed;
		Clock::duration         limit;
		bool                    held = false;
		Clock::time_point       since{};
		std::uint64_t           number = 0;
		std::uint32_t           heldFrames = 0;
		std::uint32_t           working = 0;  // frames doing game work, and handed-over work running
		std::vector<Work>       posted;
	};

	// One frame's game work: entered if no hold is on, and left at the end of the scope.
	class Frame
	{
	public:
		Frame(Hold& a_hold, Clock::time_point a_now) :
			hold(a_hold),
			entered(a_hold.Enter(a_now))
		{}

		~Frame()
		{
			if (entered) {
				hold.Leave();
			}
		}

		Frame(const Frame&) = delete;
		Frame& operator=(const Frame&) = delete;

		explicit operator bool() const { return entered; }

	private:
		Hold& hold;
		bool  entered;
	};

	// The game's hold (main.cpp).
	Hold& Global();
}
