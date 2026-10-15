// A friend's explosion set off in our game is kept from our copies of NPCs another game runs (BlastShield, used by
// WeaponFire::ApplyExplosion). The sessions below go through the real server (tests/sim/Sim.h): the thrower's game
// reports the throw and the explosion, the server passes it on, and the friend's game sets it off in a model of its
// world that keeps health the way the engine does (Npc below), with BlastShield's own
// rules deciding what is lent and given back. Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include "game/BlastShield.h"
#include "game/RemoteEdits.h"
#include "game/ThrowReplays.h"

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <vector>

using namespace Sim;
using Reason = Protocol::ClaimReason;

namespace
{
	constexpr std::uint32_t RAIDER_1 = 0x0002A001;
	constexpr std::uint32_t RAIDER_2 = 0x0002A002;
	constexpr std::uint32_t RAIDER_3 = 0x0002A003;
	constexpr std::uint32_t FRAG = 0x0001D2EE;  // a frag grenade's explosion
	constexpr float         FRAG_RADIUS = 500.0f;

	// An NPC as one game has it: its health the way the engine keeps it, where it stands, and whether it died there.
	// Health is what the record gives plus a permanent, a temporary and a damage modifier. Healing never takes it past
	// the permanent maximum (the record's and the permanent modifier) and puts it there when it is above: the live
	// test's raider, lent 100000 as a temporary modifier and hurt for 251, was "given back" 251 and came down to 30,
	// and taking the loan away killed it. Any change that leaves a living actor with no health kills it. A change of a
	// modifier through the engine is judged by the health the actor had at the last frame too (seen): the live test's
	// two copies, 33 and 18 below nothing of their own under the loan, were healed to their 30 with the loan in it and
	// then had it taken away; both read 30 at once and lay dead at 0 a moment later (a kill leaves a living actor at
	// 0). Modifiers written directly (Write) are judged by nothing.
	struct Npc
	{
		float                x = 0.0f, y = 0.0f;
		float                base = 100.0f;
		float                permanent = 0.0f;
		float                temporary = 0.0f;
		float                damage = 0.0f;
		bool                 dead = false;
		std::optional<float> seen;  // its health at the last frame

		[[nodiscard]] float Maximum() const { return base + permanent; }
		[[nodiscard]] float Health() const { return base + permanent + temporary + damage; }
		void Frame() { seen = Health(); }
		void Settle(float a_change = 0.0f)
		{
			if (dead) {
				return;
			}
			if (Health() <= 0.0f) {
				dead = true;
			} else if (a_change < 0.0f && seen && *seen + a_change <= 0.0f) {
				dead = true;
				damage -= Health();
			}
		}
		void Hurt(float a_amount)
		{
			if (dead) {
				return;
			}
			damage -= a_amount;
			Settle();
		}
		void Restore(float a_amount)  // RestoreActorValue
		{
			if (a_amount > 0.0f) {
				damage += std::min(Health() + a_amount, Maximum()) - Health();
				Settle();
			}
		}
		void ModDamage(float a_amount) { damage += a_amount, Settle(a_amount); }
		void ModPermanent(float a_amount) { permanent += a_amount, Settle(a_amount); }
		void ModTemporary(float a_amount) { temporary += a_amount, Settle(a_amount); }
		void Write(const BlastShield::Modifiers& a_modifiers)
		{
			permanent = a_modifiers.permanent;
			temporary = a_modifiers.temporary;
			damage = a_modifiers.damage;
		}
		[[nodiscard]] BlastShield::Modifiers Modifiers() const { return { permanent, temporary, damage }; }
	};

	constexpr float GRENADE_DAMAGE = 100.0f;                    // a grenade of a copy's own, at its feet
	constexpr auto  FUSE = std::chrono::milliseconds{ 1000 };    // from leaving the hand to going off

	// One player's game: its NPCs, and what WeaponFire and WorldSync do there with a friend's explosion, a friend's
	// throw replayed on a copy, our own player's blows and another game's health report. A death goes out once, as
	// WorldSync reports it. Time goes by a frame at a time (Wait).
	struct Game
	{
		Client&                        client;
		std::map<std::uint32_t, Npc>   npcs;
		BlastShield::Book              lending;
		ThrowReplays::Book             throws;
		BlastShield::Clock::time_point now{};
		std::set<std::uint32_t>        reported;
		// Loans paid back as WeaponFire did before: healed with the loan in it, then the loan taken away, through the
		// engine.
		bool throughEngine = false;

		struct Grenade
		{
			float                          x = 0.0f, y = 0.0f;
			BlastShield::Clock::time_point at{};
		};
		std::vector<Grenade> grenades;  // real ones in this game, thrown by our copies themselves

		// WeaponFire::ApplyExplosion: our copies of NPCs another game runs are lent health first, then the
		// explosion hurts everyone in reach (a_damage, the same to each here).
		void SetOff(const Protocol::Explosion& a_explosion, float a_damage)
		{
			const auto reach = [&](const Npc& a_npc, float a_range) {
				return RemoteEdits::Within(std::hypot(a_npc.x - a_explosion.x, a_npc.y - a_explosion.y), a_range);
			};
			for (auto& [id, npc] : npcs) {
				if (!npc.dead && reach(npc, FRAG_RADIUS + BlastShield::MARGIN) && BlastShield::Shields(client.OwnerOf(id), client.Id())) {
					const float was = npc.Health();
					if (const float amount = lending.Begin(id, was, now); amount > 0.0f) {
						npc.ModPermanent(amount);
						if (npc.Health() < was + amount * 0.5f) {
							npc.Restore(amount);
						}
					}
				}
			}
			for (auto& [id, npc] : npcs) {
				if (reach(npc, FRAG_RADIUS)) {
					Hurt(id, a_damage);
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

		// WorldSync::ReportDeaths.
		void Died(std::uint32_t a_id)
		{
			if (npcs.at(a_id).dead && reported.insert(a_id).second) {
				client.Kill(a_id);
			}
		}

		// WeaponFire::EndLendingOf: the copy's modifiers are written to leave it the health it keeps with the loan gone,
		// then a copy our player's blows killed is killed.
		void EndLending(std::uint32_t a_id)
		{
			auto&      npc = npcs.at(a_id);
			const bool wasDead = npc.dead;
			const auto release = lending.End(a_id, npc.Health(), npc.dead);
			if (!release) {
				return;
			}
			if (throughEngine) {
				const float delta = release->health + release->lent - npc.Health();
				if (delta < 0.0f) {
					npc.ModDamage(delta);
				} else {
					npc.Restore(delta);
				}
				npc.ModPermanent(-release->lent);
			} else {
				npc.Write(BlastShield::Repaid(npc.Modifiers(), npc.base, release->lent, release->health));
				// Paying the loan back never kills a copy, nor leaves the loan's worth on a corpse.
				CHECK((wasDead || !npc.dead));
				CHECK(npc.permanent == 0.0f);
				CHECK(npc.Health() > -1.0f);
				CHECK((wasDead || release->kill || npc.Health() == release->health));
			}
			if (release->kill) {
				npc.dead = true;  // the console's kill
			}
			Died(a_id);
		}

		// Time goes by in this game, a frame at a time: grenades go off, lending that is over ends (WeaponFire::Frame).
		void Wait(BlastShield::Clock::duration a_for)
		{
			constexpr BlastShield::Clock::duration FRAME = std::chrono::milliseconds{ 50 };
			const auto                             until = now + a_for;
			while (now < until) {
				now = std::min(now + FRAME, until);
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
				for (const auto id : lending.Due(now)) {
					EndLending(id);
				}
				for (auto& [id, npc] : npcs) {
					npc.Frame();
				}
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
	CHECK(c.bobGame.npcs[RAIDER_1].permanent == 0.0f);
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

TEST("blast shield: a host raider throwing four grenades two seconds apart, each throw replayed by the friend's copy, is alive in both games afterwards")
{
	// The live test: the host's raider 0007BCED (30 health, all of it) threw four times, two seconds apart. The friend's
	// copy replays each throw, its graph letting go of each half a second after the next replay (later than the two
	// seconds the ghost watch once kept), and each explosion is set off beside it; someone's grenade goes off at its
	// feet under the loan too.
	Corvega c;
	auto&   host = c.host;
	auto&   bob = c.bob;
	for (auto* game : { &c.hostGame, &c.bobGame }) {
		game->npcs[RAIDER_1].base = 30.0f;
	}
	const auto shotsBefore = bob.Heard(MT::kShotFired);
	const float blasts[] = { 70.0f, 60.0f, 66.0f, 55.0f };  // 251 in all
	for (int i = 0; i < 4; ++i) {
		host.Raw(NpcThrow(RAIDER_1), false);
		c.s.Settle();
		REQUIRE(bob.Heard(MT::kShotFired) - shotsBefore == static_cast<std::size_t>(i + 1));  // to be replayed
		c.bobGame.ReplayThrow(RAIDER_1);
		host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
		c.s.Settle();
		REQUIRE(bob.Explosions().size() == static_cast<std::size_t>(i + 1));
		c.bobGame.SetOff(bob.Explosions().back(), blasts[i]);
		CHECK(!c.bobGame.npcs[RAIDER_1].dead);
		c.bobGame.Wait(std::chrono::milliseconds{ 500 });
		if (i > 0) {
			c.bobGame.LetGo(RAIDER_1);  // the last throw's release, 2.5 s after its replay
		}
		if (i == 1) {
			c.bobGame.Wait(std::chrono::milliseconds{ 500 });
			c.bobGame.Hurt(RAIDER_1, 40.0f);  // someone's grenade, under the loan
			c.bobGame.Wait(std::chrono::seconds{ 1 });
		} else {
			c.bobGame.Wait(std::chrono::milliseconds{ 1500 });
		}
		CHECK(c.bobGame.lending.Lent(RAIDER_1));  // each explosion holds the loan on
	}
	c.bobGame.Wait(std::chrono::milliseconds{ 500 });
	c.bobGame.LetGo(RAIDER_1);
	CHECK(c.bobGame.grenades.empty());  // not one of the copy's own left its hand
	CHECK(c.bobGame.throws.Size() == 0);
	c.bobGame.Wait(BlastShield::HOLD + FUSE);
	CHECK(!c.bobGame.lending.Lent(RAIDER_1));
	CHECK(!c.bobGame.npcs[RAIDER_1].dead);
	CHECK(c.bobGame.npcs[RAIDER_1].Health() == 30.0f);
	CHECK(c.bobGame.npcs[RAIDER_1].permanent == 0.0f);

	// A fifth, after the loan ended: lent afresh, and paid back again.
	host.Raw(NpcThrow(RAIDER_1), false);
	host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
	c.s.Settle();
	REQUIRE(bob.Explosions().size() == 5);
	c.bobGame.SetOff(bob.Explosions().back(), 50.0f);
	c.bobGame.Wait(BlastShield::HOLD);
	CHECK(!c.bobGame.npcs[RAIDER_1].dead);
	CHECK(c.bobGame.npcs[RAIDER_1].Health() == 30.0f);
	c.s.Settle();
	for (const auto player : { &host, &bob }) {
		CHECK(!player->WorldNow().dead.contains(RAIDER_1));
	}
	CHECK(bob.OwnerOf(RAIDER_1) == host.Id());
	CHECK(!c.s.Logged("death of 0002A001"));
}

TEST("blast shield: the first loan, a temporary modifier, killed the copy when it was paid back")
{
	// Why the live test's copy died with -100020 health: healing never goes past the permanent maximum, so giving a
	// copy lent health as a temporary modifier back the 251 the grenades took set it to that maximum, 30, and taking
	// the loan away left it 99970 below nothing, dead (the fifth grenade then hit the corpse for 50).
	Npc raider{ .base = 30.0f };
	raider.ModTemporary(BlastShield::LEND);
	raider.Hurt(251.0f);
	CHECK(!raider.dead);
	raider.Restore(251.0f);
	CHECK(raider.Health() == 30.0f);
	raider.ModTemporary(-BlastShield::LEND);
	CHECK(raider.dead);
	CHECK(raider.Health() == 30.0f - BlastShield::LEND);

	// Lent as a permanent modifier and paid back as WeaponFire does now, it keeps its 30.
	Npc copy{ .base = 30.0f };
	copy.ModPermanent(BlastShield::LEND);
	copy.Hurt(251.0f);
	copy.Frame();
	BlastShield::Book book;
	book.Begin(RAIDER_1, 30.0f, {});
	const auto release = book.End(RAIDER_1, copy.Health(), copy.dead);
	REQUIRE(release);
	copy.Write(BlastShield::Repaid(copy.Modifiers(), copy.base, release->lent, release->health));
	CHECK(!copy.dead);
	CHECK(copy.Health() == 30.0f);
	CHECK(copy.permanent == 0.0f);
	CHECK(copy.damage == 0.0f);
}

TEST("blast shield: the second loan, paid back through the engine's changes, killed the copies a blast took below nothing of their own")
{
	// Why the live test's two copies lay dead at 0 a moment after their loans ended with 30 each: the blast had left
	// them 33 and 18 below nothing of their own, they were healed to 30 with the loan in them, and taking the loan away
	// was judged by the health they had before that healing.
	Npc raider{ .base = 30.0f };
	raider.ModPermanent(BlastShield::LEND);
	raider.Hurt(63.0f);
	raider.Frame();
	raider.Restore(63.0f);
	CHECK(raider.Health() == 30.0f + BlastShield::LEND);
	raider.ModPermanent(-BlastShield::LEND);
	CHECK(raider.dead);
	CHECK(raider.Health() == 0.0f);

	// One the blast left with some of its own came through it (a blast of 20).
	Npc spared{ .base = 30.0f };
	spared.ModPermanent(BlastShield::LEND);
	spared.Hurt(20.0f);
	spared.Frame();
	spared.Restore(20.0f);
	spared.ModPermanent(-BlastShield::LEND);
	CHECK(!spared.dead);

	// The loan paid back by writing the modifiers, from the same state: alive with its 30, and nothing lent left.
	Npc copy{ .base = 30.0f };
	copy.ModPermanent(BlastShield::LEND);
	copy.Hurt(63.0f);
	copy.Frame();
	copy.Write(BlastShield::Repaid(copy.Modifiers(), copy.base, BlastShield::LEND, 30.0f));
	CHECK(!copy.dead);
	CHECK(copy.Health() == 30.0f);
	CHECK(copy.permanent == 0.0f);

	// What Repaid leaves: the loan out of the permanent modifier, the temporary one as it was, the damage one making up
	// the rest, never above nothing.
	const auto repaid = BlastShield::Repaid({ .permanent = BlastShield::LEND + 5.0f, .temporary = 10.0f, .damage = -300.0f }, 30.0f, BlastShield::LEND, 25.0f);
	CHECK(repaid.permanent == 5.0f);
	CHECK(repaid.temporary == 10.0f);
	CHECK(repaid.damage == -20.0f);  // 30 + 5 + 10 - 20 = 25
	CHECK(BlastShield::Repaid({ .permanent = BlastShield::LEND }, 30.0f, BlastShield::LEND, 80.0f).damage == 0.0f);  // no more than its maximum
	CHECK(BlastShield::Repaid({ .permanent = BlastShield::LEND, .damage = -500.0f }, 30.0f, BlastShield::LEND, 0.0f).damage == -30.0f);  // a corpse at 0
}

namespace
{
	constexpr std::uint32_t RAIDER_4 = 0x0002A004;

	// The live test's third round: the host's raider (RAIDER_1, 30 health) throws once; the friend's copy replays the
	// throw (a_kept: as WeaponFire does now, keeping its release from the engine), and its graph lets go of it two and
	// a half seconds later, after the two seconds the ghost watch once kept. The owner's explosion arrives a moment
	// after the replay and takes 63 from each copy beside it, the raider's and another raider the host's game runs
	// (RAIDER_4, 30 health); their loans end three seconds later.
	void ThrowOnceBesideTwo(Corvega& c, bool a_kept)
	{
		c.host.Claim(RAIDER_4);
		c.s.Settle();
		REQUIRE(c.bob.OwnerOf(RAIDER_4) == c.host.Id());
		c.host.RunNpc(RAIDER_4, 1180.0f, 2120.0f);
		c.s.Settle();
		for (auto* game : { &c.hostGame, &c.bobGame }) {
			game->npcs[RAIDER_1].base = 30.0f;
			game->npcs[RAIDER_4] = Npc{ .x = 1180.0f, .y = 2120.0f, .base = 30.0f };
		}
		const auto shotsBefore = c.bob.Heard(MT::kShotFired);
		c.host.Raw(NpcThrow(RAIDER_1), false);
		c.s.Settle();
		REQUIRE(c.bob.Heard(MT::kShotFired) == shotsBefore + 1);
		if (a_kept) {
			c.bobGame.ReplayThrow(RAIDER_1);
		}
		c.bobGame.Wait(std::chrono::milliseconds{ 150 });
		c.host.Raw(ExplosionAt(1220.0f, 2100.0f), false);
		c.s.Settle();
		REQUIRE(c.bob.Explosions().size() == 1);
		c.bobGame.SetOff(c.bob.Explosions().front(), 63.0f);
		CHECK(!c.bobGame.npcs[RAIDER_1].dead);
		CHECK(!c.bobGame.npcs[RAIDER_4].dead);
		c.bobGame.Wait(std::chrono::milliseconds{ 2350 });
		c.bobGame.LetGo(RAIDER_1);
		c.bobGame.Wait(std::chrono::seconds{ 10 });
		c.s.Settle();
	}
}

TEST("blast shield: a host raider's throw replayed by the friend's copy throws no grenade of the copy's own, however late its graph lets go")
{
	Corvega c;
	ThrowOnceBesideTwo(c, true);
	CHECK(c.bobGame.grenades.empty());
	CHECK(c.bobGame.throws.Size() == 0);
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(!c.bobGame.npcs[id].dead);
		CHECK(c.bobGame.npcs[id].Health() == 30.0f);
		CHECK(c.bobGame.npcs[id].permanent == 0.0f);
		for (const auto player : { &c.host, &c.bob }) {
			CHECK(!player->WorldNow().dead.contains(id));
		}
	}
	CHECK(!c.s.Logged("death of 0002A001"));
	CHECK(!c.s.Logged("death of 0002A004"));
}

TEST("blast shield: had the copy's graph let go of the replayed throw to the engine, its own grenade would have killed both copies after their loans, in the friend's game only")
{
	Corvega c;
	ThrowOnceBesideTwo(c, false);
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(c.bobGame.npcs[id].dead);
		CHECK(c.bobGame.npcs[id].Health() < -1.0f);  // a grenade's damage: not the 0 the live test showed
		CHECK(c.bob.WorldNow().dead.contains(id));
		CHECK(!c.host.WorldNow().dead.contains(id));
	}
	CHECK(c.s.Logged("death of 0002A001 from player"));
}

TEST("blast shield: the third round's copies, their loans paid back through the engine's changes, lay dead at 0 in the friend's game only")
{
	Corvega c;
	c.bobGame.throughEngine = true;
	ThrowOnceBesideTwo(c, true);
	CHECK(c.bobGame.grenades.empty());
	for (const auto id : { RAIDER_1, RAIDER_4 }) {
		CHECK(c.bobGame.npcs[id].dead);
		CHECK(c.bobGame.npcs[id].Health() == 0.0f);  // "hp=-0"
		CHECK(c.bob.WorldNow().dead.contains(id));
		CHECK(!c.host.WorldNow().dead.contains(id));
	}
	CHECK(c.s.Logged("death of 0002A001 from player"));
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
	CHECK(c.bobGame.npcs[RAIDER_1].permanent == 0.0f);

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
	CHECK(c.bobGame.npcs[RAIDER_1].permanent == 0.0f);
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
	CHECK(back->health == 60.0f);  // what it had: the 300 the blasts took come back
	CHECK(back->lent == BlastShield::LEND);
	CHECK(!back->kill);
	CHECK(!book.End(RAIDER_1, 60.0f, false));  // ended already

	// Its owner's game said it died (a console kill): the loan goes, and the corpse never reads its worth below nothing.
	CHECK(book.Begin(RAIDER_2, 60.0f, t0) == BlastShield::LEND);
	const auto dead = book.End(RAIDER_2, 0.0f, true);
	REQUIRE(dead);
	CHECK(dead->health == 0.0f);
	CHECK(!dead->kill);
	CHECK(dead->lent == BlastShield::LEND);

	// A stimpak meanwhile: it keeps what it gained.
	CHECK(book.Begin(RAIDER_3, 60.0f, t0) == BlastShield::LEND);
	const auto healed = book.End(RAIDER_3, 90.0f + BlastShield::LEND, false);
	REQUIRE(healed);
	CHECK(healed->health == 90.0f);

	// A copy with no health left is none of the lending's business, and only so many are lent at once.
	CHECK(book.Begin(0x0002B000, 0.0f, t0) == 0.0f);
	for (std::uint32_t i = 0; i < BlastShield::MAX_LENT; ++i) {
		book.Begin(0x0003A000 + i, 50.0f, t0);
	}
	CHECK(book.Size() == BlastShield::MAX_LENT);
	CHECK(book.Begin(0x0002B001, 50.0f, t0) == 0.0f);
}
