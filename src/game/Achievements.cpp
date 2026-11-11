#include "game/Achievements.h"

#include "game/AchievementGate.h"
#include "game/WorldSync.h"
#include "steam/SteamApi.h"

#include <array>
#include <atomic>
#include <bitset>
#include <mutex>

namespace Achievements
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The game names its Steam achievements NEW_ACHIEVEMENT_<n>_0 (the format string is in Fallout4.exe).
		// The game and its add-ons have fewer than this; the log says how many Steam knows.
		constexpr int MAX_ID = 100;
		using Unlocked = std::bitset<MAX_ID + 1>;

		// A stage's script runs a moment after the stage is set. Steam is asked this long after the last
		// stage of a run, and no later than CHECK_BY after its first.
		constexpr auto CHECK_AFTER = 3s;
		constexpr auto CHECK_BY = 10s;

		Unlocked                         unlocked;
		bool                             known = false;  // unlocked holds what Steam said
		bool                             loadCheck = false;
		std::optional<bool>              eligible;
		std::optional<Clock::time_point> checkAt;
		std::optional<Clock::time_point> checkBy;
		std::uint32_t                    lastQuest = 0;
		std::uint16_t                    lastStage = 0;
		std::uint32_t                    stagesSet = 0;
		std::uint32_t                    checks = 0;
		std::uint32_t                    newlyUnlocked = 0;  // seen unlocking while this game runs

		// --- the hook on Game.AddAchievement --------------------------------------------------------------------
		// The VM holds one function object per native, bound at start-up (GameVM's BindNativeMethod calls, the
		// engine's own NativeFunction<> template, CommonLibF4's NF_util::NativeFunctionBase) and attached to the
		// script type that declares it once that type is loaded and linked. The engine calls a native through its
		// object's vtable: NativeFunctionBase::Call (engine code) then the virtual MarshallAndDispatch, slot 0x16
		// (CommonLibF4 plugins' own natives are called through the same slots, so the layout holds). The one object
		// for Game.AddAchievement gets its vtable pointer pointed at a copy of its vtable whose slot 0x16 is Dispatched
		// below; nothing else changes, no code is written, and every other native keeps the engine's vtable. The
		// object is found by name in the "Game" type's global functions, which checks the layout as it goes: the
		// virtual getters must say native, static, one parameter, "Game", "AddAchievement".
		using RE::BSScript::NF_util::NativeFunctionBase;
		using Dispatch = bool (*)(const NativeFunctionBase*, RE::BSScript::Variable&, RE::BSScript::Internal::VirtualMachine&, std::uint32_t, RE::BSScript::Variable&,
			const RE::BSScript::StackFrame&);

		constexpr std::size_t DISPATCH_SLOT = 0x16;  // NativeFunctionBase::MarshallAndDispatch
		constexpr std::size_t SLOTS = 0x17;          // IFunction's 0x15, HasStub, MarshallAndDispatch: all the engine calls on one
		constexpr auto        HOOK_RETRY = 5s;       // the "Game" type loads with the first script that names it
		constexpr int         HOOK_TRIES = 24;       // ... two minutes in the world without it: given up, with a warning

		enum class Hook
		{
			kNone,
			kHooked,
			kGivenUp
		};

		// [0] the vtable's RTTI locator (the pointer before the first slot), the slots from [1]: an object whose vptr is
		// &vtableCopy[1] reads as before for everything but DISPATCH_SLOT.
		std::array<std::uintptr_t, SLOTS + 1> vtableCopy{};
		const std::uintptr_t*                 originalVtable = nullptr;
		Dispatch                              original = nullptr;
		std::vector<RE::BSTSmartPointer<RE::BSScript::IFunction>> held;  // the hooked objects, kept alive: a vptr of ours must never be on freed memory
		Hook                                  hookState = Hook::kNone;
		std::optional<Clock::time_point>      hookTriedAt;
		int                                   hookTries = 0;
		std::string                           hookWhyNot;  // the last reason the hook wasn't made, logged when it changes
		bool                                  tasklet = false;  // the native runs on the VM's threads, not the main one

		std::mutex              marksLock;
		AchievementGate::Marks  marks;  // under marksLock: the hooked call runs on the VM's thread
		std::atomic<std::uint32_t> dropped{ 0 };
		std::atomic<std::uint32_t> passed{ 0 };
		std::atomic<std::uint32_t> marked{ 0 };

		std::string_view Text(const RE::BSFixedString& a_string)
		{
			return a_string.c_str() ? std::string_view{ a_string.c_str() } : std::string_view{};
		}

		// The achievement id the call passes (the native's first and only parameter, its frame's variable 0), or -1.
		int IdOf(const RE::BSScript::StackFrame& a_frame)
		{
			const auto stack = a_frame.parent;
			if (!stack || a_frame.size < 1) {
				return -1;
			}
			const auto  page = stack->GetPageForFrame(&a_frame);
			const auto& var = stack->GetStackFrameVariable(&a_frame, 0, page);
			return var.is<std::int32_t>() ? RE::BSScript::get<std::int32_t>(var) : -1;
		}

		// Slot DISPATCH_SLOT of the hooked object: the game's call, unless it was made from the stage fragment of a
		// caught-up stage. On the VM's thread when the native is tasklet-callable, else the main thread; only reads
		// the frames of the stack that is running this call.
		bool Dispatched(const NativeFunctionBase* a_self, RE::BSScript::Variable& a_this, RE::BSScript::Internal::VirtualMachine& a_vm, std::uint32_t a_stackId,
			RE::BSScript::Variable& a_ret, const RE::BSScript::StackFrame& a_frame)
		{
			const int                          id = IdOf(a_frame);
			std::string                        caller;
			std::optional<AchievementGate::Key> key;
			const RE::BSScript::StackFrame*    frame = a_frame.previousFrame;
			for (std::size_t n = 0; n < AchievementGate::FRAMES && frame; ++n, frame = frame->previousFrame) {
				const auto function = frame->owningFunction.get();
				if (!function) {
					break;
				}
				const auto script = Text(function->GetObjectTypeName());
				const auto name = Text(function->GetName());
				if (caller.empty()) {
					caller = std::format("{}.{}", script, name);
				}
				if (key = AchievementGate::Fragment(script, name); key) {
					break;
				}
			}
			if (key) {
				std::optional<AchievementGate::Clock::duration> drop;
				{
					const std::lock_guard l{ marksLock };
					drop = marks.Drops(*key, Clock::now());
				}
				if (drop) {
					++dropped;
					REX::INFO("Achievements: Game.AddAchievement({}) from {}: dropped (file {} quest {:06X} stage {} was caught up from a friend {:.1f} s ago)", id, caller,
						key->file, key->quest, key->stage, std::chrono::duration<double>(*drop).count());
					a_ret = nullptr;
					return true;
				}
			}
			++passed;
			REX::INFO("Achievements: Game.AddAchievement({}) from {}: {}", id, caller.empty() ? "no script frame" : caller,
				key ? "a live stage's, the game's call runs" : "not a quest stage's fragment, the game's call runs");
			return original(a_self, a_this, a_vm, a_stackId, a_ret, a_frame);
		}

		// The "Game" type's AddAchievement function object, once the type is loaded and linked, else null with why.
		NativeFunctionBase* FindNative(std::string& a_whyNot)
		{
			const auto game = RE::GameVM::GetSingleton();
			const auto vm = game ? game->GetVM() : nullptr;
			if (!vm) {
				a_whyNot = "no script VM";
				return nullptr;
			}
			const auto                                          impl = static_cast<RE::BSScript::Internal::VirtualMachine*>(vm.get());
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
			{
				RE::BSAutoLock l{ impl->typeInfoLock };
				const auto     it = impl->objectTypeMap.find(RE::BSFixedString{ "Game" });
				if (it != impl->objectTypeMap.end()) {
					type = it->second;
				}
			}
			if (!type) {
				a_whyNot = "the Game script type isn't loaded yet";
				return nullptr;
			}
			if (!type->IsLinked()) {
				a_whyNot = "the Game script type isn't linked yet";
				return nullptr;
			}
			const auto functions = type->GetGlobalFuncIter();
			const auto count = type->GetNumGlobalFuncs();
			if (!functions || count == 0) {
				a_whyNot = "the Game script type has no global functions";
				return nullptr;
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto function = functions[i].func.get();
				if (!function || !function->GetIsNative() || Text(function->GetName()) != "AddAchievement") {
					continue;
				}
				if (!function->GetIsStatic() || function->GetParamCount() != 1 || Text(function->GetObjectTypeName()) != "Game") {
					a_whyNot = std::format("Game.AddAchievement reads static={} params={} object='{}', not as expected", function->GetIsStatic(), function->GetParamCount(),
						Text(function->GetObjectTypeName()));
					return nullptr;
				}
				return static_cast<NativeFunctionBase*>(function);
			}
			a_whyNot = "the Game script type has no native AddAchievement";
			return nullptr;
		}

		// Points a_native's vtable pointer at the copy, making the copy from its vtable the first time. False (with
		// why) if its vtable isn't the one the copy was made from.
		bool Rebind(NativeFunctionBase* a_native, std::string& a_whyNot)
		{
			const auto vptr = reinterpret_cast<std::uintptr_t*>(a_native);
			const auto vtable = reinterpret_cast<const std::uintptr_t*>(*vptr);
			if (vtable == &vtableCopy[1]) {
				return true;  // already ours
			}
			if (!originalVtable) {
				if (!vtable || !vtable[DISPATCH_SLOT]) {
					a_whyNot = "Game.AddAchievement's vtable has no dispatch slot";
					return false;
				}
				vtableCopy[0] = vtable[-1];
				for (std::size_t i = 0; i < SLOTS; ++i) {
					vtableCopy[i + 1] = vtable[i];
				}
				original = reinterpret_cast<Dispatch>(vtable[DISPATCH_SLOT]);
				vtableCopy[DISPATCH_SLOT + 1] = reinterpret_cast<std::uintptr_t>(&Dispatched);
				originalVtable = vtable;
			} else if (vtable != originalVtable) {
				a_whyNot = "a new Game.AddAchievement object has another vtable than the one hooked";
				return false;
			}
			held.emplace_back(a_native);
			std::atomic_ref<std::uintptr_t>{ *vptr }.store(reinterpret_cast<std::uintptr_t>(&vtableCopy[1]));  // one aligned store: a call under way reads one vtable or the other
			return true;
		}

		// Main thread, in the world: makes the hook once the Game type is there, and keeps it on the object the type
		// holds (a type unloaded and loaded again would have a new one, with the engine's vtable).
		void KeepHooked(Clock::time_point a_now)
		{
			if (hookState == Hook::kGivenUp || (hookTriedAt && a_now - *hookTriedAt < HOOK_RETRY)) {
				return;
			}
			hookTriedAt = a_now;
			std::string whyNot;
			const auto  native = FindNative(whyNot);
			if (native && Rebind(native, whyNot)) {
				if (hookState != Hook::kHooked || held.size() > 1) {
					tasklet = native->CanBeCalledFromTasklets();
					REX::INFO("Achievements: Game.AddAchievement hooked{} (its native runs on {}; {} dispatch slots copied)", held.size() > 1 ? " again (a new function object)" : "",
						tasklet ? "the script VM's threads" : "the main thread", SLOTS);
				}
				hookState = Hook::kHooked;
				hookWhyNot.clear();
				return;
			}
			if (hookState == Hook::kHooked) {
				if (whyNot != hookWhyNot) {
					REX::WARN("Achievements: Game.AddAchievement's hook can't be kept: {}", whyNot);
					hookWhyNot = whyNot;
				}
				return;
			}
			if (++hookTries >= HOOK_TRIES) {
				hookState = Hook::kGivenUp;
				REX::WARN("Achievements: Game.AddAchievement not hooked ({}; given up after {} tries): a friend's caught-up stage awards here as a live one", whyNot, hookTries);
			} else if (whyNot != hookWhyNot) {
				REX::INFO("Achievements: Game.AddAchievement not hooked yet: {}", whyNot);
				hookWhyNot = whyNot;
			}
		}

		// What Steam says this player has unlocked, and how many achievements it knows; nothing if it
		// can't say (no Steam, or this player's stats haven't arrived yet).
		std::optional<std::pair<Unlocked, int>> ReadSteam()
		{
			const auto api = SteamApi::Get();
			if (!api || !api->userStats || !api->GetAchievementAndUnlockTime) {
				return std::nullopt;
			}
			Unlocked out;
			int      count = 0;
			for (int id = 0; id <= MAX_ID; ++id) {
				const auto    name = std::format("NEW_ACHIEVEMENT_{}_0", id);
				bool          achieved = false;
				std::uint32_t unlockTime = 0;
				if (api->GetAchievementAndUnlockTime(api->userStats, name.c_str(), &achieved, &unlockTime)) {
					++count;
					out[id] = achieved;
				}
			}
			if (count == 0) {
				return std::nullopt;
			}
			return std::pair{ out, count };
		}

		// The check Game.AddAchievement makes before it awards one: TESDataHandler::CheckModsLoaded(true)
		// (ID 2192323, +2E8F90) is true with a plugin loaded that is neither one of the game's eight
		// masters nor a Creation it knows, or with the save marked as ever modded (PlayerCharacter::
		// everModded, +E02 bit 6), and then the game awards nothing. It only reads.
		void CheckEligible()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!handler || !player) {
				return;
			}
			const bool plugins = handler->CheckModsLoaded(false);
			eligible = !handler->CheckModsLoaded(true);
			REX::INFO("Achievements: eligible={} (mod plugins={}, save marked modded={})", *eligible ? "yes" : "no", plugins ? "yes" : "no",
				player->everModded ? "yes" : "no");
		}

		void Check(bool a_load)
		{
			++checks;
			const auto steam = ReadSteam();
			if (!steam) {
				if (a_load) {
					REX::INFO("Achievements: Steam has no achievements for this player yet");
				}
				return;
			}
			const auto& [now, count] = *steam;
			if (known) {
				const auto last = lastQuest ? std::format("{:08X} stage {}", lastQuest, lastStage) : std::string("none");
				for (int id = 0; id <= MAX_ID; ++id) {
					if (now[id] && !unlocked[id]) {
						++newlyUnlocked;
						REX::INFO("Achievements: NEW_ACHIEVEMENT_{}_0 unlocked (last stage set from a friend: {})", id, last);
					} else if (!now[id] && unlocked[id]) {
						REX::INFO("Achievements: NEW_ACHIEVEMENT_{}_0 no longer unlocked", id);
					}
				}
			}
			if (a_load || !known) {
				REX::INFO("Achievements: Steam: {} of {} unlocked", now.count(), count);
			}
			unlocked = now;
			known = true;
		}
	}

	void OnGameLoaded()
	{
		loadCheck = true;
		checkAt.reset();
		checkBy.reset();
		const std::lock_guard l{ marksLock };
		marks.Clear();
	}

	void StageSetHere(StageRules::File a_file, std::uint32_t a_quest, std::uint16_t a_stage, bool a_live)
	{
		if (a_live) {
			return;
		}
		const AchievementGate::Key key{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		const auto                 award = AchievementGate::Reaches(key);
		std::size_t                added = 0;
		{
			const std::lock_guard l{ marksLock };
			marks.Expire(Clock::now());
			added = marks.CaughtUp(key, Clock::now());
		}
		marked += static_cast<std::uint32_t>(added);
		if (award) {
			REX::INFO("Achievements: {} quest {:06X} stage {} caught up from a friend: {}'s award is dropped for {} min{}", StageRules::FileName(a_file), a_quest, a_stage,
				*award == key ? "its fragment" : std::format("quest {:06X} stage {}", award->quest, award->stage),
				std::chrono::duration_cast<std::chrono::minutes>(AchievementGate::MARK_FOR).count(), hookState == Hook::kHooked ? "" : " (the hook isn't in place: awarded as live)");
		}
	}

	void AfterStageSet(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		lastQuest = a_quest;
		lastStage = a_stage;
		++stagesSet;
		const auto now = Clock::now();
		checkAt = now + CHECK_AFTER;
		if (!checkBy) {
			checkBy = now + CHECK_BY;
		}
	}

	void Frame()
	{
		if (!WorldSync::InWorld()) {
			return;
		}
		const auto now = Clock::now();
		KeepHooked(now);
		if (loadCheck) {
			loadCheck = false;
			CheckEligible();
			Check(true);
			return;
		}
		if (checkAt && (now >= *checkAt || now >= *checkBy)) {
			checkAt.reset();
			checkBy.reset();
			Check(false);
		}
	}

	std::string Describe()
	{
		std::size_t marksNow = 0;
		{
			const std::lock_guard l{ marksLock };
			marksNow = marks.Size();
		}
		return std::format("achievements: eligible={} unlocked={} new={} checks={} afterStages={} hook={} marks={} marked={} dropped={} passed={}", eligible ? (*eligible ? "yes" : "no") : "?",
			known ? static_cast<int>(unlocked.count()) : -1, newlyUnlocked, checks, stagesSet, hookState == Hook::kHooked ? "yes" : hookState == Hook::kGivenUp ? "no" : "waiting",
			marksNow, marked.load(), dropped.load(), passed.load());
	}
}
