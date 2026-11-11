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
		// virtual getters must say native, static, one parameter, "Game", "AddAchievement". Nothing here is found by
		// an Address Library id, and the hooked call makes no engine call of its own but the original dispatch.
		using RE::BSScript::NF_util::NativeFunctionBase;
		using Dispatch = bool (*)(const NativeFunctionBase*, RE::BSScript::Variable&, RE::BSScript::Internal::VirtualMachine&, std::uint32_t, RE::BSScript::Variable&,
			const RE::BSScript::StackFrame&);

		constexpr std::size_t DISPATCH_SLOT = 0x16;  // NativeFunctionBase::MarshallAndDispatch
		constexpr std::size_t SLOTS = 0x20;          // IFunction's 0x15, HasStub, MarshallAndDispatch, and room past them: a virtual the engine's own template
		                                             // adds after 0x16 (none known) would be read off the copy too, as long as its .rdata can be read

		// [0] the vtable's RTTI locator (the pointer before the first slot), the slots from [1]: an object whose vptr is
		// &vtableCopy[1] reads as before for everything but DISPATCH_SLOT.
		std::array<std::uintptr_t, SLOTS + 1> vtableCopy{};
		std::size_t                           slotsCopied = 0;
		const std::uintptr_t*                 originalVtable = nullptr;
		Dispatch                              original = nullptr;
		// The hooked objects, kept alive: a vptr of ours must never be on freed memory. Leaked on purpose: a static's
		// destructor would release them at process exit, after the VM may be gone.
		auto&                   held = *new std::vector<RE::BSTSmartPointer<RE::BSScript::IFunction>>;
		AchievementGate::HookTries hookTries;
		std::string             hookWhyNot;  // the last reason the hook wasn't made, logged when it changes
		bool                    tasklet = false;  // the native runs on the VM's threads, not the main one

		std::mutex              marksLock;
		AchievementGate::Marks  marks;  // under marksLock: the hooked call runs on the VM's thread
		std::atomic<std::uint32_t> dropped{ 0 };
		std::atomic<std::uint32_t> passed{ 0 };
		std::atomic<std::uint32_t> marked{ 0 };
		std::atomic<std::uint32_t> unmarked{ 0 };  // marks a live stage took away

		std::string_view Text(const RE::BSFixedString& a_string)
		{
			return a_string.c_str() ? std::string_view{ a_string.c_str() } : std::string_view{};
		}

		// Slot DISPATCH_SLOT of the hooked object: the game's call, unless it was made from the stage fragment of a
		// caught-up stage. On the VM's thread when the native is tasklet-callable, else the main thread; only reads
		// the frames of the stack that is running this call. The achievement's id (the call's one argument) is not
		// read: that would take engine calls into the stack's pages from this thread; the fragment's name says which
		// stage, and the table's comments which achievement.
		bool Dispatched(const NativeFunctionBase* a_self, RE::BSScript::Variable& a_this, RE::BSScript::Internal::VirtualMachine& a_vm, std::uint32_t a_stackId,
			RE::BSScript::Variable& a_ret, const RE::BSScript::StackFrame& a_frame)
		{
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
					REX::INFO("Achievements: Game.AddAchievement from {}: dropped (file {} quest {:06X} stage {} was caught up from a friend {:.1f} s ago)", caller, key->file, key->quest,
						key->stage, std::chrono::duration<double>(*drop).count());
					a_ret = nullptr;
					return true;
				}
			}
			++passed;
			REX::INFO("Achievements: Game.AddAchievement from {}: {}", caller.empty() ? "no script frame" : caller,
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
			const RE::BSFixedString                             gameType{ "Game" };  // interned (a string-pool lock) before the type lock is taken
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
			{
				RE::BSAutoLock l{ impl->typeInfoLock };
				const auto     it = impl->objectTypeMap.find(gameType);
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
				if (!function || !function->GetIsNative() || !AchievementGate::SameName(Text(function->GetName()), "AddAchievement")) {
					continue;
				}
				if (!function->GetIsStatic() || function->GetParamCount() != 1 || !AchievementGate::SameName(Text(function->GetObjectTypeName()), "Game")) {
					a_whyNot = std::format("Game.AddAchievement reads static={} params={} object='{}', not as expected", function->GetIsStatic(), function->GetParamCount(),
						Text(function->GetObjectTypeName()));
					return nullptr;
				}
				return static_cast<NativeFunctionBase*>(function);
			}
			a_whyNot = "the Game script type has no native AddAchievement";
			return nullptr;
		}

		// How many slots from a vtable's start can be read: those up to the end of the committed, readable region it is
		// in (one near the end of .rdata has fewer than SLOTS after it), at most SLOTS. 0: not readable at all.
		std::size_t ReadableSlots(const std::uintptr_t* a_vtable)
		{
			constexpr std::uint32_t PAGE_GUARD = 0x100;
			constexpr std::uint32_t READABLE = REX::W32::PAGE_READONLY | REX::W32::PAGE_READWRITE | REX::W32::PAGE_WRITECOPY | REX::W32::PAGE_EXECUTE_READ |
			                                   REX::W32::PAGE_EXECUTE_READWRITE | 0x80 /* PAGE_EXECUTE_WRITECOPY */;
			REX::W32::MEMORY_BASIC_INFORMATION info{};
			if (REX::W32::VirtualQuery(a_vtable, &info, sizeof(info)) != sizeof(info) || info.state != REX::W32::MEM_COMMIT || (info.protect & PAGE_GUARD) != 0 ||
				(info.protect & READABLE) == 0) {
				return 0;
			}
			const auto end = reinterpret_cast<std::uintptr_t>(info.baseAddress) + info.regionSize;
			const auto bytes = end - reinterpret_cast<std::uintptr_t>(a_vtable);
			return static_cast<std::size_t>((std::min)(bytes / sizeof(std::uintptr_t), static_cast<std::uintptr_t>(SLOTS)));
		}

		enum class Bound
		{
			kNot,      // with why
			kAlready,  // its vptr was ours
			kNow       // its vptr was pointed at the copy by this call
		};

		// Points a_native's vtable pointer at the copy, making the copy from its vtable the first time. kNot (with why)
		// if its vtable isn't the one the copy was made from, or can't be read.
		Bound Rebind(NativeFunctionBase* a_native, std::string& a_whyNot)
		{
			const auto vptr = reinterpret_cast<std::uintptr_t*>(a_native);
			const auto vtable = reinterpret_cast<const std::uintptr_t*>(*vptr);
			if (vtable == &vtableCopy[1]) {
				return Bound::kAlready;
			}
			if (!originalVtable) {
				const auto slots = vtable ? ReadableSlots(vtable) : 0;
				if (slots <= DISPATCH_SLOT) {
					a_whyNot = std::format("Game.AddAchievement's vtable can't be read through its dispatch slot ({} slots readable)", slots);
					return Bound::kNot;
				}
				if (!vtable[DISPATCH_SLOT]) {
					a_whyNot = "Game.AddAchievement's vtable has no dispatch slot";
					return Bound::kNot;
				}
				vtableCopy[0] = vtable[-1];
				for (std::size_t i = 0; i < slots; ++i) {
					vtableCopy[i + 1] = vtable[i];
				}
				slotsCopied = slots;
				original = reinterpret_cast<Dispatch>(vtable[DISPATCH_SLOT]);
				vtableCopy[DISPATCH_SLOT + 1] = reinterpret_cast<std::uintptr_t>(&Dispatched);
				originalVtable = vtable;
			} else if (vtable != originalVtable) {
				a_whyNot = "a new Game.AddAchievement object has another vtable than the one hooked";
				return Bound::kNot;
			}
			held.emplace_back(a_native);
			std::atomic_ref<std::uintptr_t>{ *vptr }.store(reinterpret_cast<std::uintptr_t>(&vtableCopy[1]));  // one aligned store: a call under way reads one vtable or the other
			return Bound::kNow;
		}

		// Main thread, in the world: makes the hook once the Game type is there, and keeps it on the object the type
		// holds (a type unloaded and loaded again would have a new one, with the engine's vtable).
		void KeepHooked(Clock::time_point a_now)
		{
			if (!hookTries.Due(a_now)) {
				return;
			}
			hookTries.Tried(a_now);
			std::string whyNot;
			const auto  native = FindNative(whyNot);
			const auto  bound = native ? Rebind(native, whyNot) : Bound::kNot;
			if (bound != Bound::kNot) {
				if (bound == Bound::kNow) {
					tasklet = native->CanBeCalledFromTasklets();
					REX::INFO("Achievements: Game.AddAchievement hooked{} (its native runs on {}; {} vtable slots copied)", held.size() > 1 ? " again (a new function object)" : "",
						tasklet ? "the script VM's threads" : "the main thread", slotsCopied);
				}
				hookTries.Hooked(a_now);
				hookWhyNot.clear();
				return;
			}
			if (hookTries.Hooked()) {
				if (whyNot != hookWhyNot) {
					REX::WARN("Achievements: Game.AddAchievement's hook can't be kept: {}", whyNot);
					hookWhyNot = whyNot;
				}
				return;
			}
			if (hookTries.Failed()) {
				REX::WARN("Achievements: Game.AddAchievement not hooked ({}; given up after {} tries, until the next load): a friend's caught-up stage awards here as a live one", whyNot,
					hookTries.Tries());
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
		if (hookTries.Loaded()) {
			REX::INFO("Achievements: Game.AddAchievement's hook is tried again after the load");
		}
		const std::lock_guard l{ marksLock };
		marks.Clear();
	}

	void StageSetHere(StageRules::File a_file, std::uint32_t a_quest, std::uint16_t a_stage, bool a_live)
	{
		const AchievementGate::Key key{ static_cast<std::uint8_t>(a_file), a_quest, a_stage };
		if (a_live) {
			std::size_t erased = 0;
			{
				const std::lock_guard l{ marksLock };
				erased = marks.Live(key);
			}
			if (erased > 0) {
				unmarked += static_cast<std::uint32_t>(erased);
				REX::INFO("Achievements: {} quest {:06X} stage {} came live from a friend: {} mark{} from an earlier catch-up taken away, its award is the game's", StageRules::FileName(a_file),
					a_quest, a_stage, erased, erased == 1 ? "" : "s");
			}
			return;
		}
		const auto  reaches = AchievementGate::Reaches(key);
		std::size_t added = 0;
		{
			const std::lock_guard l{ marksLock };
			marks.Expire(Clock::now());
			added = marks.CaughtUp(key, Clock::now());
		}
		marked += static_cast<std::uint32_t>(added);
		if (!reaches.empty()) {
			std::string awards;
			for (const auto& reach : reaches) {
				const auto award = AchievementGate::AwardOf(reach);
				awards += awards.empty() ? "" : ", ";
				awards += award == key ? "its fragment's" : std::format("quest {:06X} stage {}'s", award.quest, award.stage);
			}
			REX::INFO("Achievements: {} quest {:06X} stage {} caught up from a friend: {} award{} dropped for {} min{}", StageRules::FileName(a_file), a_quest, a_stage, awards,
				reaches.size() == 1 ? " is" : "s are", std::chrono::duration_cast<std::chrono::minutes>(AchievementGate::MARK_FOR).count(),
				hookTries.Hooked() ? "" : " (the hook isn't in place: awarded as live)");
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
		return std::format("achievements: eligible={} unlocked={} new={} checks={} afterStages={} hook={} marks={} marked={} unmarked={} dropped={} passed={}",
			eligible ? (*eligible ? "yes" : "no") : "?", known ? static_cast<int>(unlocked.count()) : -1, newlyUnlocked, checks, stagesSet,
			hookTries.Hooked() ? "yes" : hookTries.GivenUp() ? "no" : "waiting", marksNow, marked.load(), unmarked.load(), dropped.load(), passed.load());
	}
}
