#include "game/InertShots.h"
#include "Runtime.h"

#include "game/InertShot.h"
#include "game/Puppets.h"
#include "game/SaveGuard.h"
#include "game/WeaponFire.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace InertShots
{
	namespace
	{
		// Projectile::Launch(ProjectileHandle& out, const ProjectileLaunchData&): fills out with the projectile it made
		// (nothing when none was) and returns &out. The engine's own code, after the hook's jump (InertShot::Continuation).
		using LaunchFn = RE::ProjectileHandle* (*)(RE::ProjectileHandle*, RE::ProjectileLaunchData*);
		LaunchFn          continuation = nullptr;
		std::atomic<bool> installed{ false };
		std::thread::id   mainThread;

		std::atomic<std::uint32_t> launches{ 0 };  // every launch the engine made since the hook went in
		std::atomic<std::uint32_t> swapped{ 0 };   // ... a stand-in's or a copy's, made from the inert copy of the record
		std::atomic<std::uint32_t> refused{ 0 };   // ... that got no copy (none could be made): nothing launched
		std::atomic<std::uint32_t> offMain{ 0 };   // ... swapped or refused on a thread other than the main one
		std::atomic<std::uint32_t> logged{ 0 };
		std::atomic<std::uint32_t> spawnedSwapped{ 0 };  // ... unowned children of a relayed explosion, swapped (ExpectSpawned)
		std::uint32_t              endedStale = 0;       // live shots ended by Frame: stale, or in a cell no longer attached
		std::uint32_t              endedForSave = 0;     // ... ended before a save or a load
		std::uint32_t              handedToLoad = 0;     // ... handed to a load, which freed them
		std::atomic<std::uint32_t> refusedFull{ 0 };   // launches refused for want of room in the live list
		std::uint32_t              dropped = 0;          // ... let go of: the engine deleted them (they hit something)
		std::uint32_t              droppedEarlyLogged = 0;
		std::uint32_t              aliveLogged = 0;
		std::atomic<std::uint32_t> unownedLogged{ 0 };
		std::atomic<std::uint32_t> leftLogged{ 0 };
		std::uint32_t              expectLogged = 0;

		using Clock = std::chrono::steady_clock;

		// Every inert shot in flight (InertShot.h, "what becomes of an inert shot"): its handle and when it was launched.
		// Pushed by the launch hook on whatever thread it runs; read and ended on the main thread (Frame, BeforeSaveOrLoad).
		struct Live
		{
			RE::ProjectileHandle handle;  // the engine's own, from the launch
			Clock::time_point    launched;
		};
		std::mutex            liveLock;
		std::vector<Live>     live;  // under liveLock
		constexpr std::size_t MAX_LIVE = 64;

		// The spawn record of a relayed explosion placed just now (ExpectSpawned), for the window its children are launched in.
		std::mutex         spawnLock;
		RE::BGSProjectile* expectedSpawn = nullptr;  // under spawnLock
		Clock::time_point  expectedAt{};             // under spawnLock

		// Ends an inert shot now, the way a projectile that hit something ends: disabled, deleted, and its record dropped
		// at once (SaveGuard::ExcludeOurs), so no save writes it and no load is left it. Main thread.
		void End(RE::TESObjectREFR* a_ref)
		{
			a_ref->Disable();
			a_ref->SetDelete(true);
			SaveGuard::ExcludeOurs(a_ref);
		}

		std::vector<Live> TakeLive()
		{
			std::scoped_lock l{ liveLock };
			return std::exchange(live, {});
		}

		void Keep(const Live& a_live)
		{
			std::scoped_lock l{ liveLock };
			live.push_back(a_live);
		}

		// Whether there is room to keep one more: a launch with none is refused (as the hold), so no inert shot flies untracked.
		bool HasRoom()
		{
			std::scoped_lock l{ liveLock };
			return live.size() < MAX_LIVE;
		}

		// The inert copies, one per projectile record, never freed (forms live as long as the game, as MuzzleFlash's art
		// objects do). The engine gives each a runtime form ID (FF...), kept in no save: a projectile of one caught in a save
		// loads as nothing.
		std::mutex                                                copyLock;
		std::unordered_map<RE::BGSProjectile*, RE::BGSProjectile*> copies;  // under copyLock
		constexpr std::size_t                                     MAX_COPIES = 32;

		// The record's copy, made now if there is none: the same motion, speed, range, model, light, sounds and impact
		// look, with no explosion (its proximity and timer gone with it), no force on what it hits, and nothing to pin or
		// pick up (InertShot::InertFlags). Null when the engine made no form or the copies are full.
		RE::BGSProjectile* CopyOf(RE::BGSProjectile* a_base)
		{
			std::scoped_lock l{ copyLock };
			if (const auto it = copies.find(a_base); it != copies.end()) {
				return it->second;
			}
			if (copies.size() >= MAX_COPIES) {
				return nullptr;
			}
			const auto factory = RE::IFormFactory::GetFormFactories()[std::to_underlying(RE::ENUM_FORM_ID::kPROJ)];
			const auto form = factory ? factory->DoCreate() : nullptr;
			const auto copy = form ? form->As<RE::BGSProjectile>() : nullptr;
			if (!copy) {
				REX::WARN("InertShots: no projectile record could be made for the inert copy of {:08X}", a_base->GetFormID());
				return nullptr;
			}
			copy->data = a_base->data;
			copy->data.flags = InertShot::InertFlags(a_base->data.flags);
			copy->data.explosionType = nullptr;
			copy->data.explosionProximity = 0.0f;
			copy->data.explosionTimer = 0.0f;
			copy->data.force = 0.0f;
			copy->SetModel(a_base->GetModel());
			copy->muzzleFlashModel.SetModel(a_base->muzzleFlashModel.GetModel());
			copy->soundLevel = a_base->soundLevel;
			copy->boundData = a_base->boundData;
			copies.emplace(a_base, copy);
			REX::INFO("InertShots: the inert copy of projectile {:08X} ('{}', flags {:X}, explosion {:08X}) made: flags {:X}, no explosion, no force", a_base->GetFormID(),
				a_base->GetModel() ? a_base->GetModel() : "", a_base->data.flags, a_base->data.explosionType ? a_base->data.explosionType->GetFormID() : 0, copy->data.flags);
			return copy;
		}

		std::atomic<std::uint32_t> standInsAimed{ 0 };  // stand-ins' launches of the engine's own record given their friend's aim
		std::atomic<std::uint32_t> aimedLogged{ 0 };

		// What of a launch's data steers it, kept to be put back once the engine has read it (it is the caller's own).
		struct Steer
		{
			float              zAngle;
			float              xAngle;
			RE::TESObjectREFR* homingTarget;
			bool               autoAim;
			bool               alwaysHit;
			InertShot::Angles  aimed;  // what it was given instead
		};

		// Gives a puppet's launch its owner's aim (InertShot::LaunchAngles), with nothing left to pull it back toward whoever
		// the engine picked: no homing target, no auto-aim, no hit made certain. What it had, to put back (PutBack); none
		// when the owner's aim isn't known, and the launch is left as the engine aimed it.
		std::optional<Steer> AimByOwner(RE::ProjectileLaunchData* a_data, const RE::Actor* a_actor)
		{
			const auto aim = Puppets::AimOf(a_actor);
			if (!aim) {
				return std::nullopt;
			}
			const auto  angles = InertShot::LaunchAngles(aim->heading, aim->pitch);
			const Steer given{ .zAngle = a_data->zAngle, .xAngle = a_data->xAngle, .homingTarget = a_data->homingTarget, .autoAim = a_data->autoAim, .alwaysHit = a_data->alwaysHit, .aimed = angles };
			a_data->zAngle = angles.z;
			a_data->xAngle = angles.x;
			a_data->homingTarget = nullptr;
			a_data->autoAim = false;
			a_data->alwaysHit = false;
			return given;
		}

		void PutBack(RE::ProjectileLaunchData* a_data, const Steer& a_given)
		{
			a_data->zAngle = a_given.zAngle;
			a_data->xAngle = a_given.xAngle;
			a_data->homingTarget = a_given.homingTarget;
			a_data->autoAim = a_given.autoAim;
			a_data->alwaysHit = a_given.alwaysHit;
		}

		RE::ProjectileHandle* LaunchHook(RE::ProjectileHandle* a_out, RE::ProjectileLaunchData* a_data)
		{
			launches.fetch_add(1, std::memory_order_relaxed);
			const auto base = a_data ? a_data->projectileBase : nullptr;
			const auto shooter = a_data ? a_data->shooter : nullptr;
			const auto actor = shooter ? shooter->As<RE::Actor>() : nullptr;
			if (!base) {
				return continuation(a_out, a_data);
			}
			// A stand-in's or a copy's shot that goes off; or an unowned launch of the spawn record of a relayed explosion
			// placed just now (its children: a MIRV pack's, a Sentry Bot's cluster), which the shooter's game reports as
			// well (InertShot::SwapsSpawned).
			bool spawned = false;
			if (!actor) {
				std::scoped_lock l{ spawnLock };
				const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - expectedAt);
				spawned = expectedSpawn == base && InertShot::SwapsSpawned(false, true, since);
				if (expectedSpawn && since <= InertShot::SPAWN_WINDOW && unownedLogged++ < 20) {
					REX::INFO("InertShots: an unowned launch of {:08X} ({}) {} ms after a relayed explosion that spawns {:08X} was placed; shooter {}", base->GetFormID(),
						spawned ? "swapped" : "not the spawn record", since.count(), expectedSpawn->GetFormID(), shooter ? std::format("{:08X} (no actor)", shooter->GetFormID()) : "none");
				}
			}
			const bool puppets = actor && Puppets::IsRegistered(actor) && InertShot::Swaps(true, base->data.flags, base->data.explosionType != nullptr);
			const bool standIn = actor && Puppets::IsPuppet(actor);
			if (!puppets && !spawned && InertShot::Aims(standIn, false)) {
				// A stand-in's launch of its own record (a flamer's stream, a harpoon: AttackKind::ShotReplay): the engine's,
				// sent where its friend aimed rather than at whoever the engine has it look at (InertShot::Aims).
				const auto given = AimByOwner(a_data, actor);
				const auto result = continuation(a_out, a_data);
				if (given) {
					const auto aimed = given->aimed;
					PutBack(a_data, *given);
					standInsAimed.fetch_add(1, std::memory_order_relaxed);
					if (aimedLogged.fetch_add(1) < 20) {
						REX::INFO("InertShots: {:08X}'s launch of {:08X} sent where its friend aimed (heading {:.2f} pitch {:.2f}), {:.0f} degrees from where the engine had it go{}{}",
							actor->GetFormID(), base->GetFormID(), aimed.z, aimed.x, InertShot::DegreesBetween({ .z = given->zAngle, .x = given->xAngle }, aimed),
							given->homingTarget ? std::format(", at {:08X}", given->homingTarget->GetFormID()) : "", given->autoAim || given->alwaysHit ? " (auto-aimed)" : "");
					}
				}
				return result;
			}
			if (!puppets && !spawned) {
				if (InertShot::GoesOff(base->data.flags, base->data.explosionType != nullptr) && leftLogged++ < 30) {
					REX::INFO("InertShots: a launch of {:08X} (goes off) left to the engine: shooter {}{}", base->GetFormID(), shooter ? std::format("{:08X}", shooter->GetFormID()) : "none",
						actor ? (actor == RE::PlayerCharacter::GetSingleton() ? " (our player)" : " (an actor)") : shooter ? " (no actor)" : "");
				}
				return continuation(a_out, a_data);
			}
			const auto shooterId = actor ? actor->GetFormID() : 0;
			const bool onMain = std::this_thread::get_id() == mainThread;
			if (!onMain) {
				++offMain;
			}
			if (!HasRoom()) {
				// The live list is full (MAX_LIVE in flight at once: near impossible, each ends on impact or by 12 s). As the hold:
				// nothing leaves the gun, so no inert shot ever flies untracked.
				++refusedFull;
				if (a_out) {
					a_out->reset();
				}
				if (logged.fetch_add(1) < 20) {
					REX::WARN("InertShots: {:08X}'s launch of {:08X} refused: {} inert shots in flight already", shooterId, base->GetFormID(), MAX_LIVE);
				}
				return a_out;
			}
			const auto copy = CopyOf(base);
			if (!copy) {
				// As the hold: nothing leaves the gun. The blast comes as the shooter's report all the same.
				++refused;
				if (a_out) {
					a_out->reset();
				}
				if (logged.fetch_add(1) < 20) {
					REX::WARN("InertShots: {:08X}'s launch of {:08X} refused: no inert copy of the record to be had", shooterId, base->GetFormID());
				}
				return a_out;
			}
			// A puppet's shot goes where its owner aimed, not at whoever the engine has it look at (InertShot::Aims).
			const auto given = InertShot::Aims(standIn, puppets) ? AimByOwner(a_data, actor) : std::nullopt;
			const bool aimed = given.has_value();
			a_data->projectileBase = copy;
			const auto result = continuation(a_out, a_data);
			a_data->projectileBase = base;  // the caller's own data again: nothing of ours is left for it to read
			if (given) {
				PutBack(a_data, *given);
			}
			++swapped;
			if (spawned) {
				++spawnedSwapped;
			}
			// What the copy's record already says, said again on the projectile itself: no blast, no damage of its own.
			RE::NiPointer<RE::Projectile> made;
			if (a_out) {
				made = a_out->get();
			}
			const auto projectile = made.get();
			if (projectile) {
				projectile->explosion = nullptr;
				projectile->damage = 0.0f;
				Keep({ .handle = *a_out, .launched = Clock::now() });
			}
			if (logged.fetch_add(1) < 20) {
				REX::INFO("InertShots: {:08X}'s {:08X} launched inert{}{}{} as projectile {:08X}{}: its blast comes as its shooter's game's report", shooterId, base->GetFormID(),
					spawned ? " (a relayed explosion's child)" : "", aimed ? std::format(" aimed at heading {:.2f} pitch {:.2f}, {:.0f} degrees from where the engine had it go", given->aimed.z, given->aimed.x, InertShot::DegreesBetween({ .z = given->zAngle, .x = given->xAngle }, given->aimed)) : "", onMain ? "" : " (off the main thread)", projectile ? projectile->GetFormID() : 0, projectile ? "" : " (none made)");
			}
			return result;
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		static_assert(InertShot::LAUNCH_ID == 2236958);  // the number spelled out for tools/addrcheck.ps1
		constexpr REL::ID LAUNCH{ 0, 0, 2236958 };  // old-gen and next-gen not read yet
		static const Runtime::Feature feature{ "InertShots", { LAUNCH } };
		if (!feature.On()) {
			return false;
		}
		const auto function = *Runtime::TryAddress(LAUNCH);
		std::array<std::uint8_t, InertShot::ENTRY_BYTES.size()> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(function), bytes.size());
		if (!InertShot::EntryMatches(bytes)) {
			REX::WARN("InertShots: Projectile::Launch doesn't begin as 1.11.240 has it (another plugin's hook?); friends' missiles and shells stay held, not shown in flight");
			return false;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < InertShot::TRAMPOLINE_NEEDED) {
			REX::WARN("InertShots: no room for the hook on Projectile::Launch ({} bytes free); friends' missiles and shells stay held, not shown in flight", trampoline.free_size());
			return false;
		}
		const auto stub = reinterpret_cast<std::uintptr_t>(trampoline.allocate(InertShot::CONTINUATION_SIZE));
		const auto code = InertShot::Continuation(stub, function);
		if (!code) {
			REX::WARN("InertShots: the trampoline is out of a jump's reach of Projectile::Launch; friends' missiles and shells stay held, not shown in flight");
			return false;
		}
		std::memcpy(reinterpret_cast<void*>(stub), code->data(), code->size());
		REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), reinterpret_cast<const void*>(stub), code->size());
		continuation = reinterpret_cast<LaunchFn>(stub);
		mainThread = std::this_thread::get_id();
		trampoline.write_jmp<5>(function, &LaunchHook);
		installed = true;
		REX::INFO("InertShots: Projectile::Launch (+{:X}) hooked: a stand-in's or a copy's shot that would go off is launched inert", function - REX::FModule::GetExecutingModule().GetBaseAddress());
		return true;
	}

	bool Installed()
	{
		return installed.load(std::memory_order_relaxed);
	}

	void Prepare(RE::BGSProjectile* a_base)
	{
		if (installed && a_base && InertShot::GoesOff(a_base->data.flags, a_base->data.explosionType != nullptr)) {
			CopyOf(a_base);
		}
	}

	bool HasCopy(RE::BGSProjectile* a_base)
	{
		std::scoped_lock l{ copyLock };
		return copies.contains(a_base);
	}

	void Frame()
	{
		if (!installed) {
			return;
		}
		auto       shots = TakeLive();
		const auto now = Clock::now();
		std::erase_if(shots, [&](const Live& a_live) {
			const auto ref = a_live.handle.get();
			if (!ref || ref->IsDeleted()) {
				++dropped;
				if (const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - a_live.launched); age < std::chrono::milliseconds(500) && droppedEarlyLogged++ < 10) {
					REX::INFO("InertShots: an inert shot was gone {} ms after its launch ({})", age.count(), ref ? "deleted" : "its handle no longer resolves");
				}
				return true;
			}
			const auto cell = ref->GetParentCell();
			if (aliveLogged < 4) {
				++aliveLogged;
				REX::INFO("InertShots: inert shot {:08X} alive {} ms after its launch at ({:.0f},{:.0f},{:.0f}), 3d={} cell={:08X}{}", ref->GetFormID(),
					std::chrono::duration_cast<std::chrono::milliseconds>(now - a_live.launched).count(), ref->data.location.x, ref->data.location.y, ref->data.location.z,
					ref->Get3D() != nullptr, cell ? cell->GetFormID() : 0, cell && cell->IsAttached() ? " (attached)" : "");
			}
			if (InertShot::Stale(std::chrono::duration_cast<std::chrono::milliseconds>(now - a_live.launched), cell && cell->IsAttached())) {
				End(ref.get());
				++endedStale;
				return true;
			}
			return false;
		});
		for (const auto& shot : shots) {
			Keep(shot);  // what the hook pushed meanwhile stays too
		}
	}

	std::uint32_t BeforeSave()
	{
		std::uint32_t ended = 0;
		for (const auto& shot : TakeLive()) {
			if (const auto ref = shot.handle.get(); ref && !ref->IsDeleted()) {
				End(ref.get());
				++ended;
			}
		}
		if (ended) {
			endedForSave += ended;
			REX::INFO("InertShots: {} inert shot{} in flight ended before the save: none goes into it", ended, ended == 1 ? "" : "s");
		}
		return ended;
	}

	std::uint32_t BeforeLoad()
	{
		std::uint32_t handed = 0;
		for (const auto& shot : TakeLive()) {
			if (const auto ref = shot.handle.get(); ref && !ref->IsDeleted()) {
				SaveGuard::HandOver(ref.get(), "an inert shot in flight");
				++handed;
			}
		}
		if (handed) {
			handedToLoad += handed;
			REX::INFO("InertShots: {} inert shot{} in flight handed to the load, which frees {}", handed, handed == 1 ? "" : "s", handed == 1 ? "it" : "them");
		}
		return handed;
	}

	void ExpectSpawned(RE::BGSProjectile* a_spawn)
	{
		if (!a_spawn) {
			return;
		}
		std::scoped_lock l{ spawnLock };
		expectedSpawn = a_spawn;
		expectedAt = Clock::now();
		if (expectLogged++ < 10) {
			REX::INFO("InertShots: a relayed explosion that spawns projectile {:08X} was placed: an unowned launch of it in the next {} ms flies inert", a_spawn->GetFormID(),
				InertShot::SPAWN_WINDOW.count());
		}
	}

	std::string Describe()
	{
		std::size_t made = 0;
		{
			std::scoped_lock l{ copyLock };
			made = copies.size();
		}
		std::size_t flying = 0;
		{
			std::scoped_lock l{ liveLock };
			flying = live.size();
		}
		return std::format("inert={}/{} inertRefused={} inertCopies={} inertOffMain={} inertLive={} inertEnded={}/{} inertHanded={} inertDropped={} inertRefusedFull={} inertSpawned={} standInsAimed={}", swapped.load(), launches.load(), refused.load(), made, offMain.load(),
			flying, endedStale, endedForSave, handedToLoad, dropped, refusedFull.load(), spawnedSwapped.load(), standInsAimed.load());
	}
}
