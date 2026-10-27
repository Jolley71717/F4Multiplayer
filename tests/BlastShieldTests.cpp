// A friend's explosion set off in our game passes by our copies of NPCs another game runs: they are made ghosts while
// it goes off (BlastShield, used by WeaponFire::ApplyExplosion), and a friend's throw replayed on a copy throws no
// grenade of its own (ThrowReplays). The sessions below go through the real server (tests/sim/Sim.h): the thrower's
// game reports the throw and the explosion, the server passes them on, and the friend's game replays the throw and
// sets the explosion off in a model of its world (Game below), with BlastShield's and ThrowReplays' own rules deciding
// who is a ghost and which releases are kept. Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include "game/BlastShield.h"
#include "game/CopyHealth.h"
#include "game/GhostLedger.h"
#include "game/RemoteEdits.h"
#include "game/ThrowReplays.h"

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace Sim;
using Reason = Protocol::ClaimReason;

namespace
{
	constexpr std::uint32_t RAIDER_1 = 0x0002A001;
	constexpr std::uint32_t RAIDER_2 = 0x0002A002;
	constexpr std::uint32_t RAIDER_3 = 0x0002A003;
	constexpr std::uint32_t RAIDER_4 = 0x0002A004;
	constexpr std::uint32_t RAIDER_BASE = 0x0001D4D2;  // a plugin's raider, the NPC a game makes at runtime
	constexpr std::uint32_t SPAWN = 0xFF001234;        // the spawn id the host's game shares its runtime raider under
	constexpr std::uint32_t HOST_SPAWN = 0xFF000A10;   // that raider's form id in the host's game
	constexpr std::uint32_t BOB_COPY = 0xFF000F8B;     // the friend's copy's form id, its game's own
	constexpr std::uint32_t FRAG = 0x0001D2EE;         // a frag grenade's explosion
	constexpr float         FRAG_RADIUS = 500.0f;
	constexpr float         GRENADE_DAMAGE = 100.0f;                  // a grenade of a copy's own, at its feet
	constexpr auto          FUSE = std::chrono::milliseconds{ 1000 };  // from leaving the hand to going off

	// An NPC as one game has it: its health, where it stands, whether it died there, and whether it is a ghost (one of
	// ours against a friend's explosion, or one of its own story's). Nothing hurts a ghost. It is keyed by its form id
	// in that game; one made at runtime (a plugin's NPC has the same id in every game) goes by its spawn id on the wire.
	struct Npc
	{
		float         x = 0.0f, y = 0.0f;
		float         health = 100.0f;
		bool          dead = false;
		bool          ghost = false;       // its ghost flag: made one by BlastShield (or a save that held it so)
		bool          ownGhost = false;    // one before we came near it (its base record's)
		bool          questAlias = false;  // a quest holds it in an alias (a story's own ghost is set on such an actor)
		bool          friendly = false;    // not hostile to our player
		bool          loaded = true;       // here with 3D (the console's command takes on it; a door or a fast travel unloads it)
		std::uint32_t stubborn = 0;        // setghost 0s the engine ignores before one takes (a ghost that will not go)
		std::uint32_t sessionId = 0;       // the id the session knows it by, when not its own (NpcSync::SessionId)

		void Hurt(float a_amount)
		{
			if (dead || ghost || ownGhost) {
				return;
			}
			health -= a_amount;
			dead = health <= 0.0f;
		}
	};

	// One player's game: its NPCs, and what WeaponFire and WorldSync do there with a friend's explosion, a friend's
	// throw replayed on a copy, our own player's blows and another game's health report, and what CopyWatch gives
	// back to a copy that something other than our player hurt. A death goes out once, as WorldSync reports it. Time
	// goes by a frame at a time (Wait).
	struct Game
	{
		Client&                        client;
		std::map<std::uint32_t, Npc>   npcs;
		BlastShield::Book              shields;
		ThrowReplays::Book             throws;
		CopyHealth::Book               copies;
		GhostLedger::Book              ledger;  // every ghost we set, by our form id, until it reads no ghost (WeaponFire::TendLedger)
		bool                           watchCopies = true;  // CopyWatch (off: as before it)
		float                          givenBack = 0.0f;
		BlastShield::Clock::time_point now{};
		std::uint64_t                  frame = 0;
		std::set<std::uint32_t>        reported;
		// The console's setghost 0 lands a frame after it is sent (consoleLag; off: at once, as the older cases have it).
		bool                           consoleLag = false;
		std::vector<std::uint32_t>     unghosting;  // its commands still to land, by our form id
		std::uint32_t                  unghostsSent = 0;
		bool                           repairOnLoad = true;  // WeaponFire::RepairGhosts (off: as before it)
		bool                           keepLedger = true;    // the ledger (off: as before it, up to 0.9.2)
		std::set<std::uint32_t>        repaired;             // the repair's, once each since the load

		struct Grenade
		{
			float                          x = 0.0f, y = 0.0f;
			BlastShield::Clock::time_point at{};
		};
		std::vector<Grenade> grenades;  // real ones in this game, thrown by our copies themselves

		// The id the session knows the NPC under a_key by (NpcSync::SessionId), and back (NpcSync::LocalRef). The
		// shields, the copies and the wire go by the session's id; the NPCs here by their own.
		std::uint32_t SessionIdOf(std::uint32_t a_key) const
		{
			const auto& npc = npcs.at(a_key);
			return npc.sessionId != 0 ? npc.sessionId : a_key;
		}

		std::uint32_t KeyOf(std::uint32_t a_id) const
		{
			for (const auto& [key, npc] : npcs) {
				if (npc.sessionId == a_id) {
					return key;
				}
			}
			return a_id;
		}

		// WeaponFire::ApplyExplosion: our copies of NPCs another game runs that the explosion's damage could kill are made
		// ghosts first (not one that is a ghost already, unless it is one of ours: in the ledger, or shielded), then the
		// explosion hurts everyone in reach (a_damage, the same to each here; a_known: its record says so, as
		// ApplyExplosion reads it; else any copy in reach, as for a trap's). A copy with health to spare takes the blast
		// as a friend's blow (CopyWatch books it the owner's: OwnersBlow), given back by WatchCopies.
		void SetOff(const Protocol::Explosion& a_explosion, float a_damage, bool a_known = false)
		{
			const auto reach = [&](const Npc& a_npc, float a_range) {
				return RemoteEdits::Within(std::hypot(a_npc.x - a_explosion.x, a_npc.y - a_explosion.y), a_range);
			};
			for (auto& [key, npc] : npcs) {
				const auto                 id = SessionIdOf(key);
				const BlastShield::Reached reached{ .id = id, .owner = client.OwnerOf(id), .dead = npc.dead, .health = npc.health };
				if (reach(npc, FRAG_RADIUS + BlastShield::MARGIN) && BlastShield::Shields(reached, client.Id(), a_known ? a_damage : 0.0f) &&
					(shields.Shielded(id) || ledger.Contains(key) || !(npc.ownGhost || npc.ghost)) && shields.Begin(id, now, frame)) {
					npc.ghost = true;
					if (keepLedger) {
						ledger.Add(key, frame);
					}
				}
			}
			for (auto& [key, npc] : npcs) {
				if (reach(npc, FRAG_RADIUS)) {
					const auto id = SessionIdOf(key);
					if (!npc.ghost && !npc.ownGhost && client.OwnerOf(id) != 0 && client.OwnerOf(id) != client.Id()) {
						copies.OtherHit(id, now);
						copies.OwnersBlow(id, now);
					}
					Hurt(key, a_damage);
				}
			}
		}

		// WeaponFire::PlayAction: a friend's throw replayed on our copy of the thrower.
		void ReplayThrow(std::uint32_t a_id) { throws.Arm(a_id, now); }

		// The copy's graph lets go of a throw (WeaponFire's NPC animation hook): a replayed throw's release is kept
		// from the engine; any other throws the grenade it holds, which goes off at its feet.
		void LetGo(std::uint32_t a_id)
		{
			if (!throws.Release(a_id, now)) {
				grenades.push_back({ npcs.at(a_id).x, npcs.at(a_id).y, now + FUSE });
			}
		}

		// WorldSync::ReportDeaths (a runtime NPC's under its spawn id).
		void Died(std::uint32_t a_key)
		{
			if (npcs.at(a_key).dead && reported.insert(a_key).second) {
				client.Kill(SessionIdOf(a_key));
			}
		}

		// WeaponFire's ghost comes off (the session's id): the console's setghost 0, which takes on a loaded copy only (one
		// not loaded keeps its entry in the ledger, which sends it once the copy is), and which a stubborn ghost ignores
		// the first few times.
		void Unshield(std::uint32_t a_id)
		{
			const auto key = KeyOf(a_id);
			auto&      npc = npcs.at(key);
			if (!npc.loaded) {
				return;
			}
			++unghostsSent;
			ledger.Sent(key, frame);
			if (npc.stubborn > 0) {
				--npc.stubborn;
				return;
			}
			if (consoleLag) {
				unghosting.push_back(key);
			} else {
				npc.ghost = false;
			}
		}

		// A save, as main.cpp's kPreSaveGame has it: the shields end (EndAllShields), and the loaded copies the ledger
		// has have their flag off in what the save writes (HideGhostsFromSave). What the save holds; its record of the
		// ledger is SavedLedger.
		std::map<std::uint32_t, Npc> Save()
		{
			BeforeSave();
			auto saved = npcs;
			for (const auto key : unghosting) {
				saved.at(key).ghost = false;
			}
			for (const auto key : ledger.Ids()) {
				if (npcs.at(key).loaded) {
					saved.at(key).ghost = false;
				}
			}
			return saved;
		}

		std::vector<std::uint32_t> SavedLedger() const { return keepLedger ? ledger.Saved() : std::vector<std::uint32_t>{}; }

		// A load of a_saved: no setghost goes out for the shields it ends (WeaponFire::BeforeLoad), the copies are the
		// save's, and its record of our ghosts (a_ledger; none from a save made before the ledger) is written down again
		// (WeaponFire::OnGameLoaded).
		void Load(std::map<std::uint32_t, Npc> a_saved, const std::vector<std::uint32_t>& a_ledger = {})
		{
			shields.EndAll();
			unghosting.clear();
			ledger.Clear();
			repaired.clear();
			npcs = std::move(a_saved);
			ledger.Restore(a_ledger, frame);
		}

		// WeaponFire::TendLedger, each frame: a ghost of ours still written down has setghost 0 sent (again) once its copy
		// is loaded and no shield wants it, and is forgotten once it reads no ghost.
		void TendLedger()
		{
			for (const auto key : ledger.Ids()) {
				const auto& npc = npcs.at(key);
				const auto  id = SessionIdOf(key);
				const auto  step = ledger.Check(key, { .loaded = npc.loaded, .ghost = npc.ghost, .shielded = shields.Shielded(id) }, frame);
				if (step == GhostLedger::Step::kClear) {
					Unshield(id);
				}
			}
		}

		// WeaponFire::RepairGhosts, every REPAIR_EVERY frames: a loaded actor whose ghost looks like one of ours an older
		// save left (BlastShield::Repairs) has it taken off through the ledger, once each since the load.
		void RepairGhosts()
		{
			if (!repairOnLoad || frame % BlastShield::REPAIR_EVERY != 0) {
				return;
			}
			for (const auto& [key, npc] : npcs) {
				const auto               id = SessionIdOf(key);
				const BlastShield::Found found{
					.id = key,
					.dead = npc.dead,
					.ghost = npc.ghost,
					.baseGhost = npc.ownGhost,
					.questAlias = npc.questAlias,
					.ours = ledger.Contains(key) || shields.Shielded(id),
				};
				if (npc.loaded && BlastShield::Repairs(found) && repaired.insert(key).second) {
					ledger.Add(key, frame, true);
					Unshield(id);
				}
			}
		}

		// NpcSync stops mirroring the copy (WeaponFire::EndShield, the session's id).
		void StopMirroring(std::uint32_t a_id)
		{
			if (shields.End(a_id)) {
				Unshield(a_id);
			}
		}

		// Before a save or a load, and when the session ends (WeaponFire::EndAllShields).
		void BeforeSave()
		{
			for (const auto id : shields.EndAll()) {
				Unshield(id);
			}
		}

		// Time goes by in this game, a frame at a time: grenades go off, ghosts whose explosion is over come off, as do
		// those whose NPC is no longer another game's (WeaponFire::Frame).
		void Wait(BlastShield::Clock::duration a_for)
		{
			constexpr BlastShield::Clock::duration FRAME = std::chrono::milliseconds{ 50 };
			const auto                             until = now + a_for;
			while (now < until) {
				now = std::min(now + FRAME, until);
				++frame;
				for (const auto key : std::exchange(unghosting, {})) {
					npcs.at(key).ghost = false;  // the setghost 0 sent last frame lands
				}
				std::erase_if(grenades, [&](const Grenade& a_grenade) {
					if (now < a_grenade.at) {
						return false;
					}
					for (auto& [id, npc] : npcs) {
						if (RemoteEdits::Within(std::hypot(npc.x - a_grenade.x, npc.y - a_grenade.y), FRAG_RADIUS)) {
							Hurt(id, GRENADE_DAMAGE);
						}
					}
					return true;
				});
				for (const auto id : shields.Ids()) {
					if (!BlastShield::Shields(client.OwnerOf(id), client.Id()) && shields.End(id)) {
						Unshield(id);
					}
				}
				for (const auto id : shields.Due(now, frame)) {
					Unshield(id);
				}
				TendLedger();
				RepairGhosts();
				WatchCopies();
			}
		}

		// CopyWatch::Frame: a living copy of an NPC another game runs gets back what it lost with no blow of our
		// player's on it.
		void WatchCopies()
		{
			if (!watchCopies) {
				return;
			}
			for (auto& [key, npc] : npcs) {
				const auto id = SessionIdOf(key);
				const auto owner = client.OwnerOf(id);
				if (npc.dead || owner == 0 || owner == client.Id()) {
					continue;
				}
				const float give = copies.Check(id, npc.health, now);
				if (give > 0.0f) {
					npc.health += give;
					givenBack += give;
					copies.Set(id, npc.health);
				}
			}
		}

		// Something other than our player hurts an NPC here.
		void Hurt(std::uint32_t a_id, float a_damage)
		{
			npcs.at(a_id).Hurt(a_damage);
			Died(a_id);
		}

		// Our player hits an NPC (WorldSync's HitWatcher, then its Frame): a shot that lands on a copy another game
		// runs claims it (which tells the server we fight it) and sends its health; a ghost's passes it by, no hit.
		// The hit's event can be heard a frame after its damage (a_heardLate), and says what it took (its HitData figure).
		// a_key is the NPC's form id here; the book and the wire go by the session's id.
		void OurBlow(std::uint32_t a_key, float a_damage, bool a_heardLate = false)
		{
			auto& npc = npcs.at(a_key);
			if (npc.ghost || npc.ownGhost || npc.dead) {
				return;
			}
			const auto id = SessionIdOf(a_key);
			npc.Hurt(a_damage);
			if (a_heardLate) {
				Wait(std::chrono::milliseconds{ 50 });
			}
			copies.OurHit(id, now, a_damage);
			if (client.OwnerOf(id) != client.Id()) {
				client.Claim(id, Reason::kInteract);
			}
			if (!npc.dead) {
				client.Hurt(id, npc.health / 100.0f);
			}
			Died(a_key);
		}

		// WorldSync::ApplyRemoteHealth and TrySetHealth: another game's report of the NPC's health (a fraction of 100
		// here) under the session's id, set on our NPC (a spawn's copy has a form id of our game's own) and in the
		// book under the session's id, as CopyWatch::HealthSet keeps it (bookByOwnId: under our form id, as before).
		bool bookByOwnId = false;

		void HealthReport(std::uint32_t a_id, float a_fraction)
		{
			const auto key = KeyOf(a_id);
			auto&      npc = npcs.at(key);
			if (!npc.dead) {
				npc.health = a_fraction * 100.0f;
				copies.Set(bookByOwnId ? key : SessionIdOf(key), npc.health);
			}
		}

		// The reports our game has heard for the NPC (a_key, our form id), applied as they come.
		void ApplyHeard(std::uint32_t a_key)
		{
			const auto id = SessionIdOf(a_key);
			if (const auto heard = client.HealthHeard(id)) {
				HealthReport(id, *heard);
			}
		}
	};

	Packet NpcThrow(std::uint32_t a_ref)
	{
		return Protocol::Encode(Protocol::Shot{ 0, a_ref, Protocol::ShotAction::kThrow }, MT::kReportShot);
	}

	Packet PlayerThrow()
	{
		return Protocol::Encode(Protocol::Shot{ 0, 0, Protocol::ShotAction::kThrow }, MT::kReportShot);
	}

	Packet ExplosionAt(float a_x, float a_y)
	{
		return Protocol::Encode(Protocol::Explosion{ 0, FRAG, a_x, a_y, 0.0f, 0, COMMONWEALTH }, MT::kReportExplosion);
	}

	// A host and a friend side by side at Corvega, the host's game running RAIDER_1, the friend's RAIDER_2;
	// RAIDER_3 nobody runs. Both games have every raider at the same spot.
	struct Corvega
	{
		Session s;
		Client& host = s.Host();
		Client& bob = s.Player("Bob");
		Game    hostGame{ host };
		Game    bobGame{ bob };

		Corvega()
		{
			REQUIRE(host.Join());
			REQUIRE(bob.Join());
			host.Move(1000.0f, 2000.0f);
			bob.Move(1100.0f, 2000.0f);
			host.Claim(RAIDER_1);
			bob.Claim(RAIDER_2);
			s.Settle();
			REQUIRE(bob.OwnerOf(RAIDER_1) == host.Id());
			REQUIRE(host.OwnerOf(RAIDER_2) == bob.Id());
			host.RunNpc(RAIDER_1, 1200.0f, 2100.0f);
			bob.RunNpc(RAIDER_2, 1250.0f, 2100.0f);
			s.Settle();
			for (auto* game : { &hostGame, &bobGame }) {
				game->npcs[RAIDER_1] = Npc{ .x = 1200.0f, .y = 2100.0f };
				game->npcs[RAIDER_2] = Npc{ .x = 1250.0f, .y = 2100.0f };
				game->npcs[RAIDER_3] = Npc{ .x = 1150.0f, .y = 2150.0f };
			}
		}

		// The host's game runs RAIDER_4 too, beside RAIDER_1. Each of the two has a_health in both games.
		void SecondHostRaider(float a_health)
		{
			host.Claim(RAIDER_4);
			s.Settle();
			REQUIRE(bob.OwnerOf(RAIDER_4) == host.Id());
			host.RunNpc(RAIDER_4, 1180.0f, 2120.0f);
			s.Settle();
			for (auto* game : { &hostGame, &bobGame }) {
				game->npcs[RAIDER_1].health = a_health;
				game->npcs[RAIDER_4] = Npc{ .x = 1180.0f, .y = 2120.0f, .health = a_health };
			}
		}

		// The host's raider throws: the friend's game hears it and (a_replayed) replays it on its copy.
		void HostRaiderThrows(bool a_replayed = true)
		{
			const auto shotsBefore = bob.Heard(MT::kShotFired);
			host.Raw(NpcThrow(RAIDER_1), false);
			s.Settle();
			REQUIRE(bob.Heard(MT::kShotFired) == shotsBefore + 1);
			if (a_replayed) {
				bobGame.ReplayThrow(RAIDER_1);
			}
		}

		// The host's game reports the explosion; the friend's game sets it off, a_damage to each in reach.
		void HostExplosionReachesBob(float a_damage)
		{
			const auto before = bob.Explosions().size();
			host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
			s.Settle();
			REQUIRE(bob.Explosions().size() == before + 1);
			CHECK(bob.Explosions().back().playerId == host.Id());
			bobGame.SetOff(bob.Explosions().back(), a_damage);
		}
	};

	// The live test's third round: the host's raider (RAIDER_1, 30 health) throws once beside another raider the host's
	// game runs (RAIDER_4, 30 health). The friend's copy replays the throw (a_replayed: the release is kept from the
	// engine, as WeaponFire does now; without, the engine throws the copy's grenade), and its graph lets go of it two
	// and a half seconds later. The owner's explosion arrives a moment after the replay, enough to kill both copies.
	void ThrowOnceBesideTwo(Corvega& c, bool a_replayed)
	{
		c.SecondHostRaider(30.0f);
		c.HostRaiderThrows(a_replayed);
		c.bobGame.Wait(std::chrono::milliseconds{ 150 });
		c.HostExplosionReachesBob(63.0f);
		CHECK(!c.bobGame.npcs[RAIDER_1].dead);
		CHECK(!c.bobGame.npcs[RAIDER_4].dead);
		c.bobGame.Wait(std::chrono::milliseconds{ 2350 });
		c.bobGame.LetGo(RAIDER_1);
		c.bobGame.Wait(std::chrono::seconds{ 10 });
		c.s.Settle();
	}

	// The live test of the ghosts, fourth round: the host's raider (RAIDER_1) threw four times beside RAIDER_4, both
	// with 30 health, and kept it in the host's game. In the friend's game both copies were ghosts for each explosion
	// (their health then the same as before), but between the explosions something there burned RAIDER_1's copy down,
	// 30, 26, 21, 19, 12, 11, until it lay dead at 0. Here a fire burns both copies half a point a frame between the
	// explosions (a ghost is passed by): eighty points in all, more than either has. Returns the least health either
	// copy had along the way.
	float BurnedBetweenFourThrows(Corvega& c)
	{
		c.SecondHostRaider(30.0f);
		float least = 30.0f;
		for (int i = 0; i < 4; ++i) {
			c.HostRaiderThrows();
			c.bobGame.Wait(std::chrono::milliseconds{ 150 });
			c.HostExplosionReachesBob(63.0f);
			for (int tick = 0; tick < 40; ++tick) {
				for (const auto id : { RAIDER_1, RAIDER_4 }) {
					c.bobGame.Hurt(id, 0.5f);
					least = std::min(least, c.bobGame.npcs[id].health);
				}
				c.bobGame.Wait(std::chrono::milliseconds{ 50 });
			}
			c.bobGame.LetGo(RAIDER_1);
		}
		c.bobGame.Wait(std::chrono::seconds{ 1 });
		c.s.Settle();
		return least;
	}
}

TEST("blast shield: only copies of NPCs another game runs are kept from a friend's explosion")
{
	CHECK(BlastShield::Shields(2, 1));   // the friend's (or a third player's) NPC
	CHECK(!BlastShield::Shields(1, 1));  // ours: the explosion is what hurts it here
	CHECK(!BlastShield::Shields(0, 1));  // nobody runs it: hurt here like ours
	CHECK(!BlastShield::Shields(2, 0));  // not in a session (not welcomed yet, or it ended)

	// An actor the explosion reaches, by the id the session knows it by: a plugin's NPC and our copy of a friend's
	// spawn (its spawn id) alike, alive and another game's. Not one never shared (a runtime NPC of our game's own, a
	// stand-in: no id), nor one dead already, nor ours or nobody's.
	using BlastShield::Reached;
	CHECK(BlastShield::Shields(Reached{ .id = RAIDER_1, .owner = 2 }, 1));
	CHECK(BlastShield::Shields(Reached{ .id = SPAWN, .owner = 2 }, 1));
	CHECK(!BlastShield::Shields(Reached{ .id = 0, .owner = 2 }, 1));
	CHECK(!BlastShield::Shields(Reached{ .id = RAIDER_1, .owner = 2, .dead = true }, 1));
	CHECK(!BlastShield::Shields(Reached{ .id = SPAWN, .owner = 1 }, 1));
	CHECK(!BlastShield::Shields(Reached{ .id = SPAWN, .owner = 0 }, 1));
	CHECK(!BlastShield::Shields(Reached{ .id = SPAWN, .owner = 2 }, 0));
}

TEST("blast shield: a host raider's grenade passes by the friend's copy of an enemy the host's game spawned, which the session knows by its spawn id")
{
	// The host's game made a raider at runtime (an ambush) and shares it: the server grants it to the host's game, and
	// the friend's game places a copy under a form id of its own, which the session knows by the spawn id (Spawns). Its
	// own form id is nobody's, so by that it was never a ghost, and the friend's copies of the host's spawned enemies
	// died of every grenade the host's raiders threw.
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	host.Raw(Protocol::Encode(Protocol::Spawn{ 0, SPAWN, RAIDER_BASE, 0, COMMONWEALTH, 1230.0f, 2100.0f, 0.0f, 0.0f }, MT::kReportSpawn));
	c.s.Settle();
	REQUIRE(bob.OwnerOf(SPAWN) == host.Id());
	REQUIRE(host.OwnerOf(SPAWN) == host.Id());
	c.hostGame.npcs[HOST_SPAWN] = Npc{ .x = 1230.0f, .y = 2100.0f, .health = 30.0f, .sessionId = SPAWN };
	c.bobGame.npcs[BOB_COPY] = Npc{ .x = 1230.0f, .y = 2100.0f, .health = 30.0f, .sessionId = SPAWN };

	// The host's raider (RAIDER_1) throws beside it; the host's game sets the grenade off for real, and the spawned
	// raider survives it there with 10 health.
	c.HostRaiderThrows();
	c.hostGame.Hurt(HOST_SPAWN, 20.0f);
	CHECK(!c.hostGame.npcs[HOST_SPAWN].dead);
	c.HostExplosionReachesBob(150.0f);
	CHECK(c.bobGame.npcs[BOB_COPY].ghost);
	CHECK(c.bobGame.shields.Shielded(SPAWN));
	CHECK(!c.bobGame.npcs[BOB_COPY].dead);
	CHECK(c.bobGame.npcs[BOB_COPY].health == 30.0f);  // the host's to hurt; its report of 10 comes as any copy's
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);           // the plugin's raider, as before
	CHECK(c.bobGame.npcs[RAIDER_2].dead);            // the friend's own raider dies of it

	// The ghost comes off once the explosion is over, and the copy's death was never reported: the spawned raider is
	// alive in both games, still the host's.
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(!c.bobGame.npcs[BOB_COPY].ghost);
	CHECK(c.bobGame.shields.Size() == 0);
	c.s.Settle();
	for (const auto player : { &host, &bob }) {
		CHECK(!player->WorldNow().dead.contains(SPAWN));
	}
	CHECK(bob.OwnerOf(SPAWN) == host.Id());
	CHECK(!c.s.Logged("death of FF001234"));

	// By its own form id, nobody's in the session, the copy would not have been a ghost: that was the bug.
	CHECK(bob.OwnerOf(BOB_COPY) == 0);
	CHECK(!BlastShield::Shields(BlastShield::Reached{ .id = BOB_COPY, .owner = bob.OwnerOf(BOB_COPY) }, bob.Id()));

	// The copy stops being mirrored under its spawn id (NpcSync::StopMirroring): a ghost of ours comes off at once.
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(50.0f);
	REQUIRE(c.bobGame.npcs[BOB_COPY].ghost);
	c.bobGame.StopMirroring(SPAWN);
	CHECK(!c.bobGame.npcs[BOB_COPY].ghost);
	CHECK(!c.bobGame.shields.Shielded(SPAWN));
}

TEST("blast shield: a host raider's grenade set off in a friend's game passes by the friend's copy of it, alive as it is in the host's")
{
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	// The fight has worn the friend's copy down to 60.
	c.bobGame.npcs[RAIDER_1].health = 60.0f;

	// The host's raider throws a grenade at its own feet. The host's game sets it off for real (it is not replayed
	// there) and reports the throw and the explosion; the raider survives it there.
	c.HostRaiderThrows();
	c.hostGame.Hurt(RAIDER_1, 20.0f);
	c.HostExplosionReachesBob(150.0f);  // enough to kill anything beside it
	CHECK(RemoteEdits::SetOff(RemoteEdits::SpotOf(bob.Explosions().front()), { 0, COMMONWEALTH, 1100.0f, 2000.0f }, FRAG_RADIUS) == RemoteEdits::Blast::kSetOff);
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(!c.bobGame.npcs[RAIDER_1].dead);           // the host's raider: its owner dealt that damage already
	CHECK(c.bobGame.npcs[RAIDER_1].health == 60.0f);  // not touched: nothing to give back
	CHECK(c.bobGame.npcs[RAIDER_2].dead);             // the friend's own raider dies of it, as in the release notes
	CHECK(c.bobGame.npcs[RAIDER_3].dead);             // so does one nobody runs
	CHECK(!c.bobGame.npcs[RAIDER_2].ghost);
	CHECK(!c.bobGame.npcs[RAIDER_3].ghost);

	// The ghost comes off once the explosion is over.
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.shields.Size() == 0);
	c.s.Settle();

	// The friend's kills reached the host (the friend's game runs RAIDER_2; nobody ran RAIDER_3); the host's raider
	// fights on in both games, still the host's to run.
	for (const auto player : { &host, &bob }) {
		CHECK(!player->WorldNow().dead.contains(RAIDER_1));
		CHECK(player->WorldNow().dead.contains(RAIDER_2));
		CHECK(player->WorldNow().dead.contains(RAIDER_3));
	}
	CHECK(bob.OwnerOf(RAIDER_1) == host.Id());
	CHECK(!c.s.Logged("death of 0002A001"));  // nothing was reported for it to drop
}

TEST("blast shield: a host raider throwing four grenades two seconds apart, each throw replayed by the friend's copy, is alive in both games afterwards")
{
	// The live test: the host's raider 0007BCED (30 health, all of it) threw four times, two seconds apart. The friend's
	// copy replays each throw, its graph letting go of each half a second after the next replay (later than the two
	// seconds the ghost watch once kept), and each explosion is set off beside it; someone's grenade goes off at its
	// feet while it is a ghost too.
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	c.SecondHostRaider(30.0f);
	const float blasts[] = { 70.0f, 60.0f, 66.0f, 55.0f };
	for (int i = 0; i < 4; ++i) {
		c.HostRaiderThrows();
		c.HostExplosionReachesBob(blasts[i]);
		CHECK(!c.bobGame.npcs[RAIDER_1].dead);
		CHECK(c.bobGame.npcs[RAIDER_1].ghost);
		c.bobGame.Wait(std::chrono::milliseconds{ 500 });
		if (i > 0) {
			c.bobGame.LetGo(RAIDER_1);  // the last throw's release, 2.5 s after its replay
		}
		if (i == 1) {
			c.bobGame.Hurt(RAIDER_1, 40.0f);  // someone's grenade, while it is a ghost
		}
		c.bobGame.Wait(std::chrono::milliseconds{ 1500 });
		CHECK(!c.bobGame.npcs[RAIDER_1].ghost);  // between the throws, no ghost: a second's worth each time
	}
	c.bobGame.Wait(std::chrono::milliseconds{ 500 });
	c.bobGame.LetGo(RAIDER_1);
	CHECK(c.bobGame.grenades.empty());  // not one of the copy's own left its hand
	CHECK(c.bobGame.throws.Size() == 0);
	c.bobGame.Wait(BlastShield::HOLD + FUSE);
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(!c.bobGame.npcs[id].dead);
		CHECK(c.bobGame.npcs[id].health == 30.0f);
		CHECK(!c.bobGame.npcs[id].ghost);
	}
	c.s.Settle();
	for (const auto player : { &host, &bob }) {
		CHECK(!player->WorldNow().dead.contains(RAIDER_1));
		CHECK(!player->WorldNow().dead.contains(RAIDER_4));
	}
	CHECK(bob.OwnerOf(RAIDER_1) == host.Id());
	CHECK(!c.s.Logged("death of 0002A001"));
}

TEST("blast shield: a host raider's throw replayed by the friend's copy throws no grenade of the copy's own, however late its graph lets go")
{
	Corvega c;
	ThrowOnceBesideTwo(c, true);
	CHECK(c.bobGame.grenades.empty());
	CHECK(c.bobGame.throws.Size() == 0);
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(!c.bobGame.npcs[id].dead);
		CHECK(c.bobGame.npcs[id].health == 30.0f);
		CHECK(!c.bobGame.npcs[id].ghost);
		for (const auto player : { &c.host, &c.bob }) {
			CHECK(!player->WorldNow().dead.contains(id));
		}
	}
	CHECK(!c.s.Logged("death of 0002A001"));
	CHECK(!c.s.Logged("death of 0002A004"));
}

TEST("blast shield: had the copy's graph let go of the replayed throw to the engine, its own grenade would have killed both copies after their ghosts came off, in the friend's game only")
{
	Corvega c;
	ThrowOnceBesideTwo(c, false);
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(c.bobGame.npcs[id].dead);
		CHECK(c.bob.WorldNow().dead.contains(id));
		CHECK(!c.host.WorldNow().dead.contains(id));
	}
	CHECK(c.s.Logged("death of 0002A001 from player"));
}

TEST("blast shield: without the ghost, the friend's copy would die of the host raider's grenade only in the friend's game")
{
	// What the friend saw before: the replayed grenade killed their copy, its death was reported, and the server
	// rightly dropped it (the host's game runs the raider and the friend wasn't fighting it): a corpse in one game, a
	// raider fighting on in the other.
	Corvega c;
	c.host.Raw(NpcThrow(RAIDER_1), false);
	c.host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(c.bob.Explosions().size() == 1);
	c.bobGame.Hurt(RAIDER_1, 150.0f);  // set off with no ghost
	c.s.Settle();
	CHECK(c.bob.WorldNow().dead.contains(RAIDER_1));
	CHECK(!c.host.WorldNow().dead.contains(RAIDER_1));
	CHECK(c.s.Logged("death of 0002A001 from player"));
}

TEST("blast shield: a friend's own grenade still kills the host's raiders beside them, in their game and the host's")
{
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	// Bob throws. In his own game the grenade is his (not replayed): its blows are his player's, so the host's
	// raider beside it is claimed, its health goes out, and its death after that.
	bob.Raw(PlayerThrow(), false);
	bob.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.bobGame.OurBlow(RAIDER_1, 150.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].dead);
	c.s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));  // the server believes it: Bob was fighting it
	c.hostGame.npcs[RAIDER_1].dead = true;  // WorldSync's kill of it in the host's game, not one it reports
	c.hostGame.reported.insert(RAIDER_1);

	// The host's game sets Bob's explosion off: what the host's game runs is hurt by it there; Bob's raider is his
	// game's to hurt, and it already was.
	REQUIRE(host.Explosions().size() == 1);
	c.hostGame.SetOff(host.Explosions().front(), 150.0f);
	CHECK(!c.hostGame.npcs[RAIDER_2].dead);
	CHECK(c.hostGame.npcs[RAIDER_2].ghost);
	c.bobGame.Hurt(RAIDER_2, 150.0f);  // in Bob's game his grenade kills his own raider too
	c.hostGame.Wait(BlastShield::HOLD);
	CHECK(!c.hostGame.npcs[RAIDER_2].ghost);
	c.s.Settle();
	for (const auto player : { &host, &bob }) {
		CHECK(player->WorldNow().dead.contains(RAIDER_1));
		CHECK(player->WorldNow().dead.contains(RAIDER_2));
	}
}

TEST("blast shield: the player's shots pass a ghosted copy by for the second it lasts, then land, and a kill of theirs is believed")
{
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	c.bobGame.npcs[RAIDER_1].health = 60.0f;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);

	// Bob shoots it at once: a ghost, it is not hit (a second of that, beside a grenade going off, is the price).
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 60.0f);

	// A second later his shots land, and enough of them kill it: his kill, which the server believes.
	c.bobGame.Wait(BlastShield::HOLD);
	REQUIRE(!c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 35.0f);
	c.bobGame.OurBlow(RAIDER_1, 40.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].dead);
	c.s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));
	CHECK(bob.WorldNow().dead.contains(RAIDER_1));
}

TEST("blast shield: a copy that was a ghost of its own story's is left one, and a health report for a ghosted copy is applied as it comes")
{
	Corvega c;
	c.bobGame.npcs[RAIDER_1].ownGhost = true;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	CHECK(!c.bobGame.shields.Shielded(RAIDER_1));  // not ours to take off
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(c.bobGame.npcs[RAIDER_1].ownGhost);
	CHECK(!c.bobGame.npcs[RAIDER_1].dead);

	Corvega d;
	d.HostRaiderThrows();
	d.HostExplosionReachesBob(150.0f);
	REQUIRE(d.bobGame.npcs[RAIDER_1].ghost);
	d.bobGame.HealthReport(RAIDER_1, 0.25f);  // the host shot it meanwhile
	CHECK(d.bobGame.npcs[RAIDER_1].health == 25.0f);
	d.bobGame.Wait(BlastShield::HOLD);
	CHECK(d.bobGame.npcs[RAIDER_1].health == 25.0f);  // nothing to give back
}

TEST("blast shield: a ghost comes off before a save, when its copy stops being mirrored, and when its NPC becomes ours")
{
	Corvega c;
	c.SecondHostRaider(100.0f);
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(50.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	REQUIRE(c.bobGame.npcs[RAIDER_4].ghost);
	c.bobGame.StopMirroring(RAIDER_4);
	CHECK(!c.bobGame.npcs[RAIDER_4].ghost);
	c.bobGame.BeforeSave();
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.shields.Size() == 0);

	// Another explosion, then the host leaves the raider to Bob's game: it is his to hurt from the next frame.
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(50.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	c.host.Release(RAIDER_1);
	c.bob.Claim(RAIDER_1);
	c.s.Settle();
	REQUIRE(c.bob.OwnerOf(RAIDER_1) == c.bob.Id());
	c.bobGame.Wait(std::chrono::milliseconds{ 50 });
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(!c.bobGame.shields.Shielded(RAIDER_1));
}

TEST("blast shield: a save made as a ghost of ours ends holds no ghost, though the console's setghost 0 lands a frame later")
{
	Corvega c;
	c.bobGame.consoleLag = true;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(50.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	const auto saved = c.bobGame.Save();
	CHECK(!saved.at(RAIDER_1).ghost);
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);  // the live copy, its command still to land
	c.bobGame.Wait(std::chrono::milliseconds{ 50 });
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
}

TEST("blast shield: a game loaded from a save holding a ghost of ours on a hostile copy another game runs has it taken off, and the player's shots then land and their kill is believed")
{
	// A 0.8.11 save made mid-fight: the host's raider's copy written as a ghost. Loaded (the friend died), shots passed
	// through it for good, and every later save wrote it again.
	Corvega c;
	c.bobGame.consoleLag = true;
	auto saved = c.bobGame.npcs;
	saved[RAIDER_1].ghost = true;
	saved[RAIDER_1].health = 60.0f;
	c.bobGame.Load(saved);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 60.0f);

	c.bobGame.Wait(std::chrono::seconds{ 1 });
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.unghostsSent == 1);
	c.bobGame.Wait(std::chrono::seconds{ 2 });
	CHECK(c.bobGame.unghostsSent == 1);  // once

	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 35.0f);
	c.bobGame.OurBlow(RAIDER_1, 40.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].dead);
	c.s.Settle();
	for (const auto player : { &c.host, &c.bob }) {
		CHECK(player->WorldNow().dead.contains(RAIDER_1));
	}
}

TEST("blast shield: had a load left the ghosts a save held, the host's raider's copy would have stayed one through the next explosion, the player's shots passing through it")
{
	Corvega c;
	c.bobGame.consoleLag = true;
	c.bobGame.repairOnLoad = false;
	auto saved = c.bobGame.npcs;
	saved[RAIDER_1].ghost = true;
	c.bobGame.Load(saved);
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(50.0f);
	CHECK(!c.bobGame.shields.Shielded(RAIDER_1));  // a ghost already: not ours to clear (WasGhost)
	c.bobGame.Wait(BlastShield::HOLD + std::chrono::seconds{ 5 });
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 100.0f);
	CHECK(c.bobGame.Save().at(RAIDER_1).ghost);  // and the next save writes it again
}

TEST("blast shield: a ghost an older save left comes off after the load whoever runs the NPC and whether or not it is hostile; one its base record or a quest makes, and a dead one, are left")
{
	// A save from before the ledger (no record of our ghosts): the repair scan alone tells ours from the game's own.
	// Before, a friendly copy's, one nobody ran and one our own game had come to run were left ghosts for good.
	Corvega c;
	c.SecondHostRaider(100.0f);
	c.bobGame.consoleLag = true;
	auto saved = c.bobGame.npcs;
	saved[RAIDER_1].ghost = true;
	saved[RAIDER_1].ownGhost = true;  // its base record's: its story's own
	saved[RAIDER_4].ghost = true;
	saved[RAIDER_4].friendly = true;
	saved[RAIDER_3].ghost = true;  // nobody runs it
	saved[RAIDER_2].ghost = true;  // Bob's own game runs it
	c.bobGame.Load(saved);
	c.bobGame.Wait(std::chrono::seconds{ 3 });
	CHECK(c.bobGame.unghostsSent == 3);
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(!c.bobGame.npcs[RAIDER_2].ghost);
	CHECK(!c.bobGame.npcs[RAIDER_3].ghost);
	CHECK(!c.bobGame.npcs[RAIDER_4].ghost);
	CHECK(c.bobGame.ledger.Size() == 0);  // each read as no ghost: forgotten
	CHECK(c.bobGame.ledger.Count().fromSaves == 3);

	// One a quest holds in an alias, and a dead copy, a ghost in the save, are left too.
	Corvega d;
	auto quest = d.bobGame.npcs;
	quest[RAIDER_1].ghost = true;
	quest[RAIDER_1].questAlias = true;
	quest[RAIDER_2].ghost = true;
	quest[RAIDER_2].dead = true;
	d.bobGame.Load(quest);
	d.bobGame.Wait(std::chrono::seconds{ 3 });
	CHECK(d.bobGame.unghostsSent == 0);
	CHECK(d.bobGame.npcs[RAIDER_1].ghost);
	CHECK(d.bobGame.npcs[RAIDER_2].ghost);

	// A ghost taken off once that a script sets again is the script's: left alone from then on (once per actor per load).
	c.bobGame.npcs[RAIDER_3].ghost = true;
	c.bobGame.Wait(std::chrono::seconds{ 3 });
	CHECK(c.bobGame.npcs[RAIDER_3].ghost);
	CHECK(c.bobGame.unghostsSent == 3);
}

TEST("blast shield: a copy that leaves the loaded world inside the shield's second, and is saved a ghost, is no ghost again once it loads, in this run or after a load of that save")
{
	// A door, a fast travel or a despawn between the grenade and its second: the copy had no 3D for its setghost 0.
	// Before, the setghost 0 was sent only if the copy loaded again in the same run, and the save (its flag could not be
	// hidden on an unloaded copy) brought it back a ghost for good.
	Corvega c;
	c.bobGame.consoleLag = true;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.npcs[RAIDER_1].loaded = false;
	c.bobGame.Wait(BlastShield::HOLD + std::chrono::seconds{ 1 });
	CHECK(c.bobGame.shields.Size() == 0);
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);  // no command could take: still written down
	CHECK(c.bobGame.ledger.Contains(RAIDER_1));
	CHECK(c.bobGame.unghostsSent == 0);

	// The save, made while it is away, holds the ghost and the ledger's record of it.
	const auto saved = c.bobGame.Save();
	const auto record = c.bobGame.SavedLedger();
	CHECK(saved.at(RAIDER_1).ghost);
	CHECK(record == std::vector<std::uint32_t>{ RAIDER_1 });

	// In this run it comes back: no ghost within a frame or two.
	c.bobGame.npcs[RAIDER_1].loaded = true;
	c.bobGame.Wait(std::chrono::milliseconds{ 150 });
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.ledger.Size() == 0);
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 75.0f);

	// The save loaded (the friend died and loads their last save): the copy is the save's, a ghost, and its owner is now
	// Bob's own game (the host let it go meanwhile). It comes off all the same, and Bob's shots land.
	Corvega d;
	d.bobGame.consoleLag = true;
	d.host.Release(RAIDER_1);
	d.bob.Claim(RAIDER_1);
	d.s.Settle();
	REQUIRE(d.bob.OwnerOf(RAIDER_1) == d.bob.Id());
	d.bobGame.Load(saved, record);
	REQUIRE(d.bobGame.npcs[RAIDER_1].ghost);
	CHECK(d.bobGame.ledger.Contains(RAIDER_1));
	d.bobGame.npcs[RAIDER_1].loaded = false;  // not in the loaded cells yet
	d.bobGame.Wait(std::chrono::seconds{ 2 });
	CHECK(d.bobGame.npcs[RAIDER_1].ghost);
	d.bobGame.npcs[RAIDER_1].loaded = true;
	d.bobGame.Wait(std::chrono::milliseconds{ 150 });
	CHECK(!d.bobGame.npcs[RAIDER_1].ghost);
	CHECK(d.bobGame.unghostsSent == 1);
	d.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(d.bobGame.npcs[RAIDER_1].health == 75.0f);
}

TEST("blast shield: without the ledger, the copy saved a ghost while away would have loaded a ghost and stayed one with the repair for a hostile copy another game runs")
{
	Corvega c;
	c.bobGame.consoleLag = true;
	c.bobGame.keepLedger = false;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	c.bobGame.npcs[RAIDER_1].loaded = false;
	c.bobGame.Wait(BlastShield::HOLD + std::chrono::seconds{ 1 });
	const auto saved = c.bobGame.Save();
	CHECK(saved.at(RAIDER_1).ghost);
	CHECK(c.bobGame.SavedLedger().empty());

	// Loaded into a game that runs the raider itself: the old repair (another game's, hostile) never touched it; the new
	// one does, as a ghost no base record or quest made.
	Corvega d;
	d.bobGame.consoleLag = true;
	d.host.Release(RAIDER_1);
	d.bob.Claim(RAIDER_1);
	d.s.Settle();
	d.bobGame.Load(saved);
	d.bobGame.npcs[RAIDER_1].loaded = true;
	d.bobGame.Wait(std::chrono::seconds{ 2 });
	CHECK(!d.bobGame.npcs[RAIDER_1].ghost);
	CHECK(d.bobGame.ledger.Count().fromSaves == 1);
}

TEST("blast shield: a ghost the engine will not let go of is sent setghost 0 again until it goes, and the player's shots land after")
{
	// The rig: a setghost 1 sent in the frame a save began landed after the setghost 0 that followed it. Before, three
	// tries and then the copy was left a ghost for good.
	Corvega c;
	c.bobGame.consoleLag = true;
	c.bobGame.npcs[RAIDER_1].stubborn = 5;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.unghostsSent == 1);
	c.bobGame.Wait(std::chrono::seconds{ 10 });
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.unghostsSent == 6);
	CHECK(c.bobGame.ledger.Count().stuck == 1);
	CHECK(c.bobGame.ledger.Count().unstuck == 1);
	CHECK(c.bobGame.ledger.Size() == 0);
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 75.0f);

	// A ghost of ours still to come off is ours to make a ghost again for the next explosion (not the game's own).
	c.bobGame.npcs[RAIDER_1].stubborn = 2;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.Wait(BlastShield::HOLD + std::chrono::milliseconds{ 100 });
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);  // stubborn still
	CHECK(c.bobGame.ledger.Contains(RAIDER_1));
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(150.0f);
	CHECK(c.bobGame.shields.Shielded(RAIDER_1));  // shielded again, not left alone as a story's ghost
	c.bobGame.Wait(std::chrono::seconds{ 5 });
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 75.0f);
}

TEST("blast shield: a friend's explosion with damage short of the copy's health makes no ghost: the copy takes the blast as the owner's blow and is given it back, and the player's shots land meanwhile")
{
	// A friend's explosive rounds (15 a round, a dozen a second from an automatic): before, every round made the copies
	// around its target ghosts for a second, so the other player's shots passed through them for as long as the friend
	// kept firing.
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	c.bobGame.Wait(std::chrono::milliseconds{ 50 });  // the copies are first seen at 100
	c.HostRaiderThrows();
	host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(bob.Explosions().size() == 1);
	c.bobGame.SetOff(bob.Explosions().back(), 15.0f, true);
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(!c.bobGame.shields.Shielded(RAIDER_1));
	CHECK(c.bobGame.npcs[RAIDER_1].health == 85.0f);
	CHECK(c.bobGame.npcs[RAIDER_2].health == 85.0f);  // Bob's own raider: hurt for real

	// Bob's shot lands at once, and is his (the explosion's blow is the owner's, given back, not his).
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 60.0f);
	c.bobGame.Wait(std::chrono::milliseconds{ 500 });
	CHECK(c.bobGame.npcs[RAIDER_1].health == 75.0f);  // the blast's 15 given back; his 25 kept
	CHECK(c.bobGame.givenBack == 15.0f);

	// One with less health than twice the damage is a ghost as before: the blast could kill it.
	c.bobGame.npcs[RAIDER_1].health = 30.0f;
	c.bobGame.copies.Set(RAIDER_1, 30.0f);
	host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	c.bobGame.SetOff(bob.Explosions().back(), 15.0f, true);
	CHECK(c.bobGame.npcs[RAIDER_1].ghost);
	CHECK(c.bobGame.npcs[RAIDER_1].health == 30.0f);
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
}

TEST("blast shield: a load sends no setghost for the ghosts of ours it ends, and the save's copies are as it holds them")
{
	Corvega c;
	c.bobGame.consoleLag = true;
	const auto saved = c.bobGame.npcs;
	c.HostRaiderThrows();
	c.HostExplosionReachesBob(50.0f);
	REQUIRE(c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.Load(saved);
	CHECK(c.bobGame.shields.Size() == 0);
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
	c.bobGame.Wait(std::chrono::seconds{ 2 });
	CHECK(c.bobGame.unghostsSent == 0);
	CHECK(!c.bobGame.npcs[RAIDER_1].ghost);
}

TEST("blast shield: a loaded actor's ghost is taken off only when it looks like one of ours an older save left: a plugin's reference, alive, its base no ghost, no quest holding it, not ours now")
{
	using BlastShield::Found;
	using BlastShield::Repairs;
	const Found raider{ .id = RAIDER_1, .ghost = true };
	CHECK(Repairs(raider));
	CHECK(!Repairs({ .id = RAIDER_1, .ghost = false }));
	CHECK(!Repairs({ .id = 0, .ghost = true }));
	CHECK(!Repairs({ .id = BOB_COPY, .ghost = true }));  // a runtime actor: never saved as a ghost of ours
	CHECK(!Repairs({ .id = RAIDER_1, .dead = true, .ghost = true }));
	CHECK(!Repairs({ .id = RAIDER_1, .ghost = true, .baseGhost = true }));
	CHECK(!Repairs({ .id = RAIDER_1, .ghost = true, .questAlias = true }));
	CHECK(!Repairs({ .id = RAIDER_1, .ghost = true, .ours = true }));  // the ledger's, or shielded now: cleared that way
}

TEST("blast shield: a copy is made a ghost only when the explosion's damage comes near its health, or either is not known")
{
	using BlastShield::Lethal;
	using BlastShield::Reached;
	CHECK(Lethal(100.0f, 151.0f));  // a frag grenade beside a raider
	CHECK(Lethal(300.0f, 151.0f));  // within the margin: resistances and the engine's own figures are not read here
	CHECK(!Lethal(303.0f, 151.0f));
	CHECK(!Lethal(100.0f, 15.0f));  // explosive rounds
	CHECK(Lethal(30.0f, 15.0f));
	CHECK(Lethal(100.0f, 0.0f));   // damage not known (a trap's grenades, a death blast)
	CHECK(Lethal(-1.0f, 15.0f));   // health not read
	CHECK(BlastShield::Shields(Reached{ .id = RAIDER_1, .owner = 2, .health = 100.0f }, 1, 15.0f) == false);
	CHECK(BlastShield::Shields(Reached{ .id = RAIDER_1, .owner = 2, .health = 100.0f }, 1, 151.0f));
	CHECK(BlastShield::Shields(Reached{ .id = RAIDER_1, .owner = 2, .health = 100.0f }, 1));  // damage not given: as before
	CHECK(!BlastShield::Shields(Reached{ .id = RAIDER_1, .owner = 1, .health = 10.0f }, 1, 151.0f));  // ours: never
}

TEST("blast shield: a ghost lasts its second and its frames, longer for another explosion, and only so many at once")
{
	using BlastShield::Book;
	const BlastShield::Clock::time_point t0{};
	Book book;
	CHECK(book.Begin(RAIDER_1, t0, 100));
	CHECK(!book.Begin(RAIDER_1, t0 + std::chrono::milliseconds{ 500 }, 105));  // a second explosion: held longer
	CHECK(book.Due(t0 + BlastShield::HOLD, 200).empty());
	CHECK(book.Due(t0 + std::chrono::milliseconds{ 500 } + BlastShield::HOLD, 105 + BlastShield::HOLD_FRAMES - 1).empty());  // a hitch: frames still to come
	CHECK(book.Due(t0 + std::chrono::milliseconds{ 500 } + BlastShield::HOLD, 105 + BlastShield::HOLD_FRAMES) == std::vector<std::uint32_t>{ RAIDER_1 });
	CHECK(book.Size() == 0);

	CHECK(book.Begin(RAIDER_2, t0, 0));
	CHECK(book.End(RAIDER_2));
	CHECK(!book.End(RAIDER_2));
	CHECK(book.Begin(RAIDER_3, t0, 0));
	CHECK(book.EndAll() == std::vector<std::uint32_t>{ RAIDER_3 });
	CHECK(book.Size() == 0);

	for (std::uint32_t i = 0; i < BlastShield::MAX_SHIELDED; ++i) {
		CHECK(book.Begin(0x0003A000 + i, t0, 0));
	}
	CHECK(!book.Begin(0x0002B001, t0, 0));
	CHECK(book.Size() == BlastShield::MAX_SHIELDED);
}

TEST("blast shield: a replayed throw's release is kept once per replay, within the window, and only the replayed actor's")
{
	using ThrowReplays::Book;
	const ThrowReplays::Clock::time_point t0{};
	using std::chrono::milliseconds;
	Book book;
	CHECK(!book.Release(RAIDER_1, t0));  // its own throw: nothing replayed
	book.Arm(RAIDER_1, t0);
	CHECK(book.Unreleased(RAIDER_1));
	CHECK(!book.Release(RAIDER_2, t0 + milliseconds{ 600 }));  // another actor's
	const auto first = book.Release(RAIDER_1, t0 + milliseconds{ 2500 });
	REQUIRE(first);
	CHECK(*first == milliseconds{ 2500 });
	CHECK(!book.Release(RAIDER_1, t0 + milliseconds{ 2600 }));  // one per replay
	CHECK(!book.Unreleased(RAIDER_1));

	// Two throws replayed before the first is let go of: both releases are kept, each against its own replay.
	book.Arm(RAIDER_1, t0);
	book.Arm(RAIDER_1, t0 + milliseconds{ 2000 });
	CHECK(book.Release(RAIDER_1, t0 + milliseconds{ 2500 }) == std::optional{ ThrowReplays::Clock::duration{ milliseconds{ 2500 } } });
	CHECK(book.Release(RAIDER_1, t0 + milliseconds{ 4500 }) == std::optional{ ThrowReplays::Clock::duration{ milliseconds{ 2500 } } });
	CHECK(book.Size() == 0);

	// A release long after its replay is the actor's own (the replay's was refused or never came).
	book.Arm(RAIDER_1, t0);
	CHECK(!book.Release(RAIDER_1, t0 + ThrowReplays::WINDOW + milliseconds{ 1 }));
	CHECK(book.Size() == 0);
	book.Arm(RAIDER_2, t0);
	book.Expire(t0 + ThrowReplays::WINDOW);
	CHECK(book.Size() == 1);
	book.Expire(t0 + ThrowReplays::WINDOW + milliseconds{ 1 });
	CHECK(book.Size() == 0);
	book.Arm(RAIDER_3, t0);
	book.Forget(RAIDER_3);
	CHECK(!book.Unreleased(RAIDER_3));
}

TEST("copy health: the friend's copies of the host's raiders, burned between the explosions by something in the friend's world, are given back what they lost and live on as in the host's")
{
	Corvega c;
	const float least = BurnedBetweenFourThrows(c);
	CHECK(least >= 28.0f);  // given back a tenth of a second after each burn, never worn down
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(!c.bobGame.npcs[id].dead);
		CHECK(c.bobGame.npcs[id].health == 30.0f);
		CHECK(!c.bob.WorldNow().dead.contains(id));
		CHECK(!c.host.WorldNow().dead.contains(id));
	}
	CHECK(c.bobGame.givenBack > 0.0f);
	CHECK(!c.s.Logged("death of 0002A001 from player"));
}

TEST("copy health: without the copies watched, the burn would have killed the friend's copy of the host's raider in the friend's game only")
{
	Corvega c;
	c.bobGame.watchCopies = false;
	BurnedBetweenFourThrows(c);
	CHECK(c.bobGame.npcs[RAIDER_1].dead);
	CHECK(c.bob.WorldNow().dead.contains(RAIDER_1));
	CHECK(!c.host.WorldNow().dead.contains(RAIDER_1));
	CHECK(c.s.Logged("death of 0002A001 from player"));
}

TEST("copy health: the player's own blows on a copy are kept, one heard a frame after its damage among them, and a kill of theirs is believed")
{
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	c.bobGame.Wait(std::chrono::milliseconds{ 50 });
	c.bobGame.OurBlow(RAIDER_1, 25.0f, true);
	c.bobGame.Wait(std::chrono::seconds{ 1 });
	CHECK(c.bobGame.npcs[RAIDER_1].health == 75.0f);
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	c.bobGame.Wait(std::chrono::seconds{ 3 });
	CHECK(c.bobGame.npcs[RAIDER_1].health == 50.0f);
	CHECK(c.bobGame.givenBack == 0.0f);

	// The host's game still runs it (Bob fights it, which the server knows): a fire in Bob's game three seconds
	// after his last blow is not his, and is given back.
	c.s.Settle();
	REQUIRE(bob.OwnerOf(RAIDER_1) == host.Id());
	c.bobGame.Hurt(RAIDER_1, 10.0f);
	c.bobGame.Wait(std::chrono::seconds{ 1 });
	CHECK(c.bobGame.npcs[RAIDER_1].health == 50.0f);
	CHECK(c.bobGame.givenBack == 10.0f);
	c.bobGame.OurBlow(RAIDER_1, 50.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].dead);
	c.s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));
	CHECK(bob.WorldNow().dead.contains(RAIDER_1));
}

namespace
{
	// Health that went through a report: a fraction of the maximum on the wire.
	bool Near(float a_health, float a_expected)
	{
		return std::abs(a_health - a_expected) < 0.01f;
	}

	// The host's game spawned a raider (100 health) and shares it; the friend's game has a copy under a form id of its
	// own. A spawn is run by the game that made it for good, so the friend's copy stays a copy however they fight it.
	struct SpawnedRaider
	{
		Corvega c;

		SpawnedRaider()
		{
			c.host.Raw(Protocol::Encode(Protocol::Spawn{ 0, SPAWN, RAIDER_BASE, 0, COMMONWEALTH, 1230.0f, 2100.0f, 0.0f, 0.0f }, MT::kReportSpawn));
			c.s.Settle();
			REQUIRE(c.bob.OwnerOf(SPAWN) == c.host.Id());
			c.hostGame.npcs[HOST_SPAWN] = Npc{ .x = 1230.0f, .y = 2100.0f, .sessionId = SPAWN };
			c.bobGame.npcs[BOB_COPY] = Npc{ .x = 1230.0f, .y = 2100.0f, .sessionId = SPAWN };
			c.bobGame.Wait(std::chrono::milliseconds{ 100 });  // the copy is first seen at 100
		}

		// The host shoots it for 30 in the host's game; the friend's game applies the report and goes on a second.
		void HostShoots()
		{
			c.hostGame.OurBlow(HOST_SPAWN, 30.0f);
			c.s.Settle();
			REQUIRE(c.bob.HealthHeard(SPAWN).has_value());
			c.bobGame.ApplyHeard(BOB_COPY);
			c.bobGame.Wait(std::chrono::seconds{ 1 });
		}

		// Then the friend shoots their copy for 10; the host's game applies the report.
		void FriendShoots()
		{
			c.bobGame.OurBlow(BOB_COPY, 10.0f);
			c.s.Settle();
			c.hostGame.ApplyHeard(HOST_SPAWN);
			c.hostGame.Wait(std::chrono::seconds{ 1 });
			c.bobGame.Wait(std::chrono::seconds{ 1 });
		}
	};
}

TEST("copy health: a friend's copy of an enemy the host's game spawned keeps the host's shots from their reports, and the friend's shot then hurts it in the host's game too")
{
	SpawnedRaider r;
	r.HostShoots();
	CHECK(r.c.hostGame.npcs[HOST_SPAWN].health == 70.0f);
	CHECK(Near(r.c.bobGame.npcs[BOB_COPY].health, 70.0f));  // as in the host's game
	CHECK(r.c.bobGame.givenBack == 0.0f);

	r.FriendShoots();
	CHECK(Near(r.c.bobGame.npcs[BOB_COPY].health, 60.0f));
	CHECK(Near(r.c.hostGame.npcs[HOST_SPAWN].health, 60.0f));  // both players' shots count
	CHECK(r.c.bobGame.givenBack == 0.0f);
	CHECK(r.c.bob.OwnerOf(SPAWN) == r.c.host.Id());

	// Shot down by both in turn, it dies in both games.
	for (int i = 0; i < 3 && !r.c.hostGame.npcs[HOST_SPAWN].dead; ++i) {
		r.HostShoots();
		if (!r.c.bobGame.npcs[BOB_COPY].dead && !r.c.hostGame.npcs[HOST_SPAWN].dead) {
			r.FriendShoots();
		}
	}
	CHECK(r.c.hostGame.npcs[HOST_SPAWN].dead);
	r.c.s.Settle();
	CHECK(r.c.bob.WorldNow().dead.contains(SPAWN));
}

TEST("copy health: with the report kept under the copy's own form id, the friend's copy of the host's spawned enemy was given the host's shots back, and the friend's shot healed it in the host's game")
{
	SpawnedRaider r;
	r.c.bobGame.bookByOwnId = true;
	r.HostShoots();
	CHECK(r.c.hostGame.npcs[HOST_SPAWN].health == 70.0f);
	CHECK(Near(r.c.bobGame.npcs[BOB_COPY].health, 100.0f));  // immortal to the friend
	CHECK(Near(r.c.bobGame.givenBack, 30.0f));

	r.FriendShoots();
	CHECK(Near(r.c.hostGame.npcs[HOST_SPAWN].health, 90.0f));  // healed from 70 by the friend's shot
}

TEST("copy health: a copy's drop is judged after a tenth of a second, kept around our player's hit, and set by a report")
{
	using CopyHealth::Book;
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	Book book;

	// First seen: where it stands. Losing less than rounding is kept (gaining with no report is taken back:
	// CopyGainsTests.cpp).
	CHECK(book.Check(RAIDER_1, 30.0f, t0) == 0.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 30.0f });
	CHECK(book.Check(RAIDER_1, 29.97f, t0 + milliseconds{ 100 }) == 0.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 29.97f });
	book.Set(RAIDER_1, 30.0f);

	// A drop with no hit of ours: given back once it has stood a tenth of a second, all of it.
	CHECK(book.Check(RAIDER_1, 26.0f, t0 + milliseconds{ 1000 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 25.0f, t0 + milliseconds{ 1050 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 24.0f, t0 + milliseconds{ 1000 } + CopyHealth::JUDGE_AFTER) == 6.0f);
	book.Set(RAIDER_1, 30.0f);

	// Our player's hit, heard before the drop or a frame after it: kept.
	book.OurHit(RAIDER_1, t0 + milliseconds{ 2000 });
	CHECK(book.Check(RAIDER_1, 20.0f, t0 + milliseconds{ 2050 }) == 0.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 20.0f });
	CHECK(book.Check(RAIDER_1, 15.0f, t0 + milliseconds{ 5000 }) == 0.0f);
	book.OurHit(RAIDER_1, t0 + milliseconds{ 5050 });
	CHECK(book.Check(RAIDER_1, 15.0f, t0 + milliseconds{ 5100 }) == 0.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 15.0f });

	// Long after our hit, a drop is something else's again.
	CHECK(book.Check(RAIDER_1, 10.0f, t0 + milliseconds{ 8000 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 10.0f, t0 + milliseconds{ 8000 } + CopyHealth::JUDGE_AFTER) == 5.0f);

	// A report sets where it stands, lower or higher, and nothing is given back for it.
	book.Set(RAIDER_1, 5.0f);
	CHECK(book.Check(RAIDER_1, 5.0f, t0 + milliseconds{ 9000 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 5.0f, t0 + milliseconds{ 9500 }) == 0.0f);

	// A hit of ours before it was first seen counts too; a copy no longer watched is forgotten.
	book.OurHit(RAIDER_2, t0);
	CHECK(!book.Health(RAIDER_2));
	CHECK(book.Check(RAIDER_2, 50.0f, t0) == 0.0f);
	CHECK(book.Check(RAIDER_2, 40.0f, t0 + milliseconds{ 500 }) == 0.0f);
	CHECK(book.Check(RAIDER_2, 40.0f, t0 + milliseconds{ 900 }) == 0.0f);
	CHECK(book.Size() == 2);
	book.Prune([](std::uint32_t a_id) { return a_id == RAIDER_2; });
	CHECK(book.Size() == 1);
	book.Forget(RAIDER_2);
	CHECK(book.Size() == 0);
	for (std::uint32_t i = 0; i < CopyHealth::MAX_COPIES + 1; ++i) {
		book.Set(0x0003A000 + i, 10.0f);
	}
	CHECK(book.Size() == CopyHealth::MAX_COPIES);
}
