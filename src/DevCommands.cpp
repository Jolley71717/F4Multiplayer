#include "DevCommands.h"

#include "game/Equipment.h"
#include "game/Papyrus.h"
#include "game/Puppets.h"
#include "game/Party.h"
#include "game/WeaponFire.h"
#include "net/Session.h"
#include "steam/Steam.h"

namespace DevCommands
{
	namespace
	{
		constexpr float TO_DEGREES = 180.0f / std::numbers::pi_v<float>;
		constexpr float TO_RADIANS = std::numbers::pi_v<float> / 180.0f;

		std::vector<std::string_view> SplitArgs(std::string_view a_args)
		{
			std::vector<std::string_view> out;
			while (!a_args.empty()) {
				const auto start = a_args.find_first_not_of(' ');
				if (start == std::string_view::npos) {
					break;
				}
				a_args.remove_prefix(start);
				const auto end = a_args.find(' ');
				out.push_back(a_args.substr(0, end));
				a_args.remove_prefix(end == std::string_view::npos ? a_args.size() : end);
			}
			return out;
		}

		std::optional<std::uint32_t> ParseHex(std::string_view a_str)
		{
			if (a_str.starts_with("0x") || a_str.starts_with("0X")) {
				a_str.remove_prefix(2);
			}
			std::uint32_t value{};
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), value, 16);
			if (ec != std::errc{} || ptr != a_str.data() + a_str.size()) {
				return std::nullopt;
			}
			return value;
		}

		std::optional<float> ParseFloat(std::string_view a_str)
		{
			float value{};
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), value);
			if (ec != std::errc{} || ptr != a_str.data() + a_str.size()) {
				return std::nullopt;
			}
			return value;
		}

		RE::TESObjectREFR* LookupRef(std::string_view a_hex);

		RE::TESObjectCELL* PlayerCell()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player ? player->GetParentCell() : nullptr;
		}

		std::string DescribeMovement(RE::TESObjectREFR* a_ref)
		{
			const auto actor = a_ref->As<RE::Actor>();
			if (!actor) {
				return {};
			}
			RE::NiPoint3 velocity;
			actor->GetLinearVelocity(velocity);
			const auto root = actor->Get3D();
			const auto w = root ? root->world.translate : RE::NiPoint3{};
			const auto values = RE::ActorValue::GetSingleton();
			const float health = values && values->health ? static_cast<RE::ActorValueOwner*>(actor)->GetActorValue(*values->health) : -1.0f;
			const auto base = actor->GetObjectReference();
			const auto npc = base ? base->As<RE::TESNPC>() : nullptr;
			return std::format(" base={:08X} female={} template={}", base ? base->GetFormID() : 0, npc && npc->IsFemale(), npc && npc->UsesTemplate()) +
			       std::format(" hp={:.0f} dead={}", health, actor->IsDead(false)) + std::format(" vel=({:.0f},{:.0f},{:.0f}) moveMode={:04X} 3dWorld=({:.0f},{:.0f},{:.0f})",
				velocity.x, velocity.y, velocity.z, static_cast<std::uint32_t>(static_cast<const RE::ActorState&>(*actor).moveMode), w.x, w.y, w.z);
		}

		std::string DescribeRef(RE::TESObjectREFR* a_ref)
		{
			const auto& loc = a_ref->data.location;
			const auto  cell = a_ref->GetParentCell();
			return std::format(
				"ref={:08X} x={:.1f} y={:.1f} z={:.1f} rz={:.1f} cell={:08X} 3d={}",
				a_ref->GetFormID(), loc.x, loc.y, loc.z, a_ref->data.angle.z * TO_DEGREES,
				cell ? cell->GetFormID() : 0, a_ref->Get3D() != nullptr) + DescribeMovement(a_ref);
		}

		std::string Status(std::string_view)
		{
			const auto cell = PlayerCell();
			return std::format("ingame={} cell={:08X}", cell != nullptr, cell ? cell->GetFormID() : 0);
		}

		std::string Pos(std::string_view)
		{
			if (!PlayerCell()) {
				return "error: not in game";
			}
			return DescribeRef(RE::PlayerCharacter::GetSingleton());
		}

		std::string Console(std::string_view a_args)
		{
			if (a_args.empty()) {
				return "error: usage: console <command>";
			}
			const std::string command{ a_args };
			REX::INFO("DevCommands: console {}", command);
			RE::Console::ExecuteCommand(command.c_str());
			return "ok";
		}

		// findnpc <text>: lists NPC base forms whose name contains <text> (case-insensitive).
		std::string FindNpc(std::string_view a_args)
		{
			if (a_args.empty()) {
				return "error: usage: findnpc <text>";
			}

			const auto lower = [](std::string_view a_str) {
				std::string out{ a_str };
				std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return out;
			};
			const auto needle = lower(a_args);

			std::string result;
			int         count = 0;
			for (const auto npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
				if (!npc) {
					continue;
				}
				const auto name = RE::TESFullName::GetFullName(*npc);
				if (name.empty() || lower(name).find(needle) == std::string::npos) {
					continue;
				}
				result += std::format("{}{:08X} '{}'", count ? "; " : "", npc->GetFormID(), name);
				if (++count == 15) {
					break;
				}
			}
			return count ? result : "no matches";
		}

		// findref <text>: actor references (loaded or not) whose name contains the text.
		std::string FindRef(std::string_view a_args)
		{
			if (a_args.empty()) {
				return "error: usage: findref <text>";
			}
			const auto lower = [](std::string_view a_str) {
				std::string out{ a_str };
				std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return out;
			};
			const auto  needle = lower(a_args);
			std::string result;
			int         count = 0;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "no matches";
			}
			for (const auto& [id, form] : *map) {
				const auto actor = form ? form->As<RE::Actor>() : nullptr;
				const auto base = actor ? actor->GetObjectReference() : nullptr;
				if (!base || count >= 20 || (id >> 24) == 0xFF) {
					continue;
				}
				const auto name = RE::TESFullName::GetFullName(*base);
				if (name.empty() || lower(name).find(needle) == std::string::npos) {
					continue;
				}
				result += std::format("{}{:08X} '{}' dead={} 3d={}", count ? "; " : "", id, name, actor->IsDead(false), actor->Get3D() != nullptr);
				++count;
			}
			return count ? result : "no matches";
		}

		// safenpc [female|male]: lists named human NPC bases with no factions and no templates,
		// i.e. candidates for remote player stand-ins that won't drag in crimes or quests.
		std::string SafeNpc(std::string_view a_args)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto playerRace = player ? player->race : nullptr;
			const bool wantFemale = a_args == "female";
			const bool wantMale = a_args == "male";

			std::string result;
			int         count = 0;
			for (const auto npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
				if (!npc || npc->UsesTemplate() || !npc->factions.empty() || npc->IsUnique()) {
					continue;
				}
				if (playerRace && npc->GetFormRace() != playerRace) {
					continue;
				}
				if ((wantFemale && !npc->IsFemale()) || (wantMale && npc->IsFemale())) {
					continue;
				}
				const auto name = RE::TESFullName::GetFullName(*npc);
				if (name.empty()) {
					continue;
				}
				result += std::format("{}{:08X} '{}'", count ? "; " : "", npc->GetFormID(), name);
				if (++count == 25) {
					break;
				}
			}
			return count ? result : "no matches";
		}

		// actors [radius]: lists loaded actors near the player.
		std::string Actors(std::string_view a_args)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return "error: not in game";
			}
			const float radius = ParseFloat(a_args).value_or(3000.0f);
			std::string out;
			int         count = 0;
			cell->ForEachReferenceInRange(player->data.location, radius, [&](RE::TESObjectREFR* a_ref) {
				const auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				if (actor && !actor->IsPlayerRef() && count < 30) {
					const auto base = actor->GetObjectReference();
					out += std::format("{}{:08X} '{}' dead={}", count ? "; " : "", actor->GetFormID(), base ? RE::TESFullName::GetFullName(*base) : ""sv, actor->IsDead(false));
					++count;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return count ? out : "none";
		}

		// containers [radius]: lists nearby container references.
		std::string Containers(std::string_view a_args)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return "error: not in game";
			}
			const float radius = ParseFloat(a_args).value_or(2000.0f);
			std::string out;
			int         count = 0;
			cell->ForEachReferenceInRange(player->data.location, radius, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->Is(RE::ENUM_FORM_ID::kCONT) && count < 30) {
					out += std::format("{}{:08X} '{}'", count ? "; " : "", a_ref->GetFormID(), RE::TESFullName::GetFullName(*base));
					++count;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return count ? out : "none";
		}

		// doors [radius]: lists nearby doors with open state (1 open .. 4 closing) and lock (L locked, U unlocked, - none).
		std::string Doors(std::string_view a_args)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return "error: not in game";
			}
			const float radius = ParseFloat(a_args).value_or(2000.0f);
			std::string out;
			int         count = 0;
			cell->ForEachReferenceInRange(player->data.location, radius, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->Is(RE::ENUM_FORM_ID::kDOOR) && a_ref->Get3D() && count < 30) {
					const auto lock = a_ref->GetLock();
					const char lockChar = !lock ? '-' : (lock->flags & std::to_underlying(RE::REFR_LOCK::Flags::kLocked)) ? 'L' : 'U';
					out += std::format("{}{:08X} '{}' open={} lock={}", count ? "; " : "", a_ref->GetFormID(), RE::TESFullName::GetFullName(*base),
						std::to_underlying(RE::BGSOpenCloseForm::GetOpenState(a_ref)), lockChar);
					++count;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return count ? out : "none";
		}

		// findgear <text>: weapons and armor whose name contains text.
		std::string FindGear(std::string_view a_args)
		{
			if (a_args.empty()) {
				return "error: usage: findgear <text>";
			}
			const auto lower = [](std::string_view a_str) {
				std::string out{ a_str };
				std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return out;
			};
			const auto  needle = lower(a_args);
			std::string result;
			int         count = 0;
			const auto  scan = [&](auto& a_forms) {
				for (const auto form : a_forms) {
					const auto name = form ? RE::TESFullName::GetFullName(*form) : ""sv;
					if (count < 15 && !name.empty() && lower(name).find(needle) != std::string::npos) {
						result += std::format("{}{:08X} '{}'", count++ ? "; " : "", form->GetFormID(), name);
					}
				}
			};
			const auto data = RE::TESDataHandler::GetSingleton();
			scan(data->GetFormArray<RE::TESObjectWEAP>());
			scan(data->GetFormArray<RE::TESObjectARMO>());
			return count ? result : "none";
		}

		// equipped [refHex]: armor and weapons an actor (default: the player) has equipped.
		std::string Equipped(std::string_view a_args)
		{
			RE::Actor* actor = RE::PlayerCharacter::GetSingleton();
			if (!a_args.empty()) {
				const auto id = ParseHex(a_args);
				actor = id ? RE::TESForm::GetFormByID<RE::Actor>(*id) : nullptr;
			}
			if (!actor) {
				return "error: no such actor";
			}
			std::string out = std::format("drawn={}", actor->GetWeaponMagicDrawn());
			for (const auto id : Equipment::Read(actor)) {
				const auto form = RE::TESForm::GetFormByID(id);
				out += std::format(" {:08X} '{}'", id, form ? RE::TESFullName::GetFullName(*form) : ""sv);
			}
			return out;
		}

		// combat <refHex>: whether an actor is in combat and whom it targets.
		std::string Combat(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			const auto actor = id ? RE::TESForm::GetFormByID<RE::Actor>(*id) : nullptr;
			if (!actor) {
				return "error: usage: combat <actorHex>";
			}
			const auto target = actor->currentCombatTarget.get();
			return std::format("inCombat={} target={:08X} dead={} hp={:.0f}", actor->IsInCombat(), target ? target->GetFormID() : 0, actor->IsDead(false),
				RE::ActorValue::GetSingleton()->health ? static_cast<RE::ActorValueOwner*>(actor)->GetActorValue(*RE::ActorValue::GetSingleton()->health) : -1.0f);
		}

		// quests [text]: story/faction/side quests with a stage set (or whose name contains text).
		std::string Quests(std::string_view a_args)
		{
			std::string out;
			int         count = 0;
			for (const auto quest : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>()) {
				if (!quest || quest->data.questType <= 0 || quest->data.questType == 6 || count >= 25) {
					continue;
				}
				const std::string_view name = quest->GetFullName() ? quest->GetFullName() : "";
				if (a_args.empty() ? quest->currentStage == 0 : name.find(a_args) == std::string_view::npos) {
					continue;
				}
				out += std::format("{}{:08X} '{}' type={} stage={}", count++ ? "; " : "", quest->GetFormID(), name, quest->data.questType, quest->currentStage);
			}
			return count ? out : "none";
		}

		// draw on|off: draws or holsters the player's weapon.
		std::string Draw(std::string_view a_args)
		{
			// draw on|off [actorHex]
			const auto args = SplitArgs(a_args);
			const auto ref = args.size() > 1 ? LookupRef(args[1]) : RE::PlayerCharacter::GetSingleton();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor || !actor->Get3D() || args.empty()) {
				return "error: usage: draw on|off [actorHex]";
			}
			actor->DrawWeaponMagicHands(args[0] == "on");
			return "ok";
		}

		// items [radius]: lists nearby loose items that can be picked up.
		std::string Items(std::string_view a_args)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return "error: not in game";
			}
			const float radius = ParseFloat(a_args).value_or(2000.0f);
			std::string out;
			int         count = 0;
			cell->ForEachReferenceInRange(player->data.location, radius, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				using F = RE::ENUM_FORM_ID;
				if (base && !a_ref->IsDisabled() && base->Is(F::kMISC, F::kWEAP, F::kALCH, F::kAMMO, F::kARMO, F::kBOOK, F::kNOTE) && count < 30) {
					out += std::format("{}{:08X} '{}'", count ? "; " : "", a_ref->GetFormID(), RE::TESFullName::GetFullName(*base));
					++count;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return count ? out : "none";
		}

		// count <refHex> <itemHex>: how many of an item a container/actor holds.
		std::string Count(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.size() < 2 ? nullptr : LookupRef(args[0]);
			const auto itemId = args.size() < 2 ? std::nullopt : ParseHex(args[1]);
			const auto item = itemId ? RE::TESForm::GetFormByID(*itemId) : nullptr;
			if (!ref || !item) {
				return "error: usage: count <refHex> <itemHex>";
			}
			std::uint32_t n = 0;
			ref->GetItemCount(n, item, false);
			return std::format("{}", n);
		}

		// spawn <baseHex> [distance]: places a copy of an NPC base in front of the player.
		std::string Spawn(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto baseID = args.empty() ? std::nullopt : ParseHex(args[0]);
			if (!baseID) {
				return "error: usage: spawn <baseHex> [distance]";
			}
			const auto npc = RE::TESForm::GetFormByID<RE::TESNPC>(*baseID);
			if (!npc) {
				return std::format("error: {:08X} is not an NPC base form", *baseID);
			}
			const auto cell = PlayerCell();
			if (!cell) {
				return "error: not in game";
			}

			const auto  player = RE::PlayerCharacter::GetSingleton();
			const float distance = args.size() > 1 ? ParseFloat(args[1]).value_or(150.0f) : 150.0f;
			const float heading = player->data.angle.z;

			RE::NEW_REFR_DATA data;
			data.location = player->data.location;
			data.location.x += std::sin(heading) * distance;
			data.location.y += std::cos(heading) * distance;
			data.direction = { 0.0f, 0.0f, heading + std::numbers::pi_v<float> };
			data.object = npc;
			data.interior = cell->IsInterior() ? cell : nullptr;
			data.world = cell->IsInterior() ? nullptr : cell->worldSpace;
			data.clearStillLoadingFlag = true;

			const auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			if (!ref) {
				return "error: CreateReferenceAtLocation failed";
			}
			REX::INFO("DevCommands: spawned {:08X} from base {:08X}", ref->GetFormID(), *baseID);
			return DescribeRef(ref.get());
		}

		RE::TESObjectREFR* LookupRef(std::string_view a_hex)
		{
			const auto id = ParseHex(a_hex);
			return id ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(*id) : nullptr;
		}

		// refinfo <refHex>
		std::string RefInfo(std::string_view a_args)
		{
			const auto ref = LookupRef(a_args);
			return ref ? DescribeRef(ref) : "error: no such reference";
		}

		// setpos <refHex> <x> <y> <z> [rz degrees]
		std::string SetPos(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() < 4) {
				return "error: usage: setpos <refHex> <x> <y> <z> [rz]";
			}
			const auto ref = LookupRef(args[0]);
			const auto x = ParseFloat(args[1]);
			const auto y = ParseFloat(args[2]);
			const auto z = ParseFloat(args[3]);
			if (!ref || !x || !y || !z) {
				return "error: bad reference or coordinates";
			}

			const RE::NiPoint3 pos{ *x, *y, *z };
			const auto         actor = ref->As<RE::Actor>();
			if (actor && Puppets::IsPuppet(actor)) {
				const float heading = args.size() > 4 ? ParseFloat(args[4]).value_or(0.0f) * TO_RADIANS : actor->data.angle.z;
				Puppets::SetTarget(actor, Puppets::Motion{ .position = pos, .heading = heading });
			} else if (actor) {
				actor->SetPosition(pos, true);
				if (args.size() > 4) {
					if (const auto rz = ParseFloat(args[4])) {
						actor->SetHeading(*rz * TO_RADIANS);
					}
				}
			} else {
				ref->SetLocationOnReference(pos);
			}
			return DescribeRef(ref);
		}

		// puppet <refHex> [0|1]: takes an actor out of AI control (or gives it back).
		std::string Puppet(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.empty() ? nullptr : LookupRef(args[0]);
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor || actor->IsPlayerRef()) {
				return "error: usage: puppet <actorRefHex> [0|1] (not the player)";
			}
			if (args.size() > 1 && args[1] == "0") {
				Puppets::Unregister(actor);
				return "released";
			}
			Puppets::Register(actor);
			Puppets::SetTarget(actor, Puppets::Motion{ .position = actor->data.location, .heading = actor->data.angle.z });
			return std::format("puppeted niFlags={:08X} boolFlags={:08X} moreFlags={:08X}",
				actor->niFlags.flags, actor->boolFlags.underlying(), actor->moreFlags);
		}

		// papyrus <refHex> <Script> <Function> [bool...]: calls a Papyrus method with bool args.
		std::string CallPapyrus(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.empty() ? nullptr : LookupRef(args[0]);
			if (!ref || args.size() < 3) {
				return "error: usage: papyrus <refHex> <Script> <Function> [0|1 ...]";
			}

			std::vector<char> bools;
			for (std::size_t i = 3; i < args.size(); ++i) {
				bools.push_back(args[i] == "1" || args[i] == "true");
			}

			if (bools.size() > 2) {
				return "error: at most 2 bool arguments";
			}

			// The handle type must match the object's real script type.
			const auto call = [&](auto* a_object) {
				switch (bools.size()) {
				case 0:
					return Papyrus::CallMethod(a_object, args[1], args[2]);
				case 1:
					return Papyrus::CallMethod(a_object, args[1], args[2], static_cast<bool>(bools[0]));
				default:
					return Papyrus::CallMethod(a_object, args[1], args[2], static_cast<bool>(bools[0]), static_cast<bool>(bools[1]));
				}
			};

			const auto actor = ref->As<RE::Actor>();
			const bool ok = actor ? call(actor) : call(ref);
			return ok ? "dispatched" : "error: dispatch failed";
		}

		// flags <refHex> [ni|bool|more] [set|clear] [maskHex]: reads or edits actor flag words.
		std::string Flags(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.empty() ? nullptr : LookupRef(args[0]);
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor) {
				return "error: usage: flags <actorRefHex> [ni|bool|more set|clear maskHex]";
			}

			if (args.size() >= 4) {
				const auto mask = ParseHex(args[3]);
				if (!mask) {
					return "error: bad mask";
				}
				std::uint32_t* word = nullptr;
				if (args[1] == "ni") {
					word = &actor->niFlags.flags;
				} else if (args[1] == "bool") {
					word = reinterpret_cast<std::uint32_t*>(&actor->boolFlags);
				} else if (args[1] == "more") {
					word = &actor->moreFlags;
				} else {
					return "error: field must be ni, bool or more";
				}
				*word = args[2] == "set" ? (*word | *mask) : (*word & ~*mask);
			}

			return std::format("niFlags={:08X} boolFlags={:08X} moreFlags={:08X}",
				actor->niFlags.flags, actor->boolFlags.underlying(), actor->moreFlags);
		}

		// remove <refHex>: disables and deletes a reference we spawned.
		std::string Remove(std::string_view a_args)
		{
			const auto ref = LookupRef(a_args);
			if (!ref) {
				return "error: no such reference";
			}
			if (!ref->IsCreated()) {
				return "error: refusing to delete a reference from a plugin file";
			}
			if (const auto actor = ref->As<RE::Actor>()) {
				Puppets::Unregister(actor);
			}
			ref->Disable();
			ref->SetDelete(true);
			return "ok";
		}

		// graph <refHex> get <var> [<var>...] | setf <var> <float> | setb <var> <0|1> | seti <var> <int>
		std::string Graph(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.empty() ? nullptr : LookupRef(args[0]);
			if (!ref || args.size() < 3) {
				return "error: usage: graph <refHex> get <var>... | setf|setb|seti <var> <value>";
			}
			const auto holder = static_cast<RE::IAnimationGraphManagerHolder*>(ref);

			if (args[1] == "get") {
				std::string out;
				for (std::size_t i = 2; i < args.size(); ++i) {
					const RE::BSFixedString name{ args[i] };
					float                   f = 0.0f;
					std::int32_t            n = 0;
					bool                    b = false;
					if (holder->GetGraphVariableImplFloat(name, f)) {
						out += std::format("{}={:.3f} ", args[i], f);
					} else if (holder->GetGraphVariableImplInt(name, n)) {
						out += std::format("{}={}i ", args[i], n);
					} else if (holder->GetGraphVariableImplBool(name, b)) {
						out += std::format("{}={}b ", args[i], b);
					} else {
						out += std::format("{}=? ", args[i]);
					}
				}
				return out;
			}

			if (args.size() < 4) {
				return "error: missing value";
			}
			const RE::BSFixedString name{ args[2] };
			bool                    ok = false;
			if (args[1] == "setf") {
				ok = holder->SetGraphVariableFloat(name, ParseFloat(args[3]).value_or(0.0f));
			} else if (args[1] == "setb") {
				ok = holder->SetGraphVariableBool(name, args[3] == "1");
			} else if (args[1] == "seti") {
				ok = holder->SetGraphVariableInt(name, static_cast<int>(ParseFloat(args[3]).value_or(0.0f)));
			}
			return ok ? "ok" : "error: set failed";
		}

		// event <refHex> <animEvent>: sends an animation graph event (e.g. a locomotion start).
		std::string AnimEvent(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.size() < 2 ? nullptr : LookupRef(args[0]);
			if (!ref) {
				return "error: usage: event <refHex> <eventName>";
			}
			const auto holder = static_cast<RE::IAnimationGraphManagerHolder*>(ref);
			return holder->NotifyAnimationGraphImpl(RE::BSFixedString{ args[1] }) ? "ok" : "rejected";
		}

		// animlog on|off [actorHex]: records the player's (and that actor's) animation events; returns what was recorded so far.
		std::string AnimLog(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() > 1) {
				const auto ref = LookupRef(args[1]);
				const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				WeaponFire::LogActor(actor);
			}
			return WeaponFire::LogAnimationEvents(!args.empty() && args[0] == "on");
		}

		// party list|pick|go|ping: what the multiplayer hotkeys do.
		std::string PartyCommand(std::string_view a_args)
		{
			if (a_args == "list") {
				Party::ShowPlayerList();
			} else if (a_args == "pick") {
				Party::PickTeleportTarget();
			} else if (a_args == "go") {
				Party::TeleportToTarget();
			} else if (a_args == "ping") {
				Party::SendPing();
			} else if (!a_args.empty()) {
				return "error: usage: party [list|pick|go|ping]";
			}
			return Party::Describe();
		}

		// forms <typeNumber> [text]: forms of a type (ENUM_FORM_ID) whose editor ID contains the text.
		std::string Forms(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			int        type = -1;
			if (args.empty() || std::from_chars(args[0].data(), args[0].data() + args[0].size(), type).ec != std::errc{}) {
				return "error: usage: forms <typeNumber> [text]";
			}
			std::string out;
			int         count = 0;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "none";
			}
			for (const auto& [id, form] : *map) {
				if (!form || static_cast<int>(form->GetFormType()) != type || count >= 40) {
					continue;
				}
				const std::string_view editorId = form->GetFormEditorID();
				if (args.size() > 1 && editorId.find(args[1]) == std::string_view::npos) {
					continue;
				}
				out += std::format("{:08X}:{} ", id, editorId);
				++count;
			}
			return out.empty() ? "none" : out;
		}

		// edid <formHex>: a form's editor ID (most forms have none at runtime).
		std::string EditorId(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			const auto form = id ? RE::TESForm::GetFormByID(*id) : nullptr;
			return form ? std::format("'{}' type={}", form->GetFormEditorID(), static_cast<int>(form->GetFormType())) : "error: no such form";
		}

		// net [connect <address>]
		std::string Net(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() == 2 && args[0] == "connect") {
				Session::ConnectTo(std::string(args[1]));
				return "ok";
			}
			return Session::Describe();
		}

		// steam [invite | join <lobby or Steam ID> | selftest send|recv]
		std::string SteamCommand(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.empty()) {
				return Steam::Describe();
			}
			if (args[0] == "invite") {
				return Steam::OpenInviteDialog() ? "ok" : "error: no lobby";
			}
			if (args[0] == "join" && args.size() > 1) {
				std::uint64_t id = 0;
				std::from_chars(args[1].data(), args[1].data() + args[1].size(), id);
				Steam::Join(id);
				return "ok";
			}
			if (args[0] == "selftest" && args.size() > 1) {
				return Steam::SelfTest(args[1] == "send");
			}
			return "error: usage: steam [invite | join <id> | selftest send|recv]";
		}

		// echo on|off [dx dy]: a puppet mirrors the local player, offset by (dx, dy).
		std::string Echo(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.empty() || (args[0] != "on" && args[0] != "off")) {
				return "error: usage: echo on|off [dx dy]";
			}
			const float dx = args.size() > 1 ? ParseFloat(args[1]).value_or(150.0f) : 150.0f;
			const float dy = args.size() > 2 ? ParseFloat(args[2]).value_or(0.0f) : 0.0f;
			Session::SetEcho(args[0] == "on", dx, dy);
			return "ok";
		}

		struct Entry
		{
			std::string_view name;
			Handler          handler;
		};

		constexpr std::array COMMANDS{
			Entry{ "status", Status },
			Entry{ "pos", Pos },
			Entry{ "console", Console },
			Entry{ "findnpc", FindNpc },
			Entry{ "findref", FindRef },
			Entry{ "spawn", Spawn },
			Entry{ "safenpc", SafeNpc },
			Entry{ "actors", Actors },
			Entry{ "containers", Containers },
			Entry{ "count", Count },
			Entry{ "items", Items },
			Entry{ "doors", Doors },
			Entry{ "draw", Draw },
			Entry{ "quests", Quests },
			Entry{ "combat", Combat },
			Entry{ "equipped", Equipped },
			Entry{ "findgear", FindGear },
			Entry{ "refinfo", RefInfo },
			Entry{ "setpos", SetPos },
			Entry{ "puppet", Puppet },
			Entry{ "papyrus", CallPapyrus },
			Entry{ "flags", Flags },
			Entry{ "net", Net },
			Entry{ "echo", Echo },
			Entry{ "steam", SteamCommand },
			Entry{ "animlog", AnimLog },
			Entry{ "party", PartyCommand },
			Entry{ "edid", EditorId },
			Entry{ "forms", Forms },
			Entry{ "graph", Graph },
			Entry{ "event", AnimEvent },
			Entry{ "remove", Remove },
		};
	}

	Handler Find(std::string_view a_name)
	{
		for (const auto& entry : COMMANDS) {
			if (entry.name == a_name) {
				return entry.handler;
			}
		}
		return nullptr;
	}

	std::string Help()
	{
		std::string out = "commands: ping, help";
		for (const auto& entry : COMMANDS) {
			out += ", ";
			out += entry.name;
		}
		return out;
	}
}
