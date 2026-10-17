#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The host's walk for settlement junk its save had scrapped before the session. The workshop's events
// only tell of scraps made while we watch; what the host scrapped earlier is in their save, switched
// off and marked deleted (a reference from the game's files can't really be deleted: the save keeps
// both flags), and a switched-off reference sends no load event. So the host walks the references of
// the cells that attach, a few hundred a frame, and reports each piece it finds scrapped once, the way
// a scrap made in the session is reported; the server keeps it for whoever joins later. A piece counts
// only inside a workbench's build area, and the workbenches are found by the same walk (a worldspace's
// are among its persistent references, walked once): a piece found before its workbench waits for it.
// The engine is behind Engine (WorkshopSync.cpp's, or a test's stand-in), so the bookkeeping, what
// was reported and once only, per cell, is what the tests drive. An Engine gives:
//
//   std::optional<Snapshot> Refs(const Cell&)              the cell's references (IDs) and its space, if it's attached
//   bool Attached(const Cell&)                             still, a frame later (a walk is given up when it isn't)
//   std::optional<Seen> Examine(std::uint32_t ref, bool benchesOnly)   what the reference is; nothing if it's gone
//   bool Loaded(std::uint32_t ref)                         the reference is in memory here
//   bool InBuildArea(std::uint32_t bench, std::uint32_t ref)   the piece stands in the workbench's build area
//   bool Report(std::uint32_t ref, std::uint32_t bench)    reported as scrapped; false if it couldn't be
namespace SavedScraps
{
	inline constexpr std::size_t WALK_PER_FRAME = 512;    // references looked at per frame
	inline constexpr double      PER_SECOND = 100.0;      // reports, well inside the server's event budget
	inline constexpr double      BURST = 20.0;            // reports that may go at once after a pause
	inline constexpr std::size_t MAX_WAITING = 4096;      // pieces kept waiting for a workbench

	// A cell to walk: for a worldspace's persistent references, the worldspace itself (only workbenches
	// are looked for there: it holds the whole worldspace's).
	struct Cell
	{
		std::uint32_t id;
		bool          persistent;

		bool operator==(const Cell&) const = default;
	};

	// A cell's references, taken when its walk starts: the cell's own list changes as references come
	// and go, so a place in it means nothing a frame later. Each is looked up again when its turn comes.
	struct Snapshot
	{
		std::uint32_t              space;  // the interior cell, or the worldspace: workbench and junk must share it
		std::vector<std::uint32_t> refs;
	};

	// What the walk makes of a reference.
	enum class Seen
	{
		kStanding,  // not switched off (and no workbench)
		kBench,     // a settlement's workbench
		kOff,       // switched off, not deleted: the game starts plenty of references off, and a friend's scrap only switches ours off
		kDeleted,   // switched off and deleted, but no junk workshop mode scraps: a quest's or a script's doing, a loose item
		kScrapped,  // junk scrapped in workshop mode, as far as the piece itself tells
	};

	struct Bench
	{
		std::uint32_t id;
		std::uint32_t space;
	};

	struct Counts
	{
		std::uint32_t refs = 0, disabled = 0, deleted = 0, benches = 0;
	};

	// A round of walks, done: the queue ran dry (for the log).
	struct Round
	{
		Counts      counts;   // since the last round
		std::size_t toSend;   // scraps waiting to be reported
		std::size_t waiting;  // scraps in no build area of a workbench seen
	};

	// Pieces of the world reported scrapped this session, or on their way to be: by the workshop's
	// destroyed event (which may run off the game thread), or found scrapped in the save by the walk.
	// Each is reported once: the server keeps one report, and a second would only be a message for
	// nothing. One that isn't sent after all (not in a session, or gone before its turn) is let go, to be
	// reported when the walk finds it.
	class Ledger
	{
	public:
		// False if it was reported, or is on its way, already.
		bool Note(std::uint32_t a_id)
		{
			std::scoped_lock l{ lock };
			return ids.insert(a_id).second;
		}

		void Forget(std::uint32_t a_id)
		{
			std::scoped_lock l{ lock };
			ids.erase(a_id);
		}

		[[nodiscard]] bool Holds(std::uint32_t a_id)
		{
			std::scoped_lock l{ lock };
			return ids.contains(a_id);
		}

		void Clear()
		{
			std::scoped_lock l{ lock };
			ids.clear();
		}

	private:
		std::mutex                        lock;
		std::unordered_set<std::uint32_t> ids;
	};

	template <class Engine>
	class Walk
	{
	public:
		explicit Walk(Ledger& a_ledger, std::size_t a_maxWaiting = MAX_WAITING) :
			ledger(a_ledger), maxWaiting(a_maxWaiting)
		{}

		// Queues the worldspace's persistent references (the workbenches are among them), unless they
		// were walked to the end already, or wait their turn.
		void QueueWorld(std::uint32_t a_world)
		{
			const Cell cell{ a_world, true };
			if (!walkedWorlds.contains(a_world) && std::ranges::find(toWalk, cell) == toWalk.end()) {
				toWalk.push_back(cell);
			}
		}

		// Queues a cell that attached.
		void QueueCell(std::uint32_t a_cell) { toWalk.push_back({ a_cell, false }); }

		// Walks up to a_budget references of the queued cells, oldest first. The round's summary when
		// the queue ran dry in this call.
		std::optional<Round> WalkSome(Engine& a_engine, std::size_t a_budget = WALK_PER_FRAME)
		{
			std::optional<Round> round;
			while (a_budget > 0 && !toWalk.empty()) {
				const auto cell = toWalk.front();
				if (!started) {
					auto snapshot = a_engine.Refs(cell);
					if (!snapshot) {
						round = Next(a_engine, false);  // gone since the look: queued again when it attaches again
						continue;
					}
					started = true;
					space = snapshot->space;
					refs = std::move(snapshot->refs);
					at = 0;
				} else if (!a_engine.Attached(cell)) {
					round = Next(a_engine, false);  // its cell went, or the worldspace changed (queued again when we're back)
					continue;
				}
				for (; a_budget > 0 && at < refs.size(); --a_budget) {
					Examine(a_engine, refs[at++], cell.persistent);
				}
				if (at >= refs.size()) {
					round = Next(a_engine, true);
				}
			}
			return round;
		}

		// Reports what waits to be, no more than BURST at once and PER_SECOND a second: a_elapsed is the
		// seconds since the last call (0 the first time). The number reported.
		std::size_t SendSome(Engine& a_engine, double a_elapsed)
		{
			tokens = (std::min)(BURST, tokens + a_elapsed * PER_SECOND);
			std::size_t sent = 0;
			while (tokens >= 1.0 && !toSend.empty()) {
				const auto [ref, bench] = toSend.front();
				toSend.pop_front();
				if (!a_engine.Report(ref, bench)) {
					ledger.Forget(ref);  // gone before its turn, or not sent: found again when its cell attaches
					continue;
				}
				++reported;
				++sent;
				tokens -= 1.0;
			}
			return sent;
		}

		// The workbench whose build area the piece stands in (0 if none has been seen). a_space: the
		// piece's cell's.
		std::uint32_t BenchFor(Engine& a_engine, std::uint32_t a_ref, std::uint32_t a_space) const
		{
			if (a_space == 0) {
				return 0;
			}
			for (const auto& bench : benches) {
				if (bench.space == a_space && a_engine.InBuildArea(bench.id, a_ref)) {
					return bench.id;
				}
			}
			return 0;
		}

		// Forgets everything (the session ends, or a save is loaded: its cells are walked again). What
		// waited, or was queued to go, is let go in the ledger: found again by the next walk. What was
		// reported stays reported.
		void Clear()
		{
			walkedWorlds.clear();
			toWalk.clear();
			refs.clear();
			at = 0;
			started = false;
			benches.clear();
			for (const auto& [id, inSpace] : waiting) {
				ledger.Forget(id);
			}
			waiting.clear();
			for (const auto& [id, bench] : toSend) {
				ledger.Forget(id);
			}
			toSend.clear();
			roundStart = seen;
		}

		[[nodiscard]] const std::vector<Bench>& Benches() const { return benches; }
		[[nodiscard]] const Counts&             SeenCounts() const { return seen; }
		[[nodiscard]] std::uint32_t             Reported() const { return reported; }
		[[nodiscard]] std::size_t               Waiting() const { return waiting.size(); }
		[[nodiscard]] std::size_t               ToSend() const { return toSend.size(); }
		[[nodiscard]] std::size_t               Queued() const { return toWalk.size(); }
		[[nodiscard]] bool                      Walked(std::uint32_t a_world) const { return walkedWorlds.contains(a_world); }

	private:
		// Queues a piece already noted in the ledger for reporting if it stands in a workbench's build
		// area; true when it's done with (queued, or gone from the game: let go, and found again when its
		// cell attaches).
		bool QueueScrap(Engine& a_engine, std::uint32_t a_ref, std::uint32_t a_space)
		{
			if (!a_engine.Loaded(a_ref)) {
				ledger.Forget(a_ref);
				return true;
			}
			const auto bench = BenchFor(a_engine, a_ref, a_space);
			if (bench == 0) {
				return false;
			}
			toSend.emplace_back(a_ref, bench);
			return true;
		}

		// a_persistent: a worldspace's persistent references: only workbenches are looked for there.
		void Examine(Engine& a_engine, std::uint32_t a_ref, bool a_persistent)
		{
			const auto seenAs = a_engine.Examine(a_ref, a_persistent);
			if (!seenAs) {
				return;  // gone since the snapshot
			}
			++seen.refs;
			switch (*seenAs) {
			case Seen::kBench:
				if (std::ranges::none_of(benches, [&](const Bench& a_bench) { return a_bench.id == a_ref; })) {
					benches.push_back({ a_ref, space });
					++seen.benches;
				}
				return;
			case Seen::kStanding:
				return;
			default:
				break;
			}
			if (a_persistent) {
				return;
			}
			++seen.disabled;
			if (*seenAs == Seen::kOff) {
				return;
			}
			++seen.deleted;
			if (*seenAs != Seen::kScrapped || !ledger.Note(a_ref)) {
				return;  // not scrapped junk; or reported already, on its way, or waiting
			}
			if (!QueueScrap(a_engine, a_ref, space)) {
				if (waiting.size() < maxWaiting) {
					waiting.emplace(a_ref, space);
				} else {
					ledger.Forget(a_ref);
				}
			}
		}

		// Done with the first queued walk: to the end, or given up. The round's summary if it was the last.
		std::optional<Round> Next(Engine& a_engine, bool a_finished)
		{
			if (a_finished && toWalk.front().persistent) {
				walkedWorlds.insert(toWalk.front().id);
			}
			toWalk.pop_front();
			refs.clear();
			at = 0;
			started = false;
			if (!toWalk.empty()) {
				return std::nullopt;
			}
			// Workbenches seen in this round may have pieces found before them in their build area.
			std::erase_if(waiting, [&](const auto& a_entry) { return QueueScrap(a_engine, a_entry.first, a_entry.second); });
			const Round round{ { seen.refs - roundStart.refs, seen.disabled - roundStart.disabled, seen.deleted - roundStart.deleted, seen.benches - roundStart.benches },
				toSend.size(), waiting.size() };
			roundStart = seen;
			return round;
		}

		Ledger&     ledger;
		std::size_t maxWaiting;

		std::set<std::uint32_t>                             walkedWorlds;  // worldspaces whose persistent references were walked to the end
		std::deque<Cell>                                    toWalk;
		std::vector<std::uint32_t>                          refs;  // the first queued cell's references, taken when its walk starts
		std::size_t                                         at = 0;  // the next of them to look at
		std::uint32_t                                       space = 0;  // the cell's
		bool                                                started = false;
		std::vector<Bench>                                  benches;  // workbenches seen
		std::unordered_map<std::uint32_t, std::uint32_t>    waiting;  // scrapped, with no workbench's build area around it seen yet: reference -> its space
		std::deque<std::pair<std::uint32_t, std::uint32_t>> toSend;   // (reference, its workbench)
		double                                              tokens = 0.0;
		std::uint32_t                                       reported = 0;
		Counts                                              seen;        // by the walk: references of the game's files, switched off, also deleted, workbenches
		Counts                                              roundStart;  // `seen` when the walk's queue last ran dry, for the log
	};
}
