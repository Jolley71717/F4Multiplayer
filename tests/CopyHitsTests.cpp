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
		bool                           onlyPlayer = false;  // as before: only our player's blows were ours
		bool                           claimKills = true;   // a killing blow by another actor of ours claims its NPC
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
				if (deathsSent.insert(a_target).second) {
					client.Died(a_target, onlyPlayer ? a_by == PLAYER : CopyHealth::Credited(CauseOf(a_by), local));
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

		Fight()
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

	Fight g;
	g.bobGame.claimKills = false;
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

	CHECK(Judge(player, US) == Hitter::kOurPlayer);
	CHECK(Judge(player, 0) == Hitter::kOurPlayer);
	CHECK(Judge(ourNpc, US) == Hitter::kOurs);
	CHECK(Judge(ourCompanion, US) == Hitter::kOurs);
	CHECK(Judge(robot, US) == Hitter::kOurs);
	for (const auto& cause : { sharedCompanion, friendsNpc, nobodys, turret, standIn, nothing }) {
		CHECK(Judge(cause, US) == Hitter::kNotOurs);
	}
	CHECK(Judge(ourNpc, 0) == Hitter::kNotOurs);  // out of a session
	CHECK(Judge(robot, 0) == Hitter::kNotOurs);

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
	CHECK(book.TakeOurs(RAIDER_1) == 20.0f);

	// A tick a second and a half later, long after the hit's grace: ours, nothing given back.
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 1500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(book.TakeOurs(RAIDER_1) == 5.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 75.0f });

	// And so on to the end of the effect's window; a drop after it is something else's again.
	float health = 75.0f;
	for (auto at = t0 + milliseconds{ 2500 }; at <= t0 + CopyHealth::EFFECT_WINDOW; at += seconds{ 1 }) {
		health -= 5.0f;
		CHECK(book.Check(RAIDER_1, health, at) == 0.0f);
		CHECK(book.Check(RAIDER_1, health, at + CopyHealth::JUDGE_AFTER) == 0.0f);
		CHECK(book.TakeOurs(RAIDER_1) == 5.0f);
	}
	CHECK(book.Check(RAIDER_1, health - 5.0f, t0 + CopyHealth::EFFECT_WINDOW + milliseconds{ 500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, health - 5.0f, t0 + CopyHealth::EFFECT_WINDOW + milliseconds{ 500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	CHECK(book.TakeOurs(RAIDER_1) == 0.0f);
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
	CHECK(book.TakeOurs(RAIDER_1) == 20.0f);

	// A fire in our world (a hazard's hit, no cause) reaches the copy: from then on a drop with no hit behind it may be
	// the fire's as well as the bleed's, so it is given back as before.
	book.OtherHit(RAIDER_1, t0 + milliseconds{ 2000 });
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 2500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 75.0f, t0 + milliseconds{ 2500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	CHECK(book.TakeOurs(RAIDER_1) == 0.0f);

	// A friend's grenade set off here (its blow the owner's: OwnersBlow) beside our own shot: our shot's figure is ours,
	// the rest the grenade's, given back; the NPC's own game deals the grenade's blow.
	CopyHealth::Book other;
	other.Set(RAIDER_2, 100.0f);
	other.OurHit(RAIDER_2, t0, 20.0f);
	other.OurEffect(RAIDER_2, t0);
	other.OtherHit(RAIDER_2, t0 + milliseconds{ 20 });
	other.OwnersBlow(RAIDER_2, t0 + milliseconds{ 20 });
	CHECK(other.Check(RAIDER_2, 30.0f, t0 + FRAME) == 0.0f);
	CHECK(other.TakeOurs(RAIDER_2) == 20.0f);
	CHECK(other.Check(RAIDER_2, 30.0f, t0 + FRAME + CopyHealth::JUDGE_AFTER) == 50.0f);
	CHECK(other.TakeOurs(RAIDER_2) == 0.0f);
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
	CHECK(book.TakeOurs(RAIDER_1) == 20.0f);

	// The owner took a friend's shot there too: 80 -> 60 by its report, set as it comes.
	CHECK(book.Heard(RAIDER_1, 80.0f, 60.0f, t0 + milliseconds{ 1200 }) == 60.0f);
	book.Set(RAIDER_1, 60.0f);
	// Our next tick is from there, ours.
	CHECK(book.Check(RAIDER_1, 55.0f, t0 + milliseconds{ 1500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 55.0f, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(book.TakeOurs(RAIDER_1) == 5.0f);

	// A fire's hit ends the effect's claim: the next drop is given back, to where the copy stood, no higher.
	book.OtherHit(RAIDER_1, t0 + milliseconds{ 2000 });
	CHECK(book.Check(RAIDER_1, 50.0f, t0 + milliseconds{ 2500 }) == 0.0f);
	CHECK(book.Check(RAIDER_1, 50.0f, t0 + milliseconds{ 2500 } + CopyHealth::JUDGE_AFTER) == 5.0f);
	book.Set(RAIDER_1, 55.0f);
	CHECK(book.Health(RAIDER_1) == std::optional{ 55.0f });

	// A report that would raise the copy within the effect's window is held for what our ticks took that may still be
	// on its way, as after a hit; one after the window is set at once.
	CopyHealth::Book late;
	late.Set(RAIDER_2, 100.0f);
	late.OurHit(RAIDER_2, t0, 20.0f);
	late.OurEffect(RAIDER_2, t0);
	CHECK(late.Check(RAIDER_2, 80.0f, t0 + FRAME) == 0.0f);
	CHECK(late.Check(RAIDER_2, 75.0f, t0 + milliseconds{ 1500 }) == 0.0f);
	CHECK(late.Check(RAIDER_2, 75.0f, t0 + milliseconds{ 1500 } + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(late.Heard(RAIDER_2, 75.0f, 80.0f, t0 + milliseconds{ 1700 }) == 75.0f);  // the owner's report from before the tick: held
	CHECK(late.Holds(RAIDER_2));
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
	CHECK(book.TakeOurs(RAIDER_1) == 0.0f);
	book.OurHit(RAIDER_1, t0 + milliseconds{ 16 }, 10.0f);
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 16 }, 100.0f) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER_1), 10.0f));
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
	CHECK(book.TakeOurs(RAIDER_1) == 0.0f);
	// Nor does a hit that comes long after take it.
	book.OurHit(RAIDER_1, t0 + milliseconds{ 3000 }, 10.0f);
	CHECK(book.Check(RAIDER_1, 95.0f, t0 + milliseconds{ 3000 }, 100.0f) == 0.0f);
	CHECK(book.TakeOurs(RAIDER_1) == 0.0f);
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
	CHECK(Near(book.TakeOurs(RAIDER_1), 10.0f));  // in all, not 20
}
