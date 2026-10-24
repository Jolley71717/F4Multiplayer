#include "game/Traps.h"

#include "game/Papyrus.h"
#include "game/TrapRules.h"
#include "game/WeaponFire.h"
#include "MainThread.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Traps
{
	namespace
	{
		using Clock = TrapRules::Clock;
		using TrapRules::Action;
		using TrapRules::Kind;
		using TrapRules::Phase;
		using namespace std::chrono_literals;

		// How far around our player traps are looked for: the loaded cells (outdoors, five by five of 4096 units).
		constexpr float WATCH_RANGE = 12000.0f;
		constexpr auto  DISCOVER_EVERY = 1s;
		constexpr auto  POLL_EVERY = 250ms;

		struct Watch
		{
			Kind              kind = Kind::kNone;
			Phase             last = Phase::kUnknown;
			Clock::time_point seenSince{};
			RE::NiPoint3      at;
			std::uint32_t     cell = 0;        // the cell it lies in (whether that is still loaded)
			std::uint32_t     interior = 0;    // as the session places it: the interior cell, or the worldspace
			std::uint32_t     worldspace = 0;
		};

		struct Shield
		{
			RE::NiPoint3      at;
			Clock::time_point until;
			std::uint32_t     from = 0;
		};

		struct Class
		{
			Kind              kind = Kind::kNone;
			Clock::time_point firstLooked{};
			bool              final = false;  // ClassFinal: not looked at again until the next load, or its cell is back
			std::uint32_t     cell = 0;       // its parent cell when looked at: forgotten once that is no longer loaded
		};

		std::unordered_map<std::uint32_t, Class>             classified;  // references looked at while loaded (kNone: no trap)
		std::unordered_map<std::uint32_t, Watch>             watched;
		std::unordered_map<std::uint32_t, TrapRules::Known>  known;  // the session's spent traps
		std::unordered_map<std::uint32_t, Clock::time_point> activatedByUs;
		std::vector<TrapRules::Fired>                        fired;  // traps that went off here lately (Claims)
		std::vector<TrapRules::Spot>                         mines;  // the armed mines we watch (Claims)
		std::vector<Shield>                                  shields;
		std::vector<std::vector<std::uint8_t>>               outgoing;
		Clock::time_point                                    nextDiscover{};
		Clock::time_point                                    nextPoll{};
		std::optional<Clock::time_point>                     inWorldSince;

		std::uint32_t reported = 0, setOffLive = 0, spentQuietly = 0, disarmed = 0, taken = 0, received = 0, claimed = 0;
		std::uint32_t logged = 0;

		TrapRules::Spot SpotOf(const RE::NiPoint3& a_at) { return { a_at.x, a_at.y, a_at.z }; }

		// The script object of a trap's script bound to it (or of one that extends it), or null; nothing logged when there
		// is none (Papyrus::BoundObject warns).
		RE::BSTSmartPointer<RE::BSScript::Object> ScriptObject(RE::TESObjectREFR* a_ref, std::string_view a_script)
		{
			const auto game = RE::GameVM::GetSingleton();
			const auto vm = game ? game->GetVM() : nullptr;
			if (!vm || !a_ref) {
				return {};
			}
			const auto& handles = vm->GetObjectHandlePolicy();
			const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESObjectREFR>(), a_ref);
			if (handle == handles.EmptyHandle()) {
				return {};
			}
			const RE::BSFixedString                   name{ a_script };
			RE::BSTSmartPointer<RE::BSScript::Object> object;
			if (!vm->FindBoundObject(handle, name.c_str(), false, object, false)) {
				return {};
			}
			return object;
		}

		// What kind of trap a loaded reference is: a mine (a placed grenade projectile that explodes), or an activator
		// running one of the trap scripts (or one that extends it: a grenade bouquet's TrapGrenadeBouquet extends TrapBase).
		Kind Classify(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref->GetObjectReference();
			if (!base) {
				return Kind::kNone;
			}
			if (a_ref->Is(RE::ENUM_FORM_ID::kPGRE)) {
				const auto projectile = base->As<RE::BGSProjectile>();
				return projectile && projectile->data.explosionType ? Kind::kMine : Kind::kNone;
			}
			if (!base->Is(RE::ENUM_FORM_ID::kACTI)) {
				return Kind::kNone;
			}
			// Each trap script looked up by name (FindBoundObject finds one that extends it too), not by walking the bound
			// scripts (ForEachBoundObject): the walk showed only one script of a grenade bouquet a settlement can build,
			// which has workshopobjectscript beside its trap script, and which one changed from one run to the next.
			for (const auto& script : TrapRules::SCRIPTS) {
				if (ScriptObject(a_ref, script.name)) {
					return script.kind;
				}
			}
			return Kind::kNone;
		}

		// Classify, remembered: once it is known (ClassFinal), it isn't looked at again until the next load.
		Kind KindOf(RE::TESObjectREFR* a_ref, Clock::time_point a_now)
		{
			const auto id = a_ref->GetFormID();
			auto       it = classified.find(id);
			if (it != classified.end() && it->second.final) {
				return it->second.kind;
			}
			if (it == classified.end()) {
				const auto cell = a_ref->GetParentCell();
				it = classified.emplace(id, Class{ Kind::kNone, a_now, false, cell ? cell->GetFormID() : 0 }).first;
			}
			const auto base = a_ref->GetObjectReference();
			it->second.kind = Classify(a_ref);
			it->second.final = TrapRules::ClassFinal(it->second.kind, base && base->Is(RE::ENUM_FORM_ID::kACTI), it->second.firstLooked, a_now);
			return it->second.kind;
		}

		bool CellAttached(std::uint32_t a_cell)
		{
			const auto cell = RE::TESForm::GetFormByID<RE::TESObjectCELL>(a_cell);
			return cell && cell->IsAttached();
		}

		Phase Read(std::uint32_t a_id, const Watch& a_watch)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (!ref || ref->IsDeleted() || ref->IsDisabled()) {
				return Phase::kGone;
			}
			if (a_watch.kind == Kind::kMine) {
				return Phase::kArmed;
			}
			const auto object = ScriptObject(ref, TrapRules::ScriptOf(a_watch.kind));
			if (!object) {
				return Phase::kUnknown;
			}
			const char* state = object->currentState.c_str();
			return TrapRules::PhaseOf(a_watch.kind, state ? state : "");
		}

		void Log(std::string a_line)
		{
			if (logged < 200) {
				++logged;
				REX::INFO("Traps: {}", a_line);
			}
		}

		template <class... Args>
		bool Call(RE::TESObjectREFR* a_ref, Kind a_kind, std::string_view a_function, Args... a_args)
		{
			return Papyrus::CallMethod(a_ref, TrapRules::ScriptOf(a_kind), a_function, false, a_args...);
		}

		bool GoTo(RE::TESObjectREFR* a_ref, Kind a_kind, std::string_view a_state)
		{
			return Call(a_ref, a_kind, "GoToState", RE::BSFixedString{ a_state });
		}

		// Sets a mine's blast off where it lies, as a friend's explosion is placed (WeaponFire), and the mine goes.
		void Blow(RE::TESObjectREFR* a_ref, std::uint32_t a_from)
		{
			const auto base = a_ref->GetObjectReference();
			const auto projectile = base ? base->As<RE::BGSProjectile>() : nullptr;
			if (projectile && projectile->data.explosionType) {
				WeaponFire::SetOffTrapExplosion(projectile->data.explosionType, a_ref->data.location, a_from);
			}
			a_ref->Disable();
		}

		// Does to our trap what was done to the friend's. True once it is done (or there is nothing to do).
		bool Act(std::uint32_t a_id, const Watch& a_watch, Action a_action, std::uint32_t a_from)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (!ref) {
				return true;
			}
			const auto kind = a_watch.kind;
			bool       done = true;
			switch (a_action) {
			case Action::kWait:
				return false;
			case Action::kNothing:
				return true;
			case Action::kTake:
				ref->Disable();
				++taken;
				break;
			case Action::kFireLive:
				++setOffLive;
				shields.push_back({ ref->data.location, Clock::now() + TrapRules::CLAIM_WINDOW, a_from });
				fired.push_back({ SpotOf(ref->data.location), Clock::now() });
				WeaponFire::ShieldCopiesNear(ref->data.location, TrapRules::CLAIM_RADIUS, a_from);
				switch (kind) {
				case Kind::kMine:
					Blow(ref, a_from);
					break;
				case Kind::kTripwire:
					done = Call(ref, kind, "Trigger");  // its trip: the sound, the snap, and what it is linked to set off
					break;
				case Kind::kTension:
					done = GoTo(ref, kind, "triggered");  // the same, for a tension trigger
					break;
				case Kind::kTrap:
					done = GoTo(ref, kind, "warn");  // its warning, then FireTrap
					break;
				case Kind::kBearTrap:
					done = GoTo(ref, kind, "Disarming");  // it snaps shut on nobody (no hit in that state)
					break;
				case Kind::kNone:
					break;
				}
				break;
			case Action::kFireQuiet:
				++spentQuietly;
				fired.push_back({ SpotOf(ref->data.location), Clock::now() });
				switch (kind) {
				case Kind::kMine:
					ref->Disable();
					break;
				case Kind::kTripwire:
					done = GoTo(ref, kind, "Inactive");  // tripped: shown destroyed (its own onBeginState)
					break;
				case Kind::kTension:
					// Its "triggered" would set off what it is linked to: those come spent on their own. Busy does nothing.
					done = GoTo(ref, kind, "Busy") && Call(ref, kind, "SetDestroyed", true);
					break;
				case Kind::kTrap:
					done = GoTo(ref, kind, "TrapFired") && Call(ref, kind, "SetDestroyed", true);
					break;
				case Kind::kBearTrap:
					done = GoTo(ref, kind, "Disarming");
					break;
				case Kind::kNone:
					break;
				}
				break;
			case Action::kDisarm:
				++disarmed;
				switch (kind) {
				case Kind::kMine:
					ref->Disable();
					break;
				case Kind::kTripwire:
				case Kind::kTension:
					done = GoTo(ref, kind, "Disarmed");  // its own disarmed look (the reward and the XP are in DisarmTrigger, not here)
					break;
				case Kind::kTrap:
					{
						// Not its Disarm state: DisarmTrap gives the disarm's reward and XP to our player. What it ends as instead:
						// destroyed, or gone if the trap is one a disarm takes away.
						const auto object = ScriptObject(ref, TrapRules::ScriptOf(kind));
						const auto disables = object ? object->GetProperty(RE::BSFixedString{ "DisarmDisables" }) : nullptr;
						done = GoTo(ref, kind, "TrapFired") && Call(ref, kind, "SetDestroyed", true);
						if (disables && disables->is<bool>() && RE::BSScript::get<bool>(*disables)) {
							ref->Disable();
						}
						break;
					}
				case Kind::kBearTrap:
					done = GoTo(ref, kind, "Disarming");
					break;
				case Kind::kNone:
					break;
				}
				break;
			}
			Log(std::format("{:08X} ({}) {} for player {}", a_id, TrapRules::NameOf(kind),
				a_action == Action::kFireLive ? "set off as it went off in their game" : a_action == Action::kFireQuiet ? "spent (it went off in their game)" :
				a_action == Action::kDisarm   ? "disarmed (in their game)" : "taken away (they took it)",
				a_from));
			return done;
		}

		void Discover(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			// What was looked at in cells no longer loaded is forgotten (looked at afresh when they are back): else every
			// reference a long run passes stays here.
			std::unordered_map<std::uint32_t, bool> attached;
			std::erase_if(classified, [&](const auto& a_entry) {
				const auto [it, added] = attached.try_emplace(a_entry.second.cell, false);
				if (added) {
					it->second = a_entry.second.cell != 0 && CellAttached(a_entry.second.cell);
				}
				return !it->second;
			});
			std::vector<RE::TESObjectREFR*> found;
			tes->ForEachReferenceInRange(a_player->data.location, WATCH_RANGE, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && Protocol::IsShareableRef(a_ref->GetFormID()) && !watched.contains(a_ref->GetFormID())) {
					found.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto ref : found) {
				const auto id = ref->GetFormID();
				if (ref->IsDeleted() || ref->IsDisabled() || !ref->Get3D() || !ref->GetParentCell()) {
					continue;
				}
				const auto kind = KindOf(ref, a_now);
				if (kind == Kind::kNone) {
					continue;
				}
				const auto cell = ref->GetParentCell();
				Watch watch;
				watch.kind = kind;
				watch.seenSince = a_now;
				watch.at = ref->data.location;
				watch.cell = cell->GetFormID();
				watch.interior = cell->IsInterior() ? cell->GetFormID() : 0;
				watch.worldspace = !cell->IsInterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
				watch.last = Read(id, watch);
				watched.emplace(id, watch);
			}
		}

		void Poll(Clock::time_point a_now)
		{
			mines.clear();
			std::erase_if(activatedByUs, [&](const auto& a_entry) { return a_now - a_entry.second > TrapRules::TAKE_WINDOW; });
			std::erase_if(fired, [&](const auto& a_entry) { return a_now - a_entry.when > TrapRules::CLAIM_WINDOW; });
			std::erase_if(shields, [&](const auto& a_entry) { return a_now > a_entry.until; });
			for (const auto& shield : shields) {
				WeaponFire::ShieldCopiesNear(shield.at, TrapRules::CLAIM_RADIUS, shield.from);  // its grenades, its shots, for a while
			}
			const bool settledWorld = inWorldSince.has_value();
			for (auto it = watched.begin(); it != watched.end();) {
				const auto id = it->first;
				auto&      watch = it->second;
				if (!CellAttached(watch.cell)) {
					it = watched.erase(it);  // out of the loaded area: looked at afresh when it is back
					continue;
				}
				const auto phase = Read(id, watch);
				const auto entry = known.find(id);
				const auto session = entry != known.end() ? std::optional{ entry->second.state.state } : std::nullopt;
				const bool takenByUs = activatedByUs.contains(id);
				const bool settled = settledWorld && TrapRules::Settled(watch.seenSince, *inWorldSince, a_now);
				if (const auto state = settled ? TrapRules::Change(watch.kind, watch.last, phase, takenByUs, session) : std::nullopt) {
					Protocol::TrapState report{ 0, id, *state, watch.interior, watch.worldspace, watch.at.x, watch.at.y, watch.at.z };
					outgoing.push_back(Protocol::Encode(report));
					TrapRules::Take(known, report, false, a_now);
					known[id].applied = true;
					++reported;
					Log(std::format("{:08X} ({}) {} here: reported", id, TrapRules::NameOf(watch.kind),
						*state == Protocol::TrapState::kFired ? "went off" : *state == Protocol::TrapState::kDisarmed ? "was disarmed" : "was taken"));
				}
				const bool wentOff = (watch.last == Phase::kArmed || watch.last == Phase::kGoing) &&
				                     (phase == Phase::kFired || (phase == Phase::kGone && watch.kind == Kind::kMine && !takenByUs));
				if (wentOff) {
					fired.push_back({ SpotOf(watch.at), a_now });
				}
				if (phase != Phase::kUnknown) {
					watch.last = phase;
				}
				if (watch.kind == Kind::kMine && phase == Phase::kArmed) {
					mines.push_back(SpotOf(watch.at));
				}
				if (const auto it2 = known.find(id); it2 != known.end() && !it2->second.applied && settledWorld) {
					auto&       entry2 = it2->second;
					const auto& state = entry2.state;
					const float distance = watch.at.GetDistance(RE::NiPoint3{ state.x, state.y, state.z });
					const bool  live = TrapRules::Live(entry2.live, entry2.arrived, a_now, distance);
					const auto  action = TrapRules::Decide(state.state, phase, live, TrapRules::Settled(watch.seenSince, *inWorldSince, a_now));
					if (action != Action::kWait && Act(id, watch, action, state.playerId)) {
						entry2.applied = true;
					}
				}
				++it;
			}
		}
	}

	void Frame()
	{
		MainThread::Assert("Traps::Frame");
		const auto now = Clock::now();
		if (!inWorldSince) {
			inWorldSince = now;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return;
		}
		if (now >= nextDiscover) {
			nextDiscover = now + DISCOVER_EVERY;
			Discover(player, now);
		}
		if (now >= nextPoll) {
			nextPoll = now + POLL_EVERY;
			Poll(now);
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Apply(const Protocol::TrapStates& a_traps)
	{
		const auto now = Clock::now();
		for (const auto& trap : a_traps.traps) {
			if (TrapRules::Take(known, trap, a_traps.live, now)) {
				++received;
				if (a_traps.live) {
					Log(std::format("{:08X} {} in player {}'s game", trap.refId,
						trap.state == Protocol::TrapState::kFired ? "went off" : trap.state == Protocol::TrapState::kDisarmed ? "was disarmed" : "was taken", trap.playerId));
				}
			}
		}
	}

	void OnPlayerActivated(std::uint32_t a_ref)
	{
		activatedByUs[a_ref] = Clock::now();
	}

	bool IsTrap(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		if (!a_ref->Get3D() || a_ref->IsDeleted()) {
			return false;
		}
		const auto kind = KindOf(a_ref, Clock::now());
		return kind != Kind::kNone && kind != Kind::kMine;
	}

	bool Claims(const RE::NiPoint3& a_at)
	{
		if (!TrapRules::Claims(fired, mines, SpotOf(a_at), Clock::now())) {
			return false;
		}
		++claimed;
		return true;
	}

	void OnGameLoaded()
	{
		// The loaded world's traps are the save's: every spent one is looked at again, and nothing just seen is a change.
		watched.clear();
		classified.clear();
		activatedByUs.clear();
		fired.clear();
		mines.clear();
		shields.clear();
		inWorldSince.reset();
		for (auto& [id, entry] : known) {
			entry.applied = false;
			entry.live = false;
		}
	}

	void Reset()
	{
		OnGameLoaded();
		known.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("traps={}/{} sent={} got={} live={} quiet={} disarmed={} taken={} blasts={}", watched.size(), known.size(), reported, received, setOffLive, spentQuietly,
			disarmed, taken, claimed);
	}

	std::string List()
	{
		std::string out;
		const auto  phaseName = [](Phase a_phase) {
			switch (a_phase) {
			case Phase::kArmed:
				return "armed";
			case Phase::kGoing:
				return "going";
			case Phase::kFired:
				return "fired";
			case Phase::kDisarmed:
				return "disarmed";
			case Phase::kGone:
				return "gone";
			case Phase::kUnknown:
				break;
			}
			return "unknown";
		};
		for (const auto& [id, watch] : watched) {
			const auto entry = known.find(id);
			out += std::format("{}{:08X} {} {} at ({:.0f},{:.0f},{:.0f}){}", out.empty() ? "" : "; ", id, TrapRules::NameOf(watch.kind), phaseName(watch.last), watch.at.x, watch.at.y,
				watch.at.z, entry == known.end() ? "" : std::format(" session: {} by player {}{}", entry->second.state.state, entry->second.state.playerId, entry->second.applied ? " (done)" : ""));
		}
		return out.empty() ? "no traps watched" : out;
	}

	std::string Why(std::uint32_t a_ref)
	{
		const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_ref);
		if (!ref) {
			return std::format("{:08X}: no such reference", a_ref);
		}
		const auto  base = ref->GetObjectReference();
		std::string out = std::format("{:08X} base={:08X} type={} 3d={} disabled={}", a_ref, base ? base->GetFormID() : 0, base ? static_cast<int>(base->GetFormType()) : -1,
			ref->Get3D() != nullptr, ref->IsDisabled());
		if (const auto it = classified.find(a_ref); it != classified.end()) {
			out += std::format(" taken for {} ({}, first looked {} s ago)", TrapRules::NameOf(it->second.kind), it->second.final ? "final" : "looked at again",
				std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it->second.firstLooked).count());
		} else {
			out += " not looked at";
		}
		out += watched.contains(a_ref) ? ", watched" : ", not watched";
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm) {
			return out + "; no script VM";
		}
		const auto& handles = vm->GetObjectHandlePolicy();
		const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESObjectREFR>(), ref);
		if (handle == handles.EmptyHandle()) {
			return out + "; no script handle";
		}
		// The bound scripts walked twice, the walker answering kContinue, then kStop (how many each walk sees), and each trap
		// script looked up by name.
		std::string scripts;
		int         seen = 0;
		auto        result = RE::BSContainer::ForEachResult::kContinue;
		const RE::BSTThreadScrapFunction<RE::BSContainer::ForEachResult(RE::BSScript::Object*)> visit{ [&](RE::BSScript::Object* a_object) {
			++seen;
			if (result == RE::BSContainer::ForEachResult::kContinue) {
				std::string chain;
				for (RE::BSTSmartPointer type{ a_object ? a_object->type.get() : nullptr }; type; type = type->parentTypeInfo) {
					const char* name = type->GetName();
					chain += std::format("{}{}", chain.empty() ? "" : "<", name ? name : "?");
				}
				const char* state = a_object ? a_object->currentState.c_str() : nullptr;
				scripts += std::format("{}{} [{}]", scripts.empty() ? "" : ", ", chain, state ? state : "");
			}
			return result;
		} };
		vm->ForEachBoundObject(handle, visit);
		const int seenContinuing = std::exchange(seen, 0);
		result = RE::BSContainer::ForEachResult::kStop;
		vm->ForEachBoundObject(handle, visit);
		std::string found;
		for (const auto& script : TrapRules::SCRIPTS) {
			found += std::format(" {}={}", script.name, ScriptObject(ref, script.name) ? "bound" : "no");
		}
		return out + std::format("; handle {:X}, walk (kContinue) saw {}: {}; walk (kStop) saw {}; looked up:{}", handle, seenContinuing, scripts.empty() ? "none" : scripts, seen,
			found);
	}
}
