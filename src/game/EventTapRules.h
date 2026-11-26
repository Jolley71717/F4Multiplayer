#pragma once

#include "game/TextureFlush.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

// The event tap: our player's hits reach the mod whichever other plugin's sink is ahead of ours (docs/DESIGN.md, "Event
// tap"). The engine hands an event to a source's sinks in the order they registered and any sink can return kStop, so a
// plugin whose sink registered before ours (load order does not decide it) can keep every TESHitEvent from ours: a player
// whose game sent the mod none of its player's hits had 112 drops given back as "no hit of ours" (CopyHealth::HitEvents).
// Three read-only parts, none of which changes, stops or reorders what another plugin gets:
// - A send hook (EventTap.cpp): the two places the engine builds a TESHitEvent and calls the source's Notify go through
//   it. It sees every hit before any sink does, hands the event on unchanged, and when our own sink never got it (a sink
//   ahead of ours stopped it) runs the same read-and-queue handler the sink runs. Kept here free of the game: the
//   per-thread dispatch book (Track), the counters (Tally) and the check of the call sites.
// - Our sink ahead of every other plugin's in the source's list, under the source's lock (PlaceAfterLeading), so a plugin's sink
//   that stops events stops them for those behind it, not for us. Not ahead of the engine's own sinks: the first of them
//   finishes the hit event (the cause is empty until it has run: a sink of ours in front read none of the player's hits on an
//   NPC, seen on the rig), so ours goes right behind the engine's.
// - Diagnostics: the other sinks on a source, once a session, and what the hook saw against what our sink got.
namespace EventTapRules
{
	// Where each of the engine's two senders of TESHitEvent calls the source's Notify, as 1.11.240 has it (read from disk:
	// the senders are 2201637, which takes the target, cause, source form, projectile and material and builds the event
	// with no hit data, and 2201638, which takes a HitData; each gets the source from 2201886, and the Notify is one
	// function, 2202308, which only these two calls reach). `call rel32`, five bytes.
	struct Site
	{
		std::size_t offset;  // from the sender's start
	};
	inline constexpr std::array<Site, 2> SITES{ { { 0x9D }, { 0x4E } } };

	// How many of the sites' five bytes call a_notify (a_bytes[i] are the bytes at a_at[i]). All or none are patched: a
	// site that is not the expected call means another plugin's patch or another version.
	struct SiteCheck
	{
		std::size_t total = 0;
		std::size_t calling = 0;
		[[nodiscard]] constexpr bool All() const { return total != 0 && calling == total; }
	};

	template <std::size_t N>
	constexpr SiteCheck Check(const std::array<std::array<std::uint8_t, 5>, N>& a_bytes, const std::array<std::uintptr_t, N>& a_at, std::uintptr_t a_notify)
	{
		SiteCheck out{ .total = N };
		for (std::size_t i = 0; i < N; ++i) {
			if (const auto target = TextureFlush::CallTarget(a_bytes[i], a_at[i]); target && *target == a_notify) {
				++out.calling;
			}
		}
		return out;
	}

	// Which events the hook has in flight on this thread (the engine's Notify is synchronous: the sinks run inside it, on
	// the sender's thread, with the sender's own event by reference), so a sink told the same address knows the hook sent
	// it, and the hook, after Notify returns, whether our sink got it. Events nest when a sink raises another hit
	// (the stack); one nested deeper than DEPTH is left to the sinks alone.
	class Track
	{
	public:
		static constexpr std::size_t DEPTH = 8;

		// The hook is about to hand a_event to the source. False: too deep to follow (End then must not be called).
		bool Begin(const void* a_event)
		{
			if (depth >= DEPTH) {
				return false;
			}
			stack[depth++] = { a_event, false };
			return true;
		}

		// Our sink got a_event. True when the hook is sending it (the innermost of that address), false for an event no hook
		// sent (one raised another way, or the hook not in).
		bool Sunk(const void* a_event)
		{
			for (std::size_t i = depth; i-- > 0;) {
				if (stack[i].event == a_event) {
					stack[i].sunk = true;
					return true;
				}
			}
			return false;
		}

		// Notify returned. Whether our sink got the event.
		bool End()
		{
			return depth > 0 ? stack[--depth].sunk : true;
		}

		[[nodiscard]] std::size_t Depth() const { return depth; }

	private:
		struct Entry
		{
			const void* event = nullptr;
			bool        sunk = false;
		};
		std::array<Entry, DEPTH> stack{};
		std::size_t              depth = 0;
	};

	// What the hook and our sink saw of one event source (any thread).
	class Tally
	{
	public:
		void Sent() { sent.fetch_add(1, std::memory_order_relaxed); }
		void Stopped() { stopped.fetch_add(1, std::memory_order_relaxed); }
		void Reached() { reached.fetch_add(1, std::memory_order_relaxed); }
		void Direct() { direct.fetch_add(1, std::memory_order_relaxed); }

		[[nodiscard]] std::uint32_t SentCount() const { return sent.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint32_t StoppedCount() const { return stopped.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint32_t ReachedCount() const { return reached.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint32_t DirectCount() const { return direct.load(std::memory_order_relaxed); }

		void Clear()
		{
			sent = 0;
			stopped = 0;
			reached = 0;
			direct = 0;
		}

	private:
		std::atomic<std::uint32_t> sent{ 0 };     // the hook handed the source an event
		std::atomic<std::uint32_t> stopped{ 0 };  // ... of those, ours never got it (a sink ahead of ours stopped it)
		std::atomic<std::uint32_t> reached{ 0 };  // ... of those, our sink got it
		std::atomic<std::uint32_t> direct{ 0 };   // our sink got an event the hook did not send
	};

	// Puts a_value right behind the run of sinks at the start of the list that a_isFirst names (the engine's own, which finish
	// the event before anything else reads it), ahead of every other: the others keep their order. False when a_value is not
	// there. a_moved tells whether it stood elsewhere. A_index gets where it stands now.
	template <class T, class Pred>
	bool PlaceAfterLeading(T* a_data, std::size_t a_size, const T& a_value, Pred a_isFirst, bool* a_moved = nullptr, std::size_t* a_index = nullptr)
	{
		const auto end = a_data + a_size;
		const auto it = std::find(a_data, end, a_value);
		if (it == end) {
			return false;
		}
		const auto from = static_cast<std::size_t>(it - a_data);
		std::size_t to = 0;  // the leading run, a_value itself left out
		for (std::size_t i = 0; i < a_size; ++i) {
			if (i == from) {
				continue;
			}
			if (!a_isFirst(a_data[i])) {
				break;
			}
			++to;
		}
		if (a_moved) {
			*a_moved = from != to;
		}
		if (a_index) {
			*a_index = to;
		}
		if (from > to) {
			std::rotate(a_data + to, a_data + from, a_data + from + 1);
		} else if (from < to) {
			std::rotate(a_data + from, a_data + from + 1, a_data + to + 1);
		}
		return true;
	}

	// Where a_value stands in the list (0 first), or the size when it is not there.
	template <class T>
	std::size_t Position(const T* a_data, std::size_t a_size, const T& a_value)
	{
		return static_cast<std::size_t>(std::find(a_data, a_data + a_size, a_value) - a_data);
	}

	// One line for a list of sinks, ours marked: "3 sinks: [0] Fallout4.exe+0x1A2B (ours: no), ..." as the log shows it.
	struct SinkRow
	{
		std::string module;  // the module the sink's vtable is in (no path)
		std::uintptr_t offset = 0;  // the vtable's offset in that module
		bool ours = false;
	};

	inline std::string DescribeSinks(std::span<const SinkRow> a_rows)
	{
		std::string out = std::format("{} sink{}", a_rows.size(), a_rows.size() == 1 ? "" : "s");
		for (std::size_t i = 0; i < a_rows.size(); ++i) {
			const auto& row = a_rows[i];
			out += std::format("{} [{}] {}+{:X}{}", i == 0 ? ":" : ",", i, row.module.empty() ? "?" : row.module, row.offset, row.ours ? " (ours)" : "");
		}
		return out;
	}

	// What CopyWatch's "hit events don't reach the mod" warning adds when the hook saw hits our sink never got (a_stopped of
	// a_sent): the cause named, or nothing when the hook saw none stopped (the hits never were sent: a plugin that makes
	// them another way, or no hit at all).
	inline std::string CauseNote(std::uint32_t a_sent, std::uint32_t a_stopped)
	{
		if (a_stopped == 0) {
			return {};
		}
		return std::format(" The game sent {} hit events and a sink registered ahead of ours stopped {} of them before ours (the event tap "
		                   "handles those: hitStopped= in `net status`, the sinks listed in the log)",
			a_sent, a_stopped);
	}
}
