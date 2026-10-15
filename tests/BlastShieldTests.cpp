// A friend's explosion set off in our game is kept from our copies of NPCs another game runs (BlastShield, used by
// WeaponFire::ApplyExplosion). The sessions below go through the real server (tests/sim/Sim.h): the thrower's game
// reports the throw and the explosion, the server passes it on, and the friend's game sets it off in a model of its
// world that keeps health the way the engine does (base, a temporary modifier, damage), with BlastShield's own
// rules deciding what is lent and given back. Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include "game/BlastShield.h"
#include "game/RemoteEdits.h"

#include <cmath>
#include <map>
#include <set>

using namespace Sim;
using Reason = Protocol::ClaimReason;

namespace
{
	constexpr std::uint32_t RAIDER_1 = 0x0002A001;
	constexpr std::uint32_t RAIDER_2 = 0x0002A002;
	constexpr std::uint32_t RAIDER_3 = 0x0002A003;
	constexpr std::uint32_t FRAG = 0x0001D2EE;  // a frag grenade's explosion
	constexpr float         FRAG_RADIUS = 500.0f;

	// An NPC as one game has it: its health the way the engine keeps it (what it has from its record, a temporary
	// modifier such as the health lent against an explosion, and the damage it took), where it stands, and whether
	// it died there. The engine kills an actor whose health reaches nothing.
	struct Npc
	{
		float x = 0.0f, y = 0.0f;
		float base = 100.0f;
		float temporary = 0.0f;
		float damage = 0.0f;
		bool  dead = false;

		[[nodiscard]] float Health() const { return base + temporary + damage; }
		void Hurt(float a_amount)
		{
			if (dead) {
				return;
			}
			damage -= a_amount;
			dead = Health() <= 0.0f;
		}
		void Restore(float a_amount) { damage = std::min(0.0f, damage + a_amount); }  // never past its maximum
	};

	// One player's game: its NPCs, and what WeaponFire and WorldSync do there with a friend's explosion, our own
	// player's blows and another game's health report. A death goes out once, as WorldSync reports it.
	struct Game
	{
		Client&                        client;
		std::map<std::uint32_t, Npc>   npcs;
		BlastShield::Book              lending;
		BlastShield::Clock::time_point now{};
		std::set<std::uint32_t>        reported;

		// WeaponFire::ApplyExplosion: our copies of NPCs another game runs are lent health first, then the
		// explosion hurts everyone in reach (a_damage, the same to each here).
		void SetOff(const Protocol::Explosion& a_explosion, float a_damage)
		{
			const auto reach = [&](const Npc& a_npc, float a_range) {
				return RemoteEdits::Within(std::hypot(a_npc.x - a_explosion.x, a_npc.y - a_explosion.y), a_range);
			};
			for (auto& [id, npc] : npcs) {
				if (!npc.dead && reach(npc, FRAG_RADIUS + BlastShield::MARGIN) && BlastShield::Shields(client.OwnerOf(id), client.Id())) {
					npc.temporary += lending.Begin(id, npc.Health(), now);
				}
			}
			for (auto& [id, npc] : npcs) {
				if (reach(npc, FRAG_RADIUS)) {
					Hurt(id, a_damage);
				}
			}
		}

		// WorldSync::ReportDeaths.
		void Died(std::uint32_t a_id)
		{
			if (npcs.at(a_id).dead && reported.insert(a_id).second) {
				client.Kill(a_id);
			}
		}

		// WeaponFire::EndLendingOf.
		void EndLending(std::uint32_t a_id)
		{
			auto&      npc = npcs.at(a_id);
			const auto release = lending.End(a_id, npc.Health(), npc.dead);
			if (!release) {
				return;
			}
			npc.Restore(release->restore);
			if (release->kill) {
				npc.dead = true;  // the console's kill
			}
			npc.temporary -= release->lent;
			// Taking the lent health away never leaves a living copy at nothing (the engine would not know what to do).
			CHECK((npc.dead || npc.Health() > 0.0f));
			Died(a_id);
		}

		// Time goes by in this game: lending that is over ends (WeaponFire::Frame).
		void Wait(BlastShield::Clock::duration a_for)
		{
			now += a_for;
			for (const auto id : lending.Due(now)) {
				EndLending(id);
			}
		}

		// Something other than our player hurts an NPC here.
		void Hurt(std::uint32_t a_id, float a_damage)
		{
			npcs.at(a_id).Hurt(a_damage);
			Died(a_id);
		}

		// A frame of our player's blows (WorldSync's HitWatcher, then its Frame): a copy another game runs is claimed
		// (which tells the server we fight it), lending ends keeping the blows (WeaponFire::OurBlows), and the health
		// of one still alive goes out; one that died of them is reported after the claim.
		void OurBlows(const std::vector<std::pair<std::uint32_t, float>>& a_blows)
		{
			for (const auto& [id, damage] : a_blows) {
				npcs.at(id).Hurt(damage);
				if (client.OwnerOf(id) != client.Id()) {
					client.Claim(id, Reason::kInteract);
				}
				lending.OurBlow(id, damage);
			}
			for (const auto& [id, damage] : a_blows) {
				EndLending(id);
				const auto& npc = npcs.at(id);
				if (!npc.dead) {
					client.Hurt(id, npc.Health() / npc.base);
				}
				Died(id);
			}
		}

		void OurBlow(std::uint32_t a_id, float a_damage) { OurBlows({ { a_id, a_damage } }); }

		// WorldSync::TrySetHealth: another game's report of the NPC's health, measured without lent health.
		void HealthReport(std::uint32_t a_id, float a_fraction)
		{
			EndLending(a_id);
			auto&       npc = npcs.at(a_id);
			const float delta = a_fraction * npc.base - npc.Health();
			if (delta < 0.0f) {
				npc.damage += delta;
			} else {
				npc.Restore(delta);
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
	};
}

TEST("blast shield: only copies of NPCs another game runs are kept from a friend's explosion")
{
	CHECK(BlastShield::Shields(2, 1));   // the friend's (or a third player's) NPC
	CHECK(!BlastShield::Shields(1, 1));  // ours: the explosion is what hurts it here
	CHECK(!BlastShield::Shields(0, 1));  // nobody runs it: hurt here like ours
}

TEST("blast shield: a host raider's grenade set off in a friend's game leaves the friend's copy of it alive, as it is in the host's")
{
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	// The fight has worn the host's raider down further in the host's game than the friend's copy shows.
	c.hostGame.npcs[RAIDER_1].damage = -70.0f;
	c.bobGame.npcs[RAIDER_1].damage = -40.0f;

	// The host's raider throws a grenade at its own feet. The host's game sets it off for real (it is not
	// replayed there) and reports the throw and the explosion.
	host.Raw(NpcThrow(RAIDER_1), false);
	host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.hostGame.Hurt(RAIDER_1, 20.0f);  // it survives its own grenade in the host's game: 10 health left
	c.s.Settle();
	REQUIRE(bob.Explosions().size() == 1);
	const auto explosion = bob.Explosions().front();
	CHECK(explosion.playerId == host.Id());
	CHECK(RemoteEdits::SetOff(RemoteEdits::SpotOf(explosion), { 0, COMMONWEALTH, 1100.0f, 2000.0f }, FRAG_RADIUS) == RemoteEdits::Blast::kSetOff);

	// The friend's game sets it off: enough to kill anything beside it.
	c.bobGame.SetOff(explosion, 150.0f);
	CHECK(!c.bobGame.npcs[RAIDER_1].dead);  // the host's raider: its owner dealt that damage already
	CHECK(c.bobGame.npcs[RAIDER_2].dead);   // the friend's own raider dies of it, as in the release notes
	CHECK(c.bobGame.npcs[RAIDER_3].dead);   // so does one nobody runs
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(c.bobGame.npcs[RAIDER_1].Health() == 60.0f);  // what it had before: nothing the blast took is kept
	CHECK(c.bobGame.npcs[RAIDER_1].temporary == 0.0f);
	CHECK(c.bobGame.lending.Size() == 0);
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

TEST("blast shield: without the lending, the friend's copy would die of the host raider's grenade only in the friend's game")
{
	// What the friend saw before: the replayed grenade killed their copy, its death was reported, and the server
	// rightly dropped it (the host's game runs the raider and the friend wasn't fighting it): a corpse in one game, a
	// raider fighting on in the other.
	Corvega c;
	c.host.Raw(NpcThrow(RAIDER_1), false);
	c.host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(c.bob.Explosions().size() == 1);
	c.bobGame.Hurt(RAIDER_1, 150.0f);  // set off with nothing lent
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
	c.bobGame.Hurt(RAIDER_2, 150.0f);  // in Bob's game his grenade kills his own raider too
	c.hostGame.Wait(BlastShield::HOLD);
	c.s.Settle();
	for (const auto player : { &host, &bob }) {
		CHECK(player->WorldNow().dead.contains(RAIDER_1));
		CHECK(player->WorldNow().dead.contains(RAIDER_2));
	}
}

TEST("blast shield: a copy kept from a friend's explosion that the player then shoots loses only what the shots take, and dies when they are enough")
{
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	c.bobGame.npcs[RAIDER_1].damage = -40.0f;  // 60 left
	host.Raw(NpcThrow(RAIDER_1), false);
	host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(bob.Explosions().size() == 1);
	c.bobGame.SetOff(bob.Explosions().front(), 150.0f);
	REQUIRE(c.bobGame.lending.Lent(RAIDER_1));

	// Bob shoots it a moment later: the blast's damage is given back, his shot's kept, at once.
	c.bobGame.Wait(std::chrono::milliseconds{ 500 });
	c.bobGame.OurBlow(RAIDER_1, 25.0f);
	CHECK(!c.bobGame.lending.Lent(RAIDER_1));
	CHECK(c.bobGame.npcs[RAIDER_1].Health() == 35.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].temporary == 0.0f);

	// Another grenade from the host's raider, then Bob's shotgun: the pellets alone are more than it has left.
	host.Raw(NpcThrow(RAIDER_1), false);
	host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(bob.Explosions().size() == 2);
	c.bobGame.SetOff(bob.Explosions().back(), 150.0f);
	CHECK(!c.bobGame.npcs[RAIDER_1].dead);
	c.bobGame.OurBlows({ { RAIDER_1, 20.0f }, { RAIDER_1, 20.0f } });  // a frame's pellets, counted together
	CHECK(c.bobGame.npcs[RAIDER_1].dead);
	c.s.Settle();
	CHECK(host.WorldNow().dead.contains(RAIDER_1));  // his kill: the server believes it, he was fighting it
}

TEST("blast shield: a health report for a lent copy is measured without the lent health")
{
	Corvega c;
	auto&   bob = c.bob;
	c.host.Raw(NpcThrow(RAIDER_1), false);
	c.host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(bob.Explosions().size() == 1);
	c.bobGame.SetOff(bob.Explosions().front(), 150.0f);
	c.bobGame.HealthReport(RAIDER_1, 0.3f);  // the host shot it meanwhile
	CHECK(!c.bobGame.lending.Lent(RAIDER_1));
	CHECK(c.bobGame.npcs[RAIDER_1].Health() == 30.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].temporary == 0.0f);
	c.bobGame.Wait(BlastShield::HOLD);  // nothing left to give back
	CHECK(c.bobGame.npcs[RAIDER_1].Health() == 30.0f);
}

TEST("blast shield: lending is kept once per copy however many explosions, ends for a copy that died meanwhile, and keeps health it gained")
{
	using BlastShield::Book;
	const BlastShield::Clock::time_point t0{};
	Book book;
	CHECK(book.Begin(RAIDER_1, 60.0f, t0) == BlastShield::LEND);
	CHECK(book.Begin(RAIDER_1, 60.0f + BlastShield::LEND - 150.0f, t0 + std::chrono::seconds{ 2 }) == 0.0f);  // a second grenade
	CHECK(book.Due(t0 + BlastShield::HOLD).empty());  // held from the second one
	CHECK(book.Due(t0 + std::chrono::seconds{ 2 } + BlastShield::HOLD).size() == 1);
	const auto back = book.End(RAIDER_1, 60.0f + BlastShield::LEND - 300.0f, false);
	REQUIRE(back);
	CHECK(back->restore == 300.0f);
	CHECK(back->lent == BlastShield::LEND);
	CHECK(!back->kill);
	CHECK(!book.End(RAIDER_1, 60.0f, false));  // ended already

	// Its owner's game said it died (a console kill): only the lent health goes.
	CHECK(book.Begin(RAIDER_2, 60.0f, t0) == BlastShield::LEND);
	const auto dead = book.End(RAIDER_2, 0.0f, true);
	REQUIRE(dead);
	CHECK(dead->restore == 0.0f);
	CHECK(!dead->kill);
	CHECK(dead->lent == BlastShield::LEND);

	// A stimpak meanwhile: it keeps what it gained.
	CHECK(book.Begin(RAIDER_3, 60.0f, t0) == BlastShield::LEND);
	const auto healed = book.End(RAIDER_3, 90.0f + BlastShield::LEND, false);
	REQUIRE(healed);
	CHECK(healed->restore == 0.0f);

	// A copy with no health left is none of the lending's business, and only so many are lent at once.
	CHECK(book.Begin(0x0002B000, 0.0f, t0) == 0.0f);
	for (std::uint32_t i = 0; i < BlastShield::MAX_LENT; ++i) {
		book.Begin(0x0003A000 + i, 50.0f, t0);
	}
	CHECK(book.Size() == BlastShield::MAX_LENT);
	CHECK(book.Begin(0x0002B001, 50.0f, t0) == 0.0f);
}
