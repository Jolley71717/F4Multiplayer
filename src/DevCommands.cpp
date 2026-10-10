#include "DevCommands.h"

#include "game/Equipment.h"
#include "game/Face.h"
#include "game/FrameStats.h"
#include "game/NpcSync.h"
#include "game/Papyrus.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "game/QuestSync.h"
#include "game/Party.h"
#include "game/Voice.h"
#include "game/WeaponFire.h"
#include "game/WorkshopSync.h"
#include "net/Session.h"

#include <numbers>
#include "steam/Steam.h"

// user32 (windows.h clashes with CommonLibF4's names).
extern "C" {
__declspec(dllimport) void __stdcall keybd_event(unsigned char a_vk, unsigned char a_scan, unsigned long a_flags, unsigned long long a_extra);
__declspec(dllimport) unsigned int __stdcall MapVirtualKeyA(unsigned int a_code, unsigned int a_mapType);
__declspec(dllimport) void* __stdcall FindWindowA(const char* a_class, const char* a_title);
__declspec(dllimport) int __stdcall ShowWindow(void* a_window, int a_show);
__declspec(dllimport) int __stdcall SetForegroundWindow(void* a_window);
__declspec(dllimport) void* __stdcall GetForegroundWindow();
__declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void* a_window, unsigned long* a_processId);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
__declspec(dllimport) int __stdcall AttachThreadInput(unsigned long a_attach, unsigned long a_to, int a_flag);
__declspec(dllimport) int __stdcall BringWindowToTop(void* a_window);
__declspec(dllimport) void __stdcall mouse_event(unsigned long a_flags, unsigned long a_dx, unsigned long a_dy, unsigned long a_data, unsigned long long a_extra);
}

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
			       std::format(" hp={:.0f} dead={}", health, actor->IsDead(false)) + std::format(" vel=({:.0f},{:.0f},{:.0f}) moveMode={:04X} sneaking={} 3dWorld=({:.0f},{:.0f},{:.0f})",
				velocity.x, velocity.y, velocity.z, static_cast<std::uint32_t>(static_cast<const RE::ActorState&>(*actor).moveMode), actor->IsSneaking(), w.x, w.y, w.z);
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
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto location = player ? player->currentLocation : nullptr;
			return std::format("ingame={} cell={:08X} interior={} worldspace={:08X} location={:08X} '{}'", cell != nullptr, cell ? cell->GetFormID() : 0,
				cell && cell->IsInterior(), cell && cell->worldSpace ? cell->worldSpace->GetFormID() : 0, location ? location->GetFormID() : 0,
				location && location->GetFullName() ? location->GetFullName() : "");
		}

		// frames: frame times since the last call (mean, worst, hitches over 100 ms).
		std::string Frames(std::string_view)
		{
			return FrameStats::Global().Take().Describe();
		}

		std::string Pos(std::string_view)
		{
			if (!PlayerCell()) {
				return "error: not in game";
			}
			return DescribeRef(RE::PlayerCharacter::GetSingleton());
		}

		// turnto <refHex> <ms> [pitchDeg]: turns the player to face a reference over a time, like a hand on
		// the mouse would, instead of snapping (a snap is a smeared frame on video). Stepped by Frame().
		struct Turn
		{
			float startYaw = 0.0f, endYaw = 0.0f, startPitch = 0.0f, endPitch = 0.0f;
			std::chrono::steady_clock::time_point start{}, end{};
			bool active = false;
		};
		Turn turn;

		std::string TurnTo(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto ref = args.empty() ? nullptr : LookupRef(args[0]);
			if (!player || !ref || args.size() < 2) {
				return "error: usage: turnto <refHex> <ms> [pitchDeg]";
			}
			const auto& me = player->data.location;
			const auto& to = ref->data.location;
			constexpr float DEG = 180.0f / std::numbers::pi_v<float>;
			float yaw = std::atan2(to.x - me.x, to.y - me.y) * DEG;
			if (yaw < 0.0f) {
				yaw += 360.0f;
			}
			const float startYaw = player->data.angle.z * DEG;
			// Shortest way round.
			float delta = yaw - startYaw;
			while (delta > 180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;
			turn.startYaw = startYaw;
			turn.endYaw = startYaw + delta;
			turn.startPitch = player->data.angle.x * DEG;
			// Pitch: given in degrees, or toward the target's chest from the player's eyes (down is positive).
			if (args.size() > 2) {
				turn.endPitch = std::stof(std::string{ args[2] });
			} else {
				const float dz = (to.z + 60.0f) - (me.z + 120.0f);  // measured: shots at a standing raider 240 away land from 6 to 18 degrees down, auto was 5
				const float flat = std::hypot(to.x - me.x, to.y - me.y);
				turn.endPitch = -std::atan2(dz, (std::max)(flat, 1.0f)) * DEG;
			}
			turn.start = std::chrono::steady_clock::now();
			turn.end = turn.start + std::chrono::milliseconds(std::stoi(std::string{ args[1] }));
			turn.active = true;
			return std::format("turning {:.1f} -> {:.1f} over {} ms", startYaw, turn.endYaw, args[1]);
		}

		// key <vk> <holdMs>: presses a key inside the game process (down now, up after holdMs, stepped by
		// Frame()), so two games can be driven at the same instant from a script; scheduled tasks on the
		// VM used to fire seconds late. focus: brings the game window to the front (a tapped Alt lifts
		// Windows' foreground lock), so a desktop capture shows the game and not whatever popped over it.

		void KeyEvent(std::uint8_t a_vk, bool a_down)
		{
			const auto scan = static_cast<std::uint8_t>(MapVirtualKeyA(a_vk, 0));
			keybd_event(a_vk, scan, (a_down ? 0u : 2u) | 8u, 0);  // KEYEVENTF_SCANCODE (| KEYEVENTF_KEYUP)
		}

		// A tapped Alt lifts Windows' foreground lock for the next SetForegroundWindow. (AttachThreadInput
		// would be stronger, but called on the game thread it hung the game.)
		bool BringGameToFront()
		{
			const auto window = FindWindowA(nullptr, "Fallout4");
			if (!window) {
				return false;
			}
			if (GetForegroundWindow() == window) {
				return true;
			}
			KeyEvent(0x12, true);
			KeyEvent(0x12, false);
			ShowWindow(window, 9);  // SW_RESTORE
			BringWindowToTop(window);
			const bool ok = SetForegroundWindow(window) != 0;
			return ok && GetForegroundWindow() == window;
		}

		std::string KeyPress(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto vk = args.empty() ? std::nullopt : ParseHex(args[0]);
			if (!vk || *vk == 0 || *vk > 255 || args.size() < 2) {
				return "error: usage: key <vkHex> <holdMs>";
			}
			// Window and input calls run on their own thread: on the game thread they stalled the game
			// (and with it the dev channel) on the VM.
			const auto key = static_cast<std::uint8_t>(*vk);
			const auto hold = std::stoi(std::string{ args[1] });
			std::thread([key, hold] {
				BringGameToFront();
				KeyEvent(key, true);
				std::this_thread::sleep_for(std::chrono::milliseconds(hold));
				KeyEvent(key, false);
			}).detach();
			return std::format("pressing {:02X} for {} ms", *vk, args[1]);
		}

		// mouse <dx> <dy>: moves the mouse by that much (relative units), to turn the camera; with the
		// free camera (tfc) this is the only way to aim it from a script.
		std::string Mouse(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() < 2) {
				return "error: usage: mouse <dx> <dy>";
			}
			const auto dx = std::stoi(std::string{ args[0] });
			const auto dy = std::stoi(std::string{ args[1] });
			std::thread([dx, dy] {
				BringGameToFront();
				mouse_event(1u, static_cast<unsigned long>(dx), static_cast<unsigned long>(dy), 0, 0);  // MOUSEEVENTF_MOVE
			}).detach();
			return "moving";
		}

		std::string Focus(std::string_view)
		{
			// Never waited for: the window calls send messages to the game thread, which is the thread
			// running this command (waiting here deadlocked the game on 2026-10-09).
			std::thread([] { BringGameToFront(); }).detach();
			return "requested";
		}

		// pa: the local player's power armor state as the status reports it.
		std::string PowerArmorInfo(std::string_view)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return "error: no player";
			}
			const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
			const auto occupied = middle ? middle->occupiedFurniture.get() : nullptr;
			const auto last = player->lastUsedPowerArmor.get();
			const auto base = occupied ? occupied->GetObjectReference() : nullptr;
			return std::format("engineSaysPA={} extraPA={} occupied={:08X} base={:08X} lastFrame={:08X}", RE::PowerArmor::PlayerInPowerArmor(), player->extraList && player->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor), occupied ? occupied->GetFormID() : 0, base ? base->GetFormID() : 0, last ? last->GetFormID() : 0);
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
					if (!form) {
						continue;
					}
					const auto name = RE::TESFullName::GetFullName(*form);
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

		// quests [text | running:N | started]: story/faction/side quests with a stage set (or whose
		// name contains text); running quests of type N (internal ones too); quests the player has
		// started (an objective shown in the Pip-Boy). started=yes/no says which ones count.
		std::string Quests(std::string_view a_args)
		{
			std::string out;
			int         count = 0;
			std::optional<int> runningType;
			if (a_args.starts_with("running:")) {
				int type = 0;
				std::from_chars(a_args.data() + 8, a_args.data() + a_args.size(), type);
				runningType = type;
			}
			const bool startedOnly = a_args == "started";
			for (const auto quest : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>()) {
				if (!quest || count >= 25) {
					continue;
				}
				const std::string_view name = quest->GetFullName() ? quest->GetFullName() : "";
				const bool started = QuestSync::PlayerStarted(quest);
				if (runningType) {
					if (quest->data.questType != *runningType || !(quest->data.flags & 1)) {
						continue;
					}
				} else if (startedOnly) {
					if (!started) {
						continue;
					}
				} else if (quest->data.questType <= 0 || quest->data.questType == 6 ||
						   (a_args.empty() ? quest->currentStage == 0 : name.find(a_args) == std::string_view::npos)) {
					continue;
				}
				out += std::format("{}{:08X} '{}' type={} stage={} flags={:04X} started={}", count++ ? "; " : "", quest->GetFormID(), name, quest->data.questType,
					quest->currentStage, quest->data.flags, started ? "yes" : "no");
			}
			return count ? out : "none";
		}

		// objectives <questHex>: the quest's stage and objectives (index, state 0 = not shown yet).
		std::string Objectives(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			const auto quest = id ? RE::TESForm::GetFormByID<RE::TESQuest>(*id) : nullptr;
			if (!quest) {
				return "error: usage: objectives <questHex>";
			}
			std::string out = std::format("stage={} flags={:04X}", quest->currentStage, quest->data.flags);
			for (const auto objective : quest->objectives) {
				if (objective) {
					out += std::format("; {} state={} '{}'", objective->index, static_cast<int>(objective->state), objective->displayText.c_str());
				}
			}
			return out;
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
					return Papyrus::CallMethod(a_object, args[1], args[2], true);
				case 1:
					return Papyrus::CallMethod(a_object, args[1], args[2], true, static_cast<bool>(bools[0]));
				default:
					return Papyrus::CallMethod(a_object, args[1], args[2], true, static_cast<bool>(bools[0]), static_cast<bool>(bools[1]));
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

			const auto values = RE::ActorValue::GetSingleton();
			const float health = values && values->health ? static_cast<RE::ActorValueOwner*>(actor)->GetActorValue(*values->health) : -1.0f;
			return std::format("niFlags={:08X} boolFlags={:08X} moreFlags={:08X} lifeState={} health={:.1f} dead={}",
				actor->niFlags.flags, actor->boolFlags.underlying(), actor->moreFlags,
				static_cast<std::uint32_t>(static_cast<RE::ActorState&>(*actor).lifeState), health, actor->IsDead(false));
		}

		// remove <refHex>: disables and deletes a reference we spawned.
		// facemask <bits>: which pieces of a friend's face to apply (see Face::SetParts), then rebuilds the stand-ins.
		std::string FaceMask(std::string_view a_args)
		{
			const auto mask = ParseHex(a_args);
			if (!mask) {
				return "error: usage: facemask <hexBits> (1 head parts, 2 hair, 4 body, 8 sliders, 10 bones, 20 tints, 40 body tint)";
			}
			Face::SetParts(*mask);
			RemotePlayers::RebuildFaces();
			return std::format("faces rebuilt with mask {:X}", *mask);
		}

		// friendlight <hex>: the light form placed at a friend's stand-in while their Pip-Boy light is on.
		std::string FriendLight(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			if (!id) {
				return "error: usage: friendlight <formHex>";
			}
			RemotePlayers::SetLightForm(*id);
			return std::format("friend light form {:08X}", *id);
		}

		// wsreport <refHex>|place <baseHex> [workshopHex] [scrap]: reports a reference (or a new one of that base,
		// put a step in front of the player) as if just placed (or scrapped) in workshop mode.
		std::string WsReport(std::string_view a_args)
		{
			auto args = SplitArgs(a_args);
			RE::TESObjectREFR* ref = nullptr;
			if (args.size() >= 2 && args[0] == "place") {
				const auto baseId = ParseHex(args[1]);
				const auto base = baseId ? RE::TESForm::GetFormByID<RE::TESBoundObject>(*baseId) : nullptr;
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto cell = player ? player->GetParentCell() : nullptr;
				if (!base || !cell) {
					return "error: no such base form, or not in game";
				}
				RE::NEW_REFR_DATA data;
				data.location = player->data.location;
				data.location.x += 120.0f * std::sin(player->data.angle.z);
				data.location.y += 120.0f * std::cos(player->data.angle.z);
				data.object = base;
				data.interior = cell->IsInterior() ? cell : nullptr;
				data.world = cell->IsInterior() ? nullptr : cell->worldSpace;
				data.clearStillLoadingFlag = true;
				ref = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(data).get().get();
				args.erase(args.begin(), args.begin() + 2);
			} else {
				ref = args.empty() ? nullptr : LookupRef(args[0]);
				if (!args.empty()) {
					args.erase(args.begin());
				}
			}
			if (!ref) {
				return "error: usage: wsreport <refHex>|place <baseHex> [workshopHex] [scrap]";
			}
			const auto workshop = !args.empty() && args[0] != "scrap" ? LookupRef(args[0]) : nullptr;
			const bool scrap = !args.empty() && args.back() == "scrap";
			WorkshopSync::Report(ref, workshop, scrap ? Protocol::WorkshopOp::kScrapped : Protocol::WorkshopOp::kPlaced);
			return std::format("reported {:08X} ({})", ref->GetFormID(), scrap ? "scrapped" : "placed");
		}

		// mirror <refHex>: why an NPC is or isn't mirrored here.
		std::string Mirror(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			return id ? NpcSync::DescribeMirror(*id) : "error: usage: mirror <refHex>";
		}

		// motion <refHex>: the engine's movement numbers for an actor (what its locomotion graph is fed).
		std::string MotionInfo(std::string_view a_args)
		{
			const auto ref = LookupRef(a_args);
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			const auto process = actor ? actor->currentProcess : nullptr;
			const auto middle = process ? process->middleHigh : nullptr;
			const auto high = process ? process->high : nullptr;
			if (!middle || !high) {
				return "error: no process data";
			}
			return std::format("desiredSpeed={:.1f} animationSpeed={:.1f} pathCur=({:.0f},{:.0f},{:.0f}) pathDesired=({:.0f},{:.0f},{:.0f}) output=({:.0f},{:.0f},{:.0f})", middle->desiredSpeed, middle->animationSpeed, high->pathingCurrentMovementSpeed.x, high->pathingCurrentMovementSpeed.y, high->pathingCurrentMovementSpeed.z, high->pathingDesiredMovementSpeed.x, high->pathingDesiredMovementSpeed.y, high->pathingDesiredMovementSpeed.z, 0.0f, 0.0f, 0.0f);
		}

		// sneak on|off [actorHex]: crouches or stands the player (or an actor), as the sneak key would.
		std::string Sneak(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.empty() || (args[0] != "on" && args[0] != "off")) {
				return "error: usage: sneak on|off [actorHex]";
			}
			RE::Actor* actor = RE::PlayerCharacter::GetSingleton();
			if (args.size() > 1) {
				const auto ref = LookupRef(args[1]);
				actor = ref ? ref->As<RE::Actor>() : nullptr;
			}
			if (!actor) {
				return "error: no such actor";
			}
			actor->SetSneaking(args[0] == "on");
			return std::format("sneaking={}", actor->IsSneaking());
		}

		// piplight on|off: the player's Pip-Boy light.
		std::string PipLight(std::string_view a_args)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || (a_args != "on" && a_args != "off")) {
				return "error: usage: piplight on|off";
			}
			player->ShowPipboyLight(a_args == "on", false);
			return std::format("pip-boy light {} (on={})", a_args, player->IsPipboyLightOn());
		}

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
			// The text to match is everything after the type, so "Power Armor" works as two words.
			std::string text;
			for (std::size_t i = 1; i < args.size(); ++i) {
				text += (i > 1 ? " " : "") + std::string{ args[i] };
			}
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
				const auto named = form->As<RE::TESFullName>();
				const std::string_view fullName = named && named->GetFullName() ? named->GetFullName() : "";  // most editor IDs are stripped at runtime; the name is not
				if (!text.empty() && editorId.find(text) == std::string_view::npos && fullName.find(text) == std::string_view::npos) {
					continue;
				}
				out += std::format("{:08X}:{}:{} ", id, editorId, fullName);
				++count;
			}
			return out.empty() ? "none" : out;
		}

		// lights [minRadius]: light forms (LIGH) with their radius, color, flags, cone and fade, to pick one.
		std::string Lights(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			int        minRadius = 0;
			if (!args.empty()) {
				std::from_chars(args[0].data(), args[0].data() + args[0].size(), minRadius);
			}
			std::string out;
			int         count = 0;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "none";
			}
			for (const auto& [id, form] : *map) {
				const auto light = form ? form->As<RE::TESObjectLIGH>() : nullptr;
				if (!light || static_cast<int>(light->data.radius) < minRadius || (id >> 24) >= 0x02 || count >= 40) {
					continue;
				}
				out += std::format("{:08X} r={} c={:06X} f={:04X} fov={:.0f} fade={:.1f} | ", id, light->data.radius, light->data.color & 0xFFFFFF, light->data.flags, light->data.fov, light->fade);
				++count;
			}
			return out.empty() ? "none" : out;
		}

		// named <typeNumber> <text>: forms of a type whose name contains the text, with their keywords.
		std::string Named(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			int        type = -1;
			if (args.size() < 2 || std::from_chars(args[0].data(), args[0].data() + args[0].size(), type).ec != std::errc{}) {
				return "error: usage: named <typeNumber> <text>";
			}
			std::string out;
			int         count = 0;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "none";
			}
			for (const auto& [id, form] : *map) {
				if (!form || static_cast<int>(form->GetFormType()) != type || count >= 20) {
					continue;
				}
				const auto name = RE::TESFullName::GetFullName(*form);
				if (name.find(args[1]) == std::string_view::npos) {
					continue;
				}
				out += std::format("{:08X}'{}'", id, name);
				if (const auto keywords = form->As<RE::BGSKeywordForm>()) {
					for (std::uint32_t i = 0; i < keywords->GetNumKeywords(); ++i) {
						const auto keyword = keywords->GetKeywordAt(i).value_or(nullptr);
						out += std::format(" k{:08X}", keyword ? keyword->GetFormID() : 0);
					}
				}
				out += "; ";
				++count;
			}
			return out.empty() ? "none" : out;
		}

		double GfxNumber(const Scaleform::GFx::Value& a_value)
		{
			return a_value.IsNumber() ? a_value.GetNumber() : a_value.IsInt() ? a_value.GetInt() : a_value.IsUInt() ? a_value.GetUInt() : -1.0;
		}

		void DumpGfx(Scaleform::GFx::Value& a_object, const std::string& a_path, int a_depth)
		{
			Scaleform::GFx::Value count;
			if (!a_object.IsDisplayObject() || !a_object.GetMember("numChildren", &count)) {
				return;
			}
			const int children = static_cast<int>(GfxNumber(count));
			for (int i = 0; i < children && i < 60; ++i) {
				Scaleform::GFx::Value child;
				Scaleform::GFx::Value index{ i };
				if (!a_object.Invoke("getChildAt", &child, &index, 1)) {
					continue;
				}
				Scaleform::GFx::Value name, x, y, width, visible;
				child.GetMember("name", &name);
				child.GetMember("x", &x);
				child.GetMember("y", &y);
				child.GetMember("width", &width);
				child.GetMember("visible", &visible);
				const std::string path = a_path + "." + (name.IsString() ? name.GetString() : std::format("#{}", i));
				Scaleform::GFx::Value text, height, color, embed;
				child.GetMember("height", &height);
				std::string extra;
				if (child.GetMember("text", &text) && text.IsString()) {
					child.GetMember("textColor", &color);
					child.GetMember("embedFonts", &embed);
					Scaleform::GFx::Value format, font, size;
					if (child.Invoke("getTextFormat", &format, nullptr, 0)) {
						format.GetMember("font", &font);
						format.GetMember("size", &size);
					}
					extra = std::format(" text='{}' color={:06X} embed={} font='{}' size={}", text.GetString(), static_cast<std::uint32_t>(GfxNumber(color)),
						embed.IsBoolean() && embed.GetBoolean(), font.IsString() ? font.GetString() : "?", GfxNumber(size));
				}
				REX::INFO("gfx: {} x={:.1f} y={:.1f} w={:.1f} h={:.1f} visible={}{}", path, GfxNumber(x), GfxNumber(y), GfxNumber(width), GfxNumber(height),
					visible.IsBoolean() && visible.GetBoolean(), extra);
				if (a_depth > 1) {
					DumpGfx(child, path, a_depth - 1);
				}
			}
		}

		// gfx <menu> <path> [depth]: logs a menu's display objects under path (e.g. HUDMenu root 2).
		std::string Gfx(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() < 2) {
				return "error: usage: gfx <menu> <path> [depth]";
			}
			int depth = 1;
			if (args.size() > 2) {
				std::from_chars(args[2].data(), args[2].data() + args[2].size(), depth);
			}
			F4SE::GetTaskInterface()->AddUITask([menuName = std::string(args[0]), path = std::string(args[1]), depth]() {
				const auto ui = RE::UI::GetSingleton();
				const auto menu = ui ? ui->GetMenu(menuName) : nullptr;
				const auto movie = menu ? menu->uiMovie.get() : nullptr;
				if (!movie) {
					REX::INFO("gfx: no movie for {}", menuName);
					return;
				}
				Scaleform::GFx::Value object;
				if (!movie->GetVariable(&object, path.c_str())) {
					REX::INFO("gfx: no {}", path);
					return;
				}
				REX::INFO("gfx: {} type={}", path, static_cast<int>(object.GetType()));
				DumpGfx(object, path, depth);
			});
			return "queued (see the log)";
		}

		// gfxset <menu> <path> <member> <value>: sets a property (a number, true/false, or text).
		std::string GfxSet(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() < 4) {
				return "error: usage: gfxset <menu> <path> <member> <value>";
			}
			F4SE::GetTaskInterface()->AddUITask([menuName = std::string(args[0]), path = std::string(args[1]), member = std::string(args[2]), text = std::string(args[3])]() {
				const auto ui = RE::UI::GetSingleton();
				const auto menu = ui ? ui->GetMenu(menuName) : nullptr;
				const auto movie = menu ? menu->uiMovie.get() : nullptr;
				Scaleform::GFx::Value object;
				if (!movie || !movie->GetVariable(&object, path.c_str())) {
					REX::INFO("gfxset: no {}", path);
					return;
				}
				Scaleform::GFx::Value value;
				double number = 0.0;
				if (text == "true" || text == "false") {
					value = Scaleform::GFx::Value(text == "true");
				} else if (std::from_chars(text.data(), text.data() + text.size(), number).ec == std::errc{}) {
					value = Scaleform::GFx::Value(number);
				} else {
					value = Scaleform::GFx::Value(text.c_str());
				}
				REX::INFO("gfxset: {}.{} = {} -> {}", path, member, text, object.SetMember(member, value));
			});
			return "queued (see the log)";
		}

		// weapsound <actorHex>: the sound fields of the actor's equipped weapon (instance and base).
		std::string WeapSound(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			const auto actor = id ? RE::TESForm::GetFormByID<RE::Actor>(*id) : nullptr;
			const auto process = actor ? actor->currentProcess : nullptr;
			const auto middle = process ? process->middleHigh : nullptr;
			if (!middle) {
				return "error: usage: weapsound <actorHex> (loaded actor)";
			}
			const auto sound = [](const RE::BGSSoundDescriptorForm* a_form) { return a_form ? a_form->GetFormID() : 0; };
			std::string out;
			RE::BSAutoLock l{ middle->equippedItemsLock };
			for (const auto& equipped : middle->equippedItems) {
				const auto weapon = equipped.item.object ? equipped.item.object->As<RE::TESObjectWEAP>() : nullptr;
				if (!weapon) {
					continue;
				}
				const auto instance = static_cast<RE::TESObjectWEAP::InstanceData*>(equipped.item.instanceData.get());
				const auto& base = weapon->weaponData;
				out += std::format("weap={:08X} base: attack={:08X} 2d={:08X} loop={:08X} fail={:08X}", weapon->GetFormID(), sound(base.attackSound), sound(base.attackSound2D),
					sound(base.attackLoop), sound(base.attackFailSound));
				if (const auto data = static_cast<RE::EquippedWeaponData*>(equipped.data.get())) {
					const auto mapping = data->attackSoundData;
					out += std::format(" equipped: kssm={:08X} descriptor={:08X} tail={:08X} vats={:08X} handle={:X}", mapping ? mapping->GetFormID() : 0,
						sound(mapping ? mapping->descriptor : nullptr), sound(mapping ? mapping->exteriorTail : nullptr), sound(mapping ? mapping->vatsDescriptor : nullptr),
						data->attackSound.soundID);
				}
				if (instance) {
					out += std::format(" instance: attack={:08X} 2d={:08X} loop={:08X} fail={:08X} keywords:", sound(instance->attackSound), sound(instance->attackSound2D),
						sound(instance->attackLoop), sound(instance->attackFailSound));
					if (instance->keywords) {
						for (std::uint32_t i = 0; i < instance->keywords->GetNumKeywords(); ++i) {
							const auto keyword = instance->keywords->GetKeywordAt(i).value_or(nullptr);
							out += std::format(" {:08X}", keyword ? keyword->GetFormID() : 0);
						}
					}
				}
				out += "; ";
			}
			return out.empty() ? "no weapon" : out;
		}

		// playsound <soundHex> [flagsHex] [refHex]: plays a sound descriptor at a reference (default
		// the player) through the audio manager, with the given usage flags (default 10).
		std::string PlaySoundCmd(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto soundId = args.empty() ? std::nullopt : ParseHex(args[0]);
			const auto sound = soundId ? RE::TESForm::GetFormByID<RE::BGSSoundDescriptorForm>(*soundId) : nullptr;
			const auto flags = args.size() > 1 ? ParseHex(args[1]).value_or(0x10) : 0x10;
			const auto refId = args.size() > 2 ? ParseHex(args[2]) : std::nullopt;
			RE::TESObjectREFR* ref = refId ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(*refId) : RE::PlayerCharacter::GetSingleton();
			const auto audio = RE::BSAudioManager::GetSingleton();
			if (!sound || !ref || !audio) {
				return "error: usage: playsound <soundHex> [flagsHex] [refHex]";
			}
			RE::BSSoundHandle handle;
			if (!audio->GetSoundHandle(handle, sound, 0.0f, flags)) {
				return "no handle";
			}
			handle.SetPosition(ref->data.location);
			if (const auto root = ref->Get3D()) {
				handle.SetObjectToFollow(root);
			}
			const bool played = handle.Play();
			return std::format("handle={:X} played={}", handle.soundID, played);
		}

		// gs <text>: game settings whose name contains the text (case-sensitive), with their values.
		std::string GameSettings(std::string_view a_args)
		{
			const auto collection = RE::GameSettingCollection::GetSingleton();
			if (!collection || a_args.empty()) {
				return "error: usage: gs <text>";
			}
			std::vector<std::string> found;
			for (const auto& [key, setting] : collection->settings) {
				if (!setting) {
					continue;
				}
				const auto name = setting->GetKey();
				if (name.find(a_args) == std::string_view::npos) {
					continue;
				}
				switch (setting->GetType()) {
				case RE::Setting::SETTING_TYPE::kFloat:
					found.push_back(std::format("{}={}", name, setting->GetFloat()));
					break;
				case RE::Setting::SETTING_TYPE::kInt:
					found.push_back(std::format("{}={}", name, setting->GetInt()));
					break;
				case RE::Setting::SETTING_TYPE::kBinary:
					found.push_back(std::format("{}={}", name, setting->GetBinary()));
					break;
				default:
					found.push_back(std::string(name));
					break;
				}
			}
			std::ranges::sort(found);
			std::string out;
			for (const auto& entry : found | std::views::take(60)) {
				out += entry + "; ";
			}
			return out.empty() ? "none" : out;
		}

		// idles <text>: idle animations whose editor ID, event or file contains the text.
		std::string Idles(std::string_view a_args)
		{
			if (a_args.empty()) {
				return "error: usage: idles <text>";
			}
			std::string out;
			int         count = 0;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "none";
			}
			const auto text = [](const RE::BSFixedString& a_str) { return std::string_view{ a_str.c_str() ? a_str.c_str() : "" }; };
			for (const auto& [id, form] : *map) {
				const auto idle = form ? form->As<RE::TESIdleForm>() : nullptr;
				if (!idle || count >= 25) {
					continue;
				}
				const std::string_view editorId = idle->formEditorID.c_str() ? idle->formEditorID.c_str() : "";
				const auto event = text(idle->animEventName);
				const auto file = text(idle->animFileName);
				if (editorId.find(a_args) == std::string_view::npos && event.find(a_args) == std::string_view::npos && file.find(a_args) == std::string_view::npos) {
					continue;
				}
				out += std::format("{:08X} {} ev={} file={} graph={}; ", id, editorId, event, file, text(idle->behaviorGraphName));
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

		// topics <npcBaseHex>: topics with lines conditioned on that speaker (for `say`).
		std::string Topics(std::string_view a_args)
		{
			const auto parsed = ParseHex(a_args);
			if (!parsed) {
				return "error: usage: topics <npcBaseHex>";
			}
			const auto id = *parsed;
			std::set<std::uint32_t> topics;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "none";
			}
			for (const auto& [formId, form] : *map) {
				const auto info = form ? form->As<RE::TESTopicInfo>() : nullptr;
				if (info && info->parentTopic && topics.size() < 20) {
					const auto speaker = info->GetSpeaker();
					if (speaker && speaker->GetFormID() == id) {
						topics.insert(info->parentTopic->GetFormID());
					}
				}
			}
			std::string out;
			for (const auto topic : topics) {
				out += std::format("{:08X} ", topic);
			}
			return out.empty() ? "none" : out;
		}

		// say <refHex> <topicHex>: the actor says a line from the topic (the console's Say; the Papyrus route crashes).
		std::string Say(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			const auto ref = args.size() == 2 ? LookupRef(args[0]) : nullptr;
			const auto topicId = args.size() == 2 ? ParseHex(args[1]) : std::nullopt;
			const auto topic = ref && topicId ? RE::TESForm::GetFormByID<RE::TESTopic>(*topicId) : nullptr;
			if (!topic) {
				return "error: usage: say <refHex> <topicHex>";
			}
			RE::Console::ExecuteCommand(std::format("{:08X}.say {:08X}", ref->GetFormID(), topic->GetFormID()).c_str());
			return "dispatched";
		}

		// npcvoice <actorHex>: what the game knows about the actor's current line.
		std::string NpcVoice(std::string_view a_args)
		{
			const auto id = ParseHex(a_args);
			const auto actor = id ? RE::TESForm::GetFormByID<RE::Actor>(*id) : nullptr;
			if (!actor) {
				return "error: usage: npcvoice <actorHex>";
			}
			const auto process = actor->currentProcess;
			const auto high = process ? process->high : nullptr;
			if (!high) {
				return std::format("talking={} voiceTimer={:.2f} high=none", actor->IsTalking(), actor->voiceTimer);
			}
			const char* text = high->strVoiceSubtitle.c_str();
			return std::format("talking={} voiceTimer={:.2f} state={} elapsed={:.2f} hpTimer={:.2f} subtitle='{}' lastGreeting={:08X} greetingTopic={:08X} sound={:X}",
				actor->IsTalking(), actor->voiceTimer, static_cast<int>(high->voiceState.get()), high->voiceTimeElapsed, high->voiceTimer, text ? text : "",
				high->lastGreeting ? high->lastGreeting->GetFormID() : 0, high->lastGreeting && high->lastGreeting->parentTopic ? high->lastGreeting->parentTopic->GetFormID() : 0, high->soundHandle[0].soundID);
		}

		// menu <name> [hide|force]: opens (or closes) a menu, e.g. PauseMenu, PipboyMenu (force: for the Pip-Boy).
		std::string Menu(std::string_view a_args)
		{
			const auto space = a_args.find(' ');
			const std::string name{ a_args.substr(0, space) };
			const auto how = space != std::string_view::npos ? a_args.substr(space + 1) : std::string_view{};
			const auto queue = RE::UIMessageQueue::GetSingleton();
			if (name.empty() || !queue) {
				return "error: usage: menu <name> [hide|force]";
			}
			queue->AddMessage(name, how == "force" ? RE::UI_MESSAGE_TYPE::kForceHide : how == "hide" ? RE::UI_MESSAGE_TYPE::kHide : RE::UI_MESSAGE_TYPE::kShow);
			return "queued";
		}

		// paused: whether the game is paused, and by what.
		std::string Paused(std::string_view)
		{
			const auto main = RE::Main::GetSingleton();
			const auto ui = RE::UI::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			return std::format("freezeTime={} gameActive={} menuMode={} freezeFramePause={} pauseMenu={} pipboy={} difficulty={} sit={} scene={}", main ? main->freezeTime : false, main ? main->gameActive : false,
				ui ? ui->menuMode : 0, ui ? ui->freezeFramePause : 0, ui && ui->GetMenuOpen("PauseMenu"sv), ui && ui->GetMenuOpen("PipboyMenu"sv), player ? static_cast<int>(player->GetDifficultyLevel()) : -1,
				player ? static_cast<int>(player->DoGetSitSleepState()) : -1, player && player->GetCurrentScene());
		}

		// dialogue: the conversation the player is in, and whether the speaker is saying something.
		std::string Dialogue(std::string_view)
		{
			const auto topics = RE::MenuTopicManager::GetSingleton();
			if (!topics) {
				return "none";
			}
			const auto speaker = topics->speaker.get();
			const auto actor = speaker ? speaker->As<RE::Actor>() : nullptr;
			const auto last = topics->lastSpeaker.get();
			const auto ui = RE::UI::GetSingleton();
			const bool dialogueMenu = ui && ui->GetMenuOpen("DialogueMenu"sv);
			return std::format("last={:08X} dialogueMenu={} menuOpen={} allowInput={} speaker={:08X} talking={} voiceTimer={:.2f} inScene={}",
				last ? last->GetFormID() : 0, dialogueMenu, topics->menuOpen, topics->allowInput, speaker ? speaker->GetFormID() : 0, speaker ? speaker->IsTalking() : false,
				actor ? actor->voiceTimer : -1.0f, topics->overSceneActor);
		}

		// markers [text]: map markers (name from their data) with the first bytes of their data.
		std::string Markers(std::string_view a_args)
		{
			std::string result;
			int         count = 0;
			int         total = 0;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return "none";
			}
			for (const auto& [id, form] : *map) {
				const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
				const auto extra = ref && ref->extraList ? ref->extraList->GetByType<RE::ExtraMapMarker>() : nullptr;
				if (!extra || !extra->mapMarkerData) {
					continue;
				}
				++total;
				const auto bytes = reinterpret_cast<const std::uint8_t*>(extra->mapMarkerData);
				const std::string_view markerName = reinterpret_cast<const RE::BSFixedString*>(bytes + 8)->c_str();
				if (!a_args.empty() && markerName.find(a_args) == std::string_view::npos) {
					continue;
				}
				if (count >= 12) {
					continue;
				}
				std::string hex;
				for (int i = 0x10; i < 0x14; ++i) {
					hex += std::format("{:02X}{}", bytes[i], (i % 4 == 3) ? " " : "");
				}
				result += std::format("{}{:08X} '{}' [{}]", count++ ? "; " : "", id, markerName, hex);
			}
			return std::format("{} markers: {}", total, result);
		}

		// voice [talk|loop on|off]: record without the key, or hear our own voice.
		std::string VoiceCommand(std::string_view a_args)
		{
			const auto args = SplitArgs(a_args);
			if (args.size() == 2 && (args[0] == "talk" || args[0] == "loop")) {
				(args[0] == "talk" ? Voice::SetForceTalk : Voice::SetLoopback)(args[1] == "on");
			} else if (!args.empty()) {
				return "error: usage: voice [talk|loop on|off]";
			}
			return Voice::Describe();
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
			Entry{ "frames", Frames },
			Entry{ "pos", Pos },
			Entry{ "console", Console },
			Entry{ "turnto", TurnTo },
			Entry{ "key", KeyPress },
			Entry{ "focus", Focus },
			Entry{ "mouse", Mouse },
			Entry{ "pa", PowerArmorInfo },
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
			Entry{ "objectives", Objectives },
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
			Entry{ "voice", VoiceCommand },
			Entry{ "markers", Markers },
			Entry{ "dialogue", Dialogue },
			Entry{ "topics", Topics },
			Entry{ "say", Say },
			Entry{ "npcvoice", NpcVoice },
			Entry{ "menu", Menu },
			Entry{ "paused", Paused },
			Entry{ "idles", Idles },
			Entry{ "steam", SteamCommand },
			Entry{ "animlog", AnimLog },
			Entry{ "party", PartyCommand },
			Entry{ "edid", EditorId },
			Entry{ "forms", Forms },
			Entry{ "named", Named },
			Entry{ "lights", Lights },
			Entry{ "gfx", Gfx },
			Entry{ "gfxset", GfxSet },
			Entry{ "weapsound", WeapSound },
			Entry{ "playsound", PlaySoundCmd },
			Entry{ "gs", GameSettings },
			Entry{ "graph", Graph },
			Entry{ "event", AnimEvent },
			Entry{ "remove", Remove },
			Entry{ "facemask", FaceMask },
			Entry{ "friendlight", FriendLight },
			Entry{ "piplight", PipLight },
			Entry{ "sneak", Sneak },
			Entry{ "motion", MotionInfo },
			Entry{ "mirror", Mirror },
			Entry{ "wsreport", WsReport },
		};
	}

	void Frame()
	{
		if (!turn.active) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		float t = turn.end > turn.start ? std::chrono::duration<float>(now - turn.start).count() / std::chrono::duration<float>(turn.end - turn.start).count() : 1.0f;
		t = std::clamp(t, 0.0f, 1.0f);
		const float eased = t * t * (3.0f - 2.0f * t);  // smoothstep: slow start, slow stop
		float yaw = turn.startYaw + (turn.endYaw - turn.startYaw) * eased;
		while (yaw < 0.0f) yaw += 360.0f;
		while (yaw >= 360.0f) yaw -= 360.0f;
		const float pitch = turn.startPitch + (turn.endPitch - turn.startPitch) * eased;
		RE::Console::ExecuteCommand(std::format("player.setangle z {:.2f}", yaw).c_str());
		RE::Console::ExecuteCommand(std::format("player.setangle x {:.2f}", pitch).c_str());
		if (t >= 1.0f) {
			turn.active = false;
		}
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
