#include "game/Spawns.h"

#include "game/CopyKill.h"
#include "game/Dialogue.h"
#include "game/EncounterRuns.h"
#include "game/Encounters.h"
#include "game/EngineRefs.h"
#include "game/Equipment.h"
#include "game/FrameLoan.h"
#include "game/FrameLoans.h"
#include "game/FrameShare.h"
#include "game/NpcSync.h"
#include "game/Official.h"
#include "game/OpenMenus.h"
#include "game/Puppets.h"
#include "game/QuestActors.h"
#include "game/QuestFilter.h"
#include "game/QuestSync.h"
#include "game/RadiantSync.h"
#include "game/RefTag.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/RollHook.h"
#include "game/Rolls.h"
#include "game/SaveGuard.h"
#include "game/SettlerSpawns.h"
#include "game/SpawnBook.h"
#include "game/StageRules.h"
#include "game/Story.h"
#include "game/WorkshopSync.h"
#include "game/WorldSync.h"

#include <mutex>

namespace Spawns
{
	namespace
	{
		using Clock = SpawnBook::Clock;

		// Copies placed in one frame at most: a fight's worth arriving at once is placed over a few frames.
		constexpr int           PLACE_PER_FRAME = 2;
		constexpr std::size_t   MAX_KILLED_HERE = 256;
		constexpr std::uint32_t LOGGED = 20;  // lines about single spawns (the counters go on)

		SpawnBook::Ours   ours;
		SpawnBook::Theirs theirs;

		// Our copies of friends' spawns, by spawn id: the handle (its id alone may name something else once it is gone).
		// The id and base it was placed under go with it (see HeldCopy): the engine unloads a copy with its cell, its handle
		// stops resolving and the cell keeps its record, which brought it back beside the next copy placed (Refresh).
		struct Placed
		{
			RE::ObjectRefHandle handle;
			Clock::time_point   placedAt{};
			HeldCopy::Ids       ids;
		};
		std::unordered_map<std::uint32_t, Placed> placed;

		// Friends' spawns our game's own creature from the same quest alias stands for (QuestActors.h), by spawn id: ours,
		// paired with theirs (Theirs::Placed names it as our copy, so NpcSync mirrors it and deaths and hits go by the spawn
		// id). Never in placed: never deleted, never kept out of a save. Unpaired, it is our game's alone again.
		std::unordered_map<std::uint32_t, RE::ObjectRefHandle> paired;
		// When each friend's spawn named with an alias was last looked at for ours to pair (once a PAIR_LOOK at most), and
		// whether ours was there (alive in that alias) though not to be paired yet: no copy beside it meanwhile.
		struct PairLook
		{
			Clock::time_point at{};
			bool              oursThere = false;
		};
		std::unordered_map<std::uint32_t, PairLook> pairLooked;
		constexpr auto                              PAIR_LOOK = std::chrono::seconds(1);
		std::uint32_t                               pairs = 0, unpairs = 0, keyedReported = 0, twinsHeld = 0, rollsRestored = 0;
		std::unordered_map<std::uint32_t, Protocol::NpcIdentity> pairedOwn;  // by spawn id: ours as our game made it, before pairing
		std::unordered_set<std::uint32_t>           twinsNoted;  // our NPCs held back as twins, each counted once

		// Random encounters (Encounters.h): our runs and their actors, friends' encounter spawns in the server's order, and
		// the runs of ours we stopped for a friend's. By spawn id, the number each friend's spawn arrived as (Book::Next);
		// by spawn id, the encounter quest each of our offered actors is of, so its grant can take its number.
		Encounters::Book                                 encounters;
		std::unordered_map<std::uint32_t, std::uint64_t> theirSeq;
		std::unordered_map<std::uint32_t, std::uint32_t> offeredEncounter;
		// Of those, the ones offered calm (neither fighting nor hostile), by spawn id: at most Encounters::MAX_CALM at once,
		// so a fight's spawns are never refused for a crowd of bystanders; our calm actors held back for it, each noted once.
		std::unordered_set<std::uint32_t> calmOffered;
		std::unordered_set<std::uint32_t> calmHeldNoted;
		// Runs of ours to stop for a friend's (StopEncounter): stopped from Frame, never from inside the actor scan.
		std::vector<std::uint32_t> pendingStops;
		// Runs whose actors go and whose quest keeps running (StopQuestRun's a_actorsOnly): done with the stops.
		std::vector<std::uint32_t> pendingSweeps;
		// Our encounter actors as seen in the scan, by their ids, with their handles: the book names a stopped run's
		// actors by id, and the id alone could name another NPC by the time the stop is done (RemoveDoomed). Pruned as
		// their handles stop resolving (CheckRuns); never many (Encounters::MAX_ACTORS a run).
		constexpr std::size_t                                   MAX_ENCOUNTER_ACTORS = 256;
		std::unordered_map<std::uint32_t, RE::ObjectRefHandle> encounterActors;
		// Actors of stopped runs seen in the scan after the stop, with their quest (for the log): removed from Frame.
		struct Removal
		{
			RE::ObjectRefHandle handle;
			std::uint32_t       quest = 0;
		};
		std::vector<Removal> pendingRemovals;
		// The encounter quests running on the first in-world frame of a session, or after a load, are settled in the book
		// (never stopped): the book can't tell when they began. Set until that frame has been.
		bool                       settleRuns = true;
		Clock::time_point          runsChecked{};  // when our runs' quests were last checked to be running still
		constexpr auto             RUNS_CHECK = std::chrono::seconds(1);
		std::uint32_t              encountersShared = 0, encountersReceived = 0, encountersStopped = 0, calmHeld = 0, kindsVetoed = 0;
		// Our encounter actors not offered yet while they stand out Encounters::SHARE_SETTLE (a corpse the run makes dies meanwhile).
		Encounters::ShareSettle    shareSettle;
		std::uint32_t              radiantActorsShared = 0;  // actors of our own shared radiants' runs, made by their start (OwnedRadiantOf)
		// The settlers the host's game made at run time (SettlerSpawns.h): ours shared as settlers; our calm ones that found
		// no settler's place (each noted once); friends' taken as settlers; friends' taken as plain (their workbench is none
		// of our game's files').
		std::uint32_t                     settlersShared = 0, settlersHeld = 0, settlersReceived = 0, settlersVetoed = 0;
		std::unordered_set<std::uint32_t> settlersHeldNoted;
		// Runs of ours that ended (their quest stopped) and their actors shared on plain; actors of runs we stopped that we
		// removed; friends' encounters heard to be over.
		std::uint32_t encountersOver = 0, sharedOn = 0, actorsRemoved = 0, theirsOver = 0;
		// Runtime NPCs that hit our player (WorldSync's hit watch, any thread), for Frame to find their encounter runs
		// (Encounters::Book::OursHurt); runs that stood so; our own actors of a friend's run we follow, removed
		// (EncounterRuns.h: the owner's actors, our copies, are what stand there).
		std::mutex                 hitsLock;
		std::vector<std::uint32_t> hitsOnUs;
		constexpr std::size_t      MAX_HITS_ON_US = 64;
		std::vector<Removal>       followRemovals;
		std::uint32_t              runsHurt = 0, followRemoved = 0, followKept = 0;

		// When each friend's spawn was first seen here: its copy waits a moment for their roll of it (NpcRoll::IDENTITY_WAIT).
		std::unordered_map<std::uint32_t, Clock::time_point> seenAt;

		// What our copy of a friend's spawn is placed from: the NPC their game placed it as, which ours rolls and their
		// roll then makes theirs, when their roll of it came and that is a leveled NPC here; else the spawn's base.
		std::uint32_t PlaceBase(std::uint32_t a_id, std::uint32_t a_base)
		{
			const auto roll = NpcSync::RollOf(a_id);
			return NpcRoll::PlaceFrom(roll, a_base, roll && Rolls::IsLeveledNpc(roll->originalBase));
		}

		// Our NPCs killed for another game's report (a friend's kill of ours, or our copy's death in its owner's game):
		// their deaths here are not reported back.
		std::unordered_set<std::uint32_t> killedHere;

		// Our copies on their way out (in the stand-ins' graveyard until it is safe to delete them): a runtime NPC our
		// game has, hostile to our player, but never ours to share. Forgotten once the handle no longer finds it (its id
		// may then name a new NPC).
		constexpr std::size_t                                   MAX_LEAVING = 512;
		std::unordered_map<std::uint32_t, RE::ObjectRefHandle> leaving;

		bool Leaving(std::uint32_t a_local)
		{
			const auto it = leaving.find(a_local);
			if (it == leaving.end()) {
				return false;
			}
			if (const auto ref = it->second.get(); ref && ref->GetFormID() == a_local) {
				return true;
			}
			leaving.erase(it);
			return false;
		}

		std::vector<std::vector<std::uint8_t>> outgoing;

		// Power armor frames our game made that it reports or shares (answered or not), by their ids here: no more than
		// FrameShare::MAX_MADE at once.
		std::unordered_set<std::uint32_t> ownFrames;

		std::uint32_t reported = 0, granted = 0, refused = 0, received = 0;
		std::uint32_t copiesPlaced = 0, placeFailed = 0, copiesDiscarded = 0, copiesLost = 0, copiesKilled = 0, oursKilled = 0;
		std::uint32_t framesReported = 0, frameCopiesPlaced = 0, frameCopiesMoved = 0, frameCopiesTaken = 0;
		std::uint32_t copiesUnloaded = 0, copiesBack = 0;      // copies the engine unloaded with their cell: forgotten, or held again (Refresh)
		std::uint32_t copiesRolled = 0, copiesNotRolled = 0;  // copies of leveled spawns built from their owner's roll as placed (RollHook), or not
		// Friends' frames a frame of our own stands for (SpawnBook::FrameFacts::oursThere), by spawn id while it does: no copy.
		std::unordered_set<std::uint32_t> oursThere;
		std::uint32_t                     frameCopiesWithheld = 0;
		// Friends' NPC spawns not placed yet: when we last looked for one of our own of the same encounter where their game
		// has it (OwnMadeNpcAt, at most every OURS_THERE_LOOK while the copy would be placed), and what we found (0: none).
		// Not kept past that: ours gone (dead, or off fighting elsewhere), the copy is placed. How many times one was found.
		struct OursLook
		{
			Clock::time_point at{};
			std::uint32_t     own = 0;
		};
		std::unordered_map<std::uint32_t, OursLook> oursThereLooked;
		std::uint32_t                               npcCopiesWithheld = 0;
		constexpr auto                              OURS_THERE_LOOK = std::chrono::seconds(1);
		// Friends' frames whose copy would stand in our player's way out of the frame a loaded save had them in
		// (SpawnBook::FrameFacts::keptClear), by spawn id while it would: no copy.
		std::unordered_set<std::uint32_t> keptClear;
		std::uint32_t                     frameCopiesKeptClear = 0;
		std::uint32_t logged = 0;

		std::uint32_t LocalPlayer()
		{
			return NpcSync::LocalPlayer();
		}

		// Our copy of spawn a_id, kept true once its handle stops resolving (see HeldCopy): the same copy back under its id
		// with its cell is held again; one gone is forgotten (SaveGuard drops the record its cell keeps, so the cell never
		// brings it back beside a new copy), and the book then finds no copy there (kLost).
		void Refresh(std::uint32_t a_id, Placed& a_placed)
		{
			const auto local = a_placed.ids.id;
			switch (SaveGuard::Refresh(a_placed.handle, a_placed.ids)) {
			case HeldCopy::Lost::kNone:
				break;
			case HeldCopy::Lost::kBack:
				++copiesBack;
				REX::INFO("Spawns: {:08X}, our copy of spawn {:08X}, came back with its cell; held again, no second one placed", local, a_id);
				break;
			case HeldCopy::Lost::kForget:
				++copiesUnloaded;
				REX::INFO("Spawns: {:08X}, our copy of spawn {:08X}, is gone from under us (unloaded with its cell); forgotten, its record dropped", local, a_id);
				break;
			}
		}

		// Our copy of spawn a_id, if one is placed and still there (see Refresh).
		RE::NiPointer<RE::TESObjectREFR> CopyOf(std::uint32_t a_id)
		{
			const auto it = placed.find(a_id);
			if (it == placed.end()) {
				return {};
			}
			Refresh(a_id, it->second);
			return it->second.handle.get();
		}

		RemoteEdits::Spot SpotOf(const RE::TESObjectREFR* a_ref)
		{
			const auto  cell = a_ref->GetParentCell();
			const auto& at = a_ref->data.location;
			if (cell && cell->IsInterior()) {
				return { cell->GetFormID(), 0, at.x, at.y, at.z };
			}
			return { 0, cell && cell->worldSpace ? cell->worldSpace->GetFormID() : 0, at.x, at.y, at.z };
		}

		SpawnBook::Base BaseOf(RE::TESForm* a_form)
		{
			const auto npc = a_form ? a_form->As<RE::TESNPC>() : nullptr;
			if (!npc) {
				return { a_form ? a_form->GetFormID() : 0, false, false };
			}
			return { npc->GetFormID(), true, npc->UsesTemplate() };
		}

		// What every game places a copy of this NPC from (SpawnBook::PickBase), 0 if nothing.
		std::uint32_t BaseToPlace(RE::Actor* a_actor)
		{
			std::array<SpawnBook::Base, 13> templates{};
			std::size_t                     count = 0;
			SpawnBook::Base                 original{};
			if (a_actor->extraList) {
				if (const auto leveled = a_actor->extraList->GetByType<RE::ExtraLeveledCreature>()) {
					original = BaseOf(leveled->originalBase);
					for (const auto base : leveled->templates) {
						if (base) {
							templates[count++] = BaseOf(base);
						}
					}
				}
			}
			return SpawnBook::PickBase(BaseOf(a_actor->GetNPC()), std::span{ templates.data(), count }, original);
		}

		// Ours to share: an NPC our game made that is fighting, or hostile to our player (a random encounter's raiders,
		// an ambush), and alive. Never our companion (nor a stand-in, a copy of ours or the player: Share).
		bool OursToShare(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			return !a_actor->IsDead(false) && !NpcSync::IsOurCompanion(a_actor) && (a_actor->IsInCombat() || a_actor->GetHostileToActor(a_player));
		}

		// Our runtime NPCs that are hostile or fighting but cannot be reported (SpawnBook::ShareBlock), by their ids here:
		// each is said once, for a few lines in all, and counted (`net status`: spawns notShared=). A friend does not see
		// these, which is how "the host could not see the Mr Gutsy" would show.
		constexpr std::size_t           MAX_BLOCKED_NOTED = 256;
		std::unordered_set<std::uint32_t> blockedNoted;
		std::uint32_t                   blocked = 0, blockedLogged = 0;

		// What the actor's base is put together from, for the log: its own base, the NPC it was placed as, and its templates.
		std::string DescribeBase(RE::Actor* a_actor)
		{
			const auto npc = a_actor->GetNPC();
			std::string out = std::format("own {:08X}{}", npc ? npc->GetFormID() : 0, npc && npc->UsesTemplate() ? " (uses a template)" : "");
			if (a_actor->extraList) {
				if (const auto leveled = a_actor->extraList->GetByType<RE::ExtraLeveledCreature>()) {
					out += std::format(", placed as {:08X}", leveled->originalBase ? leveled->originalBase->GetFormID() : 0);
					for (const auto base : leveled->templates) {
						if (base) {
							const auto templated = base->As<RE::TESNPC>();
							out += std::format(", template {:08X}{}", base->GetFormID(), templated ? (templated->UsesTemplate() ? " (has one of its own)" : "") : " (not an NPC)");
						}
					}
				} else {
					out += ", no leveled data";
				}
			}
			return out;
		}

		void NoteBlocked(RE::Actor* a_actor, std::uint32_t a_local, SpawnBook::Block a_block, std::string_view a_why)
		{
			if (blockedNoted.size() >= MAX_BLOCKED_NOTED) {
				blockedNoted.clear();
			}
			if (!blockedNoted.insert(a_local).second) {
				return;
			}
			++blocked;
			if (blockedLogged >= LOGGED) {
				return;
			}
			++blockedLogged;
			const auto name = a_actor->GetDisplayFullName();
			REX::INFO("Spawns: {:08X} '{}' (race {:08X}) is {}, but not shared (so far): {}; friends' games do not see it ({})", a_local, name ? name : "", a_actor->race ? a_actor->race->GetFormID() : 0, a_why,
				SpawnBook::BlockName(a_block), DescribeBase(a_actor));
		}

		// Friends' plugin NPCs our game has no reference of (Absent): booked (each time, a change of hands books one afresh),
		// and those nothing names a base for, by id, with their first such state: said once (a few lines in all) once it has
		// stayed so past the wait for its roll (NpcRoll::IDENTITY_WAIT, the roll follows the claim), as long as it stays so.
		struct NoBase
		{
			Clock::time_point since{};
			bool              said = false;
		};
		std::uint32_t                             absentBooked = 0, absentLogged = 0;
		std::unordered_map<std::uint32_t, NoBase> absentNoBase;

		// Friends' spawns that have no copy here and are not about to get one, by spawn id, with the reason last said
		// (SpawnBook::Theirs::Why): a reason is logged once, after the spawn has been known UNPLACED_AFTER, a few lines for
		// each reason in all (an owner's stand-in walking off from its spawns again and again must not use up the lines a
		// unique base or a failed placing needs), and counted in `net status` (unplaced=). A spawn merely far from our
		// player is the usual case and only counted.
		constexpr auto                                                    UNPLACED_AFTER = std::chrono::seconds(3);
		std::unordered_map<std::uint32_t, SpawnBook::Theirs::Why>        unplacedWhy;
		std::array<std::uint32_t, static_cast<std::size_t>(SpawnBook::Theirs::Why::kUnknown) + 1> unplacedLogged{};  // lines said, for each reason

		void NoteUnplaced(std::uint32_t a_id, const SpawnBook::Theirs::Copy& a_copy, SpawnBook::Theirs::Why a_why, const SpawnBook::Facts& a_facts, Clock::time_point a_now)
		{
			using Why = SpawnBook::Theirs::Why;
			if (a_why == Why::kRollWait || a_why == Why::kRetryWait || a_why == Why::kPlace || a_why == Why::kHasCopy) {
				unplacedWhy.erase(a_id);
				return;
			}
			const auto seen = seenAt.find(a_id);
			if (seen == seenAt.end() || a_now - seen->second < UNPLACED_AFTER) {
				return;
			}
			auto& last = unplacedWhy.try_emplace(a_id, Why::kPlace).first->second;  // kPlace: none said yet
			if (last == a_why) {
				return;
			}
			last = a_why;
			auto& said = unplacedLogged[static_cast<std::size_t>(a_why)];
			if (a_why == Why::kFarFromUs || said >= LOGGED) {
				return;
			}
			++said;
			const float fromUs = RemoteEdits::Distance(a_copy.at, a_facts.player);
			const float fromOwner = a_facts.owner ? RemoteEdits::Distance(*a_facts.owner, a_copy.at) : -1.0f;
			REX::INFO("Spawns: player {}'s {:08X} (base {:08X}) has no copy here: {} ({:.0f} units from our player, {:.0f} from its owner's stand-in, -1 if not heard)", a_copy.spawn.playerId, a_id, a_copy.spawn.base,
				SpawnBook::Theirs::WhyName(a_why), fromUs, fromOwner);
		}

		// For `net status`: our runtime NPCs that are hostile but cannot be shared, and the spawns of friends with no copy here
		// for a lasting reason: their base is no NPC we place / it is far from our player / its owner's stand-in is unheard or far.
		std::string Unplaced()
		{
			using Why = SpawnBook::Theirs::Why;
			int base = 0, far = 0, owner = 0;
			for (const auto& [id, why] : unplacedWhy) {
				base += why == Why::kBaseNotOk || why == Why::kGaveUp;
				far += why == Why::kFarFromUs;
				owner += why == Why::kOwnerUnheard || why == Why::kOwnerFar;
			}
			return std::format(" notShared={} unplaced={}/{}/{}", blocked, base, far, owner);
		}

		// Deaths reported by other games that wait to land on our reference (CopyKill.h): our copy of a friend's spawn, or our
		// own one a friend killed first. Kills made, protections lifted for them, and deaths that never landed.
		CopyKill::Book pendingKills;
		std::uint32_t  killsTried = 0, protectionsLifted = 0, killsGivenUp = 0;

		// The kill itself (the console's, the engine's death path: animation, ragdoll, the quests' and scripts' death
		// events). A protected one (only the player may kill it) has that lifted for this kill alone: the console's kill
		// names no killer (WorldSync::KillNamingNoKiller). True when its protection was lifted.
		bool Kill(RE::Actor* a_actor)
		{
			const auto local = a_actor->GetFormID();
			if (killedHere.size() >= MAX_KILLED_HERE) {
				killedHere.clear();
			}
			killedHere.insert(local);
			const bool lift = WorldSync::KillNamingNoKiller(a_actor);
			protectionsLifted += lift;
			++killsTried;
			return lift;
		}

		// The deaths that wait (CopyKill::Book): each killed once it stands loaded in the world, tried again while it stands.
		void TryKill(std::uint32_t a_local, Clock::time_point a_now)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_local);
			const bool there = actor && !actor->IsDeleted();
			const CopyKill::Facts facts{ .there = there, .deadHere = there && actor->IsDead(false), .inWorld = WorldSync::InWorld(), .loaded = there && actor->Get3D() != nullptr };
			const auto tries = pendingKills.Tries(a_local);
			switch (pendingKills.Judge(a_local, facts, a_now)) {
			case CopyKill::Verdict::kKill:
				{
					const auto id = theirs.IdOf(a_local) ? theirs.IdOf(a_local) : ours.IdOf(a_local);
					const bool lifted = Kill(actor);
					REX::INFO("Spawns: killed {:08X}: spawn {:08X} died in another player's game{}{}", a_local, id, lifted ? " (protected here: lifted for the kill, which names no killer)" : "",
						tries > 0 ? std::format(" (try {}: it stood after the last)", tries + 1) : "");
					break;
				}
			case CopyKill::Verdict::kGiveUp:
				++killsGivenUp;
				REX::WARN("Spawns: {:08X} still stands after {} kills for its death in another player's game; left as it is (protected {}, essential {}, dead {})", a_local, CopyKill::TRIES,
					actor->boolFlags.all(RE::Actor::BOOL_FLAGS::kProtected), WorldSync::IsEssential(actor), actor->IsDead(false));
				break;
			default:
				break;
			}
		}

		void TryKills()
		{
			if (pendingKills.Size() == 0) {
				return;
			}
			const auto now = Clock::now();
			for (const auto local : pendingKills.Ids()) {
				TryKill(local, now);
			}
		}

		// Our copy of a friend's spawn, in our player's cell or worldspace (Theirs::Decide has them in the same space). The
		// engine rolls a leveled one as it places it (synchronously, in CreateReferenceAtLocation): it is built as its
		// owner's roll (RollHook::Placement), not rolled again at our player's level.
		RE::ObjectRefHandle PlaceCopy(std::uint32_t a_id, const SpawnBook::Theirs::Copy& a_copy, std::uint32_t a_base, RE::TESObjectCELL* a_cell)
		{
			const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
			if (!npc) {
				return {};
			}
			RE::NEW_REFR_DATA data;
			data.location = { a_copy.at.x, a_copy.at.y, a_copy.at.z };
			data.direction = { 0.0f, 0.0f, a_copy.heading };
			data.object = npc;
			data.interior = a_cell->IsInterior() ? a_cell : nullptr;
			data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
			data.clearStillLoadingFlag = true;
			const auto           roll = NpcSync::RollOf(a_id);
			RollHook::Placement  placement{ roll && roll->originalBase == a_base ? roll : nullptr };
			const auto           handle = EngineRefs::CreateReferenceAtLocation(data);
			const auto           ref = handle.get();
			if (placement.Armed()) {
				++(placement.Consumed() ? copiesRolled : copiesNotRolled);
				if (!placement.Consumed()) {
					REX::INFO("Spawns: our copy of {:08X} (base {:08X}) was not built from its owner's roll as it was placed; the report's check makes it so (NpcSync)", a_id, a_base);
				}
			}
			if (!ref || !ref->As<RE::Actor>()) {
				return {};
			}
			SaveGuard::Track(ref.get());
			return handle;
		}

		// ---- A quest alias's creatures, paired across games (QuestActors.h) --------------------------------------------

		QuestActors::Quest QuestFactsOf(const RE::TESQuest* a_quest)
		{
			QuestActors::Quest facts{ .official = Official::IsOfficialForm(a_quest), .flags = a_quest->data.flags, .type = a_quest->data.questType,
				.radiant = RadiantSync::IsRadiant(a_quest) };
			const auto file = facts.official ? a_quest->GetFile(0) : nullptr;
			const auto index = file ? StageRules::FileOf(file->GetFilename()) : std::nullopt;
			facts.perGame = index && QuestFilter::IsPerGame(static_cast<std::uint8_t>(*index), a_quest->GetFormID() & 0x00FFFFFF);
			facts.runsOnce = index && QuestFilter::IsRunsOnce(static_cast<std::uint8_t>(*index), a_quest->GetFormID() & 0x00FFFFFF);
			facts.shared = QuestSync::SharesHere(a_quest);
			return facts;
		}

		// Its race's flags; with no race, as if our player could talk to it (never taken for a creature).
		std::uint32_t RaceFlags(const RE::Actor* a_actor)
		{
			return a_actor->race ? static_cast<std::uint32_t>(a_actor->race->data.flags) : QuestActors::RACE_ALLOWS_DIALOGUE;
		}

		// Its race's form ID (0 with no race).
		std::uint32_t RaceForm(const RE::Actor* a_actor)
		{
			return a_actor->race ? a_actor->race->GetFormID() : 0;
		}

		struct AliasKey
		{
			std::uint32_t quest = 0;  // 0: none
			std::uint16_t alias = 0;
		};

		// The quest alias that created our NPC, when that quest's creatures are paired across games (QuestActors::Keyed).
		AliasKey KeyOf(RE::Actor* a_actor)
		{
			const auto aliases = a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraAliasInstanceArray>() : nullptr;
			if (!aliases) {
				return {};
			}
			const auto               race = RaceFlags(a_actor);
			const auto               raceForm = RaceForm(a_actor);
			const RE::BSAutoReadLock l{ aliases->aliasArrayLock };
			for (const auto& entry : aliases->aliasArray) {
				if (entry.quest && entry.alias && entry.alias->owningQuest == entry.quest &&
					QuestActors::Keyed(QuestFactsOf(entry.quest), entry.alias->fillType, race, raceForm, entry.alias->aliasID)) {
					return { entry.quest->GetFormID(), static_cast<std::uint16_t>(entry.alias->aliasID) };
				}
			}
			return {};
		}

		// Whether a friend's live spawn names that alias (QuestActors::Twin).
		bool HasTwin(const AliasKey& a_key)
		{
			if (a_key.quest == 0) {
				return false;
			}
			for (const auto id : theirs.Ids()) {
				if (const auto copy = theirs.Find(id); copy && QuestActors::Twin(copy->spawn, copy->dead || copy->gone, a_key.quest, a_key.alias)) {
					return true;
				}
			}
			return false;
		}

		// ---- Random encounters (Encounters.h) ----------------------------------------------------------------------------

		// The quests a story manager quest node lists (BGSStoryManagerQuestNode::children): the ones the story manager
		// starts on an event, every random encounter among them. Gathered once from the data handler, as first asked;
		// empty when it could not be (the nodes not loaded yet), and then no test.
		std::unordered_set<std::uint32_t> storyQuests;
		bool                              storyQuestsGathered = false;

		void GatherStoryQuests()
		{
			if (storyQuestsGathered) {
				return;
			}
			if (const auto handler = RE::TESDataHandler::GetSingleton()) {
				for (const auto node : handler->GetFormArray<RE::BGSStoryManagerQuestNode>()) {
					if (!node) {
						continue;
					}
					for (const auto quest : node->children) {
						if (quest) {
							storyQuests.insert(quest->GetFormID());
						}
					}
				}
				storyQuestsGathered = !storyQuests.empty();
			}
		}

		// Whether the story manager starts a_quest: a quest node lists it, which decides. Nothing when no node could be
		// read: the quest's event decides then (Encounters::IsStoryEvent).
		std::optional<bool> StoryStarts(const RE::TESQuest* a_quest)
		{
			GatherStoryQuests();
			if (storyQuests.empty()) {
				return std::nullopt;
			}
			return storyQuests.contains(a_quest->GetFormID());
		}

		// Whether a quest is a random encounter here (Encounters::IsEncounter: misc, named so, and the story manager starts it).
		bool IsEncounterQuest(const RE::TESQuest* a_quest)
		{
			const char* name = a_quest->formEditorID.c_str();
			return name && Encounters::IsEncounter(a_quest->data.questType, StoryStarts(a_quest), a_quest->eventID, name);
		}

		// The encounter quest whose alias created our NPC (IsEncounterQuest), running here or not (the caller asks:
		// QuestRunningHere), and that alias. Quest 0 when it is no encounter's: an alias that found an NPC of ours rather
		// than making it (a settler) names nothing.
		AliasKey EncounterOf(RE::Actor* a_actor)
		{
			const auto aliases = a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraAliasInstanceArray>() : nullptr;
			if (!aliases) {
				return {};
			}
			const RE::BSAutoReadLock l{ aliases->aliasArrayLock };
			for (const auto& entry : aliases->aliasArray) {
				if (!entry.quest || !entry.alias || entry.alias->owningQuest != entry.quest || static_cast<std::uint16_t>(entry.alias->fillType) != QuestActors::FILL_CREATED ||
					entry.alias->aliasID > 0xFFFF) {
					continue;
				}
				if (IsEncounterQuest(entry.quest)) {
					return { entry.quest->GetFormID(), static_cast<std::uint16_t>(entry.alias->aliasID) };
				}
			}
			return {};
		}

		// The quests whose own start made our NPC (an alias of theirs that creates its reference holds it), to a_fn(quest
		// id). An alias the quest's script forced onto it, or that its conditions filled with it, doesn't count: the NPC
		// is some other quest's, or a settler's, and that quest's stop never deletes it.
		template <class Fn>
		void ForCreators(RE::Actor* a_actor, Fn&& a_fn)
		{
			const auto aliases = a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraAliasInstanceArray>() : nullptr;
			if (!aliases) {
				return;
			}
			const RE::BSAutoReadLock l{ aliases->aliasArrayLock };
			for (const auto& entry : aliases->aliasArray) {
				if (entry.quest && entry.alias && entry.alias->owningQuest == entry.quest && static_cast<std::uint16_t>(entry.alias->fillType) == QuestActors::FILL_CREATED) {
					a_fn(entry.quest->GetFormID());
				}
			}
		}

		// Whether the quest's own start made our NPC (ForCreators): an alias of a_quest that makes its reference (an
		// encounter's, an attack's Attackers collection, a radiant's).
		bool CreatedBy(RE::Actor* a_actor, std::uint32_t a_quest)
		{
			bool created = false;
			ForCreators(a_actor, [&](std::uint32_t a_creator) { created = created || a_creator == a_quest; });
			return created;
		}

		// The radiant quest whose alias created our NPC, when our game follows a friend's run of it whose actors are theirs
		// (RadiantSync::FollowsActors: a courser, a patrol made at runtime in each game): ours of it is not what stands there,
		// and is removed as it comes (Share). 0 otherwise.
		std::uint32_t FollowedRadiantOf(RE::Actor* a_actor)
		{
			std::uint32_t out = 0;
			ForCreators(a_actor, [&](std::uint32_t a_creator) {
				if (out == 0 && RadiantSync::FollowsActors(a_creator)) {
					out = a_creator;
				}
			});
			return out;
		}

		// The radiant quest whose own start made our NPC, when our game's own run of it is shared with its actors the ones
		// standing in every game (RadiantSync::SharesActors: once the job is given, not on offer): ours to share from the
		// moment it stands, calm or fighting (a squire or a scribe walking with our player, a restrained synth, an assault's
		// defenders before it starts), as a random encounter's actors are. 0 otherwise.
		std::uint32_t OwnedRadiantOf(RE::Actor* a_actor)
		{
			std::uint32_t out = 0;
			ForCreators(a_actor, [&](std::uint32_t a_creator) {
				if (out == 0 && RadiantSync::SharesActors(a_creator)) {
					out = a_creator;
				}
			});
			return out;
		}

		// Friends' encounter spawns as the book judges them: where their game had each as it was shared (its encounter's
		// spot) and where their game has it now (its states), its number in the server's order, and whether it is over.
		// One whose encounter ended is shared on plain (Add), and is no encounter spawn here any more (Encounters::TheirsOf).
		std::vector<Encounters::Theirs> TheirEncounters()
		{
			return Encounters::TheirsOf(theirs, theirSeq);
		}

		// Friends' live spawns our book holds as the host's game's settlers (SettlerSpawns.h), for the dev line.
		std::size_t TheirSettlers()
		{
			std::size_t count = 0;
			for (const auto id : theirs.Ids()) {
				const auto copy = theirs.Find(id);
				count += copy && SettlerSpawns::IsSettler(copy->spawn.kind) && !copy->dead && !copy->gone;
			}
			return count;
		}

		const char* QuestName(std::uint32_t a_quest)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			const char* name = quest ? quest->formEditorID.c_str() : nullptr;
			return name ? name : "";
		}

		// The quest by its form ID, when it is an encounter quest in our game's files (Encounters::IsEncounter): a friend's
		// word for a spawn's kind is taken only then.
		RE::TESQuest* EncounterQuestHere(std::uint32_t a_quest)
		{
			const auto quest = a_quest != 0 ? RE::TESForm::GetFormByID<RE::TESQuest>(a_quest) : nullptr;
			return quest && IsEncounterQuest(quest) ? quest : nullptr;
		}

		// Whether our own game runs that quest now.
		bool QuestRunningHere(std::uint32_t a_quest)
		{
			const auto quest = a_quest != 0 ? RE::TESForm::GetFormByID<RE::TESQuest>(a_quest) : nullptr;
			return quest && (quest->data.flags & QuestActors::QUEST_RUNNING) != 0;
		}

		// Our player is in a conversation or a scene: no encounter of ours is stopped meanwhile (a stop deletes every actor
		// of the run, the speaker among them, whichever of them the scan is looking at).
		bool PlayerInTalk()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return NpcSync::TalkingTo() != 0 || OpenMenus::Open(OpenMenus::Menu::kDialogue) || (player && player->GetCurrentScene() != nullptr);
		}

		// Our run of an encounter gives way to a friend's that was first at the spot: stopped through the quest's own stop
		// (what REScript does on REParent's cleanup), its actors removed, and our copies of theirs are placed. Booked here,
		// done in Frame (StopPending), outside the actor scan that found it.
		void StopEncounter(std::uint32_t a_quest, const Encounters::Rival& a_rival, Clock::time_point a_now)
		{
			encounters.Stopped(a_quest, a_now);
			if (!std::ranges::contains(pendingStops, a_quest)) {
				pendingStops.push_back(a_quest);
			}
			++encountersStopped;
			REX::INFO("Encounters: our {:08X} '{}' is stopped: player {}'s encounter {:08X} '{}' (their {:08X}) already runs {:.0f} units from it, and was first", a_quest, QuestName(a_quest), a_rival.player,
				a_rival.quest, QuestName(a_rival.quest), a_rival.spawnId, a_rival.distance);
		}

		// An actor of a run of ours we stopped (a_quest, for the log) is removed: disabled now and deleted a moment later
		// (RemotePlayers' graveyard, as our copies go), never offered meanwhile (Leaving); one shared already is released as
		// the scan stops seeing it, and the friends' copies go. The quest's own stop deletes what its aliases created only
		// as REScript's cleanup runs with the player far away: stopped at the spot it left a travelling group's three dogs
		// alive and a corpse standing (the rig, 0.14.0), which were then shared as a fight's spawns beside the friend's
		// encounter that had won the spot, so both games showed two sets.
		void RemoveActor(RE::Actor* a_actor, std::uint32_t a_quest)
		{
			const auto local = a_actor->GetFormID();
			if (!Protocol::IsSpawnRef(local) || theirs.IdOf(local) != 0 || a_actor->IsDeleted() || Leaving(local) || NpcSync::IsOurCompanion(a_actor)) {
				return;
			}
			if (leaving.size() >= MAX_LEAVING) {
				std::erase_if(leaving, [](const auto& a_entry) { return !a_entry.second.get(); });
			}
			if (leaving.size() < MAX_LEAVING) {
				leaving[local] = a_actor->GetHandle();
			}
			RemotePlayers::Discard(a_actor->GetHandle(), Clock::now());
			++actorsRemoved;
			if (actorsRemoved <= LOGGED) {
				const auto name = a_actor->GetDisplayFullName();
				REX::INFO("Encounters: {:08X} '{}', {}an actor of our stopped {:08X} '{}', is removed{}", local, name ? name : "", a_actor->IsDead(false) ? "the corpse of " : "", a_quest, QuestName(a_quest),
					ours.Shares(local) ? "; its spawn is released as it goes" : "");
			}
		}

		// The runs booked for a stop are stopped: their actors first, gathered before any is touched (a removal must not
		// change a list under its walk), then the quest. Every actor the quest's created aliases hold, loaded or not (one
		// out of the high process list is in no walk of the loaded ones, and once the stop clears the aliases nothing
		// tells it from any NPC: hostile, it would be shared as a fight's spawn beside the friend's encounter); every
		// loaded one whose alias array still names the quest, alive or dead. The book's actors of the run (seen before the
		// stop) go too, each by the handle the scan saw it under (its id alone could name another NPC by now), and so do
		// the actors of stopped runs the scan saw since, while their quest did not run (pendingRemovals: a fresh roll of
		// the same quest within STOPPED_FOR runs again, and its actors are nobody's to remove). RemoveActor leaves a
		// friend's copy and our companion alone, whatever holds them.
		void StopPending()
		{
			std::vector<Removal> removals;
			const auto           lists = RE::ProcessLists::GetSingleton();
			// The sweeps' loaded actors (their quest runs on; what its aliases hold of them is theirs to drop).
			for (const auto quest : pendingSweeps) {
				if (lists) {
					for (const auto& handle : lists->highActorHandles) {
						const auto ptr = handle.get();
						if (const auto actor = ptr.get(); actor && CreatedBy(actor, quest)) {
							removals.push_back({ actor->GetHandle(), quest });
						}
					}
				}
			}
			pendingSweeps.clear();
			for (const auto quest : pendingStops) {
				if (const auto form = RE::TESForm::GetFormByID<RE::TESQuest>(quest)) {
					for (const auto alias : form->aliases) {
						if (!alias || alias->fillType != QuestActors::FILL_CREATED) {
							continue;
						}
						const char* type = alias->QType().c_str();
						if (!type || std::string_view{ type } != RE::BGSRefAlias::TYPE) {
							continue;
						}
						const auto ref = form->GetAliasedRef(alias->aliasID).get();
						if (const auto actor = ref ? ref->As<RE::Actor>() : nullptr) {
							removals.push_back({ actor->GetHandle(), quest });
						}
					}
				}
				if (lists) {
					for (const auto& handle : lists->highActorHandles) {
						const auto ptr = handle.get();
						if (const auto actor = ptr.get(); actor && CreatedBy(actor, quest)) {
							removals.push_back({ actor->GetHandle(), quest });
						}
					}
				}
			}
			for (const auto& doomed : encounters.TakeDoomed()) {
				if (const auto it = encounterActors.find(doomed.local); it != encounterActors.end()) {
					removals.push_back({ it->second, doomed.quest });
				}
			}
			for (const auto& removal : removals) {
				if (const auto ref = removal.handle.get(); ref && ref->As<RE::Actor>()) {
					RemoveActor(ref->As<RE::Actor>(), removal.quest);
				}
			}
			for (const auto quest : pendingStops) {
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", quest).c_str());
				NpcSync::CountCommands(1);
			}
			pendingStops.clear();
			for (const auto& removal : pendingRemovals) {
				if (const auto ref = removal.handle.get(); ref && ref->As<RE::Actor>()) {
					RemoveActor(ref->As<RE::Actor>(), removal.quest);
				}
			}
			pendingRemovals.clear();
		}

		// The encounter quests running here as the session's first in-world frame comes, or the first after a load: settled
		// in the book, never stopped for a friend's (Encounters::Book::Settle). Every run seen after this is fresh.
		void SettleRunning(Clock::time_point a_now)
		{
			settleRuns = false;
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			GatherStoryQuests();
			std::vector<std::uint32_t> running;
			for (const auto quest : handler->GetFormArray<RE::TESQuest>()) {
				const char* name = quest ? quest->formEditorID.c_str() : nullptr;
				if (!name || (quest->data.flags & QuestActors::QUEST_RUNNING) == 0 || !Encounters::IsEncounterName(name)) {
					continue;
				}
				if (IsEncounterQuest(quest)) {
					running.push_back(quest->GetFormID());
				} else if (quest->data.questType == Encounters::QUEST_TYPE_MISC) {
					// The system's bookkeeping (REFactionShared), running from the game's start: named like one, no encounter.
					// Whether a story node lists it, and what the game holds in its event field, are logged, so the rule and its
					// fallback can be checked against them (a node lists a real encounter, whose event is a word such as SCPT).
					REX::INFO("Encounters: {:08X} '{}' runs, named like an encounter, but the story manager does not start it (its event reads {:08X}, a story node {} it): not one", quest->GetFormID(), name,
						quest->eventID, storyQuests.contains(quest->GetFormID()) ? "lists" : "does not list");
				}
			}
			encounters.Settle(running, a_now);
			REX::INFO("Encounters: the story manager's quest nodes list {} quests{}", storyQuests.size(), storyQuests.empty() ? " (none read: the quest's event alone tells an encounter)" : "");
			if (!running.empty()) {
				std::string names;
				for (const auto quest : running) {
					names += std::format("{}{:08X} '{}'", names.empty() ? "" : ", ", quest, QuestName(quest));
				}
				REX::INFO("Encounters: {} already running as the session's world came up, settled (never stopped for a friend's): {}", running.size(), names);
			}
		}

		// Our runs whose quest no longer runs here (it stopped, by its own script or ours) are over for the book, and the
		// actors of an ended run still standing and shared are shared on plain: reported again under their ids with no
		// quest (Protocol::SpawnKind::kPlain), which the server passes on and the friends' copies take as they stand, so
		// no game counts an encounter as running at their spot (a radstag whose encounter had ended made a friend's game
		// stop five runs near it: the rig, 0.14.0). Not those of a run we stopped: removed (StopPending), not shared on.
		void CheckRuns(Clock::time_point a_now)
		{
			if (a_now - runsChecked < RUNS_CHECK) {
				return;
			}
			runsChecked = a_now;
			for (const auto quest : encounters.Quests()) {
				if (!QuestRunningHere(quest)) {
					encounters.OursOver(quest);
					++encountersOver;
					if (encountersOver <= LOGGED) {
						REX::INFO("Encounters: our {:08X} '{}' is over: its quest no longer runs; its actors still standing are shared on as plain spawns", quest, QuestName(quest));
					}
				}
			}
			encounters.Expire(a_now);
			shareSettle.Expire(a_now);
			std::erase_if(encounterActors, [](const auto& a_entry) {
				const auto ref = a_entry.second.get();
				return !ref || ref->GetFormID() != a_entry.first;
			});
			for (auto it = offeredEncounter.begin(); it != offeredEncounter.end();) {
				const auto [id, quest] = *it;
				const auto local = ours.LocalOf(id);  // 0 until granted: looked at again once it is
				if (local == 0 || QuestRunningHere(quest) || encounters.IsStopped(quest, a_now)) {
					++it;
					continue;
				}
				// Loaded (its 3D in), so the scan still sees it: one unloaded is being let go of (NpcSync's release), and a
				// report of it now would race the release and leave the server a spawn nobody runs. It is let go of here too.
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(local);
				if (actor && !actor->IsDead(false) && !actor->IsDeleted() && !actor->IsDisabled() && actor->GetParentCell() && actor->Get3D()) {
					const auto base = BaseToPlace(actor);
					const auto at = SpotOf(actor);
					if (base != 0 && (at.cell == 0) != (at.worldspace == 0)) {
						outgoing.push_back(Protocol::Encode(Protocol::Spawn{ 0, id, base, at.cell, at.worldspace, at.x, at.y, at.z, actor->data.angle.z, 0, 0, Protocol::SpawnKind::kPlain },
							Protocol::MessageType::kReportSpawn));
						++sharedOn;
						if (sharedOn <= LOGGED) {
							REX::INFO("Encounters: {:08X}, shared as {:08X}, is shared on plain: its encounter {:08X} '{}' is over", local, id, quest, QuestName(quest));
						}
					}
				}
				calmOffered.erase(id);
				it = offeredEncounter.erase(it);
			}
			std::erase_if(calmOffered, [&](std::uint32_t a_id) { return !offeredEncounter.contains(a_id); });
		}

		// What our run of the quest a friend's spawn names holds in that alias, and whether it is ours to pair with it now.
		struct PairCandidate
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			bool                             oursThere = false;  // an NPC of ours, alive, is in the alias
			bool                             pairs = false;      // QuestActors::Pairs
		};

		PairCandidate CandidateFor(const SpawnBook::Theirs::Copy& a_copy, const RemoteEdits::Spot& a_us)
		{
			PairCandidate out;
			QuestActors::PairFacts facts{ .keyed = a_copy.spawn.quest != 0, .over = a_copy.dead || a_copy.gone, .theirsAt = a_copy.at, .player = a_us };
			const auto quest = facts.keyed ? RE::TESForm::GetFormByID<RE::TESQuest>(a_copy.spawn.quest) : nullptr;
			if (!quest) {
				return out;
			}
			const auto questFacts = QuestFactsOf(quest);
			facts.questPairs = QuestActors::QuestPairs(questFacts);
			const auto alias = std::ranges::find_if(quest->aliases, [&](const RE::BGSBaseAlias* a_alias) { return a_alias && a_alias->aliasID == a_copy.spawn.alias; });
			if (!facts.questPairs || alias == quest->aliases.end()) {
				return out;
			}
			const char* type = (*alias)->QType().c_str();
			if (!type || std::string_view{ type } != RE::BGSRefAlias::TYPE) {
				return out;
			}
			out.ref = quest->GetAliasedRef(a_copy.spawn.alias).get();
			const auto actor = out.ref ? out.ref->As<RE::Actor>() : nullptr;
			const auto local = actor ? actor->GetFormID() : 0;
			if (!actor || !Protocol::IsSpawnRef(local) || actor->IsDeleted() || actor->IsDead(false) || actor->IsPlayerRef()) {
				return out;
			}
			out.oursThere = true;
			facts.ours = !actor->IsDisabled() && actor->Get3D() && actor->GetParentCell() && !Puppets::IsPuppet(actor) && !Puppets::IsRegistered(actor) && !SaveGuard::Placed(actor) &&
			             !NpcSync::IsOurCompanion(actor) && !Leaving(local);
			facts.creature = QuestActors::Keyed(questFacts, (*alias)->fillType, RaceFlags(actor), RaceForm(actor), (*alias)->aliasID);
			// One our game reported and the server granted is ours (the server refuses a second game's report of the alias,
			// so theirs could not be granted too); one reported and not answered yet will be refused, as this one's twin.
			facts.shared = ours.IdOf(local) != 0 || theirs.IdOf(local) != 0;
			if (facts.ours) {
				facts.oursAt = SpotOf(actor);
			}
			out.pairs = QuestActors::Pairs(facts, SpawnBook::PLACE_RANGE);
			return out;
		}

		void Unpair(std::uint32_t a_id, std::string_view a_why)
		{
			const auto it = paired.find(a_id);
			if (it == paired.end()) {
				return;
			}
			const auto copy = theirs.Find(a_id);
			NpcSync::DropMirror(a_id);  // its AI is its own again (Puppets::ReleaseNpc)
			// The owner's roll, level and factions NpcSync made ours while paired go: ours is our game's again, and saved so.
			const auto ref = it->second.get();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (const auto own = pairedOwn.find(a_id); own != pairedOwn.end()) {
				if (actor && !actor->IsDeleted() && copy && actor->GetFormID() == copy->local) {
					const auto now = Rolls::ReadOurs(actor, true);
					const auto plan = NpcRoll::Decide(now, own->second, [&](std::uint32_t a_npc) { return Rolls::ReachOf(now.originalBase).Has(a_npc); });
					if (plan.Any()) {
						Rolls::Apply(actor, own->second, plan);
						++rollsRestored;
					}
				}
				pairedOwn.erase(own);
			}
			paired.erase(it);
			if (copy) {
				REX::INFO("Spawns: {:08X}, our quest's creature paired with player {}'s {:08X}, is our game's alone again: {}", copy->local, copy->spawn.playerId, a_id, a_why);
			}
			theirs.Unplaced(a_id);
			++unpairs;
		}

		void Discard(std::uint32_t a_id);

		// A friend's spawn named with a quest alias, alive, not paired: ours from that alias paired with it (true), or held
		// back from a copy while ours is in the alias (true: nothing to place), or neither (false: as for any spawn; with
		// our player not yet on the quest, its creatures' unique base places no copy, BaseOk). A copy placed of a keyed
		// spawn (a base that isn't unique) goes once our run has its own creature there (true), which is paired next.
		bool PairOrHold(std::uint32_t a_id, const SpawnBook::Theirs::Copy& a_copy, const RemoteEdits::Spot& a_us, Clock::time_point a_now)
		{
			auto& look = pairLooked[a_id];
			if (look.at != Clock::time_point{} && a_now - look.at < PAIR_LOOK) {
				return look.oursThere && a_copy.local == 0;
			}
			look.at = a_now;
			const auto candidate = CandidateFor(a_copy, a_us);
			look.oursThere = candidate.oursThere;
			if (look.oursThere && a_copy.local != 0) {
				REX::INFO("Spawns: our quest {:08X} has its own creature from alias {} now; our copy {:08X} of player {}'s {:08X} goes, and ours pairs with it", a_copy.spawn.quest, a_copy.spawn.alias,
					a_copy.local, a_copy.spawn.playerId, a_id);
				Discard(a_id);
				look.at = {};  // looked at again next frame
				return true;
			}
			if (!candidate.pairs || a_copy.local != 0) {
				return look.oursThere && a_copy.local == 0;
			}
			const auto local = candidate.ref->GetFormID();
			paired[a_id] = candidate.ref->GetHandle();
			if (auto own = Rolls::Read(candidate.ref->As<RE::Actor>(), a_id, true)) {
				pairedOwn[a_id] = std::move(*own);  // what ours is in our game, put back as it is unpaired
			}
			theirs.Placed(a_id, local, a_now);
			++pairs;
			REX::INFO("Spawns: {:08X}, our quest {:08X}'s creature from alias {}, is paired with player {}'s {:08X} from the same alias: mirrored as theirs, no copy placed", local, a_copy.spawn.quest,
				a_copy.spawn.alias, a_copy.spawn.playerId, a_id);
			return true;
		}

		// A pairing's every frame (QuestActors::Decide).
		void UpdatePaired(std::uint32_t a_id, const SpawnBook::Theirs::Copy& a_copy, const RemoteEdits::Spot& a_us)
		{
			const auto it = paired.find(a_id);
			if (it == paired.end()) {
				return;
			}
			const auto ref = it->second.get();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			QuestActors::KeepFacts facts{ .there = actor && !actor->IsDeleted() && actor->GetFormID() == a_copy.local, .gone = a_copy.gone, .dead = a_copy.dead, .player = a_us };
			if (facts.there) {
				facts.loaded = actor->Get3D() != nullptr && actor->GetParentCell() != nullptr;
				if (facts.loaded) {
					facts.oursAt = SpotOf(actor);
				}
			}
			if (QuestActors::Decide(facts, SpawnBook::KEEP_RANGE) == QuestActors::Keep::kUnpair) {
				Unpair(a_id, !facts.there ? "it is gone" : !facts.loaded ? "it unloaded" : a_copy.gone ? "their game let go of theirs" : "our player left it behind");
			}
		}

		// Deletes our copy of a spawn (the record stays, unplaced, unless the spawn is over). Ours paired with it is
		// unpaired instead, never deleted.
		void Discard(std::uint32_t a_id)
		{
			if (paired.contains(a_id)) {
				Unpair(a_id, "our copies go");
				return;
			}
			NpcSync::DropMirror(a_id);
			if (const auto it = placed.find(a_id); it != placed.end()) {
				Refresh(a_id, it->second);  // one the engine unloaded is forgotten here, its record dropped
				if (const auto local = theirs.LocalOf(a_id)) {
					if (leaving.size() >= MAX_LEAVING) {
						std::erase_if(leaving, [](const auto& a_entry) { return !a_entry.second.get(); });
					}
					if (leaving.size() < MAX_LEAVING) {
						leaving[local] = it->second.handle;
					}
				}
				RemotePlayers::Discard(it->second.handle, it->second.placedAt);
				placed.erase(it);
				++copiesDiscarded;
			}
			theirs.Unplaced(a_id);
		}

		// The leveled NPC an NPC was placed as (its ExtraLeveledCreature's original), 0 for one that isn't leveled.
		std::uint32_t LeveledOriginal(RE::Actor* a_actor)
		{
			const auto leveled = a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraLeveledCreature>() : nullptr;
			return leveled && leveled->originalBase ? leveled->originalBase->GetFormID() : 0;
		}

		// A runtime NPC of our own standing within SpawnBook::NPC_SAME_SPOT of a_spot: alive, loaded, no copy of a friend's
		// spawn, no stand-in, not our companion, and the same encounter's NPC as the friend's spawn (SpawnBook::StandsFor: of
		// a_base as BaseToPlace picks it, or placed as the same leveled NPC, a_original). The id of the first found, 0 for
		// none. Asked only as a copy is about to be placed: a walk of the references within 128 units is cheap, and
		// ForEachReferenceInRange finds the persistent ones too.
		std::uint32_t OwnMadeNpcAt(std::uint32_t a_base, std::uint32_t a_original, const RemoteEdits::Spot& a_spot)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return 0;
			}
			std::uint32_t found = 0;
			tes->ForEachReferenceInRange(RE::NiPoint3{ a_spot.x, a_spot.y, a_spot.z }, SpawnBook::NPC_SAME_SPOT, [&](RE::TESObjectREFR* a_ref) {
				if (found != 0) {
					return RE::BSContainer::ForEachResult::kStop;
				}
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				const auto id = actor ? actor->GetFormID() : 0;
				if (!actor || !Protocol::IsSpawnRef(id) || actor->IsDeleted() || actor->IsDisabled() || actor->IsDead(false) || !actor->Get3D() || !actor->GetParentCell() ||
					actor->IsPlayerRef() || Puppets::IsPuppet(actor) || theirs.IdOf(id) != 0 || SaveGuard::Placed(actor) || NpcSync::IsOurCompanion(actor)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (!RemoteEdits::Within(RemoteEdits::Distance(SpotOf(actor), a_spot), SpawnBook::NPC_SAME_SPOT)) {
					return RE::BSContainer::ForEachResult::kContinue;  // the walk's range is a box
				}
				if (SpawnBook::StandsFor(BaseToPlace(actor), LeveledOriginal(actor), a_base, a_original)) {
					found = id;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		// A base we place copies from: an NPC, and not a unique one (a second of a named character) or a stand-in's.
		// A unique one is allowed for an encounter's actor (a_uniqueOk): the named characters random encounters create (a
		// trader, an impersonator, an escaped synth) are in our world only through our own run of that encounter, which
		// Encounters stops, or OwnMadeNpcAt stands for.
		bool BaseOk(std::uint32_t a_base, bool a_uniqueOk)
		{
			const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(a_base);
			return npc && (a_uniqueOk || !npc->IsUnique()) && !RemotePlayers::IsStandInBase(a_base);
		}

		// Somebody here is in our copy of a friend's frame (our player, getting in or in it, or anyone else of ours), or it
		// is the frame our player left last: theirs now (SpawnBook::Act::kAbandon).
		bool Occupied(RE::TESObjectREFR* a_frame)
		{
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
				const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
				if ((occupied && occupied.get() == a_frame) || player->lastUsedPowerArmor.get().get() == a_frame) {
					return true;
				}
			}
			return NpcSync::InFrame(a_frame);
		}

		// Our copy of a frame a friend's game made (SpawnBook::Theirs::DecideFrame): an empty frame of its base, as the
		// copies a stand-in gets into are (a frame's pieces are each game's own loot), standing where their game has it
		// while that is in our sight. a_cell: our player's.
		void UpdateFrameCopy(std::uint32_t a_id, RE::TESObjectCELL* a_cell, Clock::time_point a_now, int& a_placedNow)
		{
			const auto copy = theirs.Find(a_id);
			if (!copy) {
				return;
			}
			SpawnBook::FrameFacts            facts{ .inView = NpcSync::InSight(copy->at) };
			facts.lent = FrameLoans::LentToUs(a_id);  // held as it is for the loan (FrameLoans)
			// A frame of our own, of that base, standing where their game has theirs (two saves with a shared past hold the
			// same frames): ours stands for it, and no copy goes beside it (FrameShare::SAME_SPOT). Never one lent to us: our
			// copy is the frame our player borrowed, held for the loan whatever of ours stands at that spot.
			facts.oursThere = !facts.lent && facts.inView && NpcSync::OwnMadeFrameAt(copy->spawn.base, copy->at, FrameShare::SAME_SPOT);
			if (facts.oursThere) {
				if (oursThere.insert(a_id).second) {
					++frameCopiesWithheld;
					REX::INFO("Spawns: a frame of our own stands where player {}'s game has their frame {:08X} (base {:08X}), at ({:.0f},{:.0f},{:.0f}); it stands for theirs, no copy beside it", copy->spawn.playerId, a_id,
						copy->spawn.base, copy->at.x, copy->at.y, copy->at.z);
				}
			} else {
				oursThere.erase(a_id);
			}
			// Our player being sent out of the frame a loaded save had them in, near where their game has this one (the loan of
			// it over with the load, it stands again where our player got in): no copy of it there until they are out, as the
			// engine gets nobody out of power armor with a frame where they step back to (FrameLoans::KeepsClear).
			facts.keptClear = !facts.lent && FrameLoans::KeepsClear(copy->at);
			if (facts.keptClear) {
				if (keptClear.insert(a_id).second) {
					++frameCopiesKeptClear;
					REX::INFO("Spawns: player {}'s frame {:08X} stands near our player, who is being sent out of the frame a loaded save had them in; no copy of it there until they are out",
						copy->spawn.playerId, a_id);
				}
			} else if (keptClear.erase(a_id) > 0) {
				REX::INFO("Spawns: player {}'s frame {:08X} is clear of our player again; its copy may stand", copy->spawn.playerId, a_id);
			}
			RE::NiPointer<RE::TESObjectREFR> ref;
			if (copy->local != 0) {
				ref = CopyOf(a_id);  // one the engine unloaded with its cell: back, or forgotten (see Refresh)
				facts.copyThere = ref && !ref->IsDeleted() && ref->GetFormID() == copy->local;
				if (facts.copyThere) {
					facts.occupied = Occupied(ref.get());
					facts.leaving = RemotePlayers::BeingSentOut(ref.get());
					// Just placed, it has a cell before its 3D (SpawnBook::LOAD_WAIT).
					if (ref->GetParentCell()) {
						facts.copyAt = SpotOf(ref.get());
						facts.copyHeading = ref->data.angle.z;
						facts.copyLoaded = ref->Get3D() != nullptr;
					}
				}
			}
			const auto playerId = copy->spawn.playerId;
			const auto base = copy->spawn.base;
			const auto at = copy->at;
			const auto heading = copy->heading;
			switch (theirs.DecideFrame(a_id, facts, a_now)) {
			case SpawnBook::Act::kPlace:
				{
					if (a_placedNow >= PLACE_PER_FRAME) {
						break;
					}
					++a_placedNow;
					// The copy their status had us show where they left it parked (ArmorFrames::ChoosePark) is this frame:
					// taken over and moved onto the spot next, not a second suit placed in it.
					auto       handle = RemotePlayers::TakeParkedCopy(playerId, base, at);
					const bool adopted = static_cast<bool>(handle);
					if (!adopted) {
						handle = RemotePlayers::PlaceFrameCopy(base, a_cell, { at.x, at.y, at.z }, heading);
					}
					const auto placedRef = handle.get();
					if (!placedRef) {
						theirs.Placed(a_id, 0, a_now);
						++placeFailed;
						REX::WARN("Spawns: could not place a copy of player {}'s frame {:08X} (base {:08X})", playerId, a_id, base);
						break;
					}
					placed[a_id] = { handle, a_now, SaveGuard::IdsOf(placedRef.get()) };
					theirs.Placed(a_id, placedRef->GetFormID(), a_now);
					++frameCopiesPlaced;
					REX::INFO("Spawns: {} {} at ({:.0f},{:.0f},{:.0f}) as our copy of player {}'s frame {:08X} (base {:08X}), made in their game", adopted ? "took the parked copy" : "placed", RefTag::Of(placedRef.get()), at.x, at.y, at.z,
						playerId, a_id, base);
					break;
				}
			case SpawnBook::Act::kMove:
				ref->data.location = { at.x, at.y, at.z };
				ref->data.angle = { 0.0f, 0.0f, heading };
				ref->Update3DPosition(true);
				++frameCopiesMoved;
				break;
			case SpawnBook::Act::kDiscard:
				if (ref) {
					const float off = facts.copyAt ? RemoteEdits::Distance(*facts.copyAt, at) : -1.0f;
					REX::INFO("Spawns: our copy {:08X} of player {}'s frame {:08X} goes (over={} inView={} inCell={} loaded={} leaving={} oursThere={} keptClear={} {:.0f} units off)", ref->GetFormID(),
						playerId, a_id, copy->gone, facts.inView, facts.copyAt.has_value(), facts.copyLoaded, facts.leaving, facts.oursThere, facts.keptClear, off);
				}
				Discard(a_id);
				break;
			case SpawnBook::Act::kLost:
				placed.erase(a_id);
				theirs.Unplaced(a_id);
				++copiesLost;
				break;
			case SpawnBook::Act::kAbandon:
				REX::INFO("Spawns: someone here got into {:08X}, our copy of player {}'s frame {:08X}; it is theirs now", ref->GetFormID(), playerId, a_id);
				SaveGuard::Release(ref->GetFormID());  // no longer ours: never kept out of a save, nor noted at a load
				placed.erase(a_id);
				theirs.Taken(a_id);
				++frameCopiesTaken;
				break;
			case SpawnBook::Act::kForget:
				theirs.Forget(a_id);
				break;
			case SpawnBook::Act::kNone:
				break;
			}
		}
	}

	namespace
	{
		// The NPCs that hit our player since the last frame: an actor of an encounter run of ours makes that run stand for
		// good (Encounters::Book::OursHurt). Not our copies of friends' spawns (their runs are theirs), nor an encounter
		// that ended.
		void TakeHits()
		{
			std::vector<std::uint32_t> hits;
			{
				std::scoped_lock l{ hitsLock };
				hits.swap(hitsOnUs);
			}
			for (const auto local : hits) {
				if (theirs.IdOf(local) != 0) {
					continue;
				}
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(local);
				const auto key = actor && !actor->IsDeleted() ? EncounterOf(actor) : AliasKey{};
				if (key.quest == 0 || !QuestRunningHere(key.quest) || !encounters.OursHurt(key.quest)) {
					continue;
				}
				++runsHurt;
				if (runsHurt <= LOGGED) {
					REX::INFO("Encounters: our {:08X} '{}' hurt our player ({:08X} hit them): it stands, never stopped for a friend's", key.quest, QuestName(key.quest), local);
				}
			}
		}

		// Our own actors of friends' runs we follow (Share), removed as a stopped run's are.
		void RemoveFollowed()
		{
			for (const auto& removal : std::exchange(followRemovals, {})) {
				const auto ref = removal.handle.get();
				const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor || Leaving(actor->GetFormID())) {
					continue;
				}
				++followRemoved;
				if (followRemoved <= LOGGED) {
					REX::INFO("Encounters: {:08X}, made by our game for {:08X} '{}', goes: our game follows a friend's run of it, whose actors stand there", actor->GetFormID(),
						removal.quest, QuestName(removal.quest));
				}
				RemoveActor(actor, removal.quest);
			}
		}
	}

	bool IsEncounter(const RE::TESQuest* a_quest)
	{
		return a_quest && IsEncounterQuest(a_quest);
	}

	std::optional<bool> EncounterStands(std::uint32_t a_quest)
	{
		return encounters.Knows(a_quest) ? std::optional{ encounters.Stands(a_quest, Clock::now()) } : std::nullopt;
	}

	bool EncounterGivingWay(std::uint32_t a_quest)
	{
		const auto now = Clock::now();
		return encounters.IsStopped(a_quest, now) || encounters.MayStop(a_quest, now) || std::ranges::contains(pendingStops, a_quest);
	}

	bool TheirEncounterLive(std::uint32_t a_player, std::uint32_t a_quest)
	{
		for (const auto id : theirs.Ids()) {
			const auto copy = theirs.Find(id);
			if (copy && copy->spawn.kind == Protocol::SpawnKind::kEncounter && copy->spawn.quest == a_quest && copy->spawn.playerId == a_player && !copy->dead && !copy->gone) {
				return true;
			}
		}
		return false;
	}

	CopyHere CopyFor(std::uint32_t a_id)
	{
		const auto copy = theirs.Find(a_id);
		if (!copy) {
			return { 0, true };
		}
		const auto ref = copy->local != 0 ? CopyOf(a_id) : RE::NiPointer<RE::TESObjectREFR>{};
		const bool there = ref && !ref->IsDeleted() && ref->GetFormID() == copy->local;
		return { there ? copy->local : 0u, copy->dead || copy->gone };
	}

	void OnPlayerHitBy(std::uint32_t a_local)
	{
		if (!Protocol::IsSpawnRef(a_local)) {
			return;
		}
		std::scoped_lock l{ hitsLock };
		if (hitsOnUs.size() < MAX_HITS_ON_US && !std::ranges::contains(hitsOnUs, a_local)) {
			hitsOnUs.push_back(a_local);
		}
	}

	std::uint32_t Share(RE::Actor* a_actor)
	{
		if (!a_actor || a_actor->IsPlayerRef() || Puppets::IsRegistered(a_actor) || Puppets::IsPuppet(a_actor) || a_actor->IsDeleted() || a_actor->IsDisabled() ||
			!a_actor->Get3D() || !a_actor->GetParentCell()) {
			return 0;
		}
		const auto local = a_actor->GetFormID();
		if (!Protocol::IsSpawnRef(local) || theirs.IdOf(local) != 0 || LocalPlayer() == 0 || Leaving(local)) {
			return 0;
		}
		const auto now = Clock::now();
		const auto at = SpotOf(a_actor);
		// A random encounter's actor (Encounters.h) is ours to share calm or fighting, from the moment it stands: unless a
		// friend's encounter already ran at the spot when ours started, in which case ours is stopped (and none of its
		// actors offered: those seen are removed, Frame). Looked at before the grant below too: a friend's spawn that came
		// in after we offered ours, but ahead of our grant in the server's order, was first. Never while our player is in
		// a conversation or a scene (a stop would take the speaker out from under them: Encounters::Holds), nor before the
		// world's runs are settled (Frame): ours stays, and theirs is placed beside it or withheld (oursThere). Never our
		// companion (an encounter's actor recruited while its quest still runs is the companion handling's). An actor of
		// an encounter that ended (its quest stopped, by its own script) is an NPC as any: one shared already is shared on
		// plain (CheckRuns), one not shared yet goes as a fight's spawn would, hostile or fighting.
		const bool mayFollow = !a_actor->IsDead(false) && !NpcSync::IsOurCompanion(a_actor);
		if (const auto radiant = mayFollow ? FollowedRadiantOf(a_actor) : 0u) {
			// A radiant whose run our game follows with the owner's actors (RadiantSync::FollowsActors): theirs stand there, our
			// copies in its aliases; ours made for it (its start made one for each alias) is removed as it comes, never offered.
			followRemovals.push_back({ a_actor->GetHandle(), radiant });
			return 0;
		}
		auto encounter = mayFollow ? EncounterOf(a_actor) : AliasKey{};
		if (encounter.quest != 0 && EncounterRuns::Follows(encounter.quest)) {
			// A friend's run of the encounter that our game follows (EncounterRuns.h): its actors are theirs, our copies in
			// its aliases; one our game made for it (an alias their game left unshared: a reinforcement still disabled
			// there, an actor not shared yet) is removed as it comes, never offered.
			followRemovals.push_back({ a_actor->GetHandle(), encounter.quest });
			return 0;
		}
		if (encounter.quest != 0 && encounters.IsStopped(encounter.quest, now)) {
			// A quest we stopped lately: never offered. Its actor still standing while the quest does not run is the stopped
			// run's, removed (Frame); with the quest running again it is a fresh roll's, left to that run (judged after
			// STOPPED_FOR).
			if (!QuestRunningHere(encounter.quest)) {
				pendingRemovals.push_back({ a_actor->GetHandle(), encounter.quest });
			}
			return 0;
		}
		if (encounter.quest != 0 && !QuestRunningHere(encounter.quest)) {
			encounter = {};
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		// Our own run of a radiant whose actors are made at runtime and stand in every game (RadiantSync::SharesActors): its
		// actor is ours to share calm too, from the moment it stands, so a friend's game has a copy to put in its alias.
		const auto ownedRadiant = mayFollow && encounter.quest == 0 ? OwnedRadiantOf(a_actor) : 0u;
		if (encounter.quest != 0) {
			// One fighting or hostile makes the run a fight (Encounters::FIGHT_STOP_WINDOW).
			if (encounters.OursSeen(encounter.quest, local, at, now, player && OursToShare(a_actor, player))) {
				if (encounterActors.size() >= MAX_ENCOUNTER_ACTORS) {
					encounterActors.clear();
				}
				encounterActors.try_emplace(local, a_actor->GetHandle());
			}
			if (encounters.MayStop(encounter.quest, now, { .unsettled = settleRuns })) {
				const Encounters::Holds holds{ .unsettled = settleRuns, .talking = PlayerInTalk() };
				if (const auto rival = encounters.RivalOf(encounter.quest, TheirEncounters(), now, holds)) {
					StopEncounter(encounter.quest, *rival, now);
					return 0;
				}
			}
		}
		if (const auto id = ours.IdOf(local)) {
			return id;  // dead or alive: ours until the server has its death
		}
		// A settler our game made at run time (a beacon's recruit) at a settlement our game runs for the session (the
		// shared workshop, SettlerSpawns.h): ours to share calm too, from the moment it stands, as a spawn of a kind of its
		// own that names its workbench, in one of the settlers' places. With none of those left it is as it was before the
		// rule: shared while it fights, as any NPC our game made, and not while it is calm (said once).
		auto settlerBench = player && mayFollow && encounter.quest == 0 && ownedRadiant == 0 ? WorkshopSync::SettlerBenchToShare(*a_actor) : 0u;
		if (settlerBench != 0) {
			switch (SettlerSpawns::OfferAs(ours.HasRoom(true), OursToShare(a_actor, player))) {
			case SettlerSpawns::As::kSettler:
				break;
			case SettlerSpawns::As::kFight:
				settlerBench = 0;
				break;
			case SettlerSpawns::As::kWait:
				if (settlersHeldNoted.size() >= MAX_BLOCKED_NOTED) {
					settlersHeldNoted.clear();
				}
				if (settlersHeldNoted.insert(local).second) {
					++settlersHeld;
					REX::INFO("Spawns: {:08X}, a settler of our workbench {:08X}, waits: {} settlers are shared already, as many as a game shares at once; friends' games do not see it", local,
						settlerBench, ours.Settlers());
				}
				return 0;
			}
		}
		if (!player || (encounter.quest == 0 && ownedRadiant == 0 && settlerBench == 0 && !OursToShare(a_actor, player))) {
			return 0;
		}
		// A calm encounter actor takes one of MAX_CALM places; past them it waits (said once), so a fight's spawns have the
		// rest of our share. One fighting or hostile goes as any fight's spawn. A radiant's calm actor (ownedRadiant) is
		// one, two or three to a job: shared without a place, said in the log.
		const bool calm = encounter.quest != 0 && !OursToShare(a_actor, player);
		if (calm && calmOffered.size() >= Encounters::MAX_CALM) {
			if (calmHeldNoted.size() >= MAX_BLOCKED_NOTED) {
				calmHeldNoted.clear();
			}
			if (calmHeldNoted.insert(local).second) {
				++calmHeld;
				REX::INFO("Encounters: {:08X}, a calm actor of our {:08X} '{}', waits: {} calm actors are shared already", local, encounter.quest, QuestName(encounter.quest), calmOffered.size());
			}
			return 0;
		}
		const auto base = BaseToPlace(a_actor);
		if (const auto block = SpawnBook::ShareBlock(base, (at.cell == 0) != (at.worldspace == 0)); block != SpawnBook::Block::kNone) {
			NoteBlocked(a_actor, local, block,
				encounter.quest != 0 ? "a random encounter's" :
				ownedRadiant != 0    ? "a shared radiant's, made by its start" :
				settlerBench != 0    ? "a settler of a settlement our game runs" :
				                       "hostile to our player or fighting");
			return 0;
		}
		// A quest alias's creature (QuestActors.h): named with its alias. One a friend's game shares already is not
		// reported (the server would refuse it): ours pairs with theirs (Frame). An encounter's actor names its encounter
		// quest instead, with no twin: each game's run is its own.
		// A settler names its workbench in the quest's place, with no alias and no twin: the host's game alone has it.
		auto key = encounter;
		auto kind = Protocol::SpawnKind::kEncounter;
		if (settlerBench != 0) {
			key = { settlerBench, 0 };
			kind = Protocol::SpawnKind::kSettler;
		} else if (encounter.quest == 0) {
			key = KeyOf(a_actor);
			kind = key.quest != 0 ? Protocol::SpawnKind::kQuestActor : Protocol::SpawnKind::kPlain;
			if (HasTwin(key)) {
				if (twinsNoted.size() >= MAX_BLOCKED_NOTED) {
					twinsNoted.clear();
				}
				if (twinsNoted.insert(local).second) {
					++twinsHeld;
					REX::INFO("Spawns: {:08X}, our quest {:08X}'s creature from alias {}, is not shared: a friend's game shares theirs from the alias, and ours pairs with it", local, key.quest, key.alias);
				}
				return 0;
			}
		}
		const bool settler = SettlerSpawns::IsSettler(kind);
		// An encounter's actor stands Encounters::SHARE_SETTLE before it is offered, unless it fights already: a corpse the
		// run makes (killed by its own script as it loads) dies meanwhile and is never shared. One reported already and not
		// answered yet is asked again as before.
		if (kind == Protocol::SpawnKind::kEncounter && !ours.Shares(local)) {
			if (!shareSettle.Ready(local, now, a_actor->IsInCombat())) {
				return 0;
			}
		}
		const auto id = ours.Offer(local, now, [](std::uint32_t a_id) { return theirs.Knows(a_id); }, settler);
		if (!id) {
			return 0;
		}
		settlersShared += settler;
		if (kind == Protocol::SpawnKind::kEncounter) {
			shareSettle.Forget(local);  // offered: its wait is over
			offeredEncounter[*id] = key.quest;
			if (calm) {
				calmOffered.insert(*id);
			}
			++encountersShared;
		}
		outgoing.push_back(Protocol::Encode(Protocol::Spawn{ 0, *id, base, at.cell, at.worldspace, at.x, at.y, at.z, a_actor->data.angle.z, key.quest, key.alias, kind }, Protocol::MessageType::kReportSpawn));
		++reported;
		keyedReported += kind == Protocol::SpawnKind::kQuestActor;
		radiantActorsShared += ownedRadiant != 0;
		if (logged < LOGGED || (settler ? settlersShared <= LOGGED : kind != Protocol::SpawnKind::kPlain && (keyedReported <= LOGGED || encountersShared <= LOGGED)) ||
			(ownedRadiant != 0 && radiantActorsShared <= LOGGED)) {
			++logged;
			REX::INFO("Spawns: sharing {:08X} (base {:08X}) as {:08X}{}", local, base, *id,
				settler                                  ? std::format(", our settler of workbench {:08X}{}", key.quest, OursToShare(a_actor, player) ? "" : " (calm)") :
				kind == Protocol::SpawnKind::kQuestActor ? std::format(", our quest {:08X}'s creature from alias {}", key.quest, key.alias) :
				kind == Protocol::SpawnKind::kEncounter  ? std::format(", our encounter {:08X} '{}' alias {}{}", key.quest, QuestName(key.quest), key.alias, calm ? " (calm)" : "") :
				ownedRadiant != 0                        ? std::format(", made by our shared radiant {:08X} '{}'{}", ownedRadiant, QuestName(ownedRadiant), OursToShare(a_actor, player) ? "" : " (calm)") :
				                                           std::string{});
		}
		return 0;  // not until the server grants it
	}

	std::uint32_t ShareFrame(RE::TESObjectREFR* a_frame, bool a_share)
	{
		if (!a_frame || !a_share || LocalPlayer() == 0) {
			return 0;
		}
		const auto local = a_frame->GetFormID();
		if (!Protocol::IsSpawnRef(local) || theirs.IdOf(local) != 0 || Leaving(local)) {
			return 0;
		}
		if (const auto id = ours.IdOf(local)) {
			return id;
		}
		std::erase_if(ownFrames, [](std::uint32_t a_local) { return !ours.Shares(a_local); });
		if (!ownFrames.contains(local) && !FrameShare::RoomForMade(ownFrames.size())) {
			return 0;
		}
		const auto base = a_frame->GetObjectReference();
		const auto at = SpotOf(a_frame);
		if (!base || !Protocol::IsShareableRef(base->GetFormID()) || (at.cell == 0) == (at.worldspace == 0)) {
			return 0;
		}
		const auto id = ours.Offer(local, Clock::now(), [](std::uint32_t a_id) { return theirs.Knows(a_id); });
		if (!id) {
			return 0;
		}
		ownFrames.insert(local);
		outgoing.push_back(Protocol::Encode(Protocol::Spawn{ 0, *id, base->GetFormID(), at.cell, at.worldspace, at.x, at.y, at.z, a_frame->data.angle.z }, Protocol::MessageType::kReportSpawn));
		++framesReported;
		REX::INFO("Spawns: sharing our frame {:08X} (base {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X} as {:08X}", local, base->GetFormID(), at.x, at.y, at.z, at.cell ? at.cell : at.worldspace, *id);
		return 0;  // not until the server grants it
	}

	bool FrameCopyAt(std::uint32_t a_owner, const Protocol::ParkedFrame& a_parked, float a_radius)
	{
		const auto id = theirs.FrameAt(a_owner, a_parked.base, { a_parked.cell, a_parked.worldspace, a_parked.x, a_parked.y, a_parked.z }, a_radius);
		const auto it = id != 0 ? placed.find(id) : placed.end();
		const auto ref = it != placed.end() ? it->second.handle.get() : RE::NiPointer<RE::TESObjectREFR>{};
		return ref && !ref->IsDeleted() && !ref->IsDisabled() && ref->GetParentCell();
	}

	std::uint32_t FrameCopyOwnerOf(std::uint32_t a_local)
	{
		const auto id = theirs.IdOf(a_local);
		const auto copy = id != 0 && placed.contains(id) ? theirs.Find(id) : nullptr;
		return copy ? copy->spawn.playerId : 0;
	}

	std::uint32_t LocalOf(std::uint32_t a_id)
	{
		if (const auto local = ours.LocalOf(a_id)) {
			return local;
		}
		return theirs.LocalOf(a_id);
	}

	std::uint32_t IdOf(std::uint32_t a_local)
	{
		if (const auto id = ours.IdOf(a_local)) {
			return id;
		}
		return theirs.IdOf(a_local);
	}

	std::uint32_t OwnIdOf(std::uint32_t a_local)
	{
		return ours.IdOf(a_local);
	}

	std::uint32_t CopyIdOf(std::uint32_t a_local)
	{
		return theirs.IdOf(a_local);
	}

	void RemoveFollowedActor(std::uint32_t a_local, std::uint32_t a_quest)
	{
		const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_local);
		if (!actor || actor->IsDeleted() || !Protocol::IsSpawnRef(a_local) || theirs.IdOf(a_local) != 0 || NpcSync::IsOurCompanion(actor) || Leaving(a_local)) {
			return;
		}
		if (!CreatedBy(actor, a_quest)) {
			// The alias holds an NPC the quest's start didn't make (its conditions filled it with another quest's actor, or a
			// settler): the alias is forced away from it, and it stays as it is.
			++followKept;
			if (followKept <= LOGGED) {
				REX::INFO("Encounters: {:08X}, held by an alias of {:08X} '{}' that is forced onto our copy of the owner's actor, stays: the quest's start didn't make it", a_local, a_quest,
					QuestName(a_quest));
			}
			return;
		}
		++followRemoved;
		if (followRemoved <= LOGGED) {
			REX::INFO("Encounters: {:08X}, made by our game for {:08X} '{}', goes: its alias is forced onto our copy of the owner's actor", a_local, a_quest, QuestName(a_quest));
		}
		RemoveActor(actor, a_quest);
	}

	void Add(const Protocol::Spawn& a_given)
	{
		// A friend's word for an encounter's actor is taken only when the quest it names is an encounter in our game's
		// files (Encounters::IsEncounter): otherwise it is a plain spawn here, with the twin rule and the unique bases as for
		// any (a kind a game made up would skip the twin refusal and place a second of a named character).
		auto a_spawn = a_given;
		if (a_spawn.kind == Protocol::SpawnKind::kEncounter && !EncounterQuestHere(a_spawn.quest)) {
			++kindsVetoed;
			if (kindsVetoed <= LOGGED) {
				REX::WARN("Spawns: player {}'s {:08X} names {:08X} as its encounter quest, which is no random encounter here; taken as a plain spawn", a_spawn.playerId, a_spawn.refId, a_spawn.quest);
			}
			a_spawn.kind = Protocol::SpawnKind::kPlain;
			a_spawn.quest = 0;
			a_spawn.alias = 0;
		}
		// The host's game's settler (SettlerSpawns.h; the server passes the kind on from the host's own game alone) is taken
		// as one only when what it names is a settlement's workbench in our game's files: otherwise it is a plain spawn
		// here, placed as any.
		if (SettlerSpawns::IsSettler(a_spawn.kind)) {
			const bool workbench = WorkshopSync::IsWorkbenchAnchor(RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_spawn.quest));
			if (!workbench) {
				++settlersVetoed;
				if (settlersVetoed <= LOGGED) {
					REX::WARN("Spawns: player {}'s {:08X} names {:08X} as its settlement's workbench, which is none here; taken as a plain spawn", a_spawn.playerId, a_spawn.refId, a_spawn.quest);
				}
			}
			a_spawn = SettlerSpawns::Taken(a_spawn, workbench);
		}
		// An encounter's actor its owner shares on plain, under its id, as its encounter ends (Encounters.h): the record
		// takes the kind, our copy stays as it is, and no encounter runs at its spot any more for our book.
		if (const auto known = theirs.Find(a_spawn.refId);
			known && known->spawn.playerId == a_spawn.playerId && known->spawn.kind == Protocol::SpawnKind::kEncounter && a_spawn.kind == Protocol::SpawnKind::kPlain && known->spawn.base == a_spawn.base) {
			const auto quest = known->spawn.quest;
			theirs.Rekind(a_spawn.refId, a_spawn);
			++theirsOver;
			if (theirsOver <= LOGGED) {
				REX::INFO("Encounters: player {}'s {:08X} is shared on plain: their encounter {:08X} '{}' is over; our copy {:08X} stays", a_spawn.playerId, a_spawn.refId, quest, QuestName(quest), known->local);
			}
			return;
		}
		if (const auto known = theirs.Find(a_spawn.refId); known && known->local != 0) {
			REX::INFO("Spawns: player {} shared {:08X} again; our copy {:08X} of it goes", a_spawn.playerId, a_spawn.refId, known->local);
			Discard(a_spawn.refId);  // an old one under the same id: it is over
		}
		if (theirs.Add(a_spawn, LocalPlayer())) {
			++received;
			// Its number in the server's order (Encounters.h): the same in every game, so two games that rolled encounters at
			// one spot agree on which was first.
			theirSeq[a_spawn.refId] = encounters.Next();
			const bool encounter = a_spawn.kind == Protocol::SpawnKind::kEncounter;
			const bool settler = SettlerSpawns::IsSettler(a_spawn.kind);
			encountersReceived += encounter;
			settlersReceived += settler;
			if (logged < LOGGED || (encounter && encountersReceived <= LOGGED) || (settler && settlersReceived <= LOGGED)) {
				++logged;
				REX::INFO("Spawns: player {} shares {:08X} (base {:08X}) at ({:.0f},{:.0f},{:.0f}) cell {:08X} ws {:08X}{}", a_spawn.playerId, a_spawn.refId, a_spawn.base, a_spawn.x,
					a_spawn.y, a_spawn.z, a_spawn.cell, a_spawn.worldspace,
					encounter ? std::format(", their encounter {:08X} '{}' alias {}", a_spawn.quest, QuestName(a_spawn.quest), a_spawn.alias) :
					settler   ? std::format(", their settler of workbench {:08X}", a_spawn.quest) :
					            std::string{});
			}
		}
	}

	void OnOwners(const std::vector<Protocol::ActorOwner>& a_owners)
	{
		const auto now = Clock::now();
		for (const auto& owner : a_owners) {
			if (!Protocol::IsSpawnRef(owner.refId)) {
				// A friend's NPC our game has no reference of (Absent): over when nobody runs it, or for its owner when another
				// game takes it (that game's next state books it afresh).
				if (const auto copy = theirs.Find(owner.refId); copy && owner.playerId != copy->spawn.playerId) {
					theirs.Gone(owner.refId);
				}
				continue;
			}
			if (ours.Holds(owner.refId)) {
				const bool wasConfirmed = ours.LocalOf(owner.refId) != 0;
				ours.Answered(owner.refId, owner.playerId, LocalPlayer(), now);
				if (!wasConfirmed) {
					owner.playerId == LocalPlayer() ? ++granted : ++refused;
					// Our grant's number in the server's order (Encounters.h), taken by the encounter run whose actor it is.
					const auto seq = encounters.Next();
					if (const auto offered = offeredEncounter.find(owner.refId); offered != offeredEncounter.end()) {
						if (owner.playerId == LocalPlayer()) {
							encounters.OursGranted(offered->second, seq);
						} else {
							offeredEncounter.erase(offered);
						}
					}
				}
				if (!ours.Holds(owner.refId)) {
					offeredEncounter.erase(owner.refId);  // refused, or let go of
				}
				continue;
			}
			if (owner.playerId == 0) {
				theirs.Gone(owner.refId);
			}
		}
	}

	void OnState(const Protocol::ActorState& a_state)
	{
		theirs.Moved(a_state.refId, RemoteEdits::SpotOf(a_state), a_state.heading, Clock::now());
	}

	bool Absent(const Protocol::ActorState& a_state, std::uint32_t a_owner)
	{
		const auto id = a_state.refId;
		if (!Protocol::IsShareableRef(id) || a_owner == 0 || a_owner == LocalPlayer()) {
			return false;
		}
		const auto known = theirs.Find(id);
		const auto knownOwner = known && !known->gone ? known->spawn.playerId : 0;
		const auto base = knownOwner == a_owner ? 0 : SpawnBook::AbsentBase(NpcSync::RollOf(id), [](std::uint32_t a_npc) { return BaseOf(RE::TESForm::GetFormByID(a_npc)); });
		switch (SpawnBook::DecideAbsent(RE::TESForm::GetFormByID(id) != nullptr, knownOwner, a_owner, base)) {
		case SpawnBook::AbsentStep::kOurs:
			return false;
		case SpawnBook::AbsentStep::kKnown:
			OnState(a_state);
			return true;
		case SpawnBook::AbsentStep::kNoBase:
			if (absentNoBase.size() >= MAX_BLOCKED_NOTED) {
				absentNoBase.clear();
			}
			if (auto& noBase = absentNoBase.try_emplace(id, NoBase{ Clock::now() }).first->second;
				!noBase.said && Clock::now() - noBase.since >= NpcRoll::IDENTITY_WAIT && absentLogged < LOGGED) {
				noBase.said = true;
				++absentLogged;
				REX::INFO("Spawns: player {} runs {:08X}, which our game has no reference of (its cell is not loaded here), and {}: no copy of it here yet", a_owner, id,
					NpcSync::RollOf(id) ? "none of the NPCs its roll names is one we place" : "no roll of it has come to name what to place it from");
			}
			return false;
		case SpawnBook::AbsentStep::kAdd:
			break;
		}
		// Booked afresh: a copy of an old booking (another owner's, or one let go of) goes first.
		if (known) {
			if (known->local != 0) {
				Discard(id);
			}
			theirs.Forget(id);
			seenAt.erase(id);
		}
		const auto at = RemoteEdits::SpotOf(a_state);
		if (!theirs.Add(Protocol::Spawn{ a_owner, id, base, at.cell, at.worldspace, at.x, at.y, at.z, a_state.heading }, LocalPlayer())) {
			return false;
		}
		absentNoBase.erase(id);
		++absentBooked;
		if (absentLogged < LOGGED) {
			++absentLogged;
			REX::INFO("Spawns: player {} runs {:08X}, which our game has no reference of (a temporary reference a quest of theirs took out of a cell we have not loaded): our copy of it is placed from {:08X} (roll {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X}",
				a_owner, id, base, NpcSync::RollOf(id) ? NpcSync::RollOf(id)->originalBase : 0, at.x, at.y, at.z, at.cell ? at.cell : at.worldspace);
		}
		return true;
	}

	bool IsAbsent(std::uint32_t a_id)
	{
		return !Protocol::IsSpawnRef(a_id) && theirs.Knows(a_id);
	}

	bool Expects(std::uint32_t a_id)
	{
		const auto copy = theirs.Find(a_id);
		return copy && !copy->dead && !copy->gone;
	}

	bool GivenBack(const Protocol::FrameReturn& a_return)
	{
		if (FrameLoan::ChoosePlace(a_return) != FrameLoan::Place::kMove) {
			return false;
		}
		const RemoteEdits::Spot at{ a_return.cell, a_return.worldspace, a_return.x, a_return.y, a_return.z };
		const auto              copy = theirs.Find(a_return.spawnId);
		const float             from = copy ? RemoteEdits::Distance(copy->at, at) : -1.0f;
		if (!theirs.GivenBack(a_return.spawnId, at, a_return.heading, Clock::now())) {
			return false;
		}
		REX::INFO("Spawns: player {}'s frame {:08X} was given back at ({:.0f},{:.0f},{:.0f}) in {:08X}, {:.0f} units from where our book had it; our copy stands there now", copy->spawn.playerId,
			a_return.spawnId, at.x, at.y, at.z, at.cell ? at.cell : at.worldspace, from);
		return true;
	}

	void ApplyDeath(const Protocol::ActorDeath& a_death)
	{
		if (!Protocol::IsSpawnRef(a_death.refId) && !IsAbsent(a_death.refId)) {
			return;
		}
		std::uint32_t local = 0;
		if (theirs.Knows(a_death.refId)) {
			local = theirs.Died(a_death.refId);
			copiesKilled += local != 0;
		} else if ((local = ours.LocalOf(a_death.refId)) != 0) {
			++oursKilled;  // a friend killed their copy of ours: it dies here too
		}
		// Killed now if it can be, else once it stands loaded in the world (TryKills, every frame).
		if (local != 0) {
			pendingKills.Add(local);
			TryKill(local, Clock::now());
		}
	}

	bool KilledHere(std::uint32_t a_local)
	{
		return killedHere.contains(a_local);
	}

	std::uint32_t OnLocalDeath(std::uint32_t a_local)
	{
		if (killedHere.erase(a_local)) {
			return 0;
		}
		if (const auto id = ours.IdOf(a_local)) {
			return id;
		}
		const auto id = theirs.IdOf(a_local);
		return id != 0 && theirs.Died(id) != 0 ? id : 0;  // our copy, killed here first: the kill is ours to report
	}

	void StopQuestRun(std::uint32_t a_quest, bool a_actorsOnly)
	{
		auto& list = a_actorsOnly ? pendingSweeps : pendingStops;
		if (a_quest != 0 && !std::ranges::contains(list, a_quest)) {
			list.push_back(a_quest);
		}
	}

	void Frame()
	{
		// Our encounters first: runs booked for a stop are stopped here, outside the actor scan that found them, and runs
		// whose quest no longer runs are over.
		if (WorldSync::InWorld() && LocalPlayer() != 0) {
			if (settleRuns) {
				SettleRunning(Clock::now());
			}
			StopPending();
			CheckRuns(Clock::now());
			TakeHits();
			RemoveFollowed();
		}
		TryKills();
		if (theirs.Size() == 0) {
			unplacedWhy.clear();  // none left to have no copy (`net status` counts these)
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell || !WorldSync::InWorld() || Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		const auto us = SpotOf(player);
		int        placedNow = 0;
		for (const auto id : theirs.Ids()) {
			const auto copy = theirs.Find(id);
			if (!copy) {
				continue;
			}
			// A friend's NPC our game had no reference of (Absent) that it has now (its cell loaded here): ours is mirrored
			// (NpcSync), and our copy goes.
			if (!Protocol::IsSpawnRef(id) && RE::TESForm::GetFormByID(id)) {
				REX::INFO("Spawns: our game has {:08X} now; our copy of player {}'s goes, and ours is mirrored", id, copy->spawn.playerId);
				if (copy->local != 0) {
					Discard(id);
				}
				theirs.Forget(id);
				continue;
			}
			// A frame its owner's game made: placed as a friend's parked frame is, not as an NPC.
			if (Equipment::IsPowerArmorFrame(RE::TESForm::GetFormByID(copy->spawn.base))) {
				UpdateFrameCopy(id, cell, now, placedNow);
				continue;
			}
			// A quest alias's creature (QuestActors.h): ours from the same alias paired with it, or held back from a copy
			// while ours is in the alias.
			if (paired.contains(id)) {
				UpdatePaired(id, *copy, us);
				continue;
			}
			if (copy->spawn.kind == Protocol::SpawnKind::kQuestActor && copy->spawn.quest != 0 && !copy->dead && !copy->gone && PairOrHold(id, *copy, us, now)) {
				continue;
			}
			SpawnBook::Facts facts{ .player = us };
			if (const auto state = RemotePlayers::LastStateOf(copy->spawn.playerId)) {
				facts.owner = RemoteEdits::SpotOf(*state);
			}
			const auto from = PlaceBase(id, copy->spawn.base);
			if (copy->local != 0) {
				const auto ref = CopyOf(id);  // one the engine unloaded with its cell: back, or forgotten (see Refresh)
				facts.copyThere = ref && !ref->IsDeleted() && ref->GetFormID() == copy->local;
				// Just placed, it has a cell before its 3D (SpawnBook::LOAD_WAIT): not taken for one that unloaded.
				if (facts.copyThere && ref->GetParentCell()) {
					facts.copyAt = SpotOf(ref.get());
					facts.copyLoaded = ref->Get3D() != nullptr;
				}
			} else {
				// An encounter's named character gets a copy unless our own game runs that encounter now (its own is here); so
				// does an actor of a radiant's run we follow with the owner's actors (RadiantSync::FollowsSpawn: it goes into the
				// quest's alias here, in place of our own, which is removed).
				facts.baseOk = BaseOk(from, (copy->spawn.kind == Protocol::SpawnKind::kEncounter && !QuestRunningHere(copy->spawn.quest)) || RadiantSync::FollowsSpawn(id));
				const auto seen = seenAt.try_emplace(id, now).first->second;
				facts.waiting = !NpcSync::RollOf(id) && now - seen < NpcRoll::IDENTITY_WAIT;
			}
			// A runtime NPC of our own of the same encounter standing where their game has it stands for it
			// (SpawnBook::Facts::oursThere): looked for only as the copy is about to be placed, never around one placed
			// already, and not kept: ours gone, the copy is placed.
			// Not for the host's game's settler (SettlerSpawns.h): a settler of our own save's standing there is another person,
			// and a job asked for the host's names its copy.
			if (copy->local == 0 && !SettlerSpawns::IsSettler(copy->spawn.kind) && theirs.WhyNotPlaced(id, facts, now) == SpawnBook::Theirs::Why::kPlace) {
				auto& look = oursThereLooked[id];
				if (look.at == Clock::time_point{} || now - look.at >= OURS_THERE_LOOK) {
					look.at = now;
					const auto roll = NpcSync::RollOf(id);
					const auto own = OwnMadeNpcAt(copy->spawn.base, roll ? roll->originalBase : 0, copy->at);
					if (own != 0 && own != look.own) {
						++npcCopiesWithheld;
						REX::INFO("Spawns: {:08X} of our own, the same encounter's, stands where player {}'s spawn {:08X} (base {:08X}) is: ours stands for it, no copy is placed while it does",
							own, copy->spawn.playerId, id, copy->spawn.base);
					}
					look.own = own;
				}
				facts.oursThere = look.own != 0;
			} else if (copy->local != 0) {
				oursThereLooked.erase(id);
			}
			const auto act = theirs.Decide(id, facts, now);
			switch (act) {
			case SpawnBook::Act::kPlace:
				{
					if (placedNow >= PLACE_PER_FRAME) {
						break;
					}
					++placedNow;
					const auto playerId = copy->spawn.playerId;
					const auto base = copy->spawn.base;
					const auto handle = PlaceCopy(id, *copy, from, cell);
					const auto ref = handle.get();
					if (!ref) {
						theirs.Placed(id, 0, now);
						++placeFailed;
						REX::WARN("Spawns: could not place a copy of player {}'s {:08X} (base {:08X}, from {:08X})", playerId, id, base, from);
						break;
					}
					placed[id] = { handle, now, SaveGuard::IdsOf(ref.get()) };
					theirs.Placed(id, ref->GetFormID(), now);
					++copiesPlaced;
					if (logged < LOGGED) {
						++logged;
						REX::INFO("Spawns: placed {} as our copy of player {}'s {:08X}", RefTag::Of(ref.get()), playerId, id);
					}
					break;
				}
			case SpawnBook::Act::kDiscard:
				Discard(id);
				break;
			case SpawnBook::Act::kLost:
				NpcSync::DropMirror(id);
				placed.erase(id);
				theirs.Unplaced(id);
				++copiesLost;
				break;
			case SpawnBook::Act::kForget:
				theirs.Forget(id);
				seenAt.erase(id);
				break;
			case SpawnBook::Act::kMove:     // a frame's (UpdateFrameCopy): never an NPC's
			case SpawnBook::Act::kAbandon:
			case SpawnBook::Act::kNone:
				break;
			}
			if (act == SpawnBook::Act::kNone && copy->local == 0) {
				NoteUnplaced(id, *copy, theirs.WhyNotPlaced(id, facts, now), facts, now);
			} else if (act != SpawnBook::Act::kNone) {
				unplacedWhy.erase(id);
			}
		}
		// A spawn forgotten some other way (its player gone, the server's word) takes its first sighting with it.
		std::erase_if(unplacedWhy, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(seenAt, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(oursThere, [&](std::uint32_t a_id) { return !theirs.Find(a_id); });
		std::erase_if(oursThereLooked, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(pairLooked, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(paired, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(pairedOwn, [&](const auto& a_entry) { return !paired.contains(a_entry.first); });
		std::erase_if(keptClear, [&](std::uint32_t a_id) { return !theirs.Find(a_id); });
		std::erase_if(theirSeq, [&](const auto& a_entry) { return !theirs.Find(a_entry.first); });
		std::erase_if(offeredEncounter, [&](const auto& a_entry) { return !ours.Holds(a_entry.first); });
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void DiscardCopies()
	{
		for (const auto id : theirs.Ids()) {
			const auto copy = theirs.Find(id);
			if (!copy || copy->local == 0) {
				continue;
			}
			// A frame copy someone is being sent out of is kept as it is, saved with the frame they wear (a load takes it with
			// the old world): buried, the save would have them wear a frame it does not have (RemotePlayers::SomeoneSentOutOf).
			if (const auto it = placed.find(id); it != placed.end()) {
				// One our player wears on loan is saved with them, as it is (FrameLoans: the LOAN record cleans it up after a load).
				if (const auto ref = it->second.handle.get(); ref && FrameLoans::KeepsThroughSave(ref.get())) {
					REX::INFO("Spawns: our copy {:08X} of player {}'s frame is kept through the save or load; our player wears it on loan", ref->GetFormID(), copy->spawn.playerId);
					continue;
				}
				if (const auto ref = it->second.handle.get(); ref && RemotePlayers::SomeoneSentOutOf(ref.get())) {
					REX::INFO("Spawns: our copy {:08X} of player {}'s frame is kept through the save or load; someone is being sent out of it", ref->GetFormID(), copy->spawn.playerId);
					RemotePlayers::NoteKeptThroughSave(ref.get());
					continue;
				}
			}
			Discard(id);
		}
	}

	bool DropLentCopy(std::uint32_t a_id)
	{
		if (!placed.contains(a_id)) {
			return false;
		}
		Discard(a_id);
		return true;
	}

	std::size_t CopiesNear(const RE::TESObjectREFR* a_anchor, float a_radius)
	{
		if (!a_anchor || !a_anchor->GetParentCell()) {
			return 0;
		}
		const auto  anchor = SpotOf(a_anchor);
		std::size_t near = 0;
		for (const auto& [id, copy] : placed) {
			const auto spawn = theirs.Find(id);
			// (The host's settlers made at run time stand at its settlement as copies too, with the shared workshop: no attackers.)
			if (!spawn || spawn->dead || spawn->gone || SettlerSpawns::IsSettler(spawn->spawn.kind) || Equipment::IsPowerArmorFrame(RE::TESForm::GetFormByID(spawn->spawn.base))) {
				continue;
			}
			const auto ref = copy.handle.get();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor || actor->IsDeleted() || actor->IsDisabled() || actor->IsDead(false) || !actor->Get3D() || !actor->GetParentCell()) {
				continue;
			}
			const auto at = SpotOf(actor);
			const float dx = at.x - anchor.x, dy = at.y - anchor.y, dz = at.z - anchor.z;
			if (at.cell == anchor.cell && at.worldspace == anchor.worldspace && dx * dx + dy * dy + dz * dz <= a_radius * a_radius) {
				++near;
			}
		}
		return near;
	}

	std::vector<RE::ObjectRefHandle> FrameCopies()
	{
		std::vector<RE::ObjectRefHandle> out;
		for (const auto& [id, copy] : placed) {
			const auto spawn = theirs.Find(id);
			if (spawn && Equipment::IsPowerArmorFrame(RE::TESForm::GetFormByID(spawn->spawn.base))) {
				out.push_back(copy.handle);
			}
		}
		return out;
	}

	void OnGameLoaded()
	{
		// Our copies went before the load (DiscardCopies); a handle left would name something of the loaded game.
		for (const auto id : theirs.Ids()) {
			theirs.Unplaced(id);
		}
		placed.clear();
		paired.clear();  // unpaired before the load (DiscardCopies): the loaded world's quests pair afresh
		pairedOwn.clear();
		pairLooked.clear();
		killedHere.clear();
		pendingKills.Clear();
		leaving.clear();
		blockedNoted.clear();  // the loaded world's ids name other NPCs
		twinsNoted.clear();
		unplacedWhy.clear();
		oursThere.clear();  // the loaded world's frames are looked at afresh
		keptClear.clear();
		encounters.Clear();  // the loaded world's encounters are its own runs, settled as its first frame comes (Frame)
		settleRuns = true;
		offeredEncounter.clear();
		calmOffered.clear();
		shareSettle.Clear();
		calmHeldNoted.clear();
		settlersHeldNoted.clear();
		pendingStops.clear();  // a quest of the world left behind
		pendingSweeps.clear();
		pendingRemovals.clear();
		followRemovals.clear();
		{
			std::scoped_lock l{ hitsLock };  // hits by NPCs of the world left behind
			hitsOnUs.clear();
		}
		encounterActors.clear();
		// Our NPCs' ids name the loaded game's references now: the ones we shared are let go (the others' copies go,
		// before anything the loaded game makes is offered under the same id), and what it makes is shared afresh.
		const auto released = ours.Ids();
		ours.Clear();
		ownFrames.clear();
		if (LocalPlayer() == 0) {
			return;
		}
		for (std::size_t i = 0; i < released.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
			const auto end = (std::min)(released.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
			outgoing.push_back(Protocol::EncodeRelease(std::vector<std::uint32_t>(released.begin() + i, released.begin() + end)));
		}
	}

	void Reset()
	{
		// Every copy goes, as at a save, but none is kept: one someone is being sent out of waits for them in RemotePlayers
		// (or, with the session over, is left to them).
		for (const auto id : theirs.Ids()) {
			if (const auto copy = theirs.Find(id); copy && copy->local != 0) {
				Discard(id);
			}
		}
		placed.clear();
		paired.clear();
		pairedOwn.clear();
		pairLooked.clear();
		seenAt.clear();
		unplacedWhy.clear();
		blockedNoted.clear();
		twinsNoted.clear();
		absentNoBase.clear();
		theirs.Clear();
		ours.Clear();
		ownFrames.clear();
		oursThere.clear();
		keptClear.clear();
		killedHere.clear();
		pendingKills.Clear();
		encounters.Clear();
		theirSeq.clear();
		offeredEncounter.clear();
		calmOffered.clear();
		shareSettle.Clear();
		calmHeldNoted.clear();
		settlersHeldNoted.clear();
		pendingStops.clear();
		pendingSweeps.clear();
		pendingRemovals.clear();
		followRemovals.clear();
		{
			std::scoped_lock l{ hitsLock };  // hits by NPCs of the world left behind
			hitsOnUs.clear();
		}
		encounterActors.clear();
		settleRuns = true;  // the next session's world is settled as it comes up
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("spawns: ours={} theirs={} copies={} reported={} granted={} refused={} received={} placed={} placeFailed={} discarded={} lost={} killed={}/{} kills: killsTried={} killsLifted={} killsWaiting={} killsGivenUp={} frames: ours={} reported={} placed={} moved={} taken={} unloaded={} back={} oursThere={}/{} keptClear={}/{} npcOursThere={} copiesRolled={}/{} absent={}/{} questActors: paired={} pairs={} unpairs={} keyed={} twinsHeld={} restored={}", ours.Size(),
			theirs.Size(), theirs.PlacedCount(), reported, granted, refused, received, copiesPlaced, placeFailed, copiesDiscarded, copiesLost, copiesKilled, oursKilled, killsTried, protectionsLifted, pendingKills.Size(), killsGivenUp, ownFrames.size(), framesReported,
			frameCopiesPlaced, frameCopiesMoved, frameCopiesTaken, copiesUnloaded, copiesBack, oursThere.size(), frameCopiesWithheld, keptClear.size(), frameCopiesKeptClear, npcCopiesWithheld, copiesRolled, copiesNotRolled, absentBooked, absentNoBase.size(),
			paired.size(), pairs, unpairs, keyedReported, twinsHeld, rollsRestored) +
		       std::format(" encounters: runs={} shared={} calm={} calmHeld={} received={} stopped={} removed={} over={} sharedOn={} theirsOver={} kindsVetoed={} hurt={} followRemoved={} followKept={} radiantActors={} settle={}/{}/{}", encounters.Size(), encountersShared, calmOffered.size(), calmHeld,
			   encountersReceived, encountersStopped, actorsRemoved, encountersOver, sharedOn, theirsOver, kindsVetoed, runsHurt, followRemoved, followKept, radiantActorsShared, shareSettle.Size(),
			   shareSettle.Begun(), shareSettle.Expired()) +
		       // The host's game's settlers (SettlerSpawns.h). Every field's name is this segment's own: the rig reads a field by its name.
		       std::format(" settlers: settlersOurs={} settlersShared={} settlersHeld={} settlersTheirs={} settlersReceived={} settlersVetoed={}", ours.Settlers(), settlersShared, settlersHeld,
			   TheirSettlers(), settlersReceived, settlersVetoed) +
		       Unplaced() + " " + FrameLoans::Describe();
	}
}
