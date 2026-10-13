#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// Console commands the NPC sync runs in a frame (dressing a copy, handing one back to its own AI, switching one on,
// bringing one in, killing one), and how many copies it takes over from their own AI in a frame. Each command is
// compiled and run on the spot; after the owner's game comes back from a pause menu, every copy at a busy
// settlement was taken over again in the same frame, a few commands each. Taking over is spread over frames
// (MAX_REGISTRATIONS_PER_FRAME; the rest wait a frame and keep their snapshots), and the commands are counted for
// `net status`, with a log line for a busy frame. Pure, so it can be tested without the game.
namespace CommandMeter
{
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t MAX_REGISTRATIONS_PER_FRAME = 8;
	constexpr std::uint32_t BUSY_FRAME = 32;  // commands in one frame worth a log line
	constexpr auto          LOG_INTERVAL = std::chrono::seconds(10);

	class Meter
	{
	public:
		void Count(std::uint32_t a_commands) { frame += a_commands; }

		// Whether one more copy may be taken over from its own AI this frame.
		[[nodiscard]] bool MayRegister() const { return registrations < MAX_REGISTRATIONS_PER_FRAME; }

		void Registered()
		{
			++registrations;
			++totalRegistered;
		}

		void Released() { ++totalReleased; }

		// Ends the frame. True when it was busy and is worth a log line (at most one every LOG_INTERVAL).
		bool EndFrame(Clock::time_point a_now)
		{
			last = frame;
			most = (std::max)(most, frame);
			const bool busy = frame > BUSY_FRAME;
			frame = 0;
			registrations = 0;
			if (!busy) {
				return false;
			}
			++busyFrames;
			if (logged && a_now - lastLogged < LOG_INTERVAL) {
				return false;
			}
			logged = true;
			lastLogged = a_now;
			return true;
		}

		[[nodiscard]] std::uint32_t Last() const { return last; }            // commands in the last frame
		[[nodiscard]] std::uint32_t Most() const { return most; }            // in the busiest frame
		[[nodiscard]] std::uint32_t BusyFrames() const { return busyFrames; }
		[[nodiscard]] std::uint32_t Registrations() const { return totalRegistered; }
		[[nodiscard]] std::uint32_t Releases() const { return totalReleased; }

	private:
		std::uint32_t     frame = 0;
		std::uint32_t     registrations = 0;
		std::uint32_t     last = 0;
		std::uint32_t     most = 0;
		std::uint32_t     busyFrames = 0;
		std::uint32_t     totalRegistered = 0;
		std::uint32_t     totalReleased = 0;
		bool              logged = false;
		Clock::time_point lastLogged{};
	};
}
