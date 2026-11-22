#include "game/RealHits.h"

#include "game/LogPace.h"
#include "net/PlayerHitRules.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <span>

namespace RealHits
{
	namespace
	{
		// 1.11.240 (RealHits.h says what each is).
		constexpr REL::ID HIT_DATA_CTOR{ 2236839 };   // +E36380
		constexpr REL::ID HIT_DATA_DTOR{ 2236841 };   // +E366A0
		constexpr REL::ID INIT_WEAPON_HIT{ 2236845 };  // +E36AD0
		constexpr REL::ID INIT_BLAST_HIT{ 2236849 };   // +E376A0
		constexpr REL::ID QUEUE_DO_HIT_ME{ 2231111 };  // +CD1B90
		constexpr REL::ID QUEUE_CAST{ 2201036 };       // +509D40

		using CtorFn = RE::HitData* (*)(RE::HitData*);
		using DtorFn = void (*)(RE::HitData*);
		// The equip index goes by value (a 32-bit index), as the engine's callers pass it: RE::BGSEquipIndex has a
		// destructor of its own, which would make it go by address.
		using InitWeaponFn = void (*)(RE::HitData*, RE::Actor*, RE::Actor*, RE::BGSObjectInstanceT<RE::TESObjectWEAP>*, std::uint32_t, bool);
		using InitBlastFn = void (*)(RE::HitData*, RE::Actor*, RE::Actor*, RE::BGSObjectInstance*, RE::BGSExplosion*, float, float);
		using QueueHitFn = void (*)(RE::Actor*, RE::HitData*);
		using QueueCastFn = void (*)(RE::TESObjectREFR*, RE::MagicItem*, RE::TESObjectREFR*, RE::TESForm*);

		// How each begins in 1.11.240: another version or another plugin's hook there, and no hit is made real.
		struct Entry
		{
			REL::ID                     id;
			std::array<std::uint8_t, 10> bytes;
		};
		constexpr std::array<Entry, 6> ENTRIES{ {
			{ HIT_DATA_CTOR, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10 } },
			{ HIT_DATA_DTOR, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 } },
			{ INIT_WEAPON_HIT, { 0x4C, 0x8B, 0xDC, 0x4D, 0x89, 0x4B, 0x20, 0x53, 0x41, 0x56 } },
			{ INIT_BLAST_HIT, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68 } },
			{ QUEUE_DO_HIT_ME, { 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x0D, 0x6B } },
			{ QUEUE_CAST, { 0x48, 0x83, 0xEC, 0x58, 0x33, 0xC0, 0x48, 0x89, 0x54, 0x24 } },
		} };

		std::once_flag checked;
		bool           ready = false;

		std::uint32_t   made = 0, failed = 0;
		std::mutex      lastLock;  // `net status` reads it from the dev channel's thread
		std::string     last;
		LogPace::Pace   madeLog, failedLog;
		constexpr std::uint32_t LOGGED = 30;  // the first ones in full, then one per LogPace::INTERVAL

		// The weapon the hit came from, as our copy holds it (its mods: a modded gun's damage), else the bare form, else
		// none (the engine then takes the copy's own weapon, or its unarmed one).
		RE::BGSObjectInstanceT<RE::TESObjectWEAP> WeaponOf(RE::Actor* a_attacker, std::uint32_t a_weapon)
		{
			const auto form = a_weapon ? RE::TESForm::GetFormByID<RE::TESObjectWEAP>(a_weapon) : nullptr;
			if (!form) {
				return { nullptr, nullptr };
			}
			const auto process = a_attacker->currentProcess;
			const auto middle = process ? process->middleHigh : nullptr;
			if (middle) {
				RE::BSAutoLock l{ middle->equippedItemsLock };
				for (const auto& equipped : middle->equippedItems) {
					if (equipped.item.object == form) {
						return { form, equipped.item.instanceData.get() };
					}
				}
			}
			return { form, nullptr };
		}

		// The attack the friend's copy made (its event: "attackPowerStart"), from our copy's race: its damage multiplier
		// and its knock-down. None: the engine takes our copy's own current attack, if any.
		RE::BGSAttackData* AttackOf(RE::Actor* a_attacker, const std::string& a_event)
		{
			const auto race = a_attacker->race;
			const auto map = race ? race->attackDataMap.get() : nullptr;
			if (a_event.empty() || !map) {
				return nullptr;
			}
			const auto found = map->attackDataMap.find(RE::BSFixedString{ a_event });
			return found != map->attackDataMap.end() ? found->second.get() : nullptr;
		}

		// How many projectiles the weapon the hit was made with fires a shot.
		unsigned ProjectilesOf(const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& a_weapon)
		{
			const auto instance = static_cast<RE::TESObjectWEAP::InstanceData*>(a_weapon.instanceData.get());
			const auto form = a_weapon.object ? a_weapon.object->As<RE::TESObjectWEAP>() : nullptr;
			const auto ranged = instance && instance->rangedData ? instance->rangedData : form ? form->weaponData.rangedData : nullptr;
			return ranged && ranged->numProjectiles > 1 ? static_cast<unsigned>(ranged->numProjectiles) : 1u;
		}

		// Our copy's zeroed AttackDamageMult (Puppets::SetAttackDamage) lifted while the hit is made, then put back.
		class FullDamage
		{
		public:
			explicit FullDamage(RE::Actor* a_actor) :
				owner(static_cast<RE::ActorValueOwner*>(a_actor))
			{
				const auto values = RE::ActorValue::GetSingleton();
				info = values ? values->attackDamageMult : nullptr;
				if (info) {
					lifted = owner->GetModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, *info);
					if (lifted < 0.0f) {
						owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *info, -lifted);
					} else {
						lifted = 0.0f;
					}
				}
			}
			~FullDamage()
			{
				if (info && lifted < 0.0f) {
					owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *info, lifted);
				}
			}
			FullDamage(const FullDamage&) = delete;
			FullDamage& operator=(const FullDamage&) = delete;

		private:
			RE::ActorValueOwner* owner;
			RE::ActorValueInfo*  info = nullptr;
			float                lifted = 0.0f;
		};

		const char* KindName(std::uint8_t a_kind)
		{
			switch (a_kind) {
			case Protocol::HitKind::kMelee:
				return "melee";
			case Protocol::HitKind::kRanged:
				return "ranged";
			case Protocol::HitKind::kExplosion:
				return "explosion";
			default:
				return "none";
			}
		}

		void Failed(const Protocol::PlayerHit& a_hit, std::string_view a_why)
		{
			++failed;
			if (failed <= LOGGED || failedLog.Due(LogPace::Clock::now())) {
				REX::INFO("RealHits: a {} hit by {:08X} (weapon {:08X}) taken as it came, not made real: {}", KindName(a_hit.kind), a_hit.attacker, a_hit.weapon, a_why);
			}
		}
	}

	bool Ready()
	{
		std::call_once(checked, [] {
			std::string wrong;
			for (const auto& entry : ENTRIES) {
				std::array<std::uint8_t, 10> at{};
				std::memcpy(at.data(), reinterpret_cast<const void*>(entry.id.address()), at.size());
				if (at != entry.bytes) {
					wrong += std::format("{}+{:X} ", entry.id.id(), entry.id.offset());
				}
			}
			ready = wrong.empty();
			if (ready) {
				REX::INFO("RealHits: an NPC's hit on our stand-in in a friend's game is made a real hit on our player (HitData +{:X}, InitializeHitData +{:X}, QueueDoHitMe +{:X})",
					HIT_DATA_CTOR.offset(), INIT_WEAPON_HIT.offset(), QUEUE_DO_HIT_ME.offset());
			} else {
				REX::WARN("RealHits: {}do not begin as 1.11.240 has them (another version, or another plugin's hook): friends' NPCs' hits are taken as their stand-in took them", wrong);
			}
		});
		return ready;
	}

	bool Strike(RE::PlayerCharacter* a_player, RE::Actor* a_attacker, const Protocol::PlayerHit& a_hit)
	{
		if (!a_player || !a_attacker) {
			Failed(a_hit, "no player or attacker");
			return false;
		}
		if (!Ready()) {
			Failed(a_hit, "the engine's hit functions are not as read");
			return false;
		}
		const bool blast = a_hit.kind == Protocol::HitKind::kExplosion;
		const auto explosion = blast && a_hit.explosion ? RE::TESForm::GetFormByID<RE::BGSExplosion>(a_hit.explosion) : nullptr;
		if (blast && !explosion) {
			Failed(a_hit, std::format("explosion {:08X} is no explosion here", a_hit.explosion));
			return false;
		}

		static REL::Relocation<CtorFn>      ctor{ HIT_DATA_CTOR };
		static REL::Relocation<DtorFn>      dtor{ HIT_DATA_DTOR };
		static REL::Relocation<InitWeaponFn> initWeapon{ INIT_WEAPON_HIT };
		static REL::Relocation<InitBlastFn>  initBlast{ INIT_BLAST_HIT };
		static REL::Relocation<QueueHitFn>   queueHit{ QUEUE_DO_HIT_ME };
		static REL::Relocation<QueueCastFn>  queueCast{ QUEUE_CAST };

		alignas(16) std::array<std::byte, sizeof(RE::HitData)> storage{};
		const auto hit = reinterpret_cast<RE::HitData*>(storage.data());
		ctor(hit);
		auto       weapon = WeaponOf(a_attacker, a_hit.weapon);
		float      share = 1.0f;
		{
			const FullDamage full{ a_attacker };
			hit->flags.set(static_cast<RE::HitData::Flag>(a_hit.flags & PlayerHits::HitFlag::kCarried));
			if (blast) {
				const auto args = PlayerHits::BlastFrom(a_hit.rawDamage, explosion->data.damage);
				initBlast(hit, a_attacker, a_player, &weapon, explosion, args.damage, args.mult);
			} else {
				if (const auto attack = AttackOf(a_attacker, a_hit.attack)) {
					hit->attackData.reset(attack);
				}
				initWeapon(hit, a_attacker, a_player, &weapon, 0, a_hit.kind == Protocol::HitKind::kMelee);
				// A shotgun's pellets: each hit is the share of one, as the engine shares a projectile's.
				if (a_hit.kind == Protocol::HitKind::kRanged) {
					share = PlayerHits::PelletShare(ProjectilesOf(hit->weapon));
					if (share < 1.0f) {
						hit->healthDamage *= share;
						hit->totalDamage *= share;
						hit->physicalDamage *= share;
						hit->targetedLimbDamage *= share;
						hit->resistedPhysicalDamage *= share;
						hit->resistedTypedDamage *= share;
					}
				}
			}
		}
		hit->healthDamage = (std::min)(hit->healthDamage, PlayerHits::MAX_REAL_HIT);
		const float health = hit->healthDamage, limbDamage = hit->targetedLimbDamage, resisted = hit->resistedPhysicalDamage;
		const auto  limb = hit->damageLimb.underlying();
		const auto  used = hit->weapon.object ? hit->weapon.object->GetFormID() : 0;
		queueHit(a_player, hit);

		// What the engine's own step casts after the hit: the copy's perks' combat hit spell, or the explosion's
		// enchantment.
		RE::MagicItem* spell = nullptr;
		if (blast) {
			spell = explosion->formEnchanting;
		} else {
			RE::SpellItem*      hitSpell = nullptr;
			RE::BGSObjectInstance target{ a_player, nullptr };
			if (a_hit.flags & PlayerHits::HitFlag::kBash) {
				RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kApplyBashSpell, a_attacker, &target, &hitSpell);
			} else {
				RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kApplyCombatHitSpell, a_attacker, static_cast<RE::BGSObjectInstance*>(&hit->weapon), &target, &hitSpell);
			}
			spell = hitSpell;
		}
		if (spell) {
			queueCast(a_attacker, spell, a_player, hit->weapon.object);
		}
		dtor(hit);

		++made;
		std::scoped_lock l{ lastLock };
		last = std::format("{} by {:08X} weapon {:08X} health {:.1f} (stand-in took {:.1f}) resisted {:.1f} limb {} {:.1f}{}{}", KindName(a_hit.kind), a_hit.attacker, used, health,
			a_hit.damage, resisted, limb, limbDamage, share < 1.0f ? std::format(" pellet 1/{:.0f}", 1.0f / share) : "", spell ? std::format(" spell {:08X}", spell->GetFormID()) : "");
		if (made <= LOGGED || madeLog.Due(LogPace::Clock::now())) {
			REX::INFO("RealHits: a hit on our stand-in made real on our player: {} ({} so far)", last, made);
		}
		return true;
	}

	std::string Describe()
	{
		std::scoped_lock l{ lastLock };
		return std::format(" realHits={} realHitsFailed={} realLast='{}'", made, failed, last);
	}
}
