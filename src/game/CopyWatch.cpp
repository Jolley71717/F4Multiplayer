#include "game/CopyWatch.h"

#include "game/CopyHealth.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"

#include "Protocol.h"

namespace CopyWatch
{
	namespace
	{
		using Clock = CopyHealth::Clock;

		struct Hit
		{
			std::uint32_t     target = 0;
			std::uint32_t     cause = 0;
			bool              byPlayer = false;
			std::uint32_t     weapon = 0;
			std::uint32_t     source = 0;
			std::uint32_t     projectile = 0;
			float             damage = 0.0f;
			bool              usesHitData = false;
			bool              explosion = false;
			Clock::time_point at{};
		};

		struct Death
		{
			std::uint32_t id = 0;
			std::uint32_t killer = 0;
		};

		struct LastHit
		{
			std::string       what;
			Clock::time_point at{};
		};

		constexpr std::size_t MAX_INBOX = 512;
		constexpr auto        LAST_HIT_KEPT = 30s;

		std::mutex         inboxLock;
		std::vector<Hit>   hitInbox;
		std::vector<Death> deathInbox;

		CopyHealth::Book                          book;
		std::unordered_map<std::uint32_t, LastHit> lastHits;  // the last hit on each copy, for the logs

		std::uint32_t hitsOnCopies = 0;
		std::uint32_t restores = 0;       // times a copy was given back what something else took
		float         healthRestored = 0.0f;
		std::uint32_t copiesDied = 0;
		std::uint32_t reportsSet = 0;
		std::uint32_t hitsLogged = 0;
		std::uint32_t restoresLogged = 0;
		std::uint32_t deathsLogged = 0;
		std::uint32_t reportsLogged = 0;

		const RE::ActorValueInfo* HealthInfo()
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values ? values->health : nullptr;
		}

		float HealthOf(RE::Actor* a_actor)
		{
			const auto info = HealthInfo();
			return info ? static_cast<RE::ActorValueOwner*>(a_actor)->GetActorValue(*info) : -1.0f;
		}

		// The player whose game runs the NPC, when it is not ours (0 otherwise, or out of a session).
		std::uint32_t OtherOwner(std::uint32_t a_id)
		{
			const auto local = NpcSync::LocalPlayer();
			const auto owner = NpcSync::Owner(a_id);
			return local != 0 && owner != 0 && owner != local ? owner : 0;
		}

		std::string KindOf(std::uint32_t a_id, bool a_byPlayer)
		{
			if (a_byPlayer) {
				return "our player";
			}
			if (a_id == 0) {
				return "nothing";
			}
			const auto form = RE::TESForm::GetFormByID(a_id);
			const auto actor = form ? form->As<RE::Actor>() : nullptr;
			if (!actor) {
				return form ? std::format("a reference, not an actor ({:08X})", form->As<RE::TESObjectREFR>() && form->As<RE::TESObjectREFR>()->GetObjectReference() ?
				                                                                      form->As<RE::TESObjectREFR>()->GetObjectReference()->GetFormID() :
				                                                                      0) :
				              "gone";
			}
			if (actor->IsPlayerRef()) {
				return "our player";
			}
			if (Puppets::IsPuppet(actor)) {
				return "a friend's stand-in";
			}
			if (!Protocol::IsShareableRef(a_id)) {
				return "an NPC of our game's own";
			}
			const auto owner = NpcSync::Owner(a_id);
			if (owner == 0) {
				return "an NPC nobody runs";
			}
			if (owner == NpcSync::LocalPlayer()) {
				return "an NPC we run";
			}
			return std::format("player {}'s NPC", owner);
		}

		std::string Describe(const Hit& a_hit)
		{
			return std::format("by {:08X} ({}), weapon {:08X}, source {:08X}, projectile {:08X}{}, damage {}", a_hit.cause, KindOf(a_hit.cause, a_hit.byPlayer), a_hit.weapon,
				a_hit.source, a_hit.projectile, a_hit.explosion ? ", an explosion's" : "", a_hit.usesHitData ? std::format("{:.1f}", a_hit.damage) : std::string{ "unknown" });
		}

		std::string LastHitOn(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto it = lastHits.find(a_id);
			if (it == lastHits.end()) {
				return "none seen";
			}
			return std::format("{}, {} ms ago", it->second.what, std::chrono::duration_cast<std::chrono::milliseconds>(a_now - it->second.at).count());
		}

		// Hazards (a fire, gas, radiation a grenade or a mine leaves) within reach of the actor: their records.
		std::string HazardsNear(RE::Actor* a_actor)
		{
			std::string out;
			const auto  tes = RE::TES::GetSingleton();
			if (!tes) {
				return out;
			}
			tes->ForEachReferenceInRange(a_actor->data.location, 800.0f, [&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetObjectReference() : nullptr;
				if (base && base->Is(RE::ENUM_FORM_ID::kHAZD) && out.size() < 200) {
					out += std::format(" {:08X}({:08X})", a_ref->GetFormID(), base->GetFormID());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return out.empty() ? " none" : out;
		}
	}

	std::string GhostState(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "?";
		}
		// The engine's own answer, then the flag after the extra data's header as it holds it.
		const auto  extra = a_actor->extraList ? a_actor->extraList->GetByType(RE::EXTRA_DATA_TYPE::kGhost) : nullptr;
		const char* held = !extra ? "none" : *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(extra) + sizeof(RE::BSExtraData)) ? "on" : "off";
		return std::format("{} (extra {})", a_actor->GetGhost() ? "on" : "off", held);
	}

	void NoteHit(const RE::TESHitEvent& a_event)
	{
		const auto target = a_event.target.get();
		const auto actor = target ? target->As<RE::Actor>() : nullptr;
		if (!actor || actor->IsPlayerRef() || !Protocol::IsShareableRef(actor->GetFormID())) {
			return;
		}
		const auto cause = a_event.cause.get();
		const auto weapon = a_event.hitData.weapon.object;
		Hit hit{
			.target = actor->GetFormID(),
			.cause = cause ? cause->GetFormID() : 0,
			.byPlayer = cause && cause->IsPlayerRef(),
			.weapon = weapon ? weapon->GetFormID() : 0,
			.source = a_event.sourceFormID,
			.projectile = a_event.projectileFormID,
			.damage = a_event.hitData.healthDamage > 0.0f ? a_event.hitData.healthDamage : a_event.hitData.totalDamage,
			.usesHitData = a_event.usesHitData,
			.explosion = a_event.hitData.flags.any(RE::HitData::Flag::kExplosion),
			.at = Clock::now(),
		};
		std::scoped_lock l{ inboxLock };
		if (hitInbox.size() < MAX_INBOX) {
			hitInbox.push_back(hit);
		}
	}

	void NoteDeath(std::uint32_t a_refId, std::uint32_t a_killer)
	{
		std::scoped_lock l{ inboxLock };
		if (deathInbox.size() < MAX_INBOX) {
			deathInbox.push_back({ a_refId, a_killer });
		}
	}

	void HealthSet(std::uint32_t a_refId, float a_before, float a_health)
	{
		book.Set(a_refId, a_health);
		if (const auto owner = OtherOwner(a_refId)) {
			++reportsSet;
			if (reportsLogged < 40) {
				++reportsLogged;
				REX::INFO("CopyWatch: a report set our copy {:08X} (player {}'s) to {:.1f} health (from {:.1f})", a_refId, owner, a_health, a_before);
			}
		}
	}

	void Frame()
	{
		std::vector<Hit>   hits;
		std::vector<Death> deaths;
		{
			std::scoped_lock l{ inboxLock };
			hits.swap(hitInbox);
			deaths.swap(deathInbox);
		}
		const auto now = Clock::now();

		for (const auto& hit : hits) {
			if (hit.byPlayer) {
				book.OurHit(hit.target, hit.at);
			}
			const auto owner = OtherOwner(hit.target);
			if (owner == 0) {
				continue;
			}
			++hitsOnCopies;
			const auto what = Describe(hit);
			lastHits[hit.target] = { what, hit.at };
			if (hitsLogged < 100) {
				++hitsLogged;
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(hit.target);
				REX::INFO("CopyWatch: hit on our copy {:08X} (player {}'s) {}; health now {:.1f}, ghost {}", hit.target, owner, what, actor ? HealthOf(actor) : -1.0f,
					GhostState(actor));
			}
		}

		// Every loaded copy: what something other than our player took comes back.
		std::unordered_set<std::uint32_t> seen;
		const auto                        info = HealthInfo();
		const auto                        lists = RE::ProcessLists::GetSingleton();
		if (info && lists) {
			std::vector<RE::Actor*> copies;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor->IsPlayerRef() || actor->IsDeleted() || !Protocol::IsShareableRef(actor->GetFormID()) || Puppets::IsPuppet(actor) ||
					actor->IsDead(false) || !actor->Get3D() || OtherOwner(actor->GetFormID()) == 0) {
					continue;
				}
				copies.push_back(actor);
			}
			for (const auto actor : copies) {
				const auto id = actor->GetFormID();
				seen.insert(id);
				auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
				const float health = values.GetActorValue(*info);
				const float give = book.Check(id, health, now);
				if (give <= 0.0f) {
					continue;
				}
				values.RestoreActorValue(*info, give);
				const float after = values.GetActorValue(*info);
				book.Set(id, after);
				++restores;
				healthRestored += give;
				if (restoresLogged < 60) {
					++restoresLogged;
					REX::INFO("CopyWatch: our copy {:08X} (player {}'s) lost {:.1f} health with no hit of our player's on it ({:.1f} -> {:.1f}); given back, {:.1f} now. Last hit on it: {}; hazards near:{}",
						id, NpcSync::Owner(id), give, health + give, health, after, LastHitOn(id, now), HazardsNear(actor));
				}
			}
		}
		book.Prune([&](std::uint32_t a_id) { return seen.contains(a_id); });
		std::erase_if(lastHits, [&](const auto& a_entry) { return now - a_entry.second.at > LAST_HIT_KEPT; });

		for (const auto& death : deaths) {
			const auto owner = OtherOwner(death.id);
			if (owner == 0) {
				continue;
			}
			++copiesDied;
			if (deathsLogged < 40) {
				++deathsLogged;
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(death.id);
				const auto killer = RE::TESForm::GetFormByID(death.killer);
				REX::INFO("CopyWatch: our copy {:08X} (player {}'s) died here: killer {:08X} ({}), health {:.1f}, ghost {}. Last hit on it: {}; hazards near:{}", death.id, owner,
					death.killer, KindOf(death.killer, killer && killer->As<RE::Actor>() && killer->As<RE::Actor>()->IsPlayerRef()), actor ? HealthOf(actor) : -1.0f,
					GhostState(actor), LastHitOn(death.id, now), actor ? HazardsNear(actor) : std::string{ " ?" });
			}
		}
	}

	void Reset()
	{
		{
			std::scoped_lock l{ inboxLock };
			hitInbox.clear();
			deathInbox.clear();
		}
		book.Clear();
		lastHits.clear();
	}

	std::string Describe()
	{
		return std::format("copies={} copyHits={} copyRestores={}/{:.0f} copyReports={} copyDeaths={}", book.Size(), hitsOnCopies, restores, healthRestored, reportsSet, copiesDied);
	}
}
