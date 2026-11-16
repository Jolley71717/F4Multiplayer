#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// The build each NPC with mods of its own (a robot's parts from the robot workbench) has in the session, kept by the
// server (protocol 83). Each game's copy of a robot is built as that game's world has it, and the gear report of the game
// that runs it (NpcEquipment::actorMods) is what every other game gives its copy. Taken as it came, a hand-off made the
// new runner's build everyone's: a copy that hadn't taken the session's build yet (it was fighting, or half way through a
// swap) went into the previous runner's own world, over the parts its player had built. So the first runner's build is
// the session's, as its roll is (common/Server.cpp's npcIdentities), and a later runner's different one goes no further:
// the session's is passed on in its place. It is replaced only by the game that keeps it (the last to set it), by the
// NPC's companion owner (their robot, as their player built it), or by a runner whose game saw the build change while it
// ran the NPC (NpcEquipment::modsChanged: the robot workbench). Kept for the session's life (through releases and
// leaves), never saved. Kept free of the game so it can be tested.
namespace SessionBuilds
{
	using Mod = Protocol::ItemMod;

	// NPCs with a build kept: past it a report goes on as it came (logged once).
	inline constexpr std::size_t MAX_BUILDS = 4096;

	// A build as a set of (mod, attach index): the order a game's instance lists them in is the order they were attached
	// there, and a mod named twice at one index is one part.
	inline std::vector<std::pair<std::uint32_t, std::uint8_t>> Parts(std::span<const Mod> a_mods)
	{
		std::vector<std::pair<std::uint32_t, std::uint8_t>> out;
		out.reserve(a_mods.size());
		for (const auto& mod : a_mods) {
			out.emplace_back(mod.mod, mod.index);
		}
		std::ranges::sort(out);
		const auto [first, last] = std::ranges::unique(out);
		out.erase(first, last);
		return out;
	}

	// The same robot: the same mods at the same attach indexes, in any order.
	inline bool Same(std::span<const Mod> a_a, std::span<const Mod> a_b)
	{
		return Parts(a_a) == Parts(a_b);
	}

	struct Build
	{
		std::uint32_t    playerId = 0;  // the game that set it last: its later reports replace it
		std::vector<Mod> mods;
	};

	enum class Verdict : std::uint8_t
	{
		kNone,      // the report says no mods and none is kept: passed on as it came
		kFilled,    // the report says no mods, or an empty build, while one is kept: the session's go in their place
		kKept,      // the first build of the NPC: the session's now
		kSame,      // the session's build again
		kReplaced,  // a new build from its keeper, its companion owner, or a change seen where it runs
		kRefused,   // another runner's different build: the session's go in its place
		kFull,      // none kept, and no room: passed on as it came
	};

	class Book
	{
	public:
		// A runner's report of a_report.refId, from a_playerId's game (a_companionOwner: the NPC is that player's companion).
		// a_report is what the others are told: its mods are the session's after this, whenever one is kept.
		Verdict Judge(std::uint32_t a_playerId, bool a_companionOwner, Protocol::NpcEquipment& a_report)
		{
			const auto it = builds.find(a_report.refId);
			// A report of no mods while a build is kept is taken as unread, whoever sends it: the robot workbench can't make a
			// robot with no parts, so it is a read made before the robot's instance was there, and as a build it would have
			// every mirror take off every part.
			if (!a_report.modsRead || (it != builds.end() && a_report.actorMods.empty())) {
				if (it == builds.end()) {
					return Verdict::kNone;
				}
				Fill(a_report, it->second);
				return Verdict::kFilled;
			}
			if (it == builds.end()) {
				if (a_report.actorMods.empty()) {
					return Verdict::kNone;  // no parts of its own: anyone but a robot, nothing to keep
				}
				if (builds.size() >= MAX_BUILDS) {
					return Verdict::kFull;
				}
				builds[a_report.refId] = { a_playerId, a_report.actorMods };
				return Verdict::kKept;
			}
			auto& build = it->second;
			if (Same(build.mods, a_report.actorMods)) {
				a_report.modsChanged = false;
				return Verdict::kSame;
			}
			if (build.playerId == a_playerId || a_companionOwner || a_report.modsChanged) {
				build = { a_playerId, a_report.actorMods };
				return Verdict::kReplaced;
			}
			Fill(a_report, build);
			return Verdict::kRefused;
		}

		[[nodiscard]] const Build* Find(std::uint32_t a_refId) const
		{
			const auto it = builds.find(a_refId);
			return it != builds.end() ? &it->second : nullptr;
		}

		// A report told again (a newcomer's): the session's build in it, whatever its runner said.
		void Tell(Protocol::NpcEquipment& a_report) const
		{
			if (const auto it = builds.find(a_report.refId); it != builds.end()) {
				Fill(a_report, it->second);
			}
		}

		// It died: the next one a cell reset brings is built by its world again.
		void Forget(std::uint32_t a_refId) { builds.erase(a_refId); }

		[[nodiscard]] std::size_t Size() const { return builds.size(); }

		void Clear() { builds.clear(); }

	private:
		static void Fill(Protocol::NpcEquipment& a_report, const Build& a_build)
		{
			a_report.modsRead = true;
			a_report.modsChanged = false;
			a_report.actorMods = a_build.mods;
		}

		std::unordered_map<std::uint32_t, Build> builds;
	};
}
