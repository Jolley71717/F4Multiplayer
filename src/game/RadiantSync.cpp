#include "game/RadiantSync.h"

#include "Config.h"
#include "game/Official.h"
#include "game/QuestStageLayout.h"
#include "game/QuestSync.h"
#include "game/RadiantRules.h"
#include "game/Spawns.h"
#include "game/StageLog.h"
#include "game/Story.h"
#include "game/WorldSync.h"

#include <deque>

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

		RadiantRules::Book                     book;
		std::uint32_t                          self = 0;
		bool                                   inSession = false;
		std::vector<std::vector<std::uint8_t>> outgoing;
		std::deque<Protocol::RadiantStart>     pending;  // friends' runs to start or compare here, in the order they came

		std::uint32_t begun = 0;     // runs of ours shared
		std::uint32_t joined = 0;    // friends' runs shared here
		std::uint32_t declined = 0;  // friends' runs left each player's own here
		std::uint32_t sent = 0;      // stages reported under runs
		std::uint32_t taken = 0;     // friends' stages queued here

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

		std::vector<RadiantRules::Mismatch> CompareHere(RE::TESQuest* a_quest, const std::vector<GameAlias>& a_aliases, const std::vector<Protocol::RadiantFill>& a_fills)
		{
			const auto rules = Rules(a_aliases);
			return RadiantRules::Compare(rules, a_fills, Resolve, [&](Kind a_kind, std::uint32_t a_alias) { return Here(a_quest, a_kind, a_alias); });
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
		};

		struct Starting
		{
			std::uint32_t              quest = 0;
			Run                        run;
			Protocol::RadiantStart     start;
			std::vector<Saved>         saved;  // the definitions forced for it, put back once it runs
			std::vector<std::uint16_t> stages;  // the friend's stages that came meanwhile
			Clock::time_point          since{};
			bool                       stopping = false;  // this game's own run of it (not given to the player) stopping first
			bool                       uncount = false;   // ... whose offer stays counted at the Railroad's HQ: counted off once stopped
		};
		std::optional<Starting> starting;

		const char* QuestName(const RE::TESQuest* a_quest)
		{
			const char* name = a_quest ? a_quest->GetFullName() : nullptr;
			return name ? name : "";
		}

		void Restore(std::vector<Saved>& a_saved)
		{
			for (auto& saved : a_saved) {
				saved.alias->flags = static_cast<RE::BGSBaseAlias::FLAGS>(saved.flags);
				saved.alias->fillType = saved.fillType;
				if (const auto ref = AsRef(saved.alias)) {
					std::ranges::copy(saved.data, ref->fillData.data);
				} else if (const auto loc = AsLoc(saved.alias)) {
					loc->forcedLoc = saved.forcedLoc;
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
				if (const auto ref = AsRef(alias); ref && force.kind == Kind::kRef) {
					std::ranges::copy(ref->fillData.data, saved.data.begin());
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
				} else if (const auto loc = AsLoc(alias); loc && force.kind == Kind::kLoc) {
					saved.forcedLoc = loc->forcedLoc;
					const auto location = force.form ? RE::TESForm::GetFormByID<RE::BGSLocation>(force.form) : nullptr;
					if (force.form != 0 && !location) {
						Restore(a_saved);
						return false;
					}
					a_saved.push_back(saved);
					loc->forcedLoc = location;
					loc->fillType = *calibration.locForced;
				} else {
					Restore(a_saved);
					return false;
				}
				alias->flags = static_cast<RE::BGSBaseAlias::FLAGS>(Flags(alias) & ~RadiantRules::ALIAS_BY_OTHERS);
			}
			return true;
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

		void Share(std::uint32_t a_quest, const Run& a_run, std::vector<std::uint16_t> a_stages, std::string_view a_how)
		{
			book.Join(a_quest, a_run);
			++joined;
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			REX::INFO("RadiantSync: sharing player {}'s run {} of {:08X} '{}' ({}), {} stage(s) of theirs to set", a_run.origin, a_run.serial, a_quest, QuestName(quest), a_how,
				a_stages.size());
			Take(a_quest, a_run, std::move(a_stages), a_run.origin, false);
		}

		// The start ran (or ran out of time): definitions back, and the run shared if the quest runs with its fills.
		void FinishStart(bool a_timedOut)
		{
			auto       start = std::move(*starting);
			starting.reset();
			Restore(start.saved);
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(start.quest);
			if (!quest) {
				return;
			}
			if (a_timedOut) {
				Decline(start.quest, start.run, "the game didn't start it with their fills");
				return;
			}
			const auto aliases = AliasesOf(quest);
			const auto mismatches = CompareHere(quest, aliases, start.start.fills);
			for (const auto& mismatch : mismatches) {
				if (!mismatch.decisive) {
					REX::INFO("RadiantSync: {:08X}: {} (follows from the rest; not a different target)", start.quest, Describe(mismatch));
				}
			}
			if (!RadiantRules::SameTargets(mismatches)) {
				RE::Console::ExecuteCommand(std::format("stopquest {:08X}", start.quest).c_str());
				Decline(start.quest, start.run, std::format("started here, it picked other targets ({}), so it was stopped again", FirstDecisive(mismatches)));
				return;
			}
			auto stages = start.start.stages;
			stages.insert(stages.end(), start.stages.begin(), start.stages.end());
			Share(start.quest, start.run, std::move(stages), "started here with their fills");
		}

		bool Paused()
		{
			const auto ui = RE::UI::GetSingleton();
			return !ui || ui->menuMode != 0;
		}

		// Starts a friend's run here with its fills (the quest not running here), a_stages: theirs that came while this
		// game's own run of it stopped.
		void StartHere(RE::TESQuest* a_quest, const Protocol::RadiantStart& a_start, const Run& a_run, std::vector<std::uint16_t> a_stages)
		{
			if (!Calibrated()) {
				Decline(a_start.quest, a_run, "this game's aliases can't be read");
				return;
			}
			const auto aliases = AliasesOf(a_quest);
			const auto rules = Rules(aliases);
			const auto plan = RadiantRules::MakePlan(rules, a_start.fills, Resolve);
			if (!plan.Ok()) {
				Decline(a_start.quest, a_run, plan.fallback);
				return;
			}
			Starting next{ a_start.quest, a_run, a_start };
			next.stages = std::move(a_stages);
			if (!Force(a_quest, plan, next.saved)) {
				Decline(a_start.quest, a_run, "an alias of theirs couldn't be forced here");
				return;
			}
			next.since = Clock::now();
			book.Starting(a_start.quest, a_run);
			REX::INFO("RadiantSync: starting player {}'s run {} of {:08X} '{}' here with their fills ({} forced)", a_run.origin, a_run.serial, a_start.quest, QuestName(a_quest),
				plan.forces.size());
			starting = std::move(next);
			RE::Console::ExecuteCommand(std::format("startquest {:08X}", a_start.quest).c_str());
			if (a_quest->data.flags & QUEST_RUNNING) {
				FinishStart(false);
			}
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

		// This game's own run (not given to the player) stopped: theirs is started in its place.
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
			StartHere(quest, start.start, start.run, std::move(start.stages));
		}

		// A friend's run of a quest that runs here with other targets, its run here not given to this player: that run is
		// stopped (nothing of it was the player's yet) and theirs started once it has.
		void Replace(RE::TESQuest* a_quest, const Protocol::RadiantStart& a_start, const Run& a_run, std::string_view a_why)
		{
			Starting next{ a_start.quest, a_run, a_start };
			next.since = Clock::now();
			next.stopping = true;
			if (const auto offer = RadiantRules::OfferOf(a_start.quest)) {
				next.uncount = RadiantRules::LeavesCounted(a_start.quest, QuestStageLayout::Done(a_quest), GlobalValue(offer->global));
			}
			book.Starting(a_start.quest, a_run);
			REX::INFO("RadiantSync: {:08X} '{}' runs here with other targets ({}) and this player hasn't been given it: stopping it for player {}'s run {}", a_start.quest,
				QuestName(a_quest), a_why, a_run.origin, a_run.serial);
			starting = std::move(next);
			RE::Console::ExecuteCommand(std::format("stopquest {:08X}", a_start.quest).c_str());
			if (!(a_quest->data.flags & QUEST_RUNNING)) {
				StoppedForThem();
			}
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
			const bool running = (quest->data.flags & QUEST_RUNNING) != 0;
			switch (book.OnStart(a_start.quest, run, running)) {
			case Answer::kIgnore:
				return;
			case Answer::kCompare:
				{
					const auto mismatches = CompareHere(quest, AliasesOf(quest), a_start.fills);
					if (RadiantRules::SameTargets(mismatches)) {
						Share(a_start.quest, run, a_start.stages, "it runs here with the same targets");
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
			const auto run = book.Begin(id, self, done);
			Protocol::RadiantStart start{ 0, id, run.serial, FillsOf(a_quest), done };
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
			outgoing.push_back(Protocol::Encode(Protocol::RadiantStart{ 0, a_quest, a_run.serial, FillsOf(quest), done }, Protocol::MessageType::kReportRadiantStart));
		}

		void AbortStart()
		{
			if (starting) {
				Restore(starting->saved);
				if (starting->stopping && starting->uncount) {
					const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(starting->quest);
					if (quest && !(quest->data.flags & QUEST_RUNNING)) {
						CountOff(starting->quest);  // ours stopped; theirs won't start now
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

	void SetLocalPlayer(std::uint32_t a_id)
	{
		self = a_id;
	}

	void OnStart(const Protocol::RadiantStart& a_start)
	{
		if (!Enabled() || a_start.playerId == 0 || a_start.playerId == self || RadiantRules::ShareOf(a_start.quest) != RadiantRules::Share::kShared) {
			return;
		}
		const Run run{ a_start.playerId, a_start.serial };
		if (starting && starting->run == run && starting->quest == a_start.quest) {
			return;
		}
		if (std::ranges::any_of(pending, [&](const Protocol::RadiantStart& a_pending) { return a_pending.quest == a_start.quest && a_pending.playerId == run.origin && a_pending.serial == run.serial; })) {
			return;
		}
		if (pending.size() >= MAX_PENDING) {
			pending.pop_front();
		}
		pending.push_back(a_start);
	}

	void OnStage(const Protocol::RadiantStage& a_stage)
	{
		if (!Enabled() || RadiantRules::ShareOf(a_stage.quest) != RadiantRules::Share::kShared) {
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
		for (const auto& [quest, run] : book.Ours(self)) {
			SendAgain(quest, run);
		}
	}

	bool Shares(std::uint32_t a_quest)
	{
		return Enabled() && book.Shares(a_quest);
	}

	void OnDone(const RE::TESQuest* a_quest, std::span<const std::uint16_t> a_stages)
	{
		const auto id = a_quest->GetFormID();
		if (!Shares(id)) {
			return;
		}
		for (const auto stage : a_stages) {
			for (const auto& run : book.Report(id, stage)) {
				outgoing.push_back(Protocol::Encode(Protocol::RadiantStage{ 0, id, run.origin, run.serial, stage }, Protocol::MessageType::kReportRadiantStage));
				++sent;
			}
		}
	}

	void Frame(bool a_inSession)
	{
		inSession = a_inSession;
		const auto now = Clock::now();  // QuestSync polls twice a second
		if (!WorldSync::InWorld()) {
			return;
		}

		// A friend's run being started here: done once it runs (or not, in time). One that replaces this game's own run:
		// started once that has stopped.
		if (starting) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(starting->quest);
			const bool running = quest && (quest->data.flags & QUEST_RUNNING);
			if (starting->stopping) {
				if (!running) {
					StoppedForThem();
				} else if (now - starting->since >= START_TIMEOUT) {
					const auto stuck = std::move(*starting);
					starting.reset();
					Decline(stuck.quest, stuck.run, "this game's own run of it didn't stop");
				}
			} else if (running) {
				FinishStart(false);
			} else if (now - starting->since >= START_TIMEOUT) {
				FinishStart(true);
			}
		}

		// Runs that ended here: their quest stopped (completed, failed, or handed in). Never started here again.
		for (const auto id : book.Quests()) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
			if (book.IsStarting(id) || (quest && (quest->data.flags & QUEST_RUNNING))) {
				continue;
			}
			book.Ended(id);
			REX::INFO("RadiantSync: {:08X} '{}' stopped here; its shared run is over", id, QuestName(quest));
		}

		if (!a_inSession || !Enabled() || self == 0 || Story::InOpening()) {
			if (!Enabled()) {
				pending.clear();
			}
			return;
		}

		// Friends' runs, one at a time, with the game running (a start runs scripts).
		if (!starting && !pending.empty() && !Paused()) {
			const auto next = std::move(pending.front());
			pending.pop_front();
			Process(next);
		}

		// Runs of ours: a shared radiant handed out here (an objective shown), not one of a friend's.
		for (const auto& entry : RadiantRules::RADIANTS) {
			if (entry.share != RadiantRules::Share::kShared || book.Shares(entry.formId) || book.IsStarting(entry.formId)) {
				continue;
			}
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(entry.formId);
			if (!quest || !Official::IsOfficialForm(quest) || !(quest->data.flags & QUEST_RUNNING) || !QuestSync::PlayerStarted(quest)) {
				continue;
			}
			if (std::ranges::any_of(pending, [&](const Protocol::RadiantStart& a_start) { return a_start.quest == entry.formId; })) {
				continue;  // a friend's run of it comes first: it may be this one
			}
			Begin(quest);
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnSessionEnded()
	{
		AbortStart();
		pending.clear();
		book.EndSession();
		inSession = false;
	}

	void OnServerChanged()
	{
		AbortStart();
		pending.clear();
		book.Clear();
		outgoing.clear();
	}

	void Rebaseline()
	{
		AbortStart();
		pending.clear();
		book.Clear();
		outgoing.clear();
	}

	std::string DumpAliases(const RE::TESQuest* a_constQuest)
	{
		if (!a_constQuest) {
			return "no such quest";
		}
		const auto  quest = const_cast<RE::TESQuest*>(a_constQuest);
		std::string out = std::format("{:08X} '{}' flags={:04X} stage={} aliases={} share={}", quest->GetFormID(), QuestName(quest), quest->data.flags, quest->currentStage,
			quest->aliases.size(), static_cast<int>(RadiantRules::ShareOf(quest->GetFormID())));
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
					static_cast<const void*>(ref->conditions), static_cast<int>(RadiantRules::RefFill(ref->fillType, Flags(ref))), Here(quest, Kind::kRef, alias->aliasID));
			} else if (const auto loc = AsLoc(alias)) {
				out += std::format(" forced={:08X} fromAlias={:X} fromEvent={:X} eventData={:X} external={:08X}/{:X} closestTo={:X} cond={} fill={} here={:08X}",
					loc->forcedLoc ? loc->forcedLoc->GetFormID() : 0, loc->forcedFromAlias, loc->forcedFromEvent, loc->forcedEventData,
					loc->externalQuest ? loc->externalQuest->GetFormID() : 0, loc->externalAlias, loc->closestToAlias, static_cast<const void*>(loc->conditions),
					static_cast<int>(LocFillOf(loc)), Here(quest, Kind::kLoc, alias->aliasID));
			}
		}
		return out;
	}

	std::string Describe()
	{
		return std::format("radiants: runs={} begun={} joined={} declined={} sent={} taken={} pending={}{}", book.Size(), begun, joined, declined, sent, taken, pending.size(),
			starting ? " starting" : "");
	}
}
