// Hits on our copy of an NPC another game runs by the actors around our player: our companion, the NPCs our game runs,
// the turrets built at a settlement. Each hit is to count in one game only, the game that runs whoever struck it
// (CopyHealth::Judge). There it is reported to the NPC's runner as our player's hits are (WorldSync's health report, and
// for a blow that kills, its claim, so that the server believes the kill: CopyHealth::After), it is kept on our copy
// rather than given back (CopyHealth::Book, as CopyWatch keeps it), and a companion's kill is our player's
// (CopyHealth::Credited). The sessions go through the real server (tests/sim/Sim.h), the reports encoded and decoded by
// Protocol as the games send them; each game is a model of its world (Game below) whose actors are judged by those rules
// with what the server told that game about who runs what. Each test names what the players would see go wrong if it
// failed.

#include "sim/Sim.h"

#include "game/CopyHealth.h"

#include <cmath>
#include <map>
#include <set>

using namespace Sim;
using namespace std::chrono_literals;
using Reason = Protocol::ClaimReason;
using CopyHealth::Hitter;

namespace
{
	constexpr std::uint32_t RAIDER_1 = 0x0002A001;
	constexpr std::uint32_t RAIDER_2 = 0x0002A002;
	constexpr std::uint32_t SETTLER = 0x0002A0F1;
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t SANCTUARY = 0x000250FE;  // a workbench
	constexpr std::uint32_t TURRET = 0x000B6E85;     // a machine-gun turret: an actor
	constexpr std::uint32_t FIRE = 0;                // what a grenade left burning: a hit with no cause
	constexpr std::uint32_t PLAYER = Protocol::PLAYER_REF_ID;

	constexpr auto FRAME = std::chrono::milliseconds{ 50 };

	// What our hits took off the copy, sent now (CopyHealth::Book::TakeOurs): the amount, 0 for nothing.
	float Took(CopyHealth::Book& a_book, std::uint32_t a_id, CopyHealth::Clock::time_point a_now)
	{
		const auto sent = a_book.TakeOurs(a_id, a_now);
		return sent ? sent->amount : 0.0f;
	}

	// Health that went through a report: a fraction of the maximum on the wire.
	bool Near(float a_health, float a_expected)
	{
		return std::abs(a_health - a_expected) < 0.01f;
	}

	// An NPC every game has, as one game has it.
	struct Npc
	{
		float health = 100.0f;
		bool  dead = false;
	};

	// One player's game: the NPCs every game has, and the actors only it has (made at runtime: a turret built here, the
	// copy of a friend's, a robot companion, a random encounter; and the friends' stand-ins), with those that follow our
	// player. A blow (Strike) lands as the engine deals it and is then taken as CopyWatch and WorldSync take it a frame
	// later; another game's reports are applied as WorldSync::TrySetHealth does (Apply); CopyWatch gives a copy back
	// what something else took (Wait).
	struct Game
	{
		Client&                        client;
		std::map<std::uint32_t, Npc>   npcs;
		std::set<std::uint32_t>        runtime;     // actors made at runtime here
		std::set<std::uint32_t>        standIns;    // friends' stand-ins here
		std::set<std::uint32_t>        companions;  // they follow our player here
		std::set<std::uint32_t>        spawns;      // ... of those made here, the ones shared as spawns (Spawns::OwnIdOf, once granted)
		bool                           spawnsOurs = true;   // a spawn our game runs hits for us (false: up to 0.14.0-rc1, for nobody)
		bool                           onlyPlayer = false;  // as before: only our player's blows were ours
		bool                           claimKills = true;   // a killing blow by another actor of ours claims its NPC
		bool                           fallsSent = true;    // a killing blow of ours on a copy goes as damage (CopyHealth::Book::Fell)
		bool                           longFall = false;    // a death here is reported at the end of a long fall (Land), not at once
		std::map<std::uint32_t, bool>  falling;             // ... the NPCs falling, and whether the kill is our player's
		CopyHealth::Book               copies;
		CopyHealth::Clock::time_point  now{};
		float                          givenBack = 0.0f;
		std::uint32_t                  reportsSent = 0;
		std::set<std::uint32_t>        deathsSent;
		std::map<std::uint32_t, float> applied;  // the last report applied to each NPC

		// CopyWatch::CauseOf, from what this game knows.
		CopyHealth::Cause CauseOf(std::uint32_t a_id) const
		{
			CopyHealth::Cause cause;
			if (a_id == PLAYER) {
				cause.actor = true;
				cause.player = true;
				return cause;
			}
			if (!npcs.contains(a_id) && !runtime.contains(a_id) && !standIns.contains(a_id)) {
				return cause;  // nothing, or gone
			}
			cause.actor = true;
			cause.standIn = standIns.contains(a_id);
			cause.shareable = Protocol::IsShareableRef(a_id);
			cause.owner = cause.shareable ? client.OwnerOf(a_id) : 0;
			cause.ourCompanion = companions.contains(a_id);
			cause.ourSpawn = spawnsOurs && !cause.shareable && !cause.standIn && spawns.contains(a_id) && client.OwnerOf(a_id) == client.Id();
			return cause;
		}

		Hitter Judge(std::uint32_t a_by) const
		{
			if (onlyPlayer) {
				return a_by == PLAYER ? Hitter::kOurPlayer : Hitter::kNotOurs;
			}
			return CopyHealth::Judge(CauseOf(a_by), client.Id());
		}

		// a_by deals a_damage to a_target here; a frame goes by.
		void Strike(std::uint32_t a_by, std::uint32_t a_target, float a_damage)
		{
			auto& npc = npcs.at(a_target);
			if (npc.dead) {
				return;
			}
			npc.health -= a_damage;
			npc.dead = npc.health <= 0.0f;
			// CopyWatch::Frame: a hit of ours on a copy is kept.
			const auto hitter = Judge(a_by);
			if (hitter != Hitter::kNotOurs) {
				copies.OurHit(a_target, now);
			}
			// WorldSync::Frame: the claim goes this frame, the death or the health after it.
			const auto local = client.Id();
			const auto after = CopyHealth::After(hitter, client.OwnerOf(a_target), local, npc.dead);
			if (after.claim && (claimKills || hitter == Hitter::kOurPlayer)) {
				client.Claim(a_target, Reason::kInteract);
			}
			if (npc.dead) {
				// CopyWatch::Frame: the blow that killed a copy, if ours, goes to the NPC's game as damage (Book::Fell). What
				// the book took of the earlier blows here was sent with them (below), so it is passed by.
				const auto owner = client.OwnerOf(a_target);
				if (fallsSent && hitter != Hitter::kNotOurs && owner != 0 && owner != local) {
					(void)copies.TakeOurs(a_target, now);
					copies.Fell(a_target, npc.health, now);
					if (const auto sent = copies.TakeOurs(a_target, now)) {
						client.Damage(a_target, std::clamp(sent->amount / 100.0f, 0.0f, 1.0f));
					}
				}
				const bool credited = onlyPlayer ? a_by == PLAYER : CopyHealth::Credited(CauseOf(a_by), local);
				if (longFall) {
					falling.try_emplace(a_target, credited);
				} else if (deathsSent.insert(a_target).second) {
					client.Died(a_target, credited);
				}
			} else if (after.report) {
				// On a copy what the blow took goes as damage (CopyWatch's book), for the game that runs it to take off; on
				// one ours runs, or nobody does, its health.
				const auto owner = client.OwnerOf(a_target);
				if (owner != 0 && owner != local) {
					client.Damage(a_target, a_damage / 100.0f);
				} else {
					client.Hurt(a_target, npc.health / 100.0f);
				}
				++reportsSent;
			}
			Wait(FRAME);
		}

		// The falls are over: each death event comes, and is reported (WorldSync's DeathWatcher).
		void Land()
		{
			for (const auto& [id, credited] : std::exchange(falling, {})) {
				if (deathsSent.insert(id).second) {
					client.Died(id, credited);
				}
			}
		}

		// Frames go by: a living copy of an NPC another game runs gets back what it lost to no hit of ours.
		void Wait(CopyHealth::Clock::duration a_for)
		{
			const auto until = now + a_for;
			while (now < until) {
				now = std::min(now + CopyHealth::Clock::duration{ FRAME }, until);
				for (auto& [id, npc] : npcs) {
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
		}

		// The deaths and health reports the session passed on since the last time.
		void Apply()
		{
			for (auto& [id, npc] : npcs) {
				if (client.WorldNow().dead.contains(id)) {
					npc.dead = true;
					continue;
				}
				const auto heard = client.HealthHeard(id);
				if (heard && (!applied.contains(id) || applied[id] != *heard)) {
					applied[id] = *heard;
					npc.health = *heard * 100.0f;
					copies.Set(id, npc.health);
				}
			}
			// A friend's damage on one our game runs is taken off (WorldSync::ApplyRemoteDamage), and where it stands
			// then is shared (HealthShare).
			for (const auto& damage : client.TakeDamageHeard()) {
				const auto it = npcs.find(damage.refId);
				if (it == npcs.end() || it->second.dead || client.OwnerOf(damage.refId) != client.Id()) {
					continue;
				}
				it->second.health -= damage.health * 100.0f;
				// The last of its health: it dies here, its runner's game, of nobody's blow (the damage names no attacker). A
				// robot falls for 10 s here too before its death event comes (Land).
				if (it->second.health <= 0.0f) {
					it->second.dead = true;
					if (longFall) {
						falling.try_emplace(damage.refId, false);
					} else if (deathsSent.insert(damage.refId).second) {
						client.Died(damage.refId, false);
					}
					continue;
				}
				client.Hurt(damage.refId, it->second.health / 100.0f);
			}
		}
	};

	// The host and Bob side by side: the host's game runs RAIDER_1 and SETTLER, Bob's runs RAIDER_2 and his companion
	// Dogmeat. Every game has all four, at full health.
	struct Fight
	{
		Session s;
		Client& host = s.Host();
		Client& bob = s.Player("Bob");
		Game    hostGame{ host };
		Game    bobGame{ bob };

		explicit Fight(Server::Options a_options = {}) :
			s(a_options)
		{
			REQUIRE(host.Join());
			REQUIRE(bob.Join());
			host.Move(1000.0f, 2000.0f);
			bob.Move(1100.0f, 2000.0f);
			host.Claim(RAIDER_1);
			host.Claim(SETTLER);
			bob.Claim(RAIDER_2);
			bob.Claim(DOGMEAT, Reason::kCompanion);
			s.Settle();
			for (auto* player : { &host, &bob }) {
				REQUIRE(player->OwnerOf(RAIDER_1) == host.Id());
				REQUIRE(player->OwnerOf(SETTLER) == host.Id());
				REQUIRE(player->OwnerOf(RAIDER_2) == bob.Id());
				REQUIRE(player->OwnerOf(DOGMEAT) == bob.Id());
			}
			for (auto* game : { &hostGame, &bobGame }) {
				for (const auto id : { RAIDER_1, RAIDER_2, SETTLER, DOGMEAT }) {
					game->npcs[id] = Npc{};
				}
			}
			bobGame.companions.insert(DOGMEAT);
			// CopyWatch sees each copy before anything hits it: where it stands.
			hostGame.Wait(FRAME);
			bobGame.Wait(FRAME);
		}

		// A second goes by in both games, and what each sent reaches the other.
		void Pass()
		{
			hostGame.Wait(std::chrono::seconds{ 1 });
			bobGame.Wait(std::chrono::seconds{ 1 });
			// Twice: the health the runner's game shares after a friend's damage reaches the friend.
			for (int i = 0; i < 2; ++i) {
				s.Settle();
				hostGame.Apply();
				bobGame.Apply();
			}
		}

		[[nodiscard]] bool DeathDropped(std::uint32_t a_ref) const { return s.Logged(std::format("death of {:08X} from player", a_ref)); }
	};
}

TEST("copy hits: our companion's bites on a friend's raider hurt it in the friend's game and stay on our copy, and its killing bite is believed and is our player's kill")
{
	Fight f;
	f.bobGame.Strike(DOGMEAT, RAIDER_1, 30.0f);
	f.Pass();
	CHECK(f.bobGame.npcs[RAIDER_1].health == 70.0f);  // kept on Bob's copy
	CHECK(f.bobGame.givenBack == 0.0f);
	CHECK(Near(f.hostGame.npcs[RAIDER_1].health, 70.0f));  // the raider's own game took the bite
	CHECK(Near(f.bob.HealthHeard(RAIDER_1).value_or(-1.0f), 0.7f));  // and says so
	CHECK(f.bob.OwnerOf(RAIDER_1) == f.host.Id());      // a bite that doesn't kill takes nothing over

	f.bobGame.Strike(DOGMEAT, RAIDER_1, 30.0f);
	f.bobGame.Strike(DOGMEAT, RAIDER_1, 45.0f);
	f.Pass();
	CHECK(f.hostGame.npcs[RAIDER_1].dead);
	CHECK(f.host.WorldNow().dead.contains(RAIDER_1));
	CHECK(f.host.KilledBy(RAIDER_1) == std::optional{ f.bob.Id() });  // "Bob killed Raider"
	CHECK(!f.DeathDropped(RAIDER_1));
	CHECK(f.bobGame.reportsSent == 2);
}

TEST("copy hits: before, our companion's bites on a friend's raider were given back and never reached the raider's game")
{
	Fight f;
	f.bobGame.onlyPlayer = true;
	f.bobGame.Strike(DOGMEAT, RAIDER_1, 30.0f);
	f.Pass();
	CHECK(f.bobGame.npcs[RAIDER_1].health == 100.0f);
	CHECK(f.bobGame.givenBack == 30.0f);
	CHECK(!f.host.HealthHeard(RAIDER_1));
	CHECK(f.hostGame.npcs[RAIDER_1].health == 100.0f);
}

TEST("copy hits: a companion's bite that kills a friend's raider at once claims it, so the server believes the kill; without the claim the raider lived on in its own game")
{
	Fight f;
	f.bobGame.Strike(DOGMEAT, RAIDER_1, 150.0f);
	f.Pass();
	CHECK(f.host.WorldNow().dead.contains(RAIDER_1));
	CHECK(f.hostGame.npcs[RAIDER_1].dead);
	CHECK(f.host.KilledBy(RAIDER_1) == std::optional{ f.bob.Id() });
	CHECK(!f.DeathDropped(RAIDER_1));

	// Before the claim, and before a killing blow went as damage too (Book::Fell: the test after next).
	Fight g;
	g.bobGame.claimKills = false;
	g.bobGame.fallsSent = false;
	g.bobGame.Strike(DOGMEAT, RAIDER_1, 150.0f);
	g.Pass();
	CHECK(g.DeathDropped(RAIDER_1));
	CHECK(!g.host.WorldNow().dead.contains(RAIDER_1));
	CHECK(!g.hostGame.npcs[RAIDER_1].dead);  // dead on Bob's screen, fighting on in the host's
}

TEST("copy hits: an NPC our game runs that fights a friend's counts in our game only, and its kill is believed but is nobody's")
{
	Fight f;
	// Bob's raider shoots the host's settler: in the host's game Bob's raider is a copy that only plays its shots
	// (WeaponFire: no projectile, no damage), so the settler is hurt by Bob's report or not at all.
	f.bobGame.Strike(RAIDER_2, SETTLER, 40.0f);
	f.Pass();
	CHECK(Near(f.bobGame.npcs[SETTLER].health, 60.0f));
	CHECK(Near(f.hostGame.npcs[SETTLER].health, 60.0f));
	f.bobGame.Strike(RAIDER_2, SETTLER, 70.0f);
	f.Pass();
	CHECK(f.host.WorldNow().dead.contains(SETTLER));
	CHECK(f.host.KilledBy(SETTLER) == std::optional{ 0u });  // no kill of Bob's: "Bob killed Settler" would be a lie
	CHECK(!f.DeathDropped(SETTLER));

	// The host's raider shoots Bob's companion back, in the host's game: the same rule the other way.
	f.hostGame.Strike(RAIDER_1, DOGMEAT, 25.0f);
	f.Pass();
	CHECK(Near(f.hostGame.npcs[DOGMEAT].health, 75.0f));
	CHECK(Near(f.bobGame.npcs[DOGMEAT].health, 75.0f));
	CHECK(f.hostGame.givenBack == 0.0f);
}

TEST("copy hits: a companion two players share counts in the game that runs it only; the other's own copy of it bites for nothing")
{
	Fight f;
	f.hostGame.companions.insert(DOGMEAT);  // the host has Dogmeat too: Bob's game kept him
	f.host.Claim(DOGMEAT, Reason::kCompanion);
	f.s.Settle();
	REQUIRE(f.host.OwnerOf(DOGMEAT) == f.bob.Id());
	f.hostGame.Strike(DOGMEAT, RAIDER_2, 30.0f);  // the host's Dogmeat, on the host's copy of Bob's raider
	f.bobGame.Strike(DOGMEAT, RAIDER_2, 30.0f);   // Bob's, on the raider itself
	f.Pass();
	CHECK(f.bobGame.npcs[RAIDER_2].health == 70.0f);  // bitten once
	CHECK(f.hostGame.npcs[RAIDER_2].health == 100.0f);
	CHECK(f.hostGame.givenBack == 30.0f);
	CHECK(f.hostGame.reportsSent == 0);
	CHECK(!f.bob.HealthHeard(RAIDER_2));
}

TEST("copy hits: a turret built at a settlement fires in the builder's game and as a copy in the friend's; on a copy neither counts, so each raider is hit once, in its own game")
{
	Fight f;
	const auto turret = f.bob.Build(SANCTUARY, TURRET, 1150.0f, 2050.0f);
	f.s.Settle();
	REQUIRE(!Protocol::IsShareableRef(turret));  // made at runtime: a reference only Bob's game has
	REQUIRE(f.host.Copies().contains({ f.bob.Id(), turret }));
	constexpr std::uint32_t HOSTS_COPY = 0xFF00C001;  // WorkshopSync's copy of it in the host's game, made at runtime there
	f.bobGame.runtime.insert(turret);
	f.hostGame.runtime.insert(HOSTS_COPY);

	f.bobGame.Strike(turret, RAIDER_1, 25.0f);      // Bob's turret, on his copy of the host's raider
	f.hostGame.Strike(HOSTS_COPY, RAIDER_1, 25.0f);  // its copy, on the raider itself
	f.bobGame.Strike(turret, RAIDER_2, 25.0f);      // Bob's turret, on Bob's own raider
	f.hostGame.Strike(HOSTS_COPY, RAIDER_2, 25.0f);  // its copy, on the host's copy of Bob's raider
	f.Pass();
	CHECK(f.hostGame.npcs[RAIDER_1].health == 75.0f);
	CHECK(f.bobGame.npcs[RAIDER_2].health == 75.0f);
	CHECK(f.bobGame.npcs[RAIDER_1].health == 100.0f);  // given back: the host's game dealt it
	CHECK(f.hostGame.npcs[RAIDER_2].health == 100.0f);
	CHECK(f.bobGame.reportsSent == 0);
	CHECK(f.hostGame.reportsSent == 0);
	CHECK(!f.host.HealthHeard(RAIDER_1));
	CHECK(!f.bob.HealthHeard(RAIDER_2));
}

TEST("copy hits: a friend's stand-in, a fire, a random encounter of our game's own and the copy's own side never count on it; a robot companion only our game has does")
{
	Fight f;
	constexpr std::uint32_t HOSTS_STAND_IN = 0xFF00D001;
	constexpr std::uint32_t GUNNER = 0xFF00E001;  // a random encounter: only Bob's game has it
	constexpr std::uint32_t ROBOT = 0xFF00F001;   // built at Bob's robot workbench, following him
	f.bobGame.standIns.insert(HOSTS_STAND_IN);
	f.bobGame.runtime.insert(GUNNER);
	f.bobGame.runtime.insert(ROBOT);
	f.bobGame.companions.insert(ROBOT);

	for (const auto by : { HOSTS_STAND_IN, FIRE, GUNNER, SETTLER }) {
		f.bobGame.Strike(by, RAIDER_1, 10.0f);
		f.bobGame.Wait(CopyHealth::OURS_GRACE);
	}
	f.Pass();
	CHECK(f.bobGame.npcs[RAIDER_1].health == 100.0f);
	CHECK(f.bobGame.givenBack == 40.0f);
	CHECK(f.bobGame.reportsSent == 0);
	CHECK(!f.host.HealthHeard(RAIDER_1));

	f.bobGame.Strike(ROBOT, RAIDER_1, 20.0f);
	f.Pass();
	CHECK(f.bobGame.npcs[RAIDER_1].health == 80.0f);
	CHECK(Near(f.hostGame.npcs[RAIDER_1].health, 80.0f));
	f.bobGame.Strike(ROBOT, RAIDER_1, 90.0f);
	f.Pass();
	CHECK(f.host.KilledBy(RAIDER_1) == std::optional{ f.bob.Id() });
}

namespace
{
	// One quest's fight, run in both games and split across them (Rotten Landfill's DN119Fight in a 0.14.0-rc1 soak):
	// each game's run made its own scavenger, shared as a spawn (a human is never paired: QuestActors.h), and the
	// molerats paired with Bob's, which his game shared first. So the host's game runs its scavenger, Bob's runs the
	// molerat, and each has a copy of the other's (the host's molerat, paired, is booked as its copy of Bob's).
	constexpr std::uint32_t SCAVENGER = 0xFF0016EB;  // the host's spawn
	constexpr std::uint32_t MOLERAT = 0xFF001AEA;    // Bob's spawn
	constexpr std::uint32_t SCAVENGER_BASE = 0x0001D4D2;
	constexpr std::uint32_t MOLERAT_BASE = 0x0001D966;
	constexpr float         MOLERAT_HEALTH = 15.0f;

	struct SplitFight : Fight
	{
		SplitFight()
		{
			host.Spawn(SCAVENGER, SCAVENGER_BASE, 1000.0f, 2050.0f);
			bob.Spawn(MOLERAT, MOLERAT_BASE, 1300.0f, 2050.0f);
			s.Settle();
			for (auto* player : { &host, &bob }) {
				REQUIRE(player->OwnerOf(SCAVENGER) == host.Id());
				REQUIRE(player->OwnerOf(MOLERAT) == bob.Id());
			}
			REQUIRE(bob.CopyOf(SCAVENGER));
			REQUIRE(host.CopyOf(MOLERAT));
			hostGame.runtime.insert(SCAVENGER);
			hostGame.spawns.insert(SCAVENGER);
			bobGame.runtime.insert(MOLERAT);
			bobGame.spawns.insert(MOLERAT);
			for (auto* game : { &hostGame, &bobGame }) {
				game->npcs[SCAVENGER] = Npc{};
				game->npcs[MOLERAT] = Npc{ .health = MOLERAT_HEALTH };
				game->Wait(FRAME);
			}
		}

		[[nodiscard]] bool SpawnDeathDropped(std::uint32_t a_ref) const { return s.Logged(std::format("death of spawn {:08X} from player", a_ref)); }
	};
}

TEST("copy hits: a spawn our game shares and runs that fights a friend's spawn counts in our game: its blows reach the friend's game, and one that kills at once is believed there")
{
	SplitFight f;

	// The host's scavenger shoots the host's molerat, Bob's mirrored: kept on the host's copy, taken off Bob's molerat.
	f.hostGame.Strike(SCAVENGER, MOLERAT, 5.0f);
	f.Pass();
	CHECK(f.hostGame.givenBack == 0.0f);
	CHECK(f.hostGame.reportsSent == 1);
	CHECK(Near(f.bobGame.npcs[MOLERAT].health, 10.0f));
	CHECK(Near(f.hostGame.npcs[MOLERAT].health, 10.0f));

	// Bob's molerat bites Bob's copy of the scavenger: the host's scavenger takes it.
	f.bobGame.Strike(MOLERAT, SCAVENGER, 4.0f);
	f.Pass();
	CHECK(f.bobGame.givenBack == 0.0f);
	CHECK(Near(f.hostGame.npcs[SCAVENGER].health, 96.0f));

	// A shot that kills the molerat at once in the host's game: it dies in Bob's too, nobody's kill, and nothing dropped.
	f.hostGame.Strike(SCAVENGER, MOLERAT, 25.0f);
	f.Pass();
	CHECK(f.hostGame.npcs[MOLERAT].dead);
	CHECK(f.bobGame.npcs[MOLERAT].dead);
	CHECK(f.bob.WorldNow().dead.contains(MOLERAT));
	CHECK(f.bob.KilledBy(MOLERAT) == std::optional{ 0u });
	CHECK(!f.SpawnDeathDropped(MOLERAT));
	CHECK(f.host.OwnerOf(MOLERAT) == 0);  // over everywhere
	CHECK(f.bob.OwnerOf(MOLERAT) == 0);
}

TEST("copy hits: up to 0.14.0-rc1 a spawn our game runs hit a friend's spawn for nobody, and one shot that killed it here left it alive in its own game")
{
	SplitFight f;
	f.hostGame.spawnsOurs = false;
	f.bobGame.spawnsOurs = false;

	f.hostGame.Strike(SCAVENGER, MOLERAT, 5.0f);
	f.Pass();
	CHECK(f.hostGame.givenBack == 5.0f);  // given back here, never taken off Bob's
	CHECK(f.hostGame.reportsSent == 0);
	CHECK(f.bobGame.npcs[MOLERAT].health == MOLERAT_HEALTH);

	// The one-shot kill: dead in the host's game, refused by the server, alive in Bob's (the soak's "dead=true/false").
	f.hostGame.Strike(SCAVENGER, MOLERAT, 25.0f);
	f.Pass();
	CHECK(f.hostGame.npcs[MOLERAT].dead);
	CHECK(f.SpawnDeathDropped(MOLERAT));
	CHECK(!f.bobGame.npcs[MOLERAT].dead);
	CHECK(f.bob.OwnerOf(MOLERAT) == f.bob.Id());
}

TEST("copy hits: whose a hit is, what follows it, and whose kill it is")
{
	using CopyHealth::After;
	using CopyHealth::Cause;
	using CopyHealth::Credited;
	using CopyHealth::Judge;
	constexpr std::uint32_t US = 2, FRIEND = 3;

	const Cause player{ .player = true, .actor = true };
	const Cause ourNpc{ .actor = true, .shareable = true, .owner = US };
	const Cause ourCompanion{ .actor = true, .shareable = true, .owner = US, .ourCompanion = true };
	const Cause sharedCompanion{ .actor = true, .shareable = true, .owner = FRIEND, .ourCompanion = true };
	const Cause friendsNpc{ .actor = true, .shareable = true, .owner = FRIEND };
	const Cause nobodys{ .actor = true, .shareable = true };
	const Cause robot{ .actor = true, .ourCompanion = true };
	const Cause turret{ .actor = true };
	const Cause standIn{ .actor = true, .standIn = true };
	const Cause nothing{};
	const Cause ourSpawn{ .actor = true, .ourSpawn = true };  // a random encounter's raider our game shares and runs
	const Cause unshared{ .actor = true };                    // ... one not shared (yet): in no friend's game

	CHECK(Judge(player, US) == Hitter::kOurPlayer);
	CHECK(Judge(player, 0) == Hitter::kOurPlayer);
	CHECK(Judge(ourNpc, US) == Hitter::kOurs);
	CHECK(Judge(ourCompanion, US) == Hitter::kOurs);
	CHECK(Judge(robot, US) == Hitter::kOurs);
	CHECK(Judge(ourSpawn, US) == Hitter::kOurs);
	for (const auto& cause : { sharedCompanion, friendsNpc, nobodys, turret, standIn, nothing, unshared }) {
		CHECK(Judge(cause, US) == Hitter::kNotOurs);
	}
	CHECK(Judge(ourNpc, 0) == Hitter::kNotOurs);  // out of a session
	CHECK(Judge(robot, 0) == Hitter::kNotOurs);
	CHECK(Judge(ourSpawn, 0) == Hitter::kNotOurs);
	CHECK(!Credited(ourSpawn, US));  // a raider of ours killing a friend's NPC is no kill of our player's

	// Our player's hits are reported and claim, on anything; the rest of ours only on a copy, claiming only to kill.
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false).report);
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false).claim);
	CHECK(After(Hitter::kOurPlayer, US, US, false).report);
	CHECK(After(Hitter::kOurPlayer, 0, US, false).report);
	CHECK(After(Hitter::kOurs, FRIEND, US, false).report);
	CHECK(!After(Hitter::kOurs, FRIEND, US, false).claim);
	CHECK(After(Hitter::kOurs, FRIEND, US, true).claim);
	for (const std::uint32_t owner : { US, 0u }) {
		CHECK(!After(Hitter::kOurs, owner, US, true).report);
		CHECK(!After(Hitter::kOurs, owner, US, true).claim);
	}
	CHECK(!After(Hitter::kNotOurs, FRIEND, US, true).report);
	CHECK(!After(Hitter::kNotOurs, FRIEND, US, true).claim);

	CHECK(Credited(player, US));
	CHECK(Credited(ourCompanion, US));
	CHECK(Credited(robot, US));
	CHECK(!Credited(ourNpc, US));
	CHECK(!Credited(sharedCompanion, US));
	CHECK(!Credited(turret, US));
	CHECK(!Credited(nothing, US));
}

TEST("copy hits: a drop with a hit of our companion's around it is kept, one with only a friend's turret's is given back")
{
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	constexpr std::uint32_t US = 2, FRIEND = 3;
	const CopyHealth::Cause companion{ .actor = true, .shareable = true, .owner = US, .ourCompanion = true };
	const CopyHealth::Cause turretCopy{ .actor = true };
	CopyHealth::Book book;
	book.Set(RAIDER_1, 100.0f);
	book.Set(RAIDER_2, 100.0f);
	for (const auto& [id, cause] : { std::pair{ RAIDER_1, companion }, std::pair{ RAIDER_2, turretCopy } }) {
		if (CopyHealth::After(CopyHealth::Judge(cause, US), FRIEND, US, false).report) {
			book.OurHit(id, t0);
		}
	}
	CHECK(book.Check(RAIDER_1, 70.0f, t0 + FRAME) == 0.0f);
	CHECK(book.Check(RAIDER_1, 70.0f, t0 + FRAME + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 70.0f });
	CHECK(book.Check(RAIDER_2, 70.0f, t0 + FRAME) == 0.0f);
	CHECK(book.Check(RAIDER_2, 70.0f, t0 + FRAME + CopyHealth::JUDGE_AFTER) == 30.0f);
}

TEST("copy hits: a legendary weapon's bleed or burn put on a copy by our hit stays ours for its ticks, which fire no hit event, and goes to the NPC's game")
{
	// Before, each tick after the first second was "a drop with no hit of ours" and was given back: a Wounding or
	// Incendiary weapon healed its own damage back as the player watched, and the NPC's game never took it.
	using std::chrono::milliseconds;
	using std::chrono::seconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book book;
	book.Set(RAIDER_1, 100.0f);

	// The shot (its event's figure 20) and its enchantment's hit (no figure, a spell's with no weapon): the effect is on.
	book.OurHit(RAIDER_1, t0, 20.0f);
	book.OurHit(RAIDER_1, t0);
	book.OurEffect(RAIDER_1, t0);
	CHECK(book.Check(RAIDER_1, 80.0f, t0 + FRAME) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + FRAME) == 20.0f);

	// A tick a second and a half later, long after the hit's grace: ours, nothing given back.
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 1500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 75.0f });

	// And so on to the end of the effect's window; a drop after it is something else's again.
	float health = 75.0f;
	for (auto at = t0 + milliseconds{ 2500 }; at <= t0 + CopyHealth::EFFECT_WINDOW; at += seconds{ 1 }) {
		health -= 5.0f;
		CHECK(book.Check(RAIDER_1, health, at) == 0.0f);
		CHECK(book.Check(RAIDER_1, health, at + CopyHealth::JUDGE_AFTER) == 0.0f);
		CHECK(Took(book, RAIDER_1, at + CopyHealth::JUDGE_AFTER) == 5.0f);
	}
	CHECK(book.Check(RAIDER_1, health - 5.0f, t0 + CopyHealth::EFFECT_WINDOW + milliseconds{ 500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, health - 5.0f, t0 + CopyHealth::EFFECT_WINDOW + milliseconds{ 500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	CHECK(Took(book, RAIDER_1, t0 + CopyHealth::EFFECT_WINDOW + milliseconds{ 500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
}

TEST("copy hits: a bleed of ours on a copy stops being ours once something else hits the copy, and what a friend's grenade took beyond our hits is given back")
{
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book book;
	book.Set(RAIDER_1, 100.0f);
	book.OurHit(RAIDER_1, t0, 20.0f);
	book.OurEffect(RAIDER_1, t0);
	CHECK(book.Check(RAIDER_1, 80.0f, t0 + FRAME) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + FRAME) == 20.0f);

	// A fire in our world (a hazard's hit, no cause) reaches the copy: from then on a drop with no hit behind it may be
	// the fire's as well as the bleed's, so it is given back as before.
	book.OtherHit(RAIDER_1, t0 + milliseconds{ 2000 });
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 2500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 2500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	CHECK(Took(book, RAIDER_1, t0 + milliseconds{ 2500 } + CopyHealth::JUDGE_AFTER) == 0.0f);

	// A friend's grenade set off here (its blow the owner's: OwnersBlow) beside our own shot: our shot's figure is ours,
	// the rest the grenade's, given back; the NPC's own game deals the grenade's blow.
	CopyHealth::Book other;
	other.Set(RAIDER_2, 100.0f);
	other.OurHit(RAIDER_2, t0, 20.0f);
	other.OurEffect(RAIDER_2, t0);
	other.OtherHit(RAIDER_2, t0 + milliseconds{ 20 });
	other.OwnersBlow(RAIDER_2, t0 + milliseconds{ 20 });
	CHECK(other.Check(RAIDER_2, 30.0f, t0 + FRAME) == 0.0f);
	CHECK(Took(other, RAIDER_2, t0 + FRAME) == 20.0f);
	CHECK(other.Check(RAIDER_2, 30.0f, t0 + FRAME + CopyHealth::JUDGE_AFTER) == 50.0f);
	CHECK(Took(other, RAIDER_2, t0 + FRAME + CopyHealth::JUDGE_AFTER) == 0.0f);
}

TEST("copy hits: a report from the NPC's game during our bleed stands, and a tick given back never raises the copy above it")
{
	// The owner's game shares the NPC's health as it takes our damage (and whatever else it took there): a report that
	// lowers the copy is set at once, and nothing we give back undoes damage the owner really took.
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book book;
	book.Set(RAIDER_1, 100.0f);
	book.OurHit(RAIDER_1, t0, 20.0f);
	book.OurEffect(RAIDER_1, t0);
	CHECK(book.Check(RAIDER_1, 80.0f, t0 + FRAME) == 0.0f);
	const auto shot = book.TakeOurs(RAIDER_1, t0 + FRAME);
	REQUIRE(shot);
	CHECK(shot->amount == 20.0f);

	// The owner took a friend's shot there too, and ours: 80 -> 60 by its report, set as it comes once our shot is
	// answered (with it still on its way, the report less the shot: 40).
	CHECK(book.Answered(RAIDER_1, shot->seq, CopyHealth::Answer::kApplied) == 0.0f);
	CHECK(book.Heard(RAIDER_1, 80.0f, 60.0f, t0 + milliseconds{ 1200 }) == 60.0f);
	book.Set(RAIDER_1, 60.0f);
	// Our next tick is from there, ours.
	CHECK(book.Check(RAIDER_1, 55.0f, t0 + milliseconds{ 1500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 55.0f, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 5.0f);

	// A fire's hit ends the effect's claim: the next drop is given back, to where the copy stood, no higher.
	book.OtherHit(RAIDER_1, t0 + milliseconds{ 2000 });
	CHECK(book.Check(RAIDER_1, 50.0f, t0 + milliseconds{ 2500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 50.0f, t0 + milliseconds{ 2500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	book.Set(RAIDER_1, 55.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 55.0f });

	// A report that would raise the copy is set only to what it says less our ticks still on their way (sent and
	// unanswered, or not sent yet); once they are answered, a report is set as it comes.
	CopyHealth::Book late;
	late.Set(RAIDER_2, 100.0f);
	late.OurHit(RAIDER_2, t0, 20.0f);
	late.OurEffect(RAIDER_2, t0);
	CHECK(late.Check(RAIDER_2, 80.0f, t0 + FRAME) == 0.0f);
	const auto lateShot = late.TakeOurs(RAIDER_2, t0 + FRAME);
	REQUIRE(lateShot);
	CHECK(late.Check(RAIDER_2, 75.0f, t0 + milliseconds{ 1500 }) == 0.0f);
	CHECK(late.Check(RAIDER_2, 75.0f, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(late.Heard(RAIDER_2, 75.0f, 80.0f, t0 + milliseconds{ 1700 }) == 55.0f);  // the owner's report from before the shot (on its way) and the tick (not sent yet): 80 less both
	const auto tick = late.TakeOurs(RAIDER_2, t0 + milliseconds{ 1700 });
	REQUIRE(tick);
	CHECK(late.Answered(RAIDER_2, lateShot->seq, CopyHealth::Answer::kApplied) == 0.0f);
	CHECK(late.Answered(RAIDER_2, tick->seq, CopyHealth::Answer::kApplied) == 0.0f);
	CHECK(late.Heard(RAIDER_2, 75.0f, 80.0f, t0 + CopyHealth::EFFECT_WINDOW + milliseconds{ 100 }) == 80.0f);
}

TEST("copy hits: a report that lands before the hit's event is heard does not heal the hit back")
{
	// The engine takes the shot's health and sends its event a frame later; the runner's report came in between. Set to
	// the report's health, the shot was healed back and never reached the NPC's game ("my hit did nothing").
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book                    book;
	book.Set(RAIDER_1, 100.0f);
	CHECK(Near(book.Heard(RAIDER_1, 90.0f, 95.0f, t0, true), 95.0f));
	CHECK(Took(book, RAIDER_1, t0) == 0.0f);
	book.OurHit(RAIDER_1, t0 + milliseconds{ 16 }, 10.0f);
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 16 }, 100.0f) == 0.0f);
	CHECK(Near(Took(book, RAIDER_1, t0 + milliseconds{ 16 }), 10.0f));
	CHECK(Near(*book.Health(RAIDER_1), 95.0f));
}

TEST("copy hits: a drop a report found with no hit of ours after it is someone else's")
{
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book                    book;
	book.Set(RAIDER_1, 100.0f);
	CHECK(Near(book.Heard(RAIDER_1, 90.0f, 95.0f, t0, true), 95.0f));
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 150 }, 100.0f) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + milliseconds{ 150 }) == 0.0f);
	// Nor does a hit that comes long after take it.
	book.OurHit(RAIDER_1, t0 + milliseconds{ 3000 }, 10.0f);
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 3000 }, 100.0f) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + milliseconds{ 3000 }) == 0.0f);
}

TEST("copy hits: a drop a report found with a hit of ours booked already is counted once")
{
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book                    book;
	book.Set(RAIDER_1, 100.0f);
	book.OurHit(RAIDER_1, t0, 10.0f);
	CHECK(book.Check(RAIDER_1, 90.0f, t0, 100.0f) == 0.0f);
	book.Heard(RAIDER_1, 90.0f, 95.0f, t0 + milliseconds{ 5 }, true);
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 20 }, 100.0f) == 0.0f);
	CHECK(Near(Took(book, RAIDER_1, t0 + milliseconds{ 20 }), 10.0f));  // in all, not 20
}

TEST("copy hits: a book pruned with a grace keeps what a hit booked in a frame the copy was out of sight")
{
	// CopyWatch prunes by when each copy was last seen (5 s), not by this frame's loop: a frame without the copy's 3D
	// lost the hit booked in it, and the shot never reached the NPC's game.
	using std::chrono::milliseconds;
	using std::chrono::seconds;
	const CopyHealth::Clock::time_point t0{};
	const auto                          seenAt = [&](std::uint32_t a_id, CopyHealth::Clock::time_point a_seen) { return [=](std::uint32_t a_other) { return a_other == a_id && t0 + milliseconds{ 16 } - a_seen <= seconds{ 5 }; }; };

	CopyHealth::Book book;
	book.Set(RAIDER_1, 100.0f);
	book.OurHit(RAIDER_1, t0, 10.0f);
	book.Prune(seenAt(RAIDER_1, t0 - seconds{ 1 }));
	CHECK(book.Size() == 1);
	CHECK(book.Check(RAIDER_1, 90.0f, t0 + milliseconds{ 16 }, 100.0f) == 0.0f);
	CHECK(Near(Took(book, RAIDER_1, t0 + milliseconds{ 16 }), 10.0f));

	// Seen last 6 s ago: forgotten.
	CopyHealth::Book gone;
	gone.Set(RAIDER_1, 100.0f);
	gone.OurHit(RAIDER_1, t0, 10.0f);
	gone.Prune(seenAt(RAIDER_1, t0 - seconds{ 6 }));
	CHECK(gone.Size() == 0);
}

TEST("copy hits: a drop a report found is not taken by a hit that comes long after, even if the copy was not checked meanwhile")
{
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book                    book;
	book.Set(RAIDER_1, 100.0f);
	CHECK(Near(book.Heard(RAIDER_1, 90.0f, 95.0f, t0, true), 95.0f));
	// The copy had no 3D for a while: no Check between. A hit of ours 800 ms on is its own; the report's heal stands.
	book.OurHit(RAIDER_1, t0 + milliseconds{ 800 }, 10.0f);
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 800 }, 100.0f) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + milliseconds{ 800 }) == 0.0f);
}

TEST("copy hits: our companion's killing blow on a friend's robot reaches the robot's own game, though our copy's death comes at the end of a long fall")
{
	// On the rig the host's Codsworth killed the host's copy of a friend's Mr. Gutsy: the killing blow sent nothing (only
	// living copies were checked), and the copy's death came at the end of a 10 s fall, past the server's deathGrace since
	// the host's game last told it of the fight. The death was dropped, and the Gutsy stood on at 6 health in its own game.
	Server::Options options;
	options.deathGrace = 600s;  // the test moves the server's clock past it (Advance), so a busy PC can't end it early
	for (const bool fixed : { true, false }) {
		Fight f{ options };
		f.bobGame.claimKills = false;  // the blow's claim is refused, or there is none (a spawn, a plugin's NPC our game lacks)
		f.bobGame.longFall = true;
		f.bobGame.fallsSent = fixed;
		f.bobGame.Strike(DOGMEAT, RAIDER_1, 60.0f);
		f.Pass();
		REQUIRE(Near(f.hostGame.npcs[RAIDER_1].health, 40.0f));
		REQUIRE(Near(f.bobGame.npcs[RAIDER_1].health, 40.0f));

		f.bobGame.Strike(DOGMEAT, RAIDER_1, 45.0f);  // dead on Bob's screen, falling
		CHECK(f.bobGame.npcs[RAIDER_1].dead);
		f.Pass();
		f.s.Advance(601s);  // the fall outlasts deathGrace
		f.bobGame.Land();
		f.Pass();
		if (fixed) {
			CHECK(f.hostGame.npcs[RAIDER_1].dead);  // the blow took the last of its health in its own game
			CHECK(f.host.WorldNow().dead.contains(RAIDER_1));
			CHECK(f.bob.WorldNow().dead.contains(RAIDER_1));
			CHECK(f.hostGame.deathsSent.contains(RAIDER_1));  // its own game's death is the one believed
			CHECK(!f.DeathDropped(RAIDER_1));               // and Bob's, late, finds it dead already: no second death
		} else {
			CHECK(!f.hostGame.npcs[RAIDER_1].dead);  // as on the rig: it fights on in its own game
			CHECK(Near(f.hostGame.npcs[RAIDER_1].health, 40.0f));
			CHECK(f.DeathDropped(RAIDER_1));
			CHECK(!f.host.WorldNow().dead.contains(RAIDER_1));
		}
	}
}

TEST("copy hits: a copy that falls to a blow of ours books all it stood at and more, once; a fall with none of ours around it books nothing")
{
	using std::chrono::milliseconds;
	const CopyHealth::Clock::time_point t0{};

	// The blow took 30 off a copy at 6: all of the drop is ours, and sent.
	CopyHealth::Book book;
	book.Set(RAIDER_1, 6.0f);
	book.OurHit(RAIDER_1, t0, 30.0f);
	CHECK(!book.Fell(RAIDER_1, -24.0f, t0 + FRAME));
	CHECK(Near(Took(book, RAIDER_1, t0 + FRAME), 30.0f));
	CHECK(book.Lethal() == 1);
	// Once a fall: the corpse's next frames book nothing more.
	CHECK(!book.Fell(RAIDER_1, -24.0f, t0 + 2 * FRAME));
	CHECK(Took(book, RAIDER_1, t0 + 2 * FRAME) == 0.0f);

	// A copy that reads 0 dead: never less than where it stood and MIN_LEFT more, so the NPC's game, about where the copy
	// stood, is not left a sliver by rounding.
	CopyHealth::Book zero;
	zero.Set(RAIDER_1, 6.0f);
	zero.OurHit(RAIDER_1, t0);
	CHECK(!zero.Fell(RAIDER_1, 0.0f, t0 + FRAME));
	CHECK(Near(Took(zero, RAIDER_1, t0 + FRAME), 6.0f + CopyHealth::MIN_LEFT));

	// The hit's event a frame behind its damage: the fall waits for it, JUDGE_AFTER at most.
	CopyHealth::Book late;
	late.Set(RAIDER_1, 6.0f);
	CHECK(late.Fell(RAIDER_1, -10.0f, t0));
	late.OurHit(RAIDER_1, t0 + FRAME, 16.0f);
	CHECK(!late.Fell(RAIDER_1, -10.0f, t0 + FRAME));
	CHECK(Near(Took(late, RAIDER_1, t0 + FRAME), 16.0f));

	// Nothing of ours around it (a friend's shot, a hazard): nothing sent, and a hit of ours on the corpse later is no part of it.
	CopyHealth::Book theirs;
	theirs.Set(RAIDER_1, 6.0f);
	theirs.OurHit(RAIDER_1, t0 - std::chrono::seconds{ 5 }, 10.0f);
	CHECK(theirs.Fell(RAIDER_1, -10.0f, t0));
	CHECK(!theirs.Fell(RAIDER_1, -10.0f, t0 + CopyHealth::JUDGE_AFTER));
	theirs.OurHit(RAIDER_1, t0 + milliseconds{ 500 }, 10.0f);
	CHECK(!theirs.Fell(RAIDER_1, -20.0f, t0 + milliseconds{ 500 }));
	CHECK(Took(theirs, RAIDER_1, t0 + milliseconds{ 500 }) == 0.0f);
	CHECK(theirs.Lethal() == 0);

	// A throw of ours (left to the owner's game, which deals it) beside our shot: only the shot's figure is ours.
	CopyHealth::Book thrown;
	thrown.Set(RAIDER_1, 50.0f);
	thrown.OurHit(RAIDER_1, t0, 20.0f);
	thrown.OwnersBlow(RAIDER_1, t0);
	CHECK(!thrown.Fell(RAIDER_1, -40.0f, t0 + FRAME));
	CHECK(Near(Took(thrown, RAIDER_1, t0 + FRAME), 20.0f));

	// Never seen alive: no health to fall from, nothing booked.
	CopyHealth::Book unseen;
	unseen.OurHit(RAIDER_1, t0, 20.0f);
	CHECK(!unseen.Fell(RAIDER_1, -20.0f, t0 + FRAME));
	CHECK(Took(unseen, RAIDER_1, t0 + FRAME) == 0.0f);

	// Up again (a copy checked alive): its next fall is judged afresh.
	CopyHealth::Book again;
	again.Set(RAIDER_1, 6.0f);
	again.OurHit(RAIDER_1, t0, 10.0f);
	CHECK(!again.Fell(RAIDER_1, -4.0f, t0 + FRAME));
	CHECK(Near(Took(again, RAIDER_1, t0 + FRAME), 10.0f));
	again.Set(RAIDER_1, 50.0f);
	CHECK(again.Check(RAIDER_1, 50.0f, t0 + std::chrono::seconds{ 10 }) == 0.0f);
	again.OurHit(RAIDER_1, t0 + std::chrono::seconds{ 11 }, 60.0f);
	CHECK(!again.Fell(RAIDER_1, -10.0f, t0 + std::chrono::seconds{ 11 } + FRAME));
	CHECK(Near(Took(again, RAIDER_1, t0 + std::chrono::seconds{ 11 } + FRAME), 60.0f));
}

TEST("copy hits: when the friend's robot falls for 10 s in its own game too, our late death report may be dropped, but the robot ends dead in both games")
{
	// As on the rig: the fall's damage kills the robot in its own game, where it falls as long as our copy did. Our copy's
	// death report reaches the server first, still past deathGrace, and is dropped; the runner's, after it, is believed.
	Server::Options options;
	options.deathGrace = 600s;  // the test moves the server's clock past it (Advance)
	Fight f{ options };
	f.bobGame.claimKills = false;
	f.bobGame.longFall = true;
	f.hostGame.longFall = true;
	f.bobGame.Strike(DOGMEAT, RAIDER_1, 60.0f);
	f.Pass();
	f.bobGame.Strike(DOGMEAT, RAIDER_1, 45.0f);
	f.Pass();
	CHECK(f.hostGame.npcs[RAIDER_1].dead);  // falling in its own game too
	CHECK(!f.host.WorldNow().dead.contains(RAIDER_1));
	f.s.Advance(601s);
	f.bobGame.Land();
	f.Pass();
	CHECK(f.DeathDropped(RAIDER_1));  // ours, late: dropped, harmlessly
	f.hostGame.Land();
	f.Pass();
	CHECK(f.hostGame.deathsSent.contains(RAIDER_1));
	CHECK(f.host.WorldNow().dead.contains(RAIDER_1));  // the kill is not lost
	CHECK(f.bob.WorldNow().dead.contains(RAIDER_1));
	CHECK(f.bobGame.npcs[RAIDER_1].dead);
}

TEST("copy hits: a fall with only an effect of ours on the copy is ours, unless something else hit it since; an owner's blow during the wait leaves only our hit's figure")
{
	using std::chrono::seconds;
	const CopyHealth::Clock::time_point t0{};

	// A legendary's bleed of ours ticks the copy to death with no hit event: the fall is ours.
	CopyHealth::Book bleed;
	bleed.Set(RAIDER_1, 30.0f);
	bleed.OurEffect(RAIDER_1, t0);
	CHECK(!bleed.Fell(RAIDER_1, -5.0f, t0 + seconds{ 3 }));
	CHECK(bleed.FellToUs(RAIDER_1));
	CHECK(Near(Took(bleed, RAIDER_1, t0 + seconds{ 3 }), 35.0f));
	CHECK(bleed.Lethal() == 1);

	// A fire hit it after our effect: its ticks are no longer ours alone, nor is the fall.
	CopyHealth::Book burnt;
	burnt.Set(RAIDER_1, 30.0f);
	burnt.OurEffect(RAIDER_1, t0);
	burnt.OtherHit(RAIDER_1, t0 + seconds{ 1 });
	CHECK(burnt.Fell(RAIDER_1, -5.0f, t0 + seconds{ 3 }));
	CHECK(!burnt.Fell(RAIDER_1, -5.0f, t0 + seconds{ 3 } + CopyHealth::JUDGE_AFTER));
	CHECK(!burnt.FellToUs(RAIDER_1));
	CHECK(Took(burnt, RAIDER_1, t0 + seconds{ 4 }) == 0.0f);
	CHECK(burnt.Lethal() == 0);

	// The fall waits for a hit's event; our throw's blast lands in that wait beside our shot: only the shot's figure is
	// ours, the owner's game deals the blast.
	CopyHealth::Book thrown;
	thrown.Set(RAIDER_1, 50.0f);
	CHECK(thrown.Fell(RAIDER_1, -30.0f, t0));
	thrown.OwnersBlow(RAIDER_1, t0 + FRAME);
	thrown.OurHit(RAIDER_1, t0 + FRAME, 20.0f);
	CHECK(!thrown.Fell(RAIDER_1, -30.0f, t0 + FRAME));
	CHECK(Near(Took(thrown, RAIDER_1, t0 + FRAME), 20.0f));
	CHECK(thrown.FellToUs(RAIDER_1));
}

TEST("copy hits: a fall while a report's drop waits for its hit's event books all the copy stood at, once")
{
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book book;
	book.Set(RAIDER_1, 40.0f);
	// Our hit took the copy to 25, its event not here yet, and the runner's report of 40 came: the drop waits (Heard).
	CHECK(Near(book.Heard(RAIDER_1, 25.0f, 40.0f, t0), 40.0f));
	// Our next blow kills it, and both hits' events come a frame later.
	CHECK(book.Fell(RAIDER_1, -10.0f, t0 + FRAME));
	book.OurHit(RAIDER_1, t0 + 2 * FRAME);
	CHECK(!book.Fell(RAIDER_1, -10.0f, t0 + 2 * FRAME));
	const float sent = Took(book, RAIDER_1, t0 + 2 * FRAME);
	CHECK(Near(sent, 50.0f));
	CHECK(sent >= 40.0f + CopyHealth::MIN_LEFT);  // the runner, at its report, takes its last health
	// Seen alive again later (its cell reset: a report sets it), the waiting drop is not booked a second time.
	book.Set(RAIDER_1, 40.0f);
	CHECK(book.Check(RAIDER_1, 40.0f, t0 + 3 * FRAME) == 0.0f);
	CHECK(Took(book, RAIDER_1, t0 + 3 * FRAME) == 0.0f);
	CHECK(!book.FellToUs(RAIDER_1));
}

TEST("copy hits: a copy killed here for another game's word is no fall of ours, though our hit is still in its window")
{
	// The session's console kill (WorldSync's for a death another game reported, Spawns' for a spawn): the NPC is dead there
	// already. It was counted as our kill, and its damage sent only to be refused.
	const CopyHealth::Clock::time_point t0{};
	CopyHealth::Book book;
	book.Set(RAIDER_1, 40.0f);
	book.OurHit(RAIDER_1, t0, 10.0f);
	CHECK(!book.Fell(RAIDER_1, 0.0f, t0 + FRAME, true));
	CHECK(!book.FellToUs(RAIDER_1));
	CHECK(Took(book, RAIDER_1, t0 + FRAME) == 0.0f);
	CHECK(book.Lethal() == 0);
	// Judged: the corpse's next frames book nothing either.
	CHECK(!book.Fell(RAIDER_1, 0.0f, t0 + 2 * FRAME));
	CHECK(Took(book, RAIDER_1, t0 + 2 * FRAME) == 0.0f);

	// A fall of ours already booked stays booked when the word comes after it.
	CopyHealth::Book ours;
	ours.Set(RAIDER_1, 6.0f);
	ours.OurHit(RAIDER_1, t0, 30.0f);
	CHECK(!ours.Fell(RAIDER_1, -24.0f, t0 + FRAME));
	CHECK(!ours.Fell(RAIDER_1, -24.0f, t0 + 2 * FRAME, true));
	CHECK(ours.FellToUs(RAIDER_1));
	CHECK(Near(Took(ours, RAIDER_1, t0 + 2 * FRAME), 30.0f));
}
