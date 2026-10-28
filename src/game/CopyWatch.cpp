#include "game/CopyWatch.h"

#include "game/CopyHealth.h"
#include "game/NpcSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/RollingLog.h"
#include "game/WeaponFire.h"
#include "game/WorldSync.h"

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
			CopyHealth::Blow  blow;                 // what it was, for CopyHealth::OwnersToDeal (its explosion judged in Frame)
			bool              inHand = false;        // a weapon in hand that is no throwable (CopyHealth::Blow::inHand)
			std::uint32_t     flags = 0;             // HitData's flags as read, for the log: what the rig checks the throw rules against
			std::uint32_t     sourceRef = 0;         // HitData's source reference: an explosion's names it (WeaponFire::BlastAt)
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
		// The death event comes when the dying is done, 3 s after the killing blow in the 2026-10-15 test (a copy is last seen
		// alive at the blow): 2 s took every copy's death for a takeover's.
		constexpr auto        COPY_SEEN_KEPT = 5s;

		std::mutex         inboxLock;
		std::vector<Hit>   hitInbox;
		std::vector<Death> deathInbox;

		CopyHealth::Book                          book;
		std::unordered_map<std::uint32_t, LastHit> lastHits;  // the last hit on each copy, for the logs
		// When each copy was last seen alive as one (for COPY_SEEN_KEPT): a death is a copy's only if it was one just
		// before. An NPC our game ran that a friend's game took over as it died (their player's blow claimed it) dies
		// here as theirs, but no copy of ours died.
		std::unordered_map<std::uint32_t, Clock::time_point> copiesSeen;

		std::uint32_t hitsOnCopies = 0;
		std::uint32_t allyHits = 0;       // hits on copies by actors our game runs (our companion, our NPCs), kept as ours
		std::uint32_t restores = 0;       // times a copy was given back what something else took
		float         healthRestored = 0.0f;
		std::uint32_t copiesDied = 0;
		std::uint32_t reportsSet = 0;
		std::uint32_t reportsHeld = 0;  // reports that would have raised a copy just after our hits, held (Book::Heard)
		std::vector<std::pair<std::uint32_t, float>> damageOut;  // what our hits took off copies, by session id, to send
		std::uint32_t damageSent = 0;
		std::uint32_t leftToOwner = 0;  // throws' blows of ours on copies, left to the NPC's own game (OwnersToDeal)
		std::uint32_t blastsThere = 0, blastsOurs = 0;  // explosions' blows on copies judged: its owner's game sets it off too / ours alone
		std::uint32_t effectHits = 0;     // hits of ours on copies that put an effect on them (an enchantment's, a spell's: Book::OurEffect)
		std::uint32_t friendsBlasts = 0;  // blows on copies by explosions set off here for a friend, or a trap's, booked as the owner's
		float         damageTotal = 0.0f;
		std::uint32_t gainsTaken = 0;  // times a copy's gain of its own (no report behind it) was taken back
		float         healthTaken = 0.0f;
		// Lines of each kind a RollingLog::WINDOW (they were for the whole run: a long session's log had none of its
		// later fights), and the counters every WINDOW.
		RollingLog::Budget hitLog{ 100 };
		RollingLog::Budget restoreLog{ 60 };
		RollingLog::Budget reportLog{ 40 };
		RollingLog::Budget deathLog{ 40 };
		RollingLog::Budget gainLog{ 40 };
		RollingLog::Every  summary;

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

		// The id the session knows one of our game's NPCs by (NpcSync::SessionId; 0: none). Our copy of a friend's spawn
		// is a copy like any other: a FF000F8B of the host's, its copy of the VM's raider, lay dead in the host's game
		// while the raider fought on in the VM's, and nothing here had watched it.
		std::uint32_t WireOf(std::uint32_t a_local)
		{
			return NpcSync::SessionId(a_local);
		}

		// " (spawn X)" when our NPC goes by a spawn id, for the logs.
		std::string SpawnNote(std::uint32_t a_local, std::uint32_t a_wire)
		{
			return a_wire != 0 && a_wire != a_local ? std::format(" (spawn {:08X})", a_wire) : std::string{};
		}

		// The player whose game runs the NPC (by its session id: WireOf), when it is not ours (0 otherwise, or out of a session).
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
			const auto companion = NpcSync::IsOurCompanion(actor) ? ", our companion" : "";
			const auto wire = WireOf(a_id);
			if (wire == 0) {
				return std::format("an NPC of our game's own{}", companion);
			}
			const auto owner = NpcSync::Owner(wire);
			if (owner == 0) {
				return std::format("an NPC nobody runs{}", companion);
			}
			if (owner == NpcSync::LocalPlayer()) {
				return std::format("an NPC we run{}", companion);
			}
			return std::format("player {}'s NPC{}", owner, companion);
		}

		std::string Describe(const Hit& a_hit, bool a_ours)
		{
			return std::format("by {:08X} ({}{}), weapon {:08X}, source {:08X}, projectile {:08X}{}{}{}, damage {}, flags {:08X}{}", a_hit.cause, KindOf(a_hit.cause, a_hit.byPlayer),
				a_ours ? "; ours, reported" : "", a_hit.weapon, a_hit.source, a_hit.projectile, a_hit.explosion ? ", an explosion's" : "", a_hit.inHand ? ", in hand" : "",
				a_hit.blow.enchantment ? ", an enchantment's" : a_hit.blow.spellAlone ? ", a spell's" : "",
				a_hit.usesHitData ? std::format("{:.1f}", a_hit.damage) : std::string{ "unknown" }, a_hit.flags, a_hit.usesHitData ? "" : " (no hit data)");
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

	CopyHealth::Cause CauseOf(std::uint32_t a_refId)
	{
		CopyHealth::Cause cause;
		const auto        form = a_refId != 0 ? RE::TESForm::GetFormByID(a_refId) : nullptr;
		const auto        actor = form ? form->As<RE::Actor>() : nullptr;
		if (!actor || actor->IsDeleted()) {
			return cause;
		}
		cause.actor = true;
		cause.player = actor->IsPlayerRef();
		cause.standIn = Puppets::IsPuppet(actor);
		cause.shareable = Protocol::IsShareableRef(a_refId);
		cause.owner = cause.shareable ? NpcSync::Owner(a_refId) : 0;
		cause.ourCompanion = NpcSync::IsOurCompanion(actor);
		return cause;
	}

	CopyHealth::Blow BlowOf(const RE::TESHitEvent& a_event)
	{
		// A weapon's form: whether it is a throwable (a grenade, a Molotov, a mine), or one held in the hand.
		const auto weaponOf = [](const RE::TESForm* a_form) {
			return a_form && a_form->GetFormType() == RE::ENUM_FORM_ID::kWEAP ? a_form->As<RE::TESObjectWEAP>() : nullptr;
		};
		const auto throwable = [&](const RE::TESForm* a_form) {
			const auto weapon = weaponOf(a_form);
			const auto type = weapon ? weapon->weaponData.type.get() : RE::WEAPON_TYPE::kHandToHand;
			return weapon && (type == RE::WEAPON_TYPE::kGrenade || type == RE::WEAPON_TYPE::kMine);
		};
		const auto inHand = [&](const RE::TESForm* a_form) {
			return weaponOf(a_form) && !throwable(a_form);
		};
		const auto weapon = a_event.hitData.weapon.object;
		const auto source = a_event.sourceFormID != 0 ? RE::TESForm::GetFormByID(a_event.sourceFormID) : nullptr;
		// The thrower's own watch (our player's is PLAYER_REF_ID, its form id): another's throw says nothing of its fire.
		const auto cause = a_event.cause.get();
		const auto thrower = cause ? cause->GetFormID() : 0u;
		return {
			.explosion = a_event.hitData.flags.any(RE::HitData::Flag::kExplosion),
			.throwable = throwable(weapon) || throwable(source),
			.spellAlone = !weapon && source && source->IsMagicItem(),
			// A weapon's enchantment (a legendary effect's record is an ENCH); what a throw leaves burning is a hazard's SPEL.
			.enchantment = !weapon && source && source->GetFormType() == RE::ENUM_FORM_ID::kENCH,
			.inHand = inHand(weapon) || inHand(source),
			.explosionsShared = WeaponFire::ExplosionsShared(thrower),
			.threwLately = WeaponFire::ThrewLately(thrower),
		};
	}

	std::uint32_t SourceOf(const RE::TESHitEvent& a_event)
	{
		const auto ref = a_event.hitData.sourceRef.get();
		return ref ? ref->GetFormID() : 0u;
	}

	// Whether the explosion that dealt a blow on a copy goes off in the NPC's own game too (RemoteEdits::DealtThere: one we
	// reported, a trap's, a destructible object's own, a friend's set off here), told by the explosion the hit names
	// (a_sourceRef) or else the nearest that reached it (WeaponFire::BlastAt).
	static bool BlastThere(std::uint32_t a_target, std::uint32_t a_sourceRef)
	{
		const auto target = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_target);
		const auto seen = WeaponFire::BlastAt(target, a_sourceRef);
		const bool there = seen && RemoteEdits::DealtThere(*seen);
		++(there ? blastsThere : blastsOurs);
		return there;
	}

	// An explosion's blow on a copy is the NPC's own game's to deal when that explosion goes off there too (BlastThere).
	bool OwnersToDeal(CopyHealth::Blow a_blow, std::uint32_t a_target, std::uint32_t a_owner, std::uint32_t a_sourceRef)
	{
		if (a_blow.explosion && a_owner != 0) {
			a_blow.blastThere = BlastThere(a_target, a_sourceRef);
		}
		return CopyHealth::OwnersToDeal(a_blow);
	}

	void NoteHit(const RE::TESHitEvent& a_event)
	{
		const auto target = a_event.target.get();
		const auto actor = target ? target->As<RE::Actor>() : nullptr;
		// A spawn's id is looked up on the main thread (Frame).
		if (!actor || actor->IsPlayerRef() || !Protocol::IsActorRef(actor->GetFormID())) {
			return;
		}
		const auto cause = a_event.cause.get();
		const auto weapon = a_event.hitData.weapon.object;
		const auto blow = BlowOf(a_event);
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
			.blow = blow,
			.inHand = blow.inHand,
			.flags = a_event.hitData.flags.underlying(),
			.sourceRef = SourceOf(a_event),
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

	float HealthToSet(std::uint32_t a_refId, float a_before, float a_health)
	{
		// The book goes by the session's id, as Frame checks it: our copy of a friend's spawn has a form id of our game's
		// own. Set under that, a friend's report on it was taken back the next frame as a drop with no hit of ours, and
		// our player's next shot reported the copy's untouched health to its owner, healing the NPC there.
		const auto wire = WireOf(a_refId);
		if (wire == 0) {
			return a_health;  // one of our game's own, never shared: no copy of anyone's
		}
		const auto  owner = OtherOwner(wire);
		const float health = book.Heard(wire, a_before, a_health, Clock::now(), owner != 0);
		if (owner != 0) {
			++reportsSet;
			const bool held = book.Holds(wire);
			if (held) {
				++reportsHeld;
			}
			if (reportLog.Take(Clock::now())) {
				if (held) {
					REX::INFO("CopyWatch: a report would raise our copy {:08X}{} (player {}'s) to {:.1f} health just after our hits on it; set to {:.1f} (from {:.1f}), the rest held",
						a_refId, SpawnNote(a_refId, wire), owner, a_health, health, a_before);
				} else {
					REX::INFO("CopyWatch: a report set our copy {:08X}{} (player {}'s) to {:.1f} health (from {:.1f})", a_refId, SpawnNote(a_refId, wire), owner, health, a_before);
				}
			}
		}
		return health;
	}

	void HealthSet(std::uint32_t a_refId, float a_health)
	{
		if (const auto wire = WireOf(a_refId); wire != 0) {
			book.Set(wire, a_health);
		}
	}

	void TakeHits()
	{
		std::vector<Hit> hits;
		{
			std::scoped_lock l{ inboxLock };
			hits.swap(hitInbox);
		}
		const auto now = Clock::now();
		const auto local = NpcSync::LocalPlayer();

		// The book, the last hits and the copies seen go by the session's id (WireOf); the logs name our game's own too.
		for (const auto& hit : hits) {
			const auto wire = WireOf(hit.target);
			if (wire == 0) {
				continue;  // one of our game's own, never shared: no copy of anyone's
			}
			const auto owner = OtherOwner(wire);
			// Our player's hit on anything, as it may become a copy of ours; another actor's matters only on a copy.
			auto hitter = hit.byPlayer ? CopyHealth::Hitter::kOurPlayer :
			              owner != 0   ? CopyHealth::Judge(CauseOf(hit.cause), local) :
			                             CopyHealth::Hitter::kNotOurs;
			// A throw's blow on a copy: the NPC's own game deals it (the explosion goes off there too), so what it took
			// here is given back, and that game's health follows (HealthShare).
			const bool left = owner != 0 && hitter != CopyHealth::Hitter::kNotOurs && OwnersToDeal(hit.blow, hit.target, owner, hit.sourceRef);
			if (left) {
				hitter = CopyHealth::Hitter::kNotOurs;
				++leftToOwner;
				book.OwnersBlow(wire, hit.at);
			}
			if (hitter != CopyHealth::Hitter::kNotOurs) {
				book.OurHit(wire, hit.at, hit.usesHitData && hit.damage > 0.0f ? std::optional{ hit.damage } : std::nullopt);
				// An enchantment's or a spell's hit of ours puts an effect on the copy whose ticks follow with no event:
				// ours for a while (CopyHealth::EFFECT_WINDOW), not something else's to give back.
				if (hit.blow.enchantment || hit.blow.spellAlone) {
					book.OurEffect(wire, hit.at);
					++effectHits;
				}
			} else if (owner != 0 && !left) {
				// Nobody's of ours (a hazard's, a friend's explosion's set off here, another game's actor's): an effect of
				// ours on the copy no longer accounts for its drops alone. A friend's explosion that goes off in the NPC's
				// own game too is that game's blow: what it took beyond our own hits' figures is given back (Check).
				book.OtherHit(wire, hit.at);
				if (hit.blow.explosion && BlastThere(hit.target, hit.sourceRef)) {
					book.OwnersBlow(wire, hit.at);
					++friendsBlasts;
				}
			}
			if (owner == 0) {
				continue;
			}
			++hitsOnCopies;
			if (hitter == CopyHealth::Hitter::kOurs) {
				++allyHits;
			}
			const auto what = Describe(hit, hitter != CopyHealth::Hitter::kNotOurs) + (left ? "; ours, but a throw's: its own game deals it" : "");
			lastHits[wire] = { what, hit.at };
			if (hitLog.Take(now)) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(hit.target);
				REX::INFO("CopyWatch: hit on our copy {:08X}{} (player {}'s) {}, source {:08X}; health now {:.1f}, ghost {}", hit.target, SpawnNote(hit.target, wire), owner, what, hit.sourceRef,
					actor ? HealthOf(actor) : -1.0f, GhostState(actor));
			}
		}

	}

	void Frame()
	{
		std::vector<Death> deaths;
		{
			std::scoped_lock l{ inboxLock };
			deaths.swap(deathInbox);
		}
		const auto now = Clock::now();
		const auto local = NpcSync::LocalPlayer();
		TakeHits();  // those that came since Session::Frame took them (the rest of the frame's engine work)

		// Every loaded copy: what something other than our hits took comes back, and what it gained of its own goes.
		const auto info = HealthInfo();
		const auto lists = RE::ProcessLists::GetSingleton();
		if (info && lists) {
			std::vector<std::pair<RE::Actor*, std::uint32_t>> copies;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor->IsPlayerRef() || actor->IsDeleted() || !Protocol::IsActorRef(actor->GetFormID()) || Puppets::IsPuppet(actor) || actor->IsDead(false) ||
					!actor->Get3D()) {
					continue;
				}
				if (const auto wire = WireOf(actor->GetFormID()); wire != 0 && OtherOwner(wire) != 0) {
					copies.emplace_back(actor, wire);
				}
			}
			for (const auto& [actor, id] : copies) {
				copiesSeen[id] = now;
				auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
				const float health = values.GetActorValue(*info);
				const float maximum = values.GetPermanentActorValue(*info);
				const bool  held = book.Holds(id);
				const float give = book.Check(id, health, now, maximum);
				// What our hits took off it, for the game that runs it (WorldSync sends it).
				if (const float ours = book.TakeOurs(id); ours > 0.0f) {
					if (maximum > 0.0f && damageOut.size() < MAX_INBOX) {
						damageOut.push_back({ id, std::clamp(ours / maximum, 0.0f, 1.0f) });
						++damageSent;
						damageTotal += ours;
					}
				}
				if (give < 0.0f) {
					// A gain of its own, no report behind it (CopyHealth::Book::Check): taken back off, as damage.
					values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, give);
					const float after = values.GetActorValue(*info);
					book.Set(id, after);
					++gainsTaken;
					healthTaken -= give;
					if (gainLog.Take(now)) {
						REX::INFO("CopyWatch: our copy {:08X}{} (player {}'s) gained {:.1f} health with no report from its game ({:.1f} -> {:.1f} of {:.0f}); taken back, {:.1f} now: "
						          "its health is its own game's. Last hit on it: {}",
							actor->GetFormID(), SpawnNote(actor->GetFormID(), id), NpcSync::Owner(id), -give, health + give, health, maximum, after, LastHitOn(id, now));
					}
					continue;
				}
				if (give == 0.0f) {
					continue;
				}
				values.RestoreActorValue(*info, give);
				const float after = values.GetActorValue(*info);
				book.Set(id, after);
				if (held && !book.Holds(id)) {
					if (reportLog.Take(now)) {
						REX::INFO("CopyWatch: the report held for our copy {:08X}{} (player {}'s) stands now our hits are over: {:.1f} -> {:.1f}", actor->GetFormID(),
							SpawnNote(actor->GetFormID(), id), NpcSync::Owner(id), health, after);
					}
					continue;
				}
				++restores;
				healthRestored += give;
				if (restoreLog.Take(now)) {
					REX::INFO("CopyWatch: our copy {:08X}{} (player {}'s) lost {:.1f} health with no hit of ours on it ({:.1f} -> {:.1f}); given back, {:.1f} now. Last hit on it: {}; hazards near:{}",
						actor->GetFormID(), SpawnNote(actor->GetFormID(), id), NpcSync::Owner(id), give, health + give, health, after, LastHitOn(id, now), HazardsNear(actor));
				}
			}
		}
		// A copy seen lately keeps its book: a frame without its 3D (moved in, gear changed, dropped by the engine) must
		// not lose the hit booked in it, the bleed's window, or a held report.
		book.Prune([&](std::uint32_t a_id) {
			const auto it = copiesSeen.find(a_id);
			return it != copiesSeen.end() && now - it->second <= COPY_SEEN_KEPT;
		});
		std::erase_if(lastHits, [&](const auto& a_entry) { return now - a_entry.second.at > LAST_HIT_KEPT; });

		for (const auto& death : deaths) {
			const auto wire = WireOf(death.id);
			const auto owner = wire != 0 ? OtherOwner(wire) : 0;
			if (owner == 0) {
				continue;
			}
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(death.id);
			const auto killer = RE::TESForm::GetFormByID(death.killer);
			const auto killedBy = KindOf(death.killer, killer && killer->As<RE::Actor>() && killer->As<RE::Actor>()->IsPlayerRef());
			if (!copiesSeen.contains(wire)) {
				// Not a copy of ours a moment ago: player N's game took it over as it died (ours ran it, or nobody).
				if (deathLog.Take(now)) {
					REX::INFO("CopyWatch: {:08X}{} died here just as it became player {}'s (no copy of ours a moment before): killer {:08X} ({}); not a copy's death", death.id,
						SpawnNote(death.id, wire), owner, death.killer, killedBy);
				}
				continue;
			}
			++copiesDied;
			if (deathLog.Take(now)) {
				REX::INFO("CopyWatch: our copy {:08X}{} (player {}'s) died here: killer {:08X} ({}), health {:.1f}, ghost {}. Last hit on it: {}; hazards near:{}", death.id,
					SpawnNote(death.id, wire), owner, death.killer, killedBy, actor ? HealthOf(actor) : -1.0f, GhostState(actor), LastHitOn(wire, now),
					actor ? HazardsNear(actor) : std::string{ " ?" });
			}
		}
		std::erase_if(copiesSeen, [&](const auto& a_entry) { return now - a_entry.second > COPY_SEEN_KEPT; });

		// The damage counters, every RollingLog::WINDOW in a session: what `net status` would have said, for a log sent
		// from a long session whose fight's own lines were left out (logLeftOut: hits, give-backs, reports, deaths, gains).
		if (local == 0) {
			summary.Reset();
		} else if (summary.Due(now)) {
			REX::INFO("CopyWatch: damage so far: {} {} {} {} logLeftOut={}/{}/{}/{}/{}", Describe(),
				RollingLog::Pick(WorldSync::Describe(), { "dead", "revived", "settled", "healthShared", "damage", "puppetHitsOnUs" }),
				RollingLog::Pick(WeaponFire::Describe(), { "explosions", "shielded", "ghostsStuck", "ghostsHidden", "ghostsRepaired", "ghostsSpared", "ghostLedger" }),
				RollingLog::Pick(NpcSync::Describe(), { "owned", "mirrored", "fallbacks", "handOffs", "rolls" }), hitLog.LeftOut(), restoreLog.LeftOut(), reportLog.LeftOut(),
				deathLog.LeftOut(), gainLog.LeftOut());
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
		damageOut.clear();
		lastHits.clear();
		copiesSeen.clear();
	}

	std::string Describe()
	{
		return std::format("copies={} copyHits={} copyAllyHits={} copyRestores={}/{:.0f} copyGains={}/{:.0f} copyReports={} copyHeld={} copyDamage={}/{:.0f} copyThrows={} copyBlasts={}/{} copyEffects={} copyFriendsBlasts={} copyDeaths={}",
			book.Size(), hitsOnCopies, allyHits, restores, healthRestored, gainsTaken, healthTaken, reportsSet, reportsHeld, damageSent, damageTotal, leftToOwner, blastsThere, blastsOurs, effectHits,
			friendsBlasts, copiesDied);
	}

	std::vector<std::pair<std::uint32_t, float>> TakeDamage()
	{
		return std::exchange(damageOut, {});
	}
}
