#include "game/RadiantSync.h"

#include "Config.h"
#include "Text.h"
#include "game/EncounterRunBook.h"
#include "game/GameStop.h"
#include "game/Hud.h"
#include "game/Official.h"
#include "game/Papyrus.h"
#include "game/QuestActors.h"
#include "game/QuestStageLayout.h"
#include "game/QuestStart.h"
#include "game/QuestSync.h"
#include "game/RadiantRules.h"
#include "game/Spawns.h"
#include "game/StageLog.h"
#include "game/StageRules.h"
#include "game/Story.h"
#include "game/WorldSync.h"

#include <array>
#include <cstring>
#include <deque>
#include <mutex>

namespace RadiantSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using RadiantRules::Alias;
		using RadiantRules::Answer;
		using RadiantRules::Fill;
		using RadiantRules::Kind;
		using RadiantRules::Run;
		using StageLog::QUEST_RUNNING;

		// A friend's run started here is running by then, or it isn't shared here (and this game's own run, stopped for
		// theirs, has stopped by then).
		constexpr auto START_TIMEOUT = 10s;
		// TESQuest::data.flags: a quest that can't be started again once it stops.
		constexpr std::uint16_t QUEST_RUN_ONCE = 0x0100;
		constexpr std::size_t MAX_PENDING = 16;
		constexpr std::uint16_t REF_FORCED = static_cast<std::uint16_t>(RE::BGSRefAlias::FILL_TYPE::kForced);
		// BGSBaseAlias::aliasID of an alias that has none.
		constexpr std::uint32_t NO_ALIAS = 0xFFFFFFFF;

		// Run numbers start at this game's start (SerialSeed): a game that restarts never numbers a run as one of its last.
		RadiantRules::Book                     book{ RadiantRules::SerialSeed(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count())) };
		std::uint32_t                          self = 0;
		bool                                   inSession = false;
		std::vector<std::vector<std::uint8_t>> outgoing;
		std::deque<Protocol::RadiantStart>     pending;  // friends' runs to start or compare here, in the order they came
		std::set<std::uint32_t>                stopped;  // shared runs' quests seen stopped at the last poll: ended at this one
		std::uint64_t                          session = 0;  // the session welcomed into last (0: none since another server)
		// The runs shared in the world a save loaded (its record), or when the session ended: shared again as they were on the
		// welcome of their session (Adopt), forgotten in another.
		std::optional<RadiantRules::Saved> held;
		std::mutex                         loadLock;  // a load's record comes on its thread
		std::optional<RadiantRules::Saved> loaded;

		std::uint32_t begun = 0;     // runs of ours shared
		std::uint32_t joined = 0;    // friends' runs shared here
		std::uint32_t declined = 0;  // friends' runs left each player's own here
		std::uint32_t sent = 0;      // stages reported under runs
		std::uint32_t taken = 0;     // friends' stages queued here
		std::uint32_t restored = 0;  // runs shared again as they were (a save's record, or after a reconnect)

		// Radiants whose actors are made at runtime (RadiantRules::Entry::spawned). The friends' runs of them shared here
		// (followed: their fills as their game last sent them, the actors by their spawn ids once their game shares them),
		// and the aliases of each forced onto our copies of those (Encounters::RefillsOf, Refill; forgotten with the run).
		// Our runs of such quests: their fills as last sent, sent again when they change (ResendChangedFills). Our run of
		// one left hollow (RadiantRules::HollowJob: a followed run's once the owner's copies are gone, with the session, the
		// owner or a save) and since when nothing stands in for it (SweepHollow).
		RadiantRules::Followed                                             followed;
		std::map<std::uint32_t, std::vector<Encounters::AliasForce>>       forced;
		std::map<std::uint32_t, std::vector<Protocol::RadiantFill>>        sentFills;
		RadiantRules::HollowClock                                          hollow;
		constexpr std::uint32_t                                            LOGGED = 20;
		std::uint32_t                                                      refilled = 0, refillsFailed = 0, resentFills = 0, unfollowed = 0, hollowRestarted = 0;
		// RefillAliases, for this game's followed radiants and encounters alike: first asks, asks made again (not held when
		// read back, or lost since), aliases read back holding their copies after more than one ask, aliases given up on;
		// and the ForceRefTo calls the script VM ran or cancelled (ForceDone, told on one of its threads).
		std::uint32_t              refillsNew = 0, refillsRetried = 0, refillsHeld = 0, refillsGivenUp = 0, refillsNotTaken = 0;
		std::atomic<std::uint32_t> forceCallsRan{ 0 }, forceCallsCancelled{ 0 };

		// Told once a ForceRefTo call has run (or that the VM cancelled it): counted, for the log of a force that didn't hold.
		class ForceDone final :
			public RE::BSScript::IStackCallbackFunctor
		{
		public:
			void CallQueued() override {}
			void CallCanceled() override
			{
				if (!told.exchange(true)) {
					++forceCallsCancelled;
				}
			}
			void StartMultiDispatch() override {}
			void EndMultiDispatch() override {}
			void operator()(RE::BSScript::Variable) override
			{
				if (!told.exchange(true)) {
					++forceCallsRan;
				}
			}

		private:
			std::atomic<bool> told{ false };
		};

		// Asks made again through the engine's own force (Encounters::ForceWay::kEngine), and polls an ask waited through as
		// the engine would not take a force then (Encounters::ForceOpen).
		std::uint32_t forceDirect = 0, forceWaits = 0, forceWaitsLogged = 0;

		// TESQuest::ForceRefIntoAlias(aliasID, ref) (1.11.240: +66FEB0, ID 2207791): what ReferenceAlias.ForceRefTo's native
		// (+1116D50) jumps to once the quest is enabled and the reference not None, and what the console's
		// ForceRefIntoAlias calls (+5D9B5D) on the game's thread. It finds the alias, leaves it alone if it holds the
		// reference already, otherwise takes the old reference's alias data off it, puts the new one's handle in and gives
		// it the alias's data, under the quest's alias lock. Called only if it begins as read.
		constexpr REL::ID FORCE_INTO_ALIAS{ 2207791 };
		constexpr std::array<std::uint8_t, 16> FORCE_INTO_ALIAS_BEGINS{ 0x40, 0x55, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4D, 0x8B, 0xF8, 0x48, 0x8B, 0xF1, 0xE8 };

		bool ForceIntoAliasAsRead()
		{
			static const bool asRead = [] {
				const auto at = FORCE_INTO_ALIAS.address();
				const bool same = at != 0 && std::memcmp(reinterpret_cast<const void*>(at), FORCE_INTO_ALIAS_BEGINS.data(), FORCE_INTO_ALIAS_BEGINS.size()) == 0;
				if (!same) {
					REX::WARN("RadiantSync: TESQuest::ForceRefIntoAlias (ID 2207791) does not begin as read; an alias not holding our copy is asked again through the script VM only");
				}
				return same;
			}();
			return asRead;
		}

		// What a quest's alias holds and how the quest stands, for the log of a force that didn't hold: every handle the
		// alias holds (a reference alias holds one; ForceRefIntoAlias swaps the first), the quest's flags and whether its
		// references' promotion is pending, and our copy's own state.
		std::string AliasState(RE::TESQuest* a_quest, std::uint32_t a_alias, std::uint32_t a_copy)
		{
			std::optional<std::size_t> index;
			for (std::size_t i = 0; i < a_quest->aliases.size(); ++i) {
				if (const auto alias = a_quest->aliases[i]; alias && alias->aliasID == a_alias) {
					index = i;
					break;
				}
			}
			std::vector<RE::ObjectRefHandle> handles;
			if (index) {
				const RE::BSAutoReadLock l{ a_quest->aliasAccessLock };
				if (*index < a_quest->aliasedHandles.size()) {
					for (const auto& handle : a_quest->aliasedHandles[static_cast<std::uint32_t>(*index)]) {
						handles.push_back(handle);
					}
				}
			}
			std::string held;
			for (const auto& handle : handles) {
				const auto ref = handle.get();
				held += std::format("{}{:08X}{}", held.empty() ? "" : " ", ref ? ref->GetFormID() : 0u, ref && ref->IsDeleted() ? " deleted" : "");
			}
			const auto copy = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_copy);
			const auto cell = copy ? copy->GetParentCell() : nullptr;
			return std::format("alias at {} holds {} handle(s) [{}]; quest flags {:04X}, promotion {}; copy {}", index ? std::to_string(*index) : std::string{ "none" }, handles.size(),
				held, a_quest->data.flags, a_quest->promoteTask.get() ? "pending" : "none",
				copy ? std::format("{}{}{} in {:08X}", copy->IsDeleted() ? "deleted " : "", copy->IsDisabled() ? "disabled " : "", copy->Get3D() ? "3D" : "no 3D", cell ? cell->GetFormID() : 0u) :
					   std::string{ "gone" });
		}

		// Forces our reference into the quest's reference alias through the engine's own function, on the game's thread, as
		// the console's ForceRefIntoAlias does (its test first: Encounters::ForceOpen). False when it could not be made.
		bool ForceAliasRefEngine(RE::TESQuest* a_quest, std::uint32_t a_alias, std::uint32_t a_local)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_local);
			if (!a_quest || !ref || ref->IsDeleted() || !ForceIntoAliasAsRead() || !Encounters::ForceOpen(a_quest->data.flags, a_quest->promoteTask.get() != nullptr)) {
				return false;
			}
			using force_t = void(RE::TESQuest*, std::uint32_t, RE::TESObjectREFR*);
			static REL::Relocation<force_t> force{ FORCE_INTO_ALIAS };
			force(a_quest, a_alias, ref);
			++forceDirect;
			return true;
		}

		// Preston's attacks (RadiantRules::Entry::attack). Whether this game hosts the session (only the host's attack is
		// shared), the attacks of the host's runs held back here (RadiantRules::HoldsAttack), when their timers were last
		// pushed out, and whether a loaded world's are still to be let go (a save made while one was held keeps its
		// PlayerIsWaiting set, and the hold's timer: no save is made while the player waits, so set then, it is ours).
		bool                    hosting = false;
		std::set<std::uint32_t> attacksHeld;
		Clock::time_point       attackPushed{};
		bool                    attacksLoaded = false;
		std::uint32_t           attackHolds = 0, attacksLet = 0, attacksResumed = 0, attackTimersCancelled = 0;

		// A friend's run of such a quest shared here: followed with its fills from now on (a newer send of it keeps the newest).
		void Follow(std::uint32_t a_quest, const Run& a_run, std::span<const Protocol::RadiantFill> a_fills)
		{
			if (followed.Follow(a_quest, a_run, a_fills, Clock::now())) {
				forced.erase(a_quest);
			}
		}

		void Unfollow(std::uint32_t a_quest)
		{
			followed.Ended(a_quest);
			forced.erase(a_quest);
		}

		// ---- How the game fills an alias ----------------------------------------------------------------

		// How a forced reference alias holds its reference (BGSRefAlias::fillData's first word), read off the game's
		// own forced aliases once: the layout isn't known from the headers, and a wrong write would start a quest on
		// a reference that isn't one.
		enum class RefRep : std::uint8_t
		{
			kUnknown,
			kPointer,
			kFormId,
			kHandle,
		};

		struct Calibration
		{
			bool                         done = false;
			RefRep                       ref = RefRep::kUnknown;
			std::optional<std::uint16_t> locForced;  // a location alias's fill type for a forced location
			std::string                  summary;
		};
		Calibration calibration;

		// The form ID at a_address + 0x14 (TESForm::formID), the read guarded: a value that isn't a form's address
		// costs a 0, not the game.
		std::uint32_t FormIdAt(std::uint64_t a_address)
		{
			__try {
				return *reinterpret_cast<const std::uint32_t*>(a_address + 0x14);
			} __except (1) {
				return 0;
			}
		}

		std::string_view TypeOf(const RE::BGSBaseAlias* a_alias)
		{
			const char* type = a_alias->QType().c_str();
			return type ? std::string_view{ type } : std::string_view{};
		}

		RE::BGSRefAlias* AsRef(RE::BGSBaseAlias* a_alias)
		{
			return a_alias && TypeOf(a_alias) == RE::BGSRefAlias::TYPE ? static_cast<RE::BGSRefAlias*>(a_alias) : nullptr;
		}

		RE::BGSLocAlias* AsLoc(RE::BGSBaseAlias* a_alias)
		{
			return a_alias && TypeOf(a_alias) == RE::BGSLocAlias::TYPE ? static_cast<RE::BGSLocAlias*>(a_alias) : nullptr;
		}

		std::span<RE::TESQuest*> AllQuests()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return {};
			}
			auto& quests = handler->GetFormArray<RE::TESQuest>();
			return { quests.data(), quests.size() };
		}

		RefRep Classify(std::uint64_t a_word)
		{
			if (a_word >= 0x10000 && (a_word >> 48) == 0) {
				if (const auto id = FormIdAt(a_word); id != 0 && reinterpret_cast<std::uint64_t>(RE::TESForm::GetFormByID(id)) == a_word) {
					return RefRep::kPointer;
				}
			}
			if ((a_word >> 32) == 0) {
				const auto form = RE::TESForm::GetFormByID(static_cast<std::uint32_t>(a_word));
				if (form && form->As<RE::TESObjectREFR>()) {
					return RefRep::kFormId;
				}
				RE::ObjectRefHandle handle;
				const auto          native = static_cast<std::uint32_t>(a_word);
				std::memcpy(&handle, &native, sizeof(native));
				if (handle.get()) {
					return RefRep::kHandle;
				}
			}
			return RefRep::kUnknown;
		}

		const char* Name(RefRep a_rep)
		{
			switch (a_rep) {
			case RefRep::kPointer:
				return "pointer";
			case RefRep::kFormId:
				return "form id";
			case RefRep::kHandle:
				return "handle";
			default:
				return "unknown";
			}
		}

		// Reads the game's forced aliases: a representation shared by nine in ten of them (and at least a few) is the one.
		void Calibrate()
		{
			if (calibration.done) {
				return;
			}
			calibration.done = true;
			std::map<RefRep, std::size_t>        refs;
			std::map<std::uint16_t, std::size_t> locs;
			std::size_t                          refTotal = 0;
			std::size_t                          locTotal = 0;
			std::size_t                          tails = 0;  // forced fills with more than one word set
			for (const auto quest : AllQuests()) {
				if (!quest || !Official::IsOfficialForm(quest)) {
					continue;
				}
				for (const auto alias : quest->aliases) {
					if (const auto ref = AsRef(alias); ref && ref->fillType == REF_FORCED && ref->fillData.data[0] != 0) {
						++refTotal;
						++refs[Classify(ref->fillData.data[0])];
						tails += ref->fillData.data[1] != 0 || ref->fillData.data[2] != 0;
					} else if (const auto loc = AsLoc(alias); loc && loc->forcedLoc) {
						++locTotal;
						++locs[loc->fillType];
					}
				}
			}
			const auto best = [](const auto& a_counts, std::size_t a_total) {
				using Key = typename std::remove_cvref_t<decltype(a_counts)>::key_type;
				std::optional<Key> out;
				for (const auto& [key, count] : a_counts) {
					if (a_total >= 5 && count * 10 >= a_total * 9) {
						out = key;
					}
				}
				return out;
			};
			const auto ref = best(refs, refTotal);
			calibration.ref = ref && tails * 10 < refTotal ? *ref : RefRep::kUnknown;
			calibration.locForced = best(locs, locTotal);
			calibration.summary = std::format("forced references: {} of {} read as {} ({} with more set); forced locations: {} with fill type {}", ref ? refs[*ref] : 0,
				refTotal, Name(calibration.ref), tails, locTotal, calibration.locForced ? std::to_string(*calibration.locForced) : "unknown");
			if (calibration.ref == RefRep::kUnknown || !calibration.locForced) {
				REX::WARN("RadiantSync: can't tell how this game holds a forced alias ({}); radiant quests stay each player's own", calibration.summary);
			} else {
				REX::INFO("RadiantSync: {}", calibration.summary);
			}
		}

		bool Calibrated()
		{
			Calibrate();
			return calibration.ref != RefRep::kUnknown && calibration.locForced.has_value();
		}

		// ---- Aliases and fills ---------------------------------------------------------------------------

		std::uint32_t Flags(const RE::BGSBaseAlias* a_alias)
		{
			return a_alias->flags.underlying();
		}

		// A location alias, by what is set on it: a forced location or another quest's is the same everywhere; the rest
		// by its fill type (RadiantRules::LocFill).
		Fill LocFillOf(const RE::BGSLocAlias* a_alias)
		{
			if (!(Flags(a_alias) & RadiantRules::ALIAS_BY_OTHERS) && (a_alias->forcedLoc || a_alias->externalQuest)) {
				return Fill::kFixed;
			}
			return RadiantRules::LocFill(a_alias->fillType, Flags(a_alias));
		}

		struct GameAlias
		{
			Alias                rules;
			RE::BGSBaseAlias*    alias = nullptr;
			std::string          name;  // rules.name points into it (kept in a list that doesn't move)
		};

		std::vector<GameAlias> AliasesOf(RE::TESQuest* a_quest)
		{
			std::vector<GameAlias> out;
			for (const auto alias : a_quest->aliases) {
				if (!alias || alias->aliasID == NO_ALIAS) {
					continue;
				}
				GameAlias entry;
				entry.alias = alias;
				const char* name = alias->aliasName.c_str();
				entry.name = name ? name : "";
				entry.rules.id = alias->aliasID;
				entry.rules.optional = (Flags(alias) & RadiantRules::ALIAS_OPTIONAL) != 0;
				if (const auto ref = AsRef(alias)) {
					entry.rules.kind = Kind::kRef;
					entry.rules.fill = RadiantRules::RefFill(ref->fillType, Flags(ref));
				} else if (const auto loc = AsLoc(alias)) {
					entry.rules.kind = Kind::kLoc;
					entry.rules.fill = LocFillOf(loc);
				} else {
					continue;  // a collection: its members come from the others
				}
				out.push_back(std::move(entry));
			}
			for (auto& entry : out) {
				entry.rules.name = entry.name;
			}
			return out;
		}

		std::vector<Alias> Rules(const std::vector<GameAlias>& a_aliases)
		{
			std::vector<Alias> out;
			out.reserve(a_aliases.size());
			for (const auto& entry : a_aliases) {
				out.push_back(entry.rules);
			}
			return out;
		}

		// What fills an alias here (this game's form, 0: empty).
		std::uint32_t Here(RE::TESQuest* a_quest, Kind a_kind, std::uint32_t a_alias)
		{
			if (a_kind == Kind::kRef) {
				const auto ref = a_quest->GetAliasedRef(a_alias).get();
				return ref ? ref->GetFormID() : 0;
			}
			RE::BSAutoReadLock lock{ a_quest->aliasAccessLock };
			const auto         it = a_quest->aliasedLocMap.find(a_alias);
			return it != a_quest->aliasedLocMap.end() && it->second ? it->second->GetFormID() : 0;
		}

		// Our form as the friends' games name it: a plugin's as it is, our runtime NPC by the spawn id the session
		// knows it by (or as one it doesn't share). The player is in no friend's game: left out (nullopt).
		std::optional<std::uint32_t> Wire(Kind a_kind, std::uint32_t a_local)
		{
			if (a_local == 0) {
				return 0u;
			}
			if (a_local == Protocol::PLAYER_REF_ID) {
				return std::nullopt;
			}
			if ((a_local >> 24) != 0xFF) {
				return a_local;
			}
			if (a_kind == Kind::kLoc) {
				return std::nullopt;
			}
			const auto spawn = Spawns::IdOf(a_local);
			return spawn ? spawn : Protocol::RADIANT_UNSHARED_REF;
		}

		// A friend's form as ours (RadiantRules::MakePlan's a_resolve).
		std::optional<std::uint32_t> Resolve(Kind a_kind, std::uint32_t a_form)
		{
			if (a_kind == Kind::kLoc) {
				return RE::TESForm::GetFormByID<RE::BGSLocation>(a_form) ? std::optional{ a_form } : std::nullopt;
			}
			if (a_form == Protocol::RADIANT_UNSHARED_REF) {
				return std::nullopt;
			}
			if (Protocol::IsSpawnRef(a_form)) {
				const auto local = Spawns::LocalOf(a_form);
				return local ? std::optional{ local } : std::nullopt;
			}
			return RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_form) ? std::optional{ a_form } : std::nullopt;
		}

		std::vector<Protocol::RadiantFill> FillsOf(RE::TESQuest* a_quest)
		{
			std::vector<Protocol::RadiantFill> out;
			for (const auto& entry : AliasesOf(a_quest)) {
				const auto wire = Wire(entry.rules.kind, Here(a_quest, entry.rules.kind, entry.rules.id));
				if (wire && out.size() < Protocol::MAX_RADIANT_FILLS) {
					out.push_back({ entry.rules.id, static_cast<std::uint8_t>(entry.rules.kind), *wire });
				}
			}
			return out;
		}

		std::vector<RadiantRules::Mismatch> CompareHere(RE::TESQuest* a_quest, const std::vector<GameAlias>& a_aliases, const std::vector<Protocol::RadiantFill>& a_fills,
			std::span<const std::uint32_t> a_unforced = {})
		{
			const auto rules = Rules(a_aliases);
			return RadiantRules::Compare(rules, a_fills, Resolve, [&](Kind a_kind, std::uint32_t a_alias) { return Here(a_quest, a_kind, a_alias); }, a_unforced);
		}

		std::string Describe(const RadiantRules::Mismatch& a_mismatch)
		{
			return std::format("'{}' is {:08X} in their game and {:08X} here", a_mismatch.name, a_mismatch.theirs, a_mismatch.ours);
		}

		std::string FirstDecisive(const std::vector<RadiantRules::Mismatch>& a_mismatches)
		{
			const auto it = std::ranges::find_if(a_mismatches, &RadiantRules::Mismatch::decisive);
			return it != a_mismatches.end() ? Describe(*it) : std::string{};
		}

		// ---- Starting a friend's run here ----------------------------------------------------------------

		// An alias's definition as the game files it, while it is forced for a start.
		struct Saved
		{
			RE::BGSBaseAlias*            alias = nullptr;
			std::uint32_t                flags = 0;
			std::uint16_t                fillType = 0;
			std::array<std::uint64_t, 3> data{};
			RE::BGSLocation*             forcedLoc = nullptr;
			RE::TESCondition*            conditions = nullptr;  // an optional alias's, lifted while it is forced
		};

		struct Starting
		{
			std::uint32_t              quest = 0;
			Run                        run;
			Protocol::RadiantStart     start;
			std::vector<Saved>         saved;  // the definitions forced for it, put back once it runs
			std::vector<std::uint32_t> unforced;  // the plan's (RadiantRules::Plan::unforced)
			std::vector<std::uint16_t> stages;  // the friend's stages that came meanwhile
			// The run's fills as sent again meanwhile (an actor of theirs shared since: RadiantRules::Supersedes), kept for
			// the run followed once it runs; the start itself goes by the fills it was planned from.
			std::optional<std::vector<Protocol::RadiantFill>> newerFills;
			Clock::time_point          since{};
			bool                       stopping = false;  // a run of it here is stopping first (ours, stopped for it, or one that just ended): started once that is over
			bool                       uncount = false;   // ... ours, whose offer stays counted at the Railroad's HQ: counted off once stopped
			bool                       replaced = false;  // this game's own run of it was stopped for it: started again if theirs isn't (RestartOwn)
		};
		std::optional<Starting> starting;

		// A stop of the quest is still under way: it runs, or the game hasn't shut its last run down yet (QuestStartRules:
		// a start now would run with that run's fills).
		bool StopUnderWay(const RE::TESQuest* a_quest)
		{
			return (a_quest->data.flags & QUEST_RUNNING) || QuestStartRules::Waits(QuestStart::Judge(a_quest));
		}

		const char* QuestName(const RE::TESQuest* a_quest)
		{
			const char* name = a_quest ? a_quest->GetFullName() : nullptr;
			return name ? name : "";
		}

		// Whether the quest's actors are made at runtime and the run's owner's (RadiantRules::Entry::spawned).
		bool Spawned(std::uint32_t a_quest)
		{
			const auto entry = RadiantSync::EntryOf(a_quest);
			return entry && entry->spawned;
		}

		// Whether the run of the quest shared here is this game's own.
		bool OursRun(std::uint32_t a_quest)
		{
			return std::ranges::any_of(book.Ours(self), [&](const auto& a_run) { return a_run.first == a_quest; });
		}

		// The quest's WorkshopAttackScript object (MinRadiantOwned11's extends it), as the VM's ForEachBoundObject visits
		// the quest's scripts: FindBoundObject by name misses one whose type extends the name.
		RE::BSTSmartPointer<RE::BSScript::Object> AttackScriptOf(RE::TESQuest* a_quest)
		{
			const auto game = RE::GameVM::GetSingleton();
			const auto vm = game ? game->GetVM() : nullptr;
			if (!vm || !a_quest) {
				return {};
			}
			const auto& handles = vm->GetObjectHandlePolicy();
			const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESQuest>(), a_quest);
			if (handle == handles.EmptyHandle()) {
				return {};
			}
			RE::BSTSmartPointer<RE::BSScript::Object>                                                 found;
			const RE::BSTThreadScrapFunction<RE::BSContainer::ForEachResult(RE::BSScript::Object*)> visit{ [&](RE::BSScript::Object* a_object) {
				for (auto type = a_object ? a_object->type.get() : nullptr; type; type = type->GetParent()) {
					const char* name = type->GetName();
					if (name && _stricmp(name, RadiantRules::ATTACK_SCRIPT.data()) == 0) {
						found = RE::BSTSmartPointer<RE::BSScript::Object>{ a_object };
						return Papyrus::BOUND_DONE;
					}
				}
				return Papyrus::BOUND_NEXT;
			} };
			vm->ForEachBoundObject(handle, visit);
			return found;
		}

		// A script variable of the object by name (PlayerIsWaiting is one, not a property): the object's variables hold its
		// types' in turn, the oldest parent's first.
		RE::BSScript::Variable* VariableOf(RE::BSScript::Object& a_object, std::string_view a_name)
		{
			std::vector<const RE::BSScript::ObjectTypeInfo*> chain;
			for (const RE::BSScript::ObjectTypeInfo* type = a_object.type.get(); type; type = type->GetParent()) {
				chain.insert(chain.begin(), type);
			}
			const std::string plain{ a_name };
			const std::string mangled = std::format("::{}_var", a_name);
			std::uint32_t     offset = 0;
			for (const auto type : chain) {
				const auto infos = type->GetVariableIter();
				for (std::uint32_t i = 0; infos && i < type->GetNumVariables(); ++i) {
					const char* name = infos[i].name.c_str();
					if (name && (_stricmp(name, plain.c_str()) == 0 || _stricmp(name, mangled.c_str()) == 0)) {
						return std::addressof(a_object.variables[offset + i]);
					}
				}
				offset += type->GetNumVariables();
			}
			return nullptr;
		}

		// The attack's timer (ID 0) started again a_hours of game time from now.
		void StartAttackTimer(const RE::BSTSmartPointer<RE::BSScript::Object>& a_script, float a_hours)
		{
			Papyrus::CallOn(a_script, "StartTimerGameTime", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
				a_out[0] = a_hours;
				a_out[1] = RadiantRules::ATTACK_TIMER;
			});
		}

		// This game's own attack of the host's run held back (RadiantRules::ATTACK_*): the script told the player waits, and
		// (a_push) its timer pushed out of reach. Set at once as the host's run is shared here, before the stages that came
		// with it (its 120 among them) are set, and again every poll.
		void HoldAttack(RE::TESQuest* a_quest, bool a_push)
		{
			const auto script = AttackScriptOf(a_quest);
			const auto waiting = script ? VariableOf(*script, RadiantRules::ATTACK_WAITING) : nullptr;
			if (!waiting || !waiting->is<bool>()) {
				if (attacksHeld.insert(a_quest->GetFormID()).second) {
					REX::WARN("RadiantSync: {:08X} '{}': no {}.{} to hold its own attack back with", a_quest->GetFormID(), QuestName(a_quest), RadiantRules::ATTACK_SCRIPT,
						RadiantRules::ATTACK_WAITING);
				}
				return;
			}
			if (!RE::BSScript::get<bool>(*waiting)) {
				*waiting = true;
			}
			if (a_push) {
				StartAttackTimer(script, RadiantRules::ATTACK_TIMER_HELD);
			}
			if (attacksHeld.insert(a_quest->GetFormID()).second) {
				++attackHolds;
				REX::INFO("RadiantSync: {:08X} '{}' is the host's attack: this game's own is held back (its attackers stay disabled)", a_quest->GetFormID(),
					QuestName(a_quest));
			}
		}

		// The attack's timer (ID 0) cancelled (the script's CancelTimerGameTime; one that isn't running is left as it is).
		void CancelAttackTimer(const RE::BSTSmartPointer<RE::BSScript::Object>& a_script)
		{
			Papyrus::CallOn(a_script, "CancelTimerGameTime", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 1 };
				a_out[0] = RadiantRules::ATTACK_TIMER;
			});
		}

		// A held attack let go (the host's run no longer shared here, the quest stopped, or a loaded world's): the script
		// told the player isn't waiting, and the hold's timer never left running (RadiantRules::LetGo): started again for
		// an hour where the attack's clock would be running in a game of one, cancelled everywhere else (a quest that
		// stopped among them: its script stays bound, and its timer would start it again). a_held: this game held it until
		// now, so it is let go whatever PlayerIsWaiting says; a loaded world's only where that is set. The quest is then the
		// player's own, as any after its session.
		void LetAttackGo(RE::TESQuest* a_quest, bool a_held)
		{
			const auto script = AttackScriptOf(a_quest);
			const auto waiting = script ? VariableOf(*script, RadiantRules::ATTACK_WAITING) : nullptr;
			if (!waiting || !waiting->is<bool>()) {
				return;
			}
			const bool set = RE::BSScript::get<bool>(*waiting);
			const bool running = (a_quest->data.flags & QUEST_RUNNING) != 0;
			const auto clock = RadiantRules::LetGo(a_held, set, running, QuestStageLayout::Done(a_quest));
			if (clock == RadiantRules::AttackClock::kLeft) {
				return;
			}
			if (set) {
				*waiting = false;
			}
			++attacksLet;
			if (clock == RadiantRules::AttackClock::kResumed) {
				StartAttackTimer(script, RadiantRules::ATTACK_TIMER_RESUMED);
				++attacksResumed;
			} else {
				CancelAttackTimer(script);
				++attackTimersCancelled;
			}
			REX::INFO("RadiantSync: {:08X} '{}': its own attack is let go, its clock {}{}", a_quest->GetFormID(), QuestName(a_quest),
				clock == RadiantRules::AttackClock::kResumed ? "started again (an hour)" : "cancelled", running ? "" : " (it isn't running)");
		}

		void Restore(std::vector<Saved>& a_saved)
		{
			for (auto& saved : a_saved) {
				saved.alias->flags = static_cast<RE::BGSBaseAlias::FLAGS>(saved.flags);
				saved.alias->fillType = saved.fillType;
				if (const auto ref = AsRef(saved.alias)) {
					std::ranges::copy(saved.data, ref->fillData.data);
					ref->conditions = saved.conditions;
				} else if (const auto loc = AsLoc(saved.alias)) {
					loc->forcedLoc = saved.forcedLoc;
					loc->conditions = saved.conditions;
				}
			}
			a_saved.clear();
		}

		std::uint64_t RefWord(std::uint32_t a_form)
		{
			if (a_form == 0) {
				return 0;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_form);
			if (!ref) {
				return 0;
			}
			switch (calibration.ref) {
			case RefRep::kPointer:
				return reinterpret_cast<std::uint64_t>(ref);
			case RefRep::kFormId:
				return a_form;
			case RefRep::kHandle:
				{
					auto handle = ref->GetHandle();
					return handle.get_handle();
				}
			default:
				return 0;
			}
		}

		// Forces the plan's aliases on the quest's definitions (put back by Restore). False: one couldn't be.
		bool Force(RE::TESQuest* a_quest, const RadiantRules::Plan& a_plan, std::vector<Saved>& a_saved)
		{
			for (const auto& force : a_plan.forces) {
				const auto it = std::ranges::find_if(a_quest->aliases, [&](const RE::BGSBaseAlias* a_alias) { return a_alias && a_alias->aliasID == force.alias; });
				if (it == a_quest->aliases.end()) {
					Restore(a_saved);
					return false;
				}
				const auto alias = *it;
				Saved      saved{ alias, Flags(alias), alias->fillType };
				// An optional alias picks between the quest's ways by this game's state (a settlement the friend's game has unowned
				// and this one owned): the friend's way is taken, its conditions lifted for the start. A needed alias keeps its
				// own (a target that fails them here fails the start: the run stays each player's own).
				const bool lift = force.form != 0 && (Flags(alias) & RadiantRules::ALIAS_OPTIONAL);
				if (const auto ref = AsRef(alias); ref && force.kind == Kind::kRef) {
					std::ranges::copy(ref->fillData.data, saved.data.begin());
					saved.conditions = ref->conditions;
					const auto word = RefWord(force.form);
					if (force.form != 0 && word == 0) {
						Restore(a_saved);
						return false;
					}
					a_saved.push_back(saved);
					ref->fillData.data[0] = word;
					ref->fillData.data[1] = 0;
					ref->fillData.data[2] = 0;
					ref->fillType = REF_FORCED;
					if (lift) {
						ref->conditions = nullptr;
					}
				} else if (const auto loc = AsLoc(alias); loc && force.kind == Kind::kLoc) {
					saved.forcedLoc = loc->forcedLoc;
					saved.conditions = loc->conditions;
					const auto location = force.form ? RE::TESForm::GetFormByID<RE::BGSLocation>(force.form) : nullptr;
					if (force.form != 0 && !location) {
						Restore(a_saved);
						return false;
					}
					a_saved.push_back(saved);
					loc->forcedLoc = location;
					loc->fillType = *calibration.locForced;
					if (lift) {
						loc->conditions = nullptr;
					}
				} else {
					Restore(a_saved);
					return false;
				}
				// A lifted pick is taken even when another quest of this game reserves it (the settlement this game owns): see
				// RadiantRules::ForcedFlags.
				alias->flags = static_cast<RE::BGSBaseAlias::FLAGS>(RadiantRules::ForcedFlags(Flags(alias), lift));
			}
			return true;
		}

		// Whether Force could force every alias of the plan, without forcing any: asked before this game's own run is stopped
		// for a friend's, so a start that can't be made leaves ours running.
		bool CanForce(RE::TESQuest* a_quest, const RadiantRules::Plan& a_plan)
		{
			for (const auto& force : a_plan.forces) {
				const auto it = std::ranges::find_if(a_quest->aliases, [&](const RE::BGSBaseAlias* a_alias) { return a_alias && a_alias->aliasID == force.alias; });
				if (it == a_quest->aliases.end()) {
					return false;
				}
				if (AsRef(*it) && force.kind == Kind::kRef) {
					if (force.form != 0 && RefWord(force.form) == 0) {
						return false;
					}
				} else if (AsLoc(*it) && force.kind == Kind::kLoc) {
					if (force.form != 0 && !RE::TESForm::GetFormByID<RE::BGSLocation>(force.form)) {
						return false;
					}
				} else {
					return false;
				}
			}
			return true;
		}

		// This game's own runs, each stopped for a friend's that then didn't start here, or stopped as hollow (SweepHollow):
		// started again, once each has stopped, with its own fills (the giver hands it out as before). Several at once
		// (two hollow jobs whose waits end together), one quest once; one started a frame (a start runs scripts).
		struct Restart
		{
			std::uint32_t     quest = 0;
			Clock::time_point since{};
			std::string       why;
		};
		std::vector<Restart> restarts;
		std::uint32_t        restarted = 0;

		bool Restarting(std::uint32_t a_quest)
		{
			return std::ranges::contains(restarts, a_quest, &Restart::quest);
		}

		void RestartOwn(std::uint32_t a_quest, std::string a_why = "the friend's run didn't start here")
		{
			if (!RadiantRules::Restarts(a_quest)) {
				// Its giver offers it only while it isn't running (RadiantRules::NOT_RESTARTED): stopped is all it needs.
				REX::INFO("RadiantSync: {:08X} '{}': {}; this game's own run stays stopped, its giver offers it afresh", a_quest, QuestName(RE::TESForm::GetFormByID<RE::TESQuest>(a_quest)), a_why);
				return;
			}
			if (!Restarting(a_quest)) {
				restarts.push_back(Restart{ a_quest, Clock::now(), std::move(a_why) });
			}
		}

		void RestartFrame()
		{
			const auto now = Clock::now();
			for (auto it = restarts.begin(); it != restarts.end(); ++it) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(it->quest);
				if (!quest) {
					restarts.erase(it);
					return;
				}
				if (quest->data.flags & QUEST_RUNNING) {
					// Still stopping (a stopquest of theirs a moment ago), or already started again by its own scripts.
					if (now - it->since >= START_TIMEOUT) {
						restarts.erase(it);
						return;
					}
					continue;
				}
				if (QuestStartRules::Waits(QuestStart::Judge(quest)) && now - it->since < START_TIMEOUT) {
					continue;  // its stop isn't over yet
				}
				// Started by the game's own start, with its own fills. One whose aliases only the story manager's event
				// fills can't be: the game then leaves it stopped, and its giver offers the next as ever.
				const auto id = it->quest;
				const auto why = std::move(it->why);
				restarts.erase(it);
				const auto result = QuestStart::Start(quest);
				if (result.started) {
					++restarted;
					REX::INFO("RadiantSync: {:08X} '{}': {}, so this game's own run was started again", id, QuestName(quest), why);
				} else {
					REX::INFO("RadiantSync: {:08X} '{}': {}, and this game's own run couldn't be started again: {}", id, QuestName(quest), why, result.why);
				}
				return;
			}
		}

		// A friend's stages of a run shared here: each set once (Book::Accept), in order, by QuestSync's queue.
		void Take(std::uint32_t a_quest, const Run& a_run, std::vector<std::uint16_t> a_stages, std::uint32_t a_from, bool a_live)
		{
			std::ranges::sort(a_stages);
			for (const auto stage : a_stages) {
				if (!RadiantRules::Takes(a_quest, stage)) {
					continue;
				}
				if (book.Accept(a_quest, a_run, stage)) {
					QuestSync::ApplyRadiant(a_quest, stage, a_from, a_live);
					++taken;
				}
			}
		}

		void Decline(std::uint32_t a_quest, const Run& a_run, std::string_view a_why)
		{
			book.Refuse(a_quest, a_run);
			++declined;
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			REX::INFO("RadiantSync: player {}'s run {} of {:08X} '{}' isn't shared here, each player's own: {}", a_run.origin, a_run.serial, a_quest, QuestName(quest), a_why);
		}

		void Share(std::uint32_t a_quest, const Run& a_run, std::vector<std::uint16_t> a_stages, std::string_view a_how, std::span<const Protocol::RadiantFill> a_fills)
		{
			const bool again = book.Join(a_quest, a_run, a_fills);
			++joined;
			if (a_run.origin != self && Spawned(a_quest)) {
				// Theirs, with actors made at runtime: their fills kept, their actors' copies forced into our aliases as they are
				// placed (Refill), and our own actors of the quest removed as they come (Spawns, FollowsActors).
				Follow(a_quest, a_run, a_fills);
			}
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (const auto entry = RadiantSync::EntryOf(quest); quest && a_run.origin != self && entry && entry->attack) {
				HoldAttack(quest, true);  // the host's attack: ours held back before its stages (its 120 among them) are set here
			}
			if (again && quest) {
				// Shared here already under another run: the same one shared again under a new number (its game loaded a save
				// from inside it). The stages done here that its start lacks go to it under the new number, or its game falls
				// behind (they were reported here under the old one, and are never reported again).
				for (const auto stage : QuestStageLayout::Done(quest)) {
					if (std::ranges::find(a_stages, stage) == a_stages.end()) {
						outgoing.push_back(Protocol::Encode(Protocol::RadiantStage{ 0, a_quest, a_run.origin, a_run.serial, stage }, Protocol::MessageType::kReportRadiantStage));
						++sent;
					}
				}
			}
			REX::INFO("RadiantSync: sharing player {}'s run {} of {:08X} '{}' ({}), {} stage(s) of theirs to set", a_run.origin, a_run.serial, a_quest, QuestName(quest), a_how,
				a_stages.size());
			Take(a_quest, a_run, std::move(a_stages), a_run.origin, false);
		}

		// The start ran (or failed, a_why: how): definitions back, and the run shared if the quest runs with its fills.
		void FinishStart(bool a_failed, std::string_view a_why = {})
		{
			auto       start = std::move(*starting);
			starting.reset();
			Restore(start.saved);
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(start.quest);
			if (!quest) {
				return;
			}
			if (a_failed) {
				Decline(start.quest, start.run, std::format("the game didn't start it with their fills: {}", a_why));
				if (start.replaced) {
					RestartOwn(start.quest);
				}
				return;
			}
			const auto aliases = AliasesOf(quest);
			const auto mismatches = CompareHere(quest, aliases, start.start.fills, start.unforced);
			for (const auto& mismatch : mismatches) {
				if (!mismatch.decisive) {
					REX::INFO("RadiantSync: {:08X}: {} (follows from the rest; not a different target)", start.quest, Describe(mismatch));
				}
			}
			if (!RadiantRules::SameTargets(mismatches)) {
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", start.quest).c_str());
				Decline(start.quest, start.run, std::format("started here, it picked other targets ({}), so it was stopped again", FirstDecisive(mismatches)));
				if (start.replaced) {
					RestartOwn(start.quest);
				}
				return;
			}
			auto stages = start.start.stages;
			stages.insert(stages.end(), start.stages.begin(), start.stages.end());
			Share(start.quest, start.run, std::move(stages), "started here with their fills", start.newerFills ? *start.newerFills : start.start.fills);
		}

		bool Paused()
		{
			return GameStop::Now();
		}

		// Why a friend's run can't be started here with its fills (empty: it can be tried), asked before StartHere stops
		// anything.
		std::string CannotStart(RE::TESQuest* a_quest, const Protocol::RadiantStart& a_start)
		{
			if (!Calibrated()) {
				return "this game's aliases can't be read";
			}
			const auto aliases = AliasesOf(a_quest);
			const auto rules = Rules(aliases);
			const auto plan = RadiantRules::MakePlan(rules, a_start.fills, Resolve);
			if (!plan.Ok()) {
				return plan.fallback;
			}
			if (!CanForce(a_quest, plan)) {
				return "an alias of theirs couldn't be forced here";
			}
			return {};
		}

		// Starts a friend's run here with its fills (the quest not running here), a_stages: theirs that came while this
		// game's own run of it stopped (a_replaced: that run is started again if theirs isn't), a_newerFills: the run's
		// fills as sent again meanwhile (Starting::newerFills). Through the game's own start (QuestStart), at once: the
		// run is shared, or declined with the reason, before this returns. One whose last run here the game is still
		// shutting down waits for that first (Frame).
		void StartHere(RE::TESQuest* a_quest, const Protocol::RadiantStart& a_start, const Run& a_run, std::vector<std::uint16_t> a_stages, bool a_replaced = false,
			std::optional<std::vector<Protocol::RadiantFill>> a_newerFills = std::nullopt)
		{
			const auto decline = [&](std::string_view a_why) {
				Decline(a_start.quest, a_run, a_why);
				if (a_replaced) {
					RestartOwn(a_start.quest);
				}
			};
			if (!Calibrated()) {
				decline("this game's aliases can't be read");
				return;
			}
			const auto aliases = AliasesOf(a_quest);
			const auto rules = Rules(aliases);
			const auto plan = RadiantRules::MakePlan(rules, a_start.fills, Resolve);
			if (!plan.Ok()) {
				decline(plan.fallback);
				return;
			}
			Starting next{ a_start.quest, a_run, a_start };
			next.stages = std::move(a_stages);
			next.newerFills = std::move(a_newerFills);
			next.unforced = plan.unforced;
			next.replaced = a_replaced;
			next.since = Clock::now();
			if (StopUnderWay(a_quest)) {
				// Nothing is forced while it waits: the definitions are the game's own until the start itself.
				next.stopping = true;
				book.Starting(a_start.quest, a_run, a_start.fills);
				REX::INFO("RadiantSync: player {}'s run {} of {:08X} '{}' waits for the last run of it here to be over ({})", a_run.origin, a_run.serial, a_start.quest,
					QuestName(a_quest), QuestStartRules::Why(QuestStart::Judge(a_quest)));
				starting = std::move(next);
				return;
			}
			if (!Force(a_quest, plan, next.saved)) {
				decline("an alias of theirs couldn't be forced here");
				return;
			}
			book.Starting(a_start.quest, a_run, a_start.fills);
			REX::INFO("RadiantSync: starting player {}'s run {} of {:08X} '{}' here with their fills ({} forced)", a_run.origin, a_run.serial, a_start.quest, QuestName(a_quest),
				plan.forces.size());
			starting = std::move(next);
			const auto result = QuestStart::Start(a_quest);
			FinishStart(!result.started, result.why);
		}

		float GlobalValue(std::uint32_t a_global)
		{
			const auto global = a_global ? RE::TESForm::GetFormByID<RE::TESGlobal>(a_global) : nullptr;
			return global ? global->GetValue() : 0.0f;
		}

		// A Railroad job of ours stopped before it was taken: counted off the jobs on offer, as its take would have.
		void CountOff(std::uint32_t a_quest)
		{
			const auto global = RE::TESForm::GetFormByID<RE::TESGlobal>(RadiantRules::RR_QUESTS_AVAILABLE);
			if (!global) {
				return;
			}
			const float before = global->value;
			global->value = std::max(0.0f, before - 1.0f);
			REX::INFO("RadiantSync: {:08X}: this game's run stopped while on offer, so the Railroad's jobs on offer go from {} to {}", a_quest, before, global->value);
		}

		// The run of it here that theirs waited for is over (this game's own, not given to the player and stopped for
		// theirs, or one that had just ended): theirs is started.
		void StoppedForThem()
		{
			auto       start = std::move(*starting);
			starting.reset();
			if (start.uncount) {
				CountOff(start.quest);
			}
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(start.quest);
			if (!quest) {
				book.Refuse(start.quest, start.run);
				return;
			}
			StartHere(quest, start.start, start.run, std::move(start.stages), start.replaced, std::move(start.newerFills));
		}

		// A friend's run of a quest that runs here with other targets, its run here not given to this player: that run is
		// stopped (nothing of it was the player's yet) and theirs started once it has.
		void Replace(RE::TESQuest* a_quest, const Protocol::RadiantStart& a_start, const Run& a_run, std::string_view a_why)
		{
			// Theirs must be one this game can start with their fills before ours is stopped; one that still fails once ours
			// has stopped (the game's start picks others, or none in time) starts ours again (RestartOwn).
			if (const auto why = CannotStart(a_quest, a_start); !why.empty()) {
				Decline(a_start.quest, a_run, std::format("it runs here already with other targets ({}), and theirs can't start here: {}", a_why, why));
				return;
			}
			Starting next{ a_start.quest, a_run, a_start };
			next.since = Clock::now();
			next.stopping = true;
			next.replaced = true;
			if (const auto offer = RadiantRules::OfferOf(a_start.quest)) {
				next.uncount = RadiantRules::LeavesCounted(a_start.quest, QuestStageLayout::Done(a_quest), GlobalValue(offer->global));
			}
			book.Starting(a_start.quest, a_run, a_start.fills);
			REX::INFO("RadiantSync: {:08X} '{}' runs here with other targets ({}) and this player hasn't been given it: stopping it for player {}'s run {}", a_start.quest,
				QuestName(a_quest), a_why, a_run.origin, a_run.serial);
			starting = std::move(next);
			// The stop is over once the game has shut the run down and cleared its fills, a moment later (Frame).
			RE::Console::ExecuteCommand(std::format("stopquest {:08X}", a_start.quest).c_str());
		}

		// A friend's run, its turn come: started here with its fills, compared with this game's run of it (that run
		// replaced by theirs where it may be), or declined.
		void Process(const Protocol::RadiantStart& a_start)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_start.quest);
			const Run  run{ a_start.playerId, a_start.serial };
			if (!quest || !Official::IsOfficialForm(quest)) {
				return;
			}
			const auto entry = RadiantSync::EntryOf(quest);
			if (entry && !RadiantRules::Joins(*entry, hosting)) {
				Decline(a_start.quest, run, "a settlement attack is the host's to share, and this game hosts");
				return;
			}
			const bool running = (quest->data.flags & QUEST_RUNNING) != 0;
			switch (book.OnStart(a_start.quest, run, running, a_start.fills, Clock::now())) {
			case Answer::kIgnore:
				return;
			case Answer::kTake:
				// Shared here already (taken back from our save, or kept through a reconnect) and sent again by its game: its stages,
				// and those that came with it, are what that game did since. Each is set once (Book::Accept). Its fills are what
				// their game has now: an actor made at runtime shared since (its spawn id in place of none), to force our copy in.
				Take(a_start.quest, run, a_start.stages, run.origin, false);
				if (Spawned(a_start.quest)) {
					Follow(a_start.quest, run, a_start.fills);
				}
				return;
			case Answer::kDone:
				// Their game shares again a run finished here (it loaded a save from inside it, or came back): started here
				// again, its reward would be paid twice.
				Decline(a_start.quest, run, "a run with its targets is finished here already");
				return;
			case Answer::kCompare:
				{
					const auto aliases = AliasesOf(quest);  // the mismatches name its aliases: kept while they are
					const auto mismatches = CompareHere(quest, aliases, a_start.fills);
					if (RadiantRules::SameTargets(mismatches) && entry && !RadiantRules::JoinsRunning(*entry, QuestStageLayout::Done(quest))) {
						// Our own attack of it has started (its attackers stand here): joined, the host's would be a second wave.
						Decline(a_start.quest, run, "this game's own attack of it has started already");
					} else if (RadiantRules::SameTargets(mismatches)) {
						Share(a_start.quest, run, a_start.stages, "it runs here with the same targets", a_start.fills);
					} else if (RadiantRules::Replaces(a_start.quest, book.Knows(a_start.quest), QuestSync::PlayerStarted(quest), (quest->data.flags & QUEST_RUN_ONCE) != 0)) {
						Replace(quest, a_start, run, FirstDecisive(mismatches));
					} else {
						Decline(a_start.quest, run, std::format("it runs here already with other targets ({})", FirstDecisive(mismatches)));
					}
					return;
				}
			case Answer::kStart:
				break;
			}
			StartHere(quest, a_start, run, {});
		}

		// A run of ours: the quest handed out here, in a session, the player told of it.
		void Begin(RE::TESQuest* a_quest)
		{
			const auto id = a_quest->GetFormID();
			auto       done = QuestStageLayout::Done(a_quest);
			if (done.size() > Protocol::MAX_RADIANT_STAGES) {
				done.erase(done.begin(), done.end() - Protocol::MAX_RADIANT_STAGES);
			}
			auto       fills = FillsOf(a_quest);
			const auto run = book.Begin(id, self, done, fills);
			if (Spawned(id)) {
				sentFills[id] = fills;  // sent again as they change (ResendChangedFills)
			}
			Protocol::RadiantStart start{ 0, id, run.serial, std::move(fills), done };
			outgoing.push_back(Protocol::Encode(start, Protocol::MessageType::kReportRadiantStart));
			++begun;
			REX::INFO("RadiantSync: sharing our run {} of {:08X} '{}' ({} fills, {} stage(s) done)", run.serial, id, QuestName(a_quest), start.fills.size(), done.size());
		}

		void SendAgain(std::uint32_t a_quest, const Run& a_run)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest || !(quest->data.flags & QUEST_RUNNING)) {
				return;
			}
			auto done = QuestStageLayout::Done(quest);
			if (done.size() > Protocol::MAX_RADIANT_STAGES) {
				done.erase(done.begin(), done.end() - Protocol::MAX_RADIANT_STAGES);
			}
			auto fills = FillsOf(quest);
			if (Spawned(a_quest)) {
				sentFills[a_quest] = fills;
			}
			outgoing.push_back(Protocol::Encode(Protocol::RadiantStart{ 0, a_quest, a_run.serial, std::move(fills), done }, Protocol::MessageType::kReportRadiantStart));
		}

		// Our runs of quests whose actors are made at runtime: sent again when their fills change (an actor of ours shared
		// since the start, as our player came to it, names itself to the friends by its spawn id, and their games force
		// their copies in; after our load it is seen again under a new id), as a run is sent again to a game that joins.
		void ResendChangedFills()
		{
			for (const auto& [id, run] : book.Ours(self)) {
				const auto sent = sentFills.find(id);
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				if (sent == sentFills.end() || !quest || !(quest->data.flags & QUEST_RUNNING) || FillsOf(quest) == sent->second) {
					continue;
				}
				SendAgain(id, run);
				if (++resentFills <= LOGGED) {
					REX::INFO("RadiantSync: our run {} of {:08X} '{}': its fills changed (an actor of it shared since), so it is sent again", run.serial, id, QuestName(quest));
				}
			}
		}

		// The friends' runs shared here whose actors are theirs (followed): each alias that names one of their actors holds
		// our copy of it once that is placed (Encounters::RefillsOf: a copy placed later than the start, as our player came
		// near, or anew after a save). Our own actor of the quest the alias still holds (our start made one for it, not seen
		// loaded yet) goes first, as a follower's are removed (Spawns::RemoveFollowedActor).
		void Refill(Clock::time_point a_now)
		{
			for (const auto& [id, entry] : followed.Entries()) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				if (!quest || !(quest->data.flags & QUEST_RUNNING) || !book.Shares(id)) {
					continue;
				}
				const auto count = RefillAliases(quest, entry.fills, forced[id],
					std::format("RadiantSync: player {}'s run {} of {:08X} '{}'", entry.run.origin, entry.run.serial, id, QuestName(quest)), a_now);
				refilled += count.asked;
				refillsFailed += count.failed;
			}
		}

		// The quest's reference alias with this id (none: a keyword target, or one with no alias, NO_ALIAS).
		RE::BGSRefAlias* RefAliasOf(RE::TESQuest* a_quest, std::uint32_t a_alias)
		{
			for (const auto entry : a_quest->aliases) {
				if (entry && entry->aliasID == a_alias) {
					return AsRef(entry);
				}
			}
			return nullptr;
		}

		// The quest's objectives as they are here, for RadiantRules::HollowJob: each open or not, and of the aliases it
		// points at (BGSQuestObjective's targets, each by its alias), how many are targets and how many of those stand
		// (RadiantRules::TargetOf: an alias that isn't a reference alias of the quest, or holds a marker of the place
		// rather than an actor, says nothing).
		std::vector<RadiantRules::ObjectiveHere> ObjectivesOf(RE::TESQuest* a_quest)
		{
			std::vector<RadiantRules::ObjectiveHere> out;
			for (const auto objective : a_quest->objectives) {
				if (!objective) {
					continue;
				}
				RadiantRules::ObjectiveHere here{ RadiantRules::ObjectiveOpen(objective->state), 0, 0 };
				for (std::uint32_t i = 0; objective->targets && i < objective->numTargets; ++i) {
					const auto target = objective->targets[i];
					if (!target) {
						continue;
					}
					const auto alias = RefAliasOf(a_quest, target->targetAlias);
					const auto ref = alias ? a_quest->GetAliasedRef(alias->aliasID).get() : nullptr;
					here.Add(RadiantRules::TargetOf(alias != nullptr, ref != nullptr, ref && ref->As<RE::Actor>(), ref && !ref->IsDeleted() && !ref->IsDisabled()));
				}
				out.push_back(here);
			}
			return out;
		}

		// Whether a friend's run of the quest is known here: followed once and not over since (the owner gone, or the
		// session), shared in the book, or held for its session (a save from inside it loaded). Only such a run is ever
		// run again as this game's own when it is left hollow; a player's own run never is. Out of a session after the
		// game restarted, the record alone can't say whose a run was (self is unknown until a welcome): known again once
		// the player is in a session.
		bool FriendsRun(std::uint32_t a_quest)
		{
			if (followed.Once(a_quest)) {
				return true;
			}
			if (self == 0) {
				return false;
			}
			for (const auto& [id, run] : book.Shared()) {
				if (id == a_quest && run.origin != self) {
					return true;
				}
			}
			// A held run of another session says nothing: the player had another id there (RadiantRules::HeldFriendsRun).
			return held && RadiantRules::HeldFriendsRun(*held, a_quest, self, session);
		}

		// Whether an alias the quest's own start creates holds an enabled reference (RadiantRules::HollowJob's a_createdStanding).
		bool CreatedStanding(RE::TESQuest* a_quest)
		{
			for (const auto alias : a_quest->aliases) {
				if (!alias || alias->fillType != QuestActors::FILL_CREATED || !AsRef(alias)) {
					continue;
				}
				const auto ref = a_quest->GetAliasedRef(alias->aliasID).get();
				if (ref && !ref->IsDeleted() && !ref->IsDisabled()) {
					return true;
				}
			}
			return false;
		}

		// For friends whose games don't know our runs going (they joined, or loaded a save), or know a run shared here only
		// as it was (this game loaded a save inside it, or came back): our runs' starts again, and the stages done here
		// under each run shared here. A start of a run a game knows is ignored there, and each stage is set there once
		// (Book::Accept), so this pays nothing twice.
		void Resend()
		{
			for (const auto& [quest, run] : book.Ours(self)) {
				SendAgain(quest, run);
			}
			for (const auto& [id, run] : book.Shared()) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				if (!quest || !(quest->data.flags & QUEST_RUNNING)) {
					continue;
				}
				for (const auto stage : QuestStageLayout::Done(quest)) {
					outgoing.push_back(Protocol::Encode(Protocol::RadiantStage{ 0, id, run.origin, run.serial, stage }, Protocol::MessageType::kReportRadiantStage));
					++sent;
				}
			}
		}

		// A radiant running here, for Book::Restore (nothing if it doesn't run, or isn't one shared by its run).
		std::optional<RadiantRules::QuestHere> QuestHereOf(std::uint32_t a_id)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_id);
			if (!quest || !Official::IsOfficialForm(quest) || !(quest->data.flags & QUEST_RUNNING) || RadiantSync::ShareOf(quest) != RadiantRules::Share::kShared) {
				return std::nullopt;
			}
			return RadiantRules::QuestHere{ QuestStageLayout::Done(quest), FillsOf(quest) };
		}

		// The held record taken in, in a session and with the world loaded: its runs shared again as they were if it is this
		// session's (and the session shares radiants), where their quests still run here with their targets, under their
		// own runs, which a friend who finished one knows (a new number would be a new run there, its reward paid again).
		// Its runs whose quests stopped here since, or run for another run now, are finished in their session (the player
		// finished one alone after leaving it, and a friend still on it shares it again when they come back).
		void Adopt()
		{
			if (!held) {
				return;
			}
			const auto record = std::move(*held);
			held.reset();
			std::vector<RadiantRules::Saved::Quest> kept;  // running still, of another session (or radiants not shared here)
			const auto shared = book.Restore(record, QuestHereOf, Enabled(), Clock::now(), &kept);
			if (!kept.empty()) {
				held = RadiantRules::Saved{ record.session, std::move(kept), {} };  // finished in their session once over (Frame)
			}
			restored += static_cast<std::uint32_t>(shared);
			if (!record.shared.empty()) {
				REX::INFO("RadiantSync: {} of {} run(s) this world shared shared again as they were; {} of other sessions kept until over", shared, record.shared.size(),
					held ? held->shared.size() : 0);
			}
			if (shared != 0) {
				Resend();
			}
		}

		void AbortStart()
		{
			if (starting) {
				Restore(starting->saved);
				if (starting->replaced) {
					const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(starting->quest);
					if (quest && !(quest->data.flags & QUEST_RUNNING)) {
						if (starting->stopping && starting->uncount) {
							CountOff(starting->quest);  // ours stopped; theirs won't start now
						}
						RestartOwn(starting->quest);  // so ours starts again
					}
				}
				starting.reset();
			}
		}
	}

	bool Enabled()
	{
		const auto& config = Config::Get();
		return config.sharedRadiants && config.syncQuests && Story::Shared();
	}

	namespace
	{
		// A quest by its file and its form ID there (no load index), as RadiantRules::RADIANTS names it: nothing for a form
		// from a mod, a runtime-made one or a file the rules do not know. The load order of this game does not matter.
		std::optional<std::pair<std::uint8_t, std::uint32_t>> LocalOf(const RE::TESQuest* a_quest)
		{
			if (!a_quest || !Official::IsOfficialForm(a_quest)) {
				return std::nullopt;
			}
			const auto file = a_quest->GetFile(0);
			const auto index = file ? StageRules::FileOf(file->GetFilename()) : std::nullopt;
			if (!index) {
				return std::nullopt;
			}
			return std::pair{ static_cast<std::uint8_t>(*index), a_quest->GetFormID() & 0x00FFFFFFu };
		}

		// RADIANTS' quests in this game: the runtime form ID of each (0: its file is not in this load order, so the quest is
		// skipped, quietly). Resolved once the game's data is loaded (the first poll in a session): a load order is
		// fixed for the life of the process.
		std::array<std::uint32_t, RadiantRules::RADIANTS.size()> radiantQuests{};
		bool                                                      radiantsResolved = false;

		void ResolveRadiants()
		{
			if (radiantsResolved) {
				return;
			}
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			for (std::size_t i = 0; i < RadiantRules::RADIANTS.size(); ++i) {
				const auto& entry = RadiantRules::RADIANTS[i];
				const auto  quest = handler->LookupForm<RE::TESQuest>(entry.quest, StageRules::FileName(static_cast<StageRules::File>(entry.file)));
				radiantQuests[i] = quest ? quest->GetFormID() : 0;
			}
			radiantsResolved = true;
		}

		// Preston's attacks of the host's runs shared here, held back every poll (RadiantRules::HoldsAttack: the script may
		// have set PlayerIsWaiting false as a wait ended), their timers pushed out every ATTACK_TIMER_EVERY; and those no
		// longer held (the run over or no longer shared here, the session over) or a loaded world's let go. In a session or
		// out of one.
		void HoldAttacks(Clock::time_point a_now)
		{
			ResolveRadiants();
			const bool              push = a_now - attackPushed >= RadiantRules::ATTACK_TIMER_EVERY;
			std::set<std::uint32_t> holding;
			for (std::size_t i = 0; i < RadiantRules::RADIANTS.size(); ++i) {
				const auto  id = radiantQuests[i];
				const auto& entry = RadiantRules::RADIANTS[i];
				if (id == 0 || !entry.attack) {
					continue;
				}
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				if (!quest) {
					continue;
				}
				const bool running = (quest->data.flags & QUEST_RUNNING) != 0;
				if (RadiantRules::HoldsAttack(&entry, Enabled() && book.Shares(id), OursRun(id), running)) {
					HoldAttack(quest, push);
					holding.insert(id);
				} else if (attacksHeld.contains(id) || attacksLoaded) {
					LetAttackGo(quest, attacksHeld.contains(id));
				}
			}
			if (push) {
				attackPushed = a_now;
			}
			attacksHeld = std::move(holding);
			attacksLoaded = false;
		}

		// Our runs of quests whose actors are made at runtime that are hollow (RadiantRules::HollowJob: the player has an
		// objective and nothing stands to do it to): a followed run's once the owner's copies are gone, with the session,
		// the owner (left, or loaded a save from before the run) or our own save loaded alone (no copy is saved). One that
		// was a friend's (FriendsRun: never a player's own) and hollow for HOLLOW_JOB_WAIT with no friend's run of it
		// followed, waiting its turn or starting all the while (RadiantRules::HollowClock: the owner back, or their save
		// loaded from inside the run, sends it again, it is followed again and the clock starts over) is run again as this
		// game's own: the quest stopped (its run over here: the owner's later send of it is ignored, as a finished run's)
		// and started again (RestartOwn), so its start picks a place and makes its actors afresh, and the giver offers the
		// job as a fresh one; the player is told. In a session or out of one.
		void SweepHollow(Clock::time_point a_now)
		{
			ResolveRadiants();
			for (std::size_t i = 0; i < RadiantRules::RADIANTS.size(); ++i) {
				const auto id = radiantQuests[i];
				if (id == 0 || !RadiantRules::RADIANTS[i].spawned) {
					continue;
				}
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				const bool running = quest && (quest->data.flags & QUEST_RUNNING);
				const bool standsIn = followed.Follows(id) || (starting && starting->quest == id) || book.IsStarting(id) || Restarting(id) ||
				                      std::ranges::any_of(pending, [&](const Protocol::RadiantStart& a_start) { return a_start.quest == id; });
				if (!running && !standsIn) {
					followed.Ended(id);  // stopped out of a session (the book saw nothing): a later run of it is this game's own
				}
				const bool isHollow = running && RadiantRules::HollowJob(ObjectivesOf(quest), CreatedStanding(quest));
				if (!hollow.Poll(id, isHollow, standsIn, isHollow && !standsIn && FriendsRun(id), a_now) || Paused()) {
					continue;
				}
				const auto hollowFor = hollow.HollowFor(id, a_now).value_or(Clock::duration{});
				hollow.Forget(id);
				const auto runs = book.Shared();
				const auto run = std::ranges::find(runs, id, &std::pair<std::uint32_t, Run>::first);
				REX::INFO("RadiantSync: our {:08X} '{}' has been hollow {} s with nothing standing in (an objective shown, nothing standing to do it to, none of the actors its start made){}: stopped, to be run again as this game's own",
					id, QuestName(quest), std::chrono::duration_cast<std::chrono::seconds>(hollowFor).count(),
					run != runs.end() ? std::format("; player {}'s run {} of it is over here", run->second.origin, run->second.serial) : std::string{});
				book.Ended(id, a_now);  // over here: never started here again (the owner's send of it is ignored from now on)
				Unfollow(id);
				sentFills.erase(id);
				stopped.erase(id);
				++hollowRestarted;
				// A Railroad job stopped between its offer and its take would leave the jobs on offer one too high (as one
				// stopped for a friend's run would: StoppedForThem); a hollow job has been taken, so this is a guard.
				if (const auto offer = RadiantRules::OfferOf(id); offer && RadiantRules::LeavesCounted(id, QuestStageLayout::Done(quest), GlobalValue(offer->global))) {
					CountOff(id);
				}
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", id).c_str());
				RestartOwn(id, "it was left hollow (nothing stands for its objective)");
				const char* name = QuestName(quest);
				Hud::Notify(Text::Format("radiant.job_back", { { "quest", name && *name ? std::string{ name } : Text::Get("quest.a_quest") } }));
			}
		}
	}

	const RadiantRules::Entry* EntryOf(const RE::TESQuest* a_quest)
	{
		// Polled for every quest: the form ID alone (cheap) rules out nearly all of them before the file name is compared.
		if (!a_quest || !RadiantRules::MayBeRadiant(a_quest->GetFormID() & 0x00FFFFFFu)) {
			return nullptr;
		}
		const auto local = LocalOf(a_quest);
		return local ? RadiantRules::Find(local->first, local->second) : nullptr;
	}

	const RadiantRules::Entry* EntryOf(std::uint32_t a_questId)
	{
		return EntryOf(RE::TESForm::GetFormByID<RE::TESQuest>(a_questId));
	}

	RadiantRules::Share ShareOf(const RE::TESQuest* a_quest)
	{
		const auto entry = EntryOf(a_quest);
		return entry ? entry->share : RadiantRules::Share::kNotRadiant;
	}

	RadiantRules::Share ShareOf(std::uint32_t a_questId)
	{
		return ShareOf(RE::TESForm::GetFormByID<RE::TESQuest>(a_questId));
	}

	bool IsRadiant(const RE::TESQuest* a_quest)
	{
		return ShareOf(a_quest) != RadiantRules::Share::kNotRadiant;
	}

	bool IsRadiant(std::uint32_t a_questId)
	{
		return IsRadiant(RE::TESForm::GetFormByID<RE::TESQuest>(a_questId));
	}

	std::string StartWithEvent(RE::TESQuest* a_quest, std::uint32_t a_ref, std::uint32_t a_location, std::span<const std::pair<std::uint32_t, std::uint32_t>> a_aliases,
		std::string_view a_who)
	{
		if (!a_quest || !Calibrated()) {
			return "error: no such quest, or this game's aliases can't be read";
		}
		if (a_quest->data.flags & QUEST_RUNNING) {
			return "error: it is running already";
		}
		if (const auto verdict = QuestStart::Judge(a_quest); QuestStartRules::Waits(verdict)) {
			return std::format("wait: {:08X} '{}' can't be started yet: {}", a_quest->GetFormID(), QuestName(a_quest), QuestStartRules::Why(verdict));
		}
		RadiantRules::Plan plan;
		for (const auto alias : a_quest->aliases) {
			if (!alias || alias->aliasID == NO_ALIAS) {
				continue;
			}
			// Named by the caller (alias ID, form), or what the story manager's event fills: its reference (the settlement's
			// workshop, the quest giver) and, where the quest reads one, its location.
			const auto named = std::ranges::find(a_aliases, alias->aliasID, &std::pair<std::uint32_t, std::uint32_t>::first);
			if (const auto ref = AsRef(alias)) {
				if (named != a_aliases.end()) {
					plan.forces.push_back({ alias->aliasID, Kind::kRef, named->second });
				} else if (a_ref != 0 && ref->fillType == static_cast<std::uint16_t>(RE::BGSRefAlias::FILL_TYPE::kFromEvent)) {
					plan.forces.push_back({ alias->aliasID, Kind::kRef, a_ref });
				}
			} else if (const auto loc = AsLoc(alias)) {
				if (named != a_aliases.end()) {
					plan.forces.push_back({ alias->aliasID, Kind::kLoc, named->second });
				} else if (a_location != 0 && loc->forcedFromEvent != 0xFFFFFFFF && loc->forcedFromAlias == 0xFFFFFFFF) {
					plan.forces.push_back({ alias->aliasID, Kind::kLoc, a_location });
				}
			}
		}
		if (plan.forces.empty()) {
			return "error: nothing to force: none of its aliases is filled from the story manager's event, and none was named";
		}
		std::vector<Saved> saved;
		if (!Force(a_quest, plan, saved)) {
			return "error: an alias couldn't be forced";
		}
		// The caller named these, so the game's own pick between them is theirs: their conditions are lifted for the start
		// (Restore puts them back), as a friend's pick is for an optional alias.
		for (const auto& entry : saved) {
			if (const auto ref = AsRef(entry.alias)) {
				ref->conditions = nullptr;
			} else if (const auto loc = AsLoc(entry.alias)) {
				loc->conditions = nullptr;
			}
		}
		// The game's own start (QuestStart) fills the aliases before it returns, so the definitions are put back at once,
		// started or not.
		const auto result = QuestStart::Start(a_quest);
		Restore(saved);
		if (result.started) {
			REX::INFO("RadiantSync: {} of {:08X} '{}': it runs, {} alias(es) forced for the start", a_who, a_quest->GetFormID(), QuestName(a_quest), plan.forces.size());
			return std::format("ok: {:08X} '{}' is running, started with {} alias(es) forced (put back)", a_quest->GetFormID(), QuestName(a_quest), plan.forces.size());
		}
		REX::INFO("RadiantSync: {} of {:08X} '{}': it didn't start: {}", a_who, a_quest->GetFormID(), QuestName(a_quest), result.why);
		return std::format("{}: {:08X} '{}' didn't start with {} alias(es) forced (put back): {}", result.wait ? "wait" : "error", a_quest->GetFormID(), QuestName(a_quest),
			plan.forces.size(), result.why);
	}

	void SetLocalPlayer(std::uint32_t a_id)
	{
		self = a_id;
	}

	void OnWelcome(std::uint64_t a_session)
	{
		session = a_session;
		book.Bind(a_session);
		if (WorldSync::InWorld()) {
			Adopt();  // otherwise once it is (Frame), or the load to come brings its own
		}
	}

	void OnStart(const Protocol::RadiantStart& a_start)
	{
		if (!Enabled() || a_start.playerId == 0 || a_start.playerId == self || RadiantSync::ShareOf(a_start.quest) != RadiantRules::Share::kShared) {
			return;
		}
		const Run run{ a_start.playerId, a_start.serial };
		// The run sent again while its first send is being started here, or waits its turn: the newer fills are kept (an
		// actor of theirs shared since, to force our copy in once it runs) and its stages join those that came meanwhile.
		if (starting && starting->run == run && starting->quest == a_start.quest) {
			Protocol::RadiantStart kept{ a_start.playerId, a_start.quest, a_start.serial, {}, std::move(starting->stages) };
			RadiantRules::Supersedes(kept, a_start);
			starting->stages = std::move(kept.stages);
			starting->newerFills = std::move(kept.fills);
			return;
		}
		for (auto& waiting : pending) {
			if (RadiantRules::Supersedes(waiting, a_start)) {
				return;
			}
		}
		if (pending.size() >= MAX_PENDING) {
			pending.pop_front();
		}
		pending.push_back(a_start);
	}

	void OnStage(const Protocol::RadiantStage& a_stage)
	{
		if (!Enabled() || RadiantSync::ShareOf(a_stage.quest) != RadiantRules::Share::kShared) {
			return;
		}
		const Run run{ a_stage.origin, a_stage.serial };
		if (starting && starting->run == run && starting->quest == a_stage.quest) {
			starting->stages.push_back(a_stage.stage);
			return;
		}
		for (auto& start : pending) {
			if (start.quest == a_stage.quest && start.playerId == run.origin && start.serial == run.serial) {
				start.stages.push_back(a_stage.stage);
				return;
			}
		}
		Take(a_stage.quest, run, { a_stage.stage }, a_stage.playerId, true);
	}

	void OnPlayerJoined()
	{
		if (!Enabled() || self == 0) {
			return;
		}
		Resend();
	}

	namespace
	{
		// A friend left, or loaded a save: their copies here go with them, so their runs with actors made at runtime are
		// followed no more (the runs stay shared: their send of the same run when they are back, or from a save inside it,
		// is followed again, Answer::kTake). A quest of theirs left hollow here is run again as our own after a while
		// (SweepHollow). The word of a load can come after their send of a run from that load (RadiantRules::RELOAD_GRACE:
		// the server holds the word of a second load within two seconds of the first): such a run is one they still have.
		void OwnerGone(std::uint32_t a_playerId, std::string_view a_what, std::chrono::steady_clock::duration a_grace = {})
		{
			for (const auto id : followed.OwnerGone(a_playerId, Clock::now(), a_grace)) {
				forced.erase(id);
				++unfollowed;
				REX::INFO("RadiantSync: player {} {}; their run of {:08X} '{}' is followed no more (their actors' copies go with them): run again as this game's own if it is left hollow",
					a_playerId, a_what, id, QuestName(RE::TESForm::GetFormByID<RE::TESQuest>(id)));
			}
		}
	}

	void OnPlayerLeft(std::uint32_t a_playerId)
	{
		OwnerGone(a_playerId, "left");
	}

	void OnPlayerReloaded(std::uint32_t a_playerId)
	{
		if (!Enabled() || self == 0 || a_playerId == self) {
			return;
		}
		OwnerGone(a_playerId, "loaded a save", RadiantRules::RELOAD_GRACE);
		if (const auto shared = book.Shared(); !shared.empty()) {
			REX::INFO("RadiantSync: player {} loaded a save; our runs going and the stages done here of the {} run(s) shared are sent again", a_playerId, shared.size());
		}
		Resend();
	}

	bool Shares(std::uint32_t a_quest)
	{
		return Enabled() && book.Shares(a_quest);
	}

	bool HeldAttackAt(std::uint32_t a_workshop)
	{
		if (a_workshop == 0) {
			return false;
		}
		return std::ranges::any_of(attacksHeld, [&](std::uint32_t a_quest) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest || (quest->data.flags & QUEST_RUNNING) == 0) {
				return false;
			}
			const auto workshop = quest->GetAliasedRef(RadiantRules::ATTACK_WORKSHOP_ALIAS).get();
			return workshop && workshop->GetFormID() == a_workshop;
		});
	}

	bool FollowsActors(std::uint32_t a_quest)
	{
		return Enabled() && followed.Follows(a_quest) && book.Shares(a_quest);
	}

	bool SharesActors(std::uint32_t a_quest)
	{
		// Only once the job is given (an objective shown: QuestSync::PlayerStarted). Appropriation's defenders stand at
		// the site from the moment the player joins the Institute, long before the giver hands the job out; the squire,
		// the scribe and the synth are disabled until their take anyway.
		return Enabled() && Spawned(a_quest) && !followed.Follows(a_quest) && book.Shares(a_quest) && OursRun(a_quest) &&
		       QuestSync::PlayerStarted(RE::TESForm::GetFormByID<RE::TESQuest>(a_quest));
	}

	bool FollowsSpawn(std::uint32_t a_spawn)
	{
		if (!Enabled() || a_spawn == 0 || a_spawn == Protocol::RADIANT_UNSHARED_REF) {
			return false;
		}
		for (const auto& [id, entry] : followed.Entries()) {
			if (book.Shares(id) && std::ranges::any_of(entry.fills, [&](const Protocol::RadiantFill& a_fill) { return a_fill.kind == 0 && a_fill.form == a_spawn; })) {
				return true;
			}
		}
		return false;
	}

	bool ForceAliasRef(RE::TESQuest* a_quest, std::uint32_t a_alias, std::uint32_t a_local)
	{
		if (!a_quest) {
			return false;
		}
		RE::BGSRefAlias* alias = nullptr;
		for (const auto entry : a_quest->aliases) {
			if (const auto ref = entry && entry->aliasID == a_alias ? AsRef(entry) : nullptr) {
				alias = ref;
				break;
			}
		}
		const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_local);
		if (!alias || !ref || ref->IsDeleted()) {
			return false;
		}
		// A copy, made at runtime, may have no script object yet (as a stand-in: Puppets); an alias no script is attached to
		// has one made as Quest.GetAlias would.
		const auto aliasObject = Papyrus::BoundObject(alias, "ReferenceAlias", true);
		const auto target = Papyrus::BoundObject(ref, "ObjectReference", true);
		const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> done{ new ForceDone() };
		return aliasObject && target && Papyrus::CallOn(aliasObject, "ForceRefTo", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
			a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 1 };
			a_out[0] = target;
		}, done);
	}

	RefillCount RefillAliases(RE::TESQuest* a_quest, std::span<const Protocol::RadiantFill> a_fills, std::vector<Encounters::AliasForce>& a_forced, std::string_view a_who,
		Clock::time_point a_now)
	{
		RefillCount count;
		if (!a_quest) {
			return count;
		}
		const auto here = [&](std::uint32_t a_alias) {
			const auto ref = a_quest->GetAliasedRef(a_alias).get();
			return ref && !ref->IsDeleted() ? ref->GetFormID() : 0u;
		};
		for (const auto& [alias, tries] : Encounters::ReadBack(a_forced, here)) {
			if (tries > 1 && refillsHeld++ < LOGGED) {
				REX::INFO("{}: alias {} holds our copy {:08X} after {} asks", a_who, alias, here(alias), tries);
			}
		}
		// The engine takes no force while the quest stops or starts, or while its start's promotion of its references is
		// pending (Encounters::ForceOpen, the console's own test): every ask waits for a later poll then, uncounted.
		const bool open = Encounters::ForceOpen(a_quest->data.flags, a_quest->promoteTask.get() != nullptr);
		for (const auto& refill : Encounters::RefillsOf(a_fills, a_forced, [](std::uint32_t a_spawn) { return Spawns::CopyFor(a_spawn).local; }, here, a_now)) {
			// Raw: a deleted reference the alias still holds (here() reads 0 for it), named in the log.
			const auto raw = a_quest->GetAliasedRef(refill.alias).get();
			const auto rawId = raw ? raw->GetFormID() : 0u;
			if (refill.why == Encounters::RefillWhy::kGiveUp) {
				Encounters::GiveUp(a_forced, refill.alias);
				if (refillsGivenUp++ < LOGGED) {
					REX::WARN("{}: alias {} still holds {:08X}{}, not our copy {:08X}, after {} asks ({}; the script VM ran {} force call(s) and cancelled {} so far, the engine's force "
							  "made {}): given up on until another copy is placed; {}",
						a_who, refill.alias, rawId, raw && raw->IsDeleted() ? " (deleted)" : "", refill.copy, Encounters::MAX_FORCE_TRIES, Encounters::WhyText(refill.why),
						forceCallsRan.load(), forceCallsCancelled.load(), forceDirect, AliasState(a_quest, refill.alias, refill.copy));
				}
				continue;
			}
			if (!open) {
				++forceWaits;
				if (forceWaitsLogged++ < LOGGED) {
					REX::INFO("{}: alias {} waits to be forced onto our copy {:08X} ({}): the engine takes no force now (quest flags {:04X}, its references' promotion {})", a_who,
						refill.alias, refill.copy, Encounters::WhyText(refill.why), a_quest->data.flags, a_quest->promoteTask.get() ? "pending" : "done");
				}
				continue;
			}
			if (raw && !raw->IsDeleted() && Protocol::IsSpawnRef(rawId) && Spawns::CopyIdOf(rawId) == 0) {
				Spawns::RemoveFollowedActor(rawId, a_quest->GetFormID());  // only one the quest's own start made
			}
			// A first ask goes to the script VM; one made again because the alias didn't hold the copy, to the engine's own
			// force (Encounters::ForceWayOf), which is done when it returns.
			const bool engine = Encounters::ForceWayOf(refill.why) == Encounters::ForceWay::kEngine && ForceIntoAliasAsRead();
			const bool ok = engine ? ForceAliasRefEngine(a_quest, refill.alias, refill.copy) : ForceAliasRef(a_quest, refill.alias, refill.copy);
			Encounters::NoteForced(a_forced, refill.alias, refill.copy, ok, a_now);
			if (!ok) {
				++count.failed;
				// The last of the asks not taken gives the copy up (NoteForced): counted with the rest given up on.
				const auto force = std::ranges::find(a_forced, refill.alias, &Encounters::AliasForce::alias);
				const bool gaveUp = force != a_forced.end() && force->state == Encounters::ForceState::kGivenUp;
				refillsGivenUp += gaveUp ? 1 : 0;
				if (refillsNotTaken++ < LOGGED) {
					REX::WARN("{}: alias {} could not be forced onto our copy {:08X} ({})", a_who, refill.alias, refill.copy,
						gaveUp ? std::format("{} asks not taken: given up on until another copy is placed", Encounters::MAX_FORCE_TRIES) :
								 std::format("asked again next poll, {} asks in all", Encounters::MAX_FORCE_TRIES));
				}
				continue;
			}
			++count.asked;
			if (refill.why == Encounters::RefillWhy::kNew) {
				if (refillsNew++ < LOGGED) {
					REX::INFO("{}: alias {} forced onto our copy {:08X} of their actor", a_who, refill.alias, refill.copy);
				}
			} else if (refillsRetried++ < LOGGED) {
				// The engine's force is done as it returns: what the alias holds right after it says whether it took.
				const auto after = engine ? a_quest->GetAliasedRef(refill.alias).get() : nullptr;
				REX::WARN("{}: alias {} forced again onto our copy {:08X}{}: it held {:08X}{} ({}; the script VM ran {} force call(s) and cancelled {} so far){}; {}", a_who, refill.alias,
					refill.copy, engine ? " by the engine's ForceRefIntoAlias" : " through the script VM", rawId, raw && raw->IsDeleted() ? " (deleted)" : "",
					Encounters::WhyText(refill.why), forceCallsRan.load(), forceCallsCancelled.load(),
					engine ? std::format(", and holds {:08X} right after", after ? after->GetFormID() : 0u) : std::string{}, AliasState(a_quest, refill.alias, refill.copy));
			}
		}
		return count;
	}

	void OnDone(const RE::TESQuest* a_quest, std::span<const std::uint16_t> a_stages)
	{
		const auto id = a_quest->GetFormID();
		if (!Shares(id)) {
			return;
		}
		const bool ours = OursRun(id);
		for (const auto stage : a_stages) {
			if (!RadiantRules::Reports(id, stage, ours)) {
				continue;  // the owner's to report (an escort sent home as its owner gets back: RadiantRules::OWNER_STAGES)
			}
			for (const auto& run : book.Report(id, stage)) {
				outgoing.push_back(Protocol::Encode(Protocol::RadiantStage{ 0, id, run.origin, run.serial, stage }, Protocol::MessageType::kReportRadiantStage));
				++sent;
			}
		}
	}

	void Frame(bool a_inSession, bool a_hosting)
	{
		inSession = a_inSession;
		hosting = a_hosting;
		const auto now = Clock::now();  // QuestSync polls twice a second
		if (!WorldSync::InWorld()) {
			return;
		}
		RestartFrame();
		if (held && a_inSession && self != 0 && Enabled() && std::ranges::contains(held->shared, session, &RadiantRules::Saved::Quest::session)) {
			Adopt();  // welcomed before the world was loaded
		}
		// A run held for its session (out of it, or in another) whose quest stops here (or runs for another run) is over
		// here, as in one (Book::Ended): finished in its session, so a friend still on it never starts it here again when the
		// player is back.
		if (held) {
			RadiantRules::Saved over{ held->session, {}, {} };
			std::erase_if(held->shared, [&](const RadiantRules::Saved::Quest& a_quest) {
				const auto here = QuestHereOf(a_quest.quest);
				if (here && RadiantRules::TargetsOf(here->fills) == a_quest.targets) {
					return false;
				}
				over.shared.push_back(a_quest);
				return true;
			});
			if (!over.shared.empty()) {
				book.Restore(over, QuestHereOf, false, now);
				REX::INFO("RadiantSync: {} run(s) kept for the session are over here; finished", over.shared.size());
			}
			if (held->shared.empty()) {
				held.reset();
			}
		}

		// A friend's run waiting for a run of its quest here to be over (this game's own, stopped for it, or one that had
		// just ended): started once the game has shut that run down, its fills cleared. (A start itself is over within
		// StartHere; the other two cases are for a start left open by anything unforeseen.)
		if (starting) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(starting->quest);
			const bool running = quest && (quest->data.flags & QUEST_RUNNING);
			if (starting->stopping) {
				if (!quest || !StopUnderWay(quest)) {
					StoppedForThem();
				} else if (now - starting->since >= START_TIMEOUT) {
					const auto stuck = std::move(*starting);
					starting.reset();
					Decline(stuck.quest, stuck.run, std::format("the last run of it here didn't come to an end ({})", QuestStartRules::Why(QuestStart::Judge(quest))));
					if (stuck.replaced && !running) {
						if (stuck.uncount) {
							CountOff(stuck.quest);
						}
						RestartOwn(stuck.quest);  // ours was stopped for theirs
					}
				}
			} else if (running) {
				FinishStart(false);
			} else if (now - starting->since >= START_TIMEOUT) {
				FinishStart(true, "it didn't start in time");
			}
		}

		// Runs that ended here: their quest stopped (completed, failed, or handed in). Never started here again. Ended a
		// poll after it is first seen stopped: the stages that stopped it (a reward stage that stops the quest at once) are
		// seen done by QuestSync's poll after this, and still go under the run.
		for (const auto id : book.Quests()) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
			if (book.IsStarting(id) || (quest && (quest->data.flags & QUEST_RUNNING))) {
				stopped.erase(id);
				continue;
			}
			if (stopped.insert(id).second) {
				continue;
			}
			stopped.erase(id);
			book.Ended(id, now);
			Unfollow(id);
			sentFills.erase(id);
			REX::INFO("RadiantSync: {:08X} '{}' stopped here; its shared run is over", id, QuestName(quest));
		}

		if (!Story::InOpening()) {
			SweepHollow(now);  // a hollow job of ours is nobody's, in or out of a session
		}
		HoldAttacks(now);  // the host's attack the only one: ours held back while theirs is shared here, let go after

		if (!a_inSession || !Enabled() || self == 0 || Story::InOpening()) {
			if (!Enabled()) {
				pending.clear();
			}
			return;
		}

		// Runs with actors made at runtime: ours sent again as their fills change, and the friends' aliases forced onto our
		// copies of their actors as those are placed.
		ResendChangedFills();
		Refill(now);

		// Friends' runs, one at a time, with the game running (a start runs scripts).
		if (!starting && !pending.empty() && !Paused()) {
			const auto next = std::move(pending.front());
			pending.pop_front();
			Process(next);
		}

		// Runs of ours: a shared radiant handed out here (an objective shown), not one of a friend's. An attack only if this
		// game hosts (RadiantRules::Begins).
		ResolveRadiants();
		for (std::size_t i = 0; i < RadiantRules::RADIANTS.size(); ++i) {
			const auto  id = radiantQuests[i];  // 0: the add-on is not in this load order
			const auto& entry = RadiantRules::RADIANTS[i];
			if (id == 0 || !RadiantRules::Begins(entry, a_hosting) || book.Shares(id) || book.IsStarting(id)) {
				continue;
			}
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
			if (!quest || !Official::IsOfficialForm(quest) || !(quest->data.flags & QUEST_RUNNING) || !QuestSync::PlayerStarted(quest)) {
				continue;
			}
			if (std::ranges::any_of(pending, [&](const Protocol::RadiantStart& a_start) { return a_start.quest == id; })) {
				continue;  // a friend's run of it comes first: it may be this one
			}
			Begin(quest);
		}
	}

	void FrameUnsynced()
	{
		if (!WorldSync::InWorld() || (attacksHeld.empty() && !attacksLoaded)) {
			return;
		}
		HoldAttacks(Clock::now());  // nothing is shared with quest sync off (Enabled), so every one held is let go
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnSessionEnded()
	{
		AbortStart();
		pending.clear();
		if (session != 0) {
			// Shared again as they were if the player comes back to this session (Adopt), with any kept from another.
			auto ending = book.Save(session);
			if (held) {
				ending.shared.insert(ending.shared.end(), held->shared.begin(), held->shared.end());
			}
			held = std::move(ending);
		}
		book.EndSession();
		// The quests go on, each player's own: nothing is forced into their aliases any more. A followed one is left hollow
		// (our copies of the owner's actors go with the session) and run again as our own after a while, unless the
		// session comes back first (SweepHollow).
		followed.Clear();
		forced.clear();
		sentFills.clear();
		stopped.clear();
		inSession = false;
	}

	void OnServerChanged()
	{
		AbortStart();
		pending.clear();
		book.Clear();
		followed.Clear();
		forced.clear();
		sentFills.clear();
		stopped.clear();
		outgoing.clear();
		session = 0;
	}

	void Rebaseline()
	{
		AbortStart();
		restarts.clear();  // the loaded world has its own runs
		pending.clear();
		book.Reloaded();
		followed.Reset();  // a friend's run the save shares again is followed again when their game sends it (kTake)
		forced.clear();
		sentFills.clear();
		hollow.Clear();  // counted from the load
		attacksHeld.clear();
		attacksLoaded = true;  // a held attack the save kept is let go, unless its run is shared here again (HoldAttacks)
		stopped.clear();
		outgoing.clear();
		{
			std::scoped_lock l{ loadLock };
			held = std::exchange(loaded, std::nullopt);  // the loaded world's record (none: it shared no run)
		}
		if (held) {
			book.Remember(*held);  // its runs finished, in whichever session
		}
		if (self != 0) {
			Adopt();
		}
	}

		// A condition list as `fn(p1,p2)` items (the function's number as SCRIPT_OUTPUT has it, its two parameters as the game holds
		// them, hex), `|` between ones joined by or, `&` by and: for reading why an alias can't be filled.
		static std::string CondText(const RE::TESCondition* a_condition)
		{
			std::string out;
			if (!a_condition) {
				return "none";
			}
			int count = 0;
			for (auto item = a_condition->head; item && count < 12; item = item->next, ++count) {
				out += std::format("{}{}({:X},{:X})", count ? (item->data.compareOr ? "|" : "&") : "", static_cast<int>(item->data.functionData.function.underlying()),
					reinterpret_cast<std::uintptr_t>(item->data.functionData.param[0]), reinterpret_cast<std::uintptr_t>(item->data.functionData.param[1]));
			}
			return out.empty() ? "none" : out;
		}

	std::string DumpAliases(const RE::TESQuest* a_constQuest)
	{
		if (!a_constQuest) {
			return "no such quest";
		}
		const auto  quest = const_cast<RE::TESQuest*>(a_constQuest);
		std::string out = std::format("{:08X} '{}' flags={:04X} stage={} aliases={} share={}", quest->GetFormID(), QuestName(quest), quest->data.flags, quest->currentStage,
			quest->aliases.size(), static_cast<int>(RadiantSync::ShareOf(quest)));
		Calibrate();
		out += " | calibration: " + calibration.summary;
		for (const auto alias : quest->aliases) {
			if (!alias) {
				continue;
			}
			const char* name = alias->aliasName.c_str();
			out += std::format(" | {} {} {} flags={:06X} fillType={}", TypeOf(alias), alias->aliasID, name ? name : "", Flags(alias), alias->fillType);
			if (const auto ref = AsRef(alias)) {
				out += std::format(" data={:X},{:X},{:X} cond={} fill={} here={:08X}", ref->fillData.data[0], ref->fillData.data[1], ref->fillData.data[2],
					CondText(ref->conditions), static_cast<int>(RadiantRules::RefFill(ref->fillType, Flags(ref))), Here(quest, Kind::kRef, alias->aliasID));
			} else if (const auto loc = AsLoc(alias)) {
				out += std::format(" forced={:08X} fromAlias={:X} fromEvent={:X} eventData={:X} external={:08X}/{:X} closestTo={:X} cond={} fill={} here={:08X}",
					loc->forcedLoc ? loc->forcedLoc->GetFormID() : 0, loc->forcedFromAlias, loc->forcedFromEvent, loc->forcedEventData,
					loc->externalQuest ? loc->externalQuest->GetFormID() : 0, loc->externalAlias, loc->closestToAlias, CondText(loc->conditions),
					static_cast<int>(LocFillOf(loc)), Here(quest, Kind::kLoc, alias->aliasID));
			}
		}
		return out;
	}

	std::string Describe()
	{
		return std::format("radiants: runs={} begun={} joined={} declined={} sent={} taken={} pending={} restarted={} restored={} finished={} followedActors={} unfollowed={} refilled={} refillsFailed={} refillsRetried={} refillsHeld={} refillsGivenUp={} forceCalls={}/{} forceDirect={} forceWaits={} resentFills={} hollowRestarted={} attacksHeld={} attackHolds={} attacksLet={} attacksResumed={} attackTimersCancelled={}{}",
			book.Size(), begun, joined, declined, sent, taken, pending.size(), restarted, restored, book.Finished(), followed.Size(), unfollowed, refilled, refillsFailed, refillsRetried,
			refillsHeld, refillsGivenUp, forceCallsRan.load(), forceCallsCancelled.load(), forceDirect, forceWaits, resentFills,
			hollowRestarted, attacksHeld.size(), attackHolds, attacksLet, attacksResumed, attackTimersCancelled, starting ? " starting" : "") +
		       "; " + QuestStart::Describe();
	}

	std::vector<std::uint8_t> SaveRecord()
	{
		// The runs finished here, by session; the runs shared, each with its session: the book's, and those held (a load's
		// record not taken in yet, the ones kept at the session's end, or another session's still running here).
		auto out = book.Save(session);
		if (held) {
			for (const auto& quest : held->shared) {
				if (out.shared.size() < RadiantRules::MAX_SAVED_QUESTS) {
					out.shared.push_back(quest);
				}
			}
		}
		return out.shared.empty() && out.finished.empty() ? std::vector<std::uint8_t>{} : RadiantRules::Encode(out);
	}

	void LoadRecord(std::span<const std::uint8_t> a_bytes)
	{
		auto saved = RadiantRules::Decode(a_bytes);
		if (!saved) {
			REX::WARN("RadiantSync: ignoring an unreadable record of radiant runs");
		}
		std::scoped_lock l{ loadLock };
		loaded = std::move(saved);
	}

	void RevertRecord()
	{
		std::scoped_lock l{ loadLock };
		loaded.reset();
	}

	std::vector<Protocol::RadiantFill> FillsHere(RE::TESQuest* a_quest)
	{
		return a_quest ? FillsOf(a_quest) : std::vector<Protocol::RadiantFill>{};
	}
}
