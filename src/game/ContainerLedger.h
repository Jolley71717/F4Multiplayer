#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

// Which of the session's container changes this world contains. The server numbers every change to a
// container (loot taken, items put in) in session order; everything before Next() is in our world already,
// or waiting in Waiting() for its container to load. That belongs to the world rather than the connection,
// so it goes into the save (WorldSync's co-save record) and comes back with it, and a save loaded later
// asks the server only for the changes after it. Replaying one twice takes the loot twice (or duplicates
// what was put in). WorldSync's bookkeeping, kept free of the game so it can be tested.
class ContainerLedger
{
public:
	struct Pending
	{
		std::uint32_t             index;
		Protocol::ContainerChange change;
	};

	enum class Offered
	{
		kHave,     // already in our world (or waiting): nothing done
		kOurs,     // made in our own world, so it's in it
		kInvalid,  // nothing sensible to apply
		kApplied,
		kWaiting,  // its container isn't there yet (or earlier changes to it wait): kept until it is
	};

	// The server welcomed us into a_session as a_playerId. A session other than the one our world's record
	// is about: none of its changes are in our world yet.
	void OnWelcome(std::uint64_t a_session, std::uint32_t a_playerId)
	{
		liveSession = a_session;
		if (a_session != recordSession) {
			recordSession = a_session;
			next = 0;
			pending.clear();
			gaps.clear();
			ownIds.clear();
		}
		ownIds.insert(a_playerId);
	}

	// The connection is gone. The record stays: it is our world's, and a reconnect carries on from it.
	void OnDisconnected() { liveSession = 0; }

	// The game threw its world away (a load or a new game begins): until a save's record says otherwise,
	// the world holds none of any session's changes.
	void OnRevert()
	{
		recordSession = 0;
		next = 0;
		pending.clear();
		gaps.clear();
	}

	// The loaded save's record.
	void OnRecordLoaded(std::uint64_t a_session, std::uint32_t a_next, std::vector<Pending> a_pending)
	{
		recordSession = a_session;
		next = a_next;
		pending = std::move(a_pending);
		gaps.clear();
	}

	// A save was loaded (or a new game started) while we are in a session. A save without that session's
	// record holds none of its changes; from here on they are counted for that session, from the first, so
	// the next save records the session and loading that save asks only for what came after it. (Left at
	// the save's record, every later save had none, and each reload or reconnect replayed every change
	// again.) Returns whether the record changed.
	bool BindToLiveSession()
	{
		if (liveSession == 0 || recordSession == liveSession) {
			return false;
		}
		recordSession = liveSession;
		next = 0;
		pending.clear();
		gaps.clear();
		return true;
	}

	// What to ask the server for (in the Hello, and after a load).
	[[nodiscard]] Protocol::WorldRequest ResyncPoint() const { return { recordSession, next }; }

	// After a load, once the save's record is in (OnRevert, then OnRecordLoaded for a save that has one) and the
	// game has left the session if the save isn't the session's world (CharacterBinding): still in a session, the
	// world is bound to it (BindToLiveSession) and this is what to ask the server for.
	[[nodiscard]] std::optional<Protocol::WorldRequest> AfterLoad()
	{
		if (liveSession == 0) {
			return std::nullopt;
		}
		BindToLiveSession();
		return ResyncPoint();
	}

	// Our player IDs in this session. A reconnect normally gets the old one back (the server knows us by
	// Protocol::Hello::identity), but not while the server still holds the old connection; changes under
	// either ID are ours.
	[[nodiscard]] bool Ours(std::uint32_t a_playerId) const { return ownIds.contains(a_playerId); }

	// A world state's chunk, before its changes are offered. Its first index is where the server's table
	// starts for us, and a chunk without changes says the table ends there. Should we count more than that
	// (the host's game quit before the server wrote its last changes, and our save has them), the server
	// numbers its next changes from there, and they must not be taken for ones we have. Returns true when
	// we were ahead.
	bool OnWorldState(std::uint32_t a_firstIndex, bool a_hasChanges)
	{
		if (a_hasChanges || a_firstIndex >= next) {
			return false;
		}
		next = a_firstIndex;
		std::erase_if(gaps, [&](const auto& a_gap) { return a_gap.first >= next; });
		for (auto& gap : gaps) {
			gap.second = (std::min)(gap.second, next);
		}
		return true;
	}

	// A session container change reached us (live, or in a world state). a_tryApply(change) applies it to
	// our world and returns false when it can't yet (its container isn't loaded).
	template <class TryApply>
	Offered Offer(std::uint32_t a_index, std::uint32_t a_playerId, const Protocol::ContainerChange& a_change, TryApply&& a_tryApply)
	{
		if (a_index >= next) {
			// A live change past the ones we have: those between are on their way in the world state we
			// asked for after a load (the server sent this one before it had our request).
			if (a_index > next) {
				AddGap(next, a_index);
			}
			next = a_index + 1;
		} else if (!TakeFromGap(a_index)) {
			return Offered::kHave;
		}
		if (Ours(a_playerId)) {
			return Offered::kOurs;
		}
		if (!Protocol::IsValid(a_change)) {
			return Offered::kInvalid;
		}
		// Changes to one container stay in order; different containers don't wait for each other.
		const bool waiting = std::ranges::any_of(pending, [&](const Pending& a_pending) { return a_pending.change.container == a_change.container; });
		if (waiting || !a_tryApply(a_change)) {
			pending.push_back({ a_index, a_change });
			return Offered::kWaiting;
		}
		return Offered::kApplied;
	}

	struct Taken
	{
		bool          wasAhead = false;  // we counted more changes than the server has (see OnWorldState)
		std::uint32_t ours = 0;          // changes of our own, skipped
	};

	// A world state's container changes (the answer to a Hello or to AfterLoad's request), offered in order.
	template <class TryApply>
	Taken TakeWorldState(const Protocol::WorldState& a_state, TryApply&& a_tryApply)
	{
		Taken taken;
		taken.wasAhead = OnWorldState(a_state.containerFirstIndex, !a_state.containerChanges.empty());
		for (const auto& change : a_state.containerChanges) {
			taken.ours += Offer(change.index, change.playerId, change.change, a_tryApply) == Offered::kOurs;
		}
		return taken;
	}

	// Applies what waits, in order per container.
	template <class TryApply>
	void Retry(TryApply&& a_tryApply)
	{
		std::unordered_set<std::uint32_t> blocked;
		std::erase_if(pending, [&](const Pending& a_pending) {
			if (blocked.contains(a_pending.change.container)) {
				return false;
			}
			if (a_tryApply(a_pending.change)) {
				return true;
			}
			blocked.insert(a_pending.change.container);
			return false;
		});
	}

	[[nodiscard]] std::uint64_t               RecordSession() const { return recordSession; }
	[[nodiscard]] std::uint32_t               Next() const { return next; }
	[[nodiscard]] const std::vector<Pending>& Waiting() const { return pending; }

private:
	static constexpr std::size_t MAX_GAPS = 64;  // each is a range; many only if live changes keep jumping ahead

	void AddGap(std::uint32_t a_from, std::uint32_t a_to)
	{
		if (gaps.size() >= MAX_GAPS) {
			gaps.erase(gaps.begin());  // the oldest: its changes are as good as lost, as they were before gaps were kept
		}
		gaps.emplace_back(a_from, a_to);
	}

	bool TakeFromGap(std::uint32_t a_index)
	{
		const auto it = std::ranges::find_if(gaps, [&](const auto& a_gap) { return a_index >= a_gap.first && a_index < a_gap.second; });
		if (it == gaps.end()) {
			return false;
		}
		const auto [from, to] = *it;
		gaps.erase(it);
		if (from < a_index) {
			gaps.emplace_back(from, a_index);
		}
		if (a_index + 1 < to) {
			gaps.emplace_back(a_index + 1, to);
		}
		return true;
	}

	std::uint64_t                                      liveSession = 0;    // the session we're welcomed into (0: none)
	std::uint64_t                                      recordSession = 0;  // the session next and pending count for
	std::uint32_t                                      next = 0;
	std::vector<Pending>                               pending;
	std::vector<std::pair<std::uint32_t, std::uint32_t>> gaps;  // [from, to): changes below next we haven't had yet
	std::unordered_set<std::uint32_t>                  ownIds;
};
