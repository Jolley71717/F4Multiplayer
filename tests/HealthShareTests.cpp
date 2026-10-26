// The health of an NPC as the game that runs it has it. A player's report on Nexus: throwing a Molotov "transfers" the
// ability to deal damage from one player to the other. What the fire took off an NPC in the game that runs it was never
// reported (a report went out only after a hit), the friend's copy kept it (CopyWatch gives a copy back what no hit of
// ours took), and the friend's next shot reported the copy's health less the shot: the fire's damage was healed off the
// NPC and the shot seemed to do nothing. Now the game that runs an NPC shares its health whenever it changes (HealthShare),
// and a friend's hit on their copy goes as the damage it did (CopyHealth::Book::TakeOurs, kReportDamage), which the
// NPC's game takes off its own health.
//
// The sessions go through the real server (tests/sim/Sim.h), the reports encoded and decoded as the games send them;
// each game is a model of its world (Game below). Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

#include "game/CopyHealth.h"
#include "game/HealthShare.h"

#include <algorithm>
#include <cmath>
#include <thread>

using namespace Sim;

namespace
{
	using Clock = HealthShare::Clock;

	constexpr std::uint32_t RAIDER = 0x0002A001;
	constexpr auto          FRAME = std::chrono::milliseconds{ 50 };

	bool Near(float a_health, float a_expected, float a_slack = 0.6f)
	{
		return std::abs(a_health - a_expected) < a_slack;
	}

	// One player's game with the raider (100 health at most). Its player's shot is a hit; a burn is damage with no hit
	// (the fire a Molotov leaves). Each frame, as CopyWatch and WorldSync do: a copy (another game runs the raider) sends
	// what our hits took off it as damage and gets back what nothing of ours took; the game that runs it shares its
	// health when it changed. With 0.8.10 on (as before): a hit on a copy reported the copy's health, and the game that
	// runs it never shared anything but its own hits.
	struct Game
	{
		Client&              client;
		float                health = 100.0f;
		bool                 v0810 = false;
		CopyHealth::Book     copy;
		HealthShare::Book    share;
		Clock::time_point    now{};
		float                givenBack = 0.0f;
		std::uint32_t        reports = 0;
		std::optional<float> applied;

		bool Runs() const { return client.OwnerOf(RAIDER) == client.Id(); }

		void Report(float a_fraction)
		{
			client.Hurt(RAIDER, a_fraction);
			++reports;
		}

		// Our player hits the raider for a_damage; what follows goes the next frame.
		void Shoot(float a_damage)
		{
			health -= a_damage;
			copy.OurHit(RAIDER, now);
			Frame();
			if (Runs() || v0810) {
				Report(health / 100.0f);
				share.Sent(RAIDER, health / 100.0f, now);
			}
		}

		// Our player's blow of a_damage, the hit event read as a_blow, taken as CopyWatch and WorldSync take it: on a copy,
		// one OwnersToDeal leaves to the NPC's own game is no hit of ours (noted as the owner's, given back, not sent) and
		// claims nothing; any other is our hit (its event's figure a_damage) and claims the raider (a takeover the server grants 5 s after it last changed hands), the claim leaving at the
		// end of the frame, ahead of the damage the next frame sends (Session::Frame). a_rule090: 0.9.0's OwnersToDeal,
		// which took any blow whose event read an explosion, a throwable or a spell with no weapon for a throw's.
		void Strike(float a_damage, const CopyHealth::Blow& a_blow, bool a_rule090 = false)
		{
			const bool onCopy = !Runs();
			const bool owners = a_rule090 ? (a_blow.explosion || a_blow.throwable || a_blow.spellAlone) : CopyHealth::OwnersToDeal(a_blow);
			const auto after = CopyHealth::After(CopyHealth::Hitter::kOurPlayer, client.OwnerOf(RAIDER), client.Id(), false, owners);
			health -= a_damage;
			if (onCopy && owners) {
				copy.OwnersBlow(RAIDER, now);
			} else {
				copy.OurHit(RAIDER, now, a_damage);
			}
			if (after.claim) {
				client.Claim(RAIDER, Protocol::ClaimReason::kInteract);
			}
			Frame();
			if (!onCopy && after.report) {
				Report(health / 100.0f);
				share.Sent(RAIDER, health / 100.0f, now);
			}
		}

		// A fire takes a_damage off the raider here, a_ticks frames in a row (no hit).
		void Burn(float a_damage, int a_ticks = 1)
		{
			for (int i = 0; i < a_ticks; ++i) {
				health -= a_damage;
				Frame();
			}
		}

		void Frame()
		{
			now += FRAME;
			if (!Runs()) {
				const float give = copy.Check(RAIDER, health, now);
				if (const float ours = copy.TakeOurs(RAIDER); ours > 0.0f && !v0810) {
					client.Damage(RAIDER, ours / 100.0f);
				}
				if (give > 0.0f) {
					health += give;
					givenBack += give;
					copy.Set(RAIDER, health);
				}
			} else if (!v0810 && share.Due(RAIDER, health / 100.0f, now)) {
				Report(health / 100.0f);
			}
		}

		void Wait(Clock::duration a_for)
		{
			const auto until = now + a_for;
			while (now < until) {
				Frame();
			}
		}

		// What the session passed on, applied as WorldSync does: a report sets the health (TrySetHealth), a friend's damage
		// is taken off if our game runs it (ApplyRemoteDamage).
		void Apply()
		{
			const auto heard = client.HealthHeard(RAIDER);
			if (heard && applied != heard) {
				applied = heard;
				const float before = health;
				health = *heard * 100.0f;
				copy.Heard(RAIDER, before, health, now);
			}
			for (const auto& damage : client.TakeDamageHeard()) {
				if (damage.refId == RAIDER && Runs()) {
					health -= damage.health * 100.0f;
				}
			}
		}
	};

	// The host's game runs the raider; Bob stands beside him with his copy of it. Both at full health, both games have
	// seen it.
	struct Fight
	{
		Session s;
		Client& host = s.Host();
		Client& bob = s.Player("Bob");
		Game    hostGame{ host };
		Game    bobGame{ bob };

		explicit Fight(bool a_v0810 = false)
		{
			REQUIRE(host.Join());
			REQUIRE(bob.Join());
			host.Claim(RAIDER);
			s.Settle();
			REQUIRE(bob.OwnerOf(RAIDER) == host.Id());
			hostGame.v0810 = bobGame.v0810 = a_v0810;
			hostGame.Frame();
			bobGame.Frame();
			// The host's game reports the raider as it starts to run it; the throttle is out by the time the fight starts.
			hostGame.Wait(HealthShare::THROTTLE);
			Deliver();
		}

		void Deliver()
		{
			s.Settle();
			hostGame.Apply();
			bobGame.Apply();
		}

		// A while goes by in both games, and what each sent reaches the other (twice, so an answer to an answer lands).
		void Pass(Clock::duration a_for = std::chrono::seconds{ 1 })
		{
			for (int i = 0; i < 2; ++i) {
				hostGame.Wait(a_for);
				bobGame.Wait(a_for);
				Deliver();
			}
		}
	};
}

TEST("health share: the fire a Molotov left burning on the host's raider reaches Bob's copy, and Bob's next shot comes off what the fire left")
{
	Fight f;
	f.hostGame.Shoot(10.0f);    // the Molotov's blast: a hit
	f.hostGame.Burn(1.0f, 30);  // then the fire, a point a frame for a second and a half
	f.Pass();
	CHECK(Near(f.hostGame.health, 60.0f));
	CHECK(Near(f.bobGame.health, 60.0f));  // Bob sees it burn
	CHECK(f.bobGame.givenBack == 0.0f);

	f.bobGame.Shoot(15.0f);
	f.Pass();
	CHECK(Near(f.hostGame.health, 45.0f));  // the shot sticks, and the fire's damage too
	CHECK(Near(f.bobGame.health, 45.0f));
}

TEST("health share: with 0.8.10 the fire's damage stayed in the host's game, and Bob's next shot healed the raider back up past it")
{
	Fight f{ true };
	f.hostGame.Shoot(10.0f);
	f.hostGame.Burn(1.0f, 30);
	f.Pass();
	CHECK(Near(f.hostGame.health, 60.0f));
	CHECK(Near(f.bobGame.health, 90.0f));  // Bob's copy never burnt

	// Bob's report of his copy's health (75) healed 15 back on in the host's game: "my shots do nothing", or the fire did
	// nothing. The server now passes on an NPC's health only from the game that runs it, so a copy's word is dropped (and
	// with no damage sent, as in 0.8.10, Bob's shot is lost instead).
	f.bobGame.Shoot(15.0f);
	f.Pass();
	CHECK(Near(f.hostGame.health, 60.0f));
	CHECK(f.s.Logged(std::format("health of {:08X} from player {} not passed on: player {} runs it", RAIDER, f.bob.Id(), f.host.Id())));
}

TEST("health share: Bob's shot while the raider burns comes off the health the host's game has, though Bob's copy had not heard the latest burn")
{
	Fight f;
	f.hostGame.Burn(1.0f, 4);
	f.Deliver();
	f.bobGame.Shoot(10.0f);
	f.hostGame.Burn(1.0f, 4);  // while Bob's shot is on its way
	f.Deliver();
	// With 0.8.10 Bob's report of his copy's health (86) took back the 4 that burnt while it was on its way.
	CHECK(Near(f.hostGame.health, 100.0f - 8.0f - 10.0f));
	f.Pass();
	CHECK(Near(f.bobGame.health, f.hostGame.health, 0.01f));
}

TEST("health share: a burn that takes a little every frame is reported a few times a second, and where it ends is always reported")
{
	Fight f;
	const auto before = f.hostGame.reports;
	f.hostGame.Burn(0.5f, 40);  // two seconds of frames
	f.hostGame.Wait(HealthShare::THROTTLE);
	const auto sent = f.hostGame.reports - before;
	CHECK(sent >= 6);
	CHECK(sent <= 9);  // at most every THROTTLE, not 40
	f.Pass();
	CHECK(Near(f.bobGame.health, 80.0f, 0.01f));

	// Nothing changes: nothing more goes.
	const auto settled = f.hostGame.reports;
	f.Pass(std::chrono::seconds{ 3 });
	CHECK(f.hostGame.reports == settled);
}

TEST("health share: shots from both players while the raider burns all count, and both games end agreeing on its health")
{
	Fight f;
	f.hostGame.Shoot(10.0f);
	for (int round = 0; round < 4; ++round) {
		f.hostGame.Burn(1.0f, 4);
		f.bobGame.Shoot(5.0f);  // before the host's latest report reached Bob
		f.hostGame.Burn(1.0f, 4);
		f.hostGame.Shoot(5.0f);  // before Bob's shot reached the host
		f.Deliver();
	}
	f.Pass();
	CHECK(Near(f.hostGame.health, 100.0f - 10.0f - 4 * (8.0f + 5.0f + 5.0f)));  // 18: nothing taken back
	CHECK(Near(f.bobGame.health, f.hostGame.health, 0.01f));
}

TEST("health share: a host's report that lands between Bob's shot and his next frame does not wipe the shot out")
{
	Fight f;
	f.hostGame.Burn(2.0f, 1);
	f.bobGame.health -= 10.0f;  // Bob's shot lands
	f.bobGame.copy.OurHit(RAIDER, f.bobGame.now);
	f.Deliver();  // the host's report of 98 comes before Bob's next frame
	CHECK(Near(f.bobGame.health, 98.0f));
	f.Pass();
	CHECK(Near(f.hostGame.health, 88.0f));
	CHECK(Near(f.bobGame.health, 88.0f));
}

TEST("health share: the book reports an NPC on first sight, then nothing under a step, and at most once every throttle")
{
	HealthShare::Book book;
	const Clock::time_point t0{ std::chrono::hours{ 1 } };
	CHECK(book.Due(RAIDER, 0.7f, t0));  // newly ours: where it stands goes out, whatever the friends last heard
	CHECK(!book.Due(RAIDER, 0.7f - HealthShare::STEP / 2, t0 + HealthShare::THROTTLE));
	CHECK(book.Due(RAIDER, 0.6f, t0 + HealthShare::THROTTLE));  // a change once the throttle is out
	CHECK(!book.Due(RAIDER, 0.5f, t0 + HealthShare::THROTTLE + FRAME));  // then waits out the throttle
	CHECK(book.Due(RAIDER, 0.5f, t0 + 2 * HealthShare::THROTTLE));
	CHECK(book.Shared() == 3);
	CHECK(book.Firsts() == 1);

	// A hit's report is what the friends have: the same health is not sent again.
	book.Sent(RAIDER, 0.5f, t0 + std::chrono::seconds{ 5 });
	CHECK(!book.Due(RAIDER, 0.5f, t0 + std::chrono::seconds{ 6 }));
	CHECK(book.Due(RAIDER, 0.4f, t0 + std::chrono::seconds{ 6 }));

	// Out of sight a moment (a frame with no 3D, another process level): not new when it is back.
	const auto unseen = [](std::uint32_t) { return false; };
	book.Prune(unseen, t0 + std::chrono::seconds{ 7 });
	CHECK(book.Size() == 1);
	CHECK(!book.Due(RAIDER, 0.4f, t0 + std::chrono::seconds{ 8 }));
	// Gone for good: forgotten, and reported afresh should our game run it again.
	book.Prune(unseen, t0 + std::chrono::seconds{ 8 } + HealthShare::FORGET_AFTER);
	CHECK(book.Size() == 0);
	CHECK(book.Due(RAIDER, 0.4f, t0 + std::chrono::seconds{ 30 }));
}

TEST("health share: a load's crowd of NPCs newly ours is reported a few at a time, none of them lost")
{
	HealthShare::Book book;
	const Clock::time_point t0{ std::chrono::hours{ 1 } };
	constexpr std::uint32_t NPCS = 40;
	std::uint32_t           reported = 0;
	auto                    now = t0;
	std::uint32_t           mostInAThrottle = 0;
	while (reported < NPCS && now < t0 + std::chrono::seconds{ 5 }) {
		std::uint32_t thisFrame = 0;
		for (std::uint32_t i = 0; i < NPCS; ++i) {
			thisFrame += book.Due(RAIDER + i, 1.0f, now);
		}
		reported += thisFrame;
		mostInAThrottle = (std::max)(mostInAThrottle, thisFrame);
		now += HealthShare::THROTTLE;
	}
	CHECK(reported == NPCS);
	CHECK(mostInAThrottle == HealthShare::FIRST_PER_THROTTLE);
	CHECK(book.Firsts() == NPCS);
}

TEST("copy health: what our hits take off a copy is booked once as ours to send; what nothing of ours took is given back, not sent")
{
	CopyHealth::Book book;
	const CopyHealth::Clock::time_point t0{};
	book.Check(RAIDER, 100.0f, t0);
	book.OurHit(RAIDER, t0 + FRAME);
	CHECK(book.Check(RAIDER, 80.0f, t0 + FRAME) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 20.0f, 0.01f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);  // once

	// Long after the hit: a fire's 5 is given back, and none of it is ours.
	book.Check(RAIDER, 75.0f, t0 + std::chrono::seconds{ 5 });
	CHECK(Near(book.Check(RAIDER, 75.0f, t0 + std::chrono::seconds{ 5 } + CopyHealth::JUDGE_AFTER), 5.0f, 0.01f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
}

TEST("health share: Bob's Molotov on his copy of the host's raider hurts it once, in the host's game, where its explosion goes off too")
{
	for (const bool v0810 : { false, true }) {
		Fight f{ v0810 };
		// Bob's throw: the blast on his copy (with 0.8.10 taken as his hit and reported), then the same explosion set off
		// in the host's game (WeaponFire::ApplyExplosion), on the raider the host's game runs.
		f.bobGame.health -= 30.0f;
		if (v0810) {
			f.bobGame.copy.OurHit(RAIDER, f.bobGame.now);
		}
		f.bobGame.Frame();
		if (v0810) {
			f.bobGame.Report(f.bobGame.health / 100.0f);
		}
		f.Deliver();
		f.hostGame.Burn(30.0f);  // the host's own explosion: no hit of the host's player
		f.hostGame.Burn(1.0f, 20);  // and its fire
		f.Pass();
		if (v0810) {
			// With 0.8.10 the blast landed twice (20 left): Bob's report of his copy came on top of the host's own blast.
			// The server now passes on an NPC's health only from the game that runs it: once.
			CHECK(Near(f.hostGame.health, 100.0f - 30.0f - 20.0f));
			CHECK(Near(f.bobGame.health, 70.0f));  // and Bob's copy never saw the fire
		} else {
			CHECK(Near(f.hostGame.health, 100.0f - 30.0f - 20.0f));
			CHECK(Near(f.bobGame.health, f.hostGame.health));
			CHECK(f.bobGame.givenBack == 30.0f);  // the blast on Bob's copy was the host's game's to deal
		}
	}
}

namespace
{
	// Bob's Molotov on his copy as his hit events read it: its blast (reported, so set off in the host's game too), and the
	// burn its fire leaves; and his gun.
	constexpr CopyHealth::Blow BLAST{ .explosion = true, .throwable = true, .explosionsShared = true, .blastThere = true };
	constexpr CopyHealth::Blow BURN{ .spellAlone = true, .threwLately = true };
	constexpr CopyHealth::Blow GUN{ .inHand = true };
}

TEST("health share: Bob's shot and his Molotov's blast on his copy in the same frame: the host's raider loses the blast once and the shot once")
{
	Fight f;
	// The blast's 30 and the shot's 10 come off Bob's copy together; the hit events say which is which.
	f.bobGame.health -= 30.0f;
	f.bobGame.copy.OwnersBlow(RAIDER, f.bobGame.now);
	f.bobGame.Strike(10.0f, GUN);
	f.Deliver();
	f.hostGame.Burn(30.0f);  // the same explosion, set off in the host's game
	f.Pass();
	// Before, the whole 40 went as Bob's damage on top of the host's own blast: 20 left.
	CHECK(Near(f.hostGame.health, 100.0f - 30.0f - 10.0f));
	CHECK(Near(f.bobGame.health, f.hostGame.health));
	CHECK(f.bobGame.givenBack == 30.0f);
}

TEST("health share: Bob keeps shooting his copy while his Molotov's fire burns it: the host's raider loses each shot and each burn once, and both games agree")
{
	Fight f;
	f.bobGame.Strike(30.0f, BLAST);
	f.hostGame.Burn(30.0f);
	for (int round = 0; round < 5; ++round) {
		for (int tick = 0; tick < 4; ++tick) {
			f.bobGame.Strike(1.0f, BURN);  // the fire on Bob's copy, left to the host's game
			f.hostGame.Burn(1.0f);         // and on the raider there
		}
		f.bobGame.Strike(5.0f, GUN);  // within a second of the burn before and after it
	}
	f.Pass();
	// Before, every burn tick inside a shot's second went as Bob's damage too.
	CHECK(Near(f.hostGame.health, 100.0f - 30.0f - 20.0f - 25.0f));
	CHECK(Near(f.bobGame.health, f.hostGame.health));
}

TEST("health share: a legendary's hit event with no figure riding along with each of Bob's shots does not make his Molotov's burn his: the host's raider loses it once")
{
	Fight f;
	f.bobGame.Strike(30.0f, BLAST);
	f.hostGame.Burn(30.0f);
	const auto burn = [&] {
		for (int tick = 0; tick < 3; ++tick) {
			f.bobGame.Strike(1.0f, BURN);
			f.hostGame.Burn(1.0f);
		}
	};
	for (int round = 0; round < 5; ++round) {
		burn();
		f.bobGame.copy.OurHit(RAIDER, f.bobGame.now);  // the effect's own hit event: no hit data
		f.bobGame.Strike(5.0f, GUN);
		burn();
	}
	f.Pass();
	// With the first figureless event taken for the burst's, every burn tick near a shot went as Bob's damage too.
	CHECK(Near(f.hostGame.health, 100.0f - 30.0f - 30.0f - 25.0f));
	CHECK(Near(f.bobGame.health, f.hostGame.health));
}

TEST("copy health: our 10-point shot and a throw's 30-point blast on a copy in the same second book 10 as ours and give the 30 back, though the blast's event comes a frame late")
{
	CopyHealth::Book                    book;
	const CopyHealth::Clock::time_point t0{};
	const auto                          t1 = t0 + FRAME;
	book.Check(RAIDER, 100.0f, t0);
	book.OurHit(RAIDER, t1, 10.0f);
	book.OwnersBlow(RAIDER, t1);
	CHECK(book.Check(RAIDER, 60.0f, t1) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 10.0f, 0.01f));
	CHECK(Near(book.Check(RAIDER, 60.0f, t1 + CopyHealth::JUDGE_AFTER), 30.0f, 0.01f));
	CHECK(book.TakeOurs(RAIDER) == 0.0f);
	book.Set(RAIDER, 90.0f);

	// The drop is seen before the blast's hit event: held, not taken for ours, then given back all the same.
	const auto t2 = t0 + std::chrono::seconds{ 5 };
	book.OurHit(RAIDER, t2, 10.0f);
	CHECK(book.Check(RAIDER, 50.0f, t2) == 0.0f);
	book.OwnersBlow(RAIDER, t2 + FRAME);
	CHECK(book.Check(RAIDER, 50.0f, t2 + FRAME) == 0.0f);
	CHECK(Near(book.Check(RAIDER, 50.0f, t2 + CopyHealth::JUDGE_AFTER), 30.0f, 0.01f));
	CHECK(Near(book.TakeOurs(RAIDER), 10.0f, 0.01f));
}

TEST("copy health: with no blow of the owner's around, a drop beside our hit is all ours though its event's figure says less, as is one beside a hit whose event had no figure")
{
	CopyHealth::Book                    book;
	const CopyHealth::Clock::time_point t0{};
	const auto                          t1 = t0 + FRAME;
	book.Check(RAIDER, 100.0f, t0);
	book.OurHit(RAIDER, t1, 10.0f);  // a sneak attack's figure short of what it took
	CHECK(book.Check(RAIDER, 75.0f, t1) == 0.0f);
	CHECK(book.Check(RAIDER, 75.0f, t1 + CopyHealth::JUDGE_AFTER) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 25.0f, 0.01f));

	// No hit around it with a figure: the whole drop is ours, as before, an owner's blow beside it or not.
	const auto t2 = t0 + std::chrono::seconds{ 5 };
	book.OurHit(RAIDER, t2);
	book.OwnersBlow(RAIDER, t2);
	CHECK(book.Check(RAIDER, 45.0f, t2) == 0.0f);
	CHECK(Near(book.TakeOurs(RAIDER), 30.0f, 0.01f));
}

TEST("copy health: a report heard between our shot and the next frame books only the shot as ours when a throw's blast landed with it")
{
	CopyHealth::Book                    book;
	const CopyHealth::Clock::time_point t0{};
	const auto                          t1 = t0 + FRAME;
	book.Check(RAIDER, 100.0f, t0);
	book.OurHit(RAIDER, t1, 10.0f);
	book.OwnersBlow(RAIDER, t1);
	book.Heard(RAIDER, 60.0f, 65.0f, t1);  // the owner's report: its own blast, and a burn
	CHECK(Near(book.TakeOurs(RAIDER), 10.0f, 0.01f));
	CHECK(book.Health(RAIDER) == 65.0f);
}

TEST("copy health: a throw's blow on a copy (its explosion, the throwable's own hit, the burn its fire leaves) is the NPC's own game's to deal: no report, a claim only for a kill")
{
	using CopyHealth::After;
	using CopyHealth::Blow;
	using CopyHealth::Hitter;
	using CopyHealth::OwnersToDeal;
	constexpr std::uint32_t US = 2;
	constexpr std::uint32_t FRIEND = 1;
	// An explosion that goes off in the owner's game too (one we reported, a wreck's, a trap's), a throwable's own hit while
	// a throw's watch is on, and a while after for what they left burning.
	CHECK(OwnersToDeal(Blow{ .explosion = true, .blastThere = true }));
	CHECK(OwnersToDeal(Blow{ .explosion = true, .throwable = true, .explosionsShared = true, .blastThere = true }));
	CHECK(OwnersToDeal(Blow{ .throwable = true, .explosionsShared = true, .threwLately = true }));
	CHECK(OwnersToDeal(Blow{ .spellAlone = true, .threwLately = true }));
	CHECK(!OwnersToDeal(Blow{}));  // a gun's, a bat's: ours to report
	CHECK(!OwnersToDeal(Blow{ .inHand = true, .explosionsShared = true, .threwLately = true }));

	CHECK(!After(Hitter::kOurPlayer, FRIEND, US, false, true).report);
	CHECK(!After(Hitter::kOurPlayer, FRIEND, US, false, true).claim);
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, true, true).claim);  // killed it here: the server must believe it
	CHECK(!After(Hitter::kOurs, FRIEND, US, false, true).report);
	// On one our game runs, or nobody does, our player's throw is ours as before.
	CHECK(After(Hitter::kOurPlayer, US, US, false, true).report);
	CHECK(After(Hitter::kOurPlayer, 0, US, false, true).report);
	CHECK(After(Hitter::kOurPlayer, 0, US, false, true).claim);
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false, false).report);
}

TEST("copy health: a blow with a weapon in hand is ours whatever else its hit event reads, and an explosion's is the owner's only where it goes off there too")
{
	using CopyHealth::Blow;
	using CopyHealth::OwnersToDeal;
	// A gun's hit read as an explosion's, or as a spell's with no weapon: ours while no explosion reaches it there.
	CHECK(!OwnersToDeal(Blow{ .explosion = true, .inHand = true }));
	CHECK(!OwnersToDeal(Blow{ .explosion = true, .inHand = true, .threwLately = true }));
	CHECK(!OwnersToDeal(Blow{ .spellAlone = true, .inHand = true, .explosionsShared = true, .threwLately = true }));
	CHECK(!OwnersToDeal(Blow{ .throwable = true, .inHand = true, .explosionsShared = true }));
	// A missile's blast our game reported (a throw's watch was on) goes off in the owner's game: theirs to deal.
	CHECK(OwnersToDeal(Blow{ .explosion = true, .inHand = true, .explosionsShared = true, .blastThere = true }));
	// The watch on, but that blast never reported (over 6000 units from our player) or not set off there (the owner's
	// player too far from it): dealt nowhere else, so ours. Under the watch alone it was dealt in neither game.
	CHECK(!OwnersToDeal(Blow{ .explosion = true, .inHand = true, .explosionsShared = true }));
	CHECK(!OwnersToDeal(Blow{ .explosion = true, .throwable = true, .explosionsShared = true }));
	// A car's blast long after any watch: the wreck sets it off in the owner's game, so theirs.
	CHECK(OwnersToDeal(Blow{ .explosion = true, .inHand = true, .blastThere = true }));
	// No throw of ours lately: none of these is dealt elsewhere.
	CHECK(!OwnersToDeal(Blow{ .explosion = true }));
	CHECK(!OwnersToDeal(Blow{ .throwable = true }));  // a mine laid long ago, stepped on now
	CHECK(!OwnersToDeal(Blow{ .spellAlone = true }));
	// After the throw's watch: the fire it left may burn on there too.
	CHECK(!OwnersToDeal(Blow{ .explosion = true, .threwLately = true }));
	CHECK(!OwnersToDeal(Blow{ .throwable = true, .threwLately = true }));
	CHECK(OwnersToDeal(Blow{ .spellAlone = true, .threwLately = true }));
}

namespace
{
	// The host's game runs the raider; Bob and Carol have their copies of it. Within 5 s of the host's claim, so no
	// takeover is granted.
	struct Trio
	{
		Session s;
		Client& host = s.Host();
		Client& bob = s.Player("Bob");
		Client& carol = s.Player("Carol");
		Game    hostGame{ host };
		Game    bobGame{ bob };
		Game    carolGame{ carol };

		Trio()
		{
			REQUIRE(host.Join());
			REQUIRE(bob.Join());
			REQUIRE(carol.Join());
			host.Claim(RAIDER);
			s.Settle();
			REQUIRE(bob.OwnerOf(RAIDER) == host.Id());
			REQUIRE(carol.OwnerOf(RAIDER) == host.Id());
			for (auto* game : { &hostGame, &bobGame, &carolGame }) {
				game->Frame();
			}
			hostGame.Wait(HealthShare::THROTTLE);
			Deliver();
		}

		void Deliver()
		{
			s.Settle();
			for (auto* game : { &hostGame, &bobGame, &carolGame }) {
				game->Apply();
			}
		}

		void Pass()
		{
			for (int i = 0; i < 2; ++i) {
				for (auto* game : { &hostGame, &bobGame, &carolGame }) {
					game->Wait(std::chrono::seconds{ 1 });
				}
				Deliver();
			}
		}
	};

	// A gun's hit as 0.9.0 took it in the players' reports: its hit event reads the kExplosion bit with the gun as the
	// weapon in hand, or names no weapon and a spell for its source.
	constexpr CopyHealth::Blow GUN_READ_AS_EXPLOSION{ .explosion = true, .inHand = true };
	constexpr CopyHealth::Blow GUN_NO_WEAPON_READ{ .spellAlone = true };
}

TEST("copy health: in a team of three only the game that ran the raider hurt it with guns in 0.9.0; now every player's shots count")
{
	for (const bool rule090 : { true, false }) {
		Trio t;
		t.bobGame.Strike(10.0f, GUN_READ_AS_EXPLOSION, rule090);
		t.carolGame.Strike(15.0f, GUN_NO_WEAPON_READ, rule090);
		t.Deliver();
		t.hostGame.Strike(5.0f, CopyHealth::Blow{ .inHand = true }, rule090);
		t.Pass();
		if (rule090) {
			// "Exactly one player can deal damage normally": the host's 5 counts, the friends' shots came back.
			CHECK(Near(t.hostGame.health, 95.0f));
			CHECK(Near(t.bobGame.health, 95.0f));
			CHECK(Near(t.carolGame.health, 95.0f));
			CHECK(t.bobGame.givenBack == 10.0f);
			CHECK(t.carolGame.givenBack == 15.0f);
		} else {
			CHECK(Near(t.hostGame.health, 70.0f));
			CHECK(Near(t.bobGame.health, 70.0f));
			CHECK(Near(t.carolGame.health, 70.0f));
			CHECK(t.bobGame.givenBack == 0.0f);
			CHECK(t.carolGame.givenBack == 0.0f);
		}
	}
}

TEST("copy health: a gun's hit on a copy is reported and claims the NPC, as a melee hit's does")
{
	using CopyHealth::After;
	using CopyHealth::Hitter;
	using CopyHealth::OwnersToDeal;
	constexpr std::uint32_t US = 2;
	constexpr std::uint32_t FRIEND = 1;
	// In 0.9.0 a shock baton's hit claimed the NPC and a gun's did not: whoever last hit it in melee ran it, and only
	// they could hurt it.
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false, OwnersToDeal(GUN_READ_AS_EXPLOSION)).claim);
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false, OwnersToDeal(GUN_READ_AS_EXPLOSION)).report);
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false, OwnersToDeal(GUN_NO_WEAPON_READ)).claim);
	CHECK(After(Hitter::kOurPlayer, FRIEND, US, false, OwnersToDeal(GUN_NO_WEAPON_READ)).report);
}

TEST("health share: Bob's shot that takes the raider over is not lost: his game reports the raider's health as it starts to run it")
{
	Fight f;
	f.hostGame.Shoot(30.0f);
	f.Pass();
	REQUIRE(Near(f.bobGame.health, 70.0f));
	std::this_thread::sleep_for(std::chrono::milliseconds{ 5100 });  // the server's TAKEOVER_COOLDOWN
	// His claim leaves at the end of the frame, his shot's damage at the start of the next: the server gives him the
	// raider first, and the host's game drops the damage as no longer its to deal.
	f.bobGame.Strike(20.0f, CopyHealth::Blow{ .inHand = true });
	f.Deliver();
	REQUIRE(f.bob.OwnerOf(RAIDER) == f.bob.Id());
	CHECK(Near(f.hostGame.health, 70.0f));
	f.Pass();
	// Bob's game reported where the raider stands as it took it over: the host's copy follows (in 0.9.0 it stayed at 70,
	// and the host saw his own shots come off health the raider no longer had).
	CHECK(Near(f.hostGame.health, 50.0f));
	f.hostGame.Shoot(10.0f);
	f.Pass();
	CHECK(Near(f.bobGame.health, 40.0f));
	CHECK(Near(f.hostGame.health, 40.0f));
}

TEST("health share: after a host's reload brings the raider back at its save's health, Bob's copy is told, and his next shot comes off that")
{
	Fight f;
	f.hostGame.Shoot(60.0f);
	f.Pass();
	REQUIRE(Near(f.bobGame.health, 40.0f));
	// The host dies and loads their save: the raider is back at full health in the host's game, and the book let go of
	// it while the load hid it (FORGET_AFTER).
	f.hostGame.health = 100.0f;
	f.hostGame.share = HealthShare::Book{};
	f.Pass();
	CHECK(Near(f.bobGame.health, 100.0f));  // in 0.9.0 Bob's copy showed 40, and each of his shots "healed" it to the host's
	f.bobGame.Strike(10.0f, CopyHealth::Blow{ .inHand = true });
	f.Pass();
	CHECK(Near(f.hostGame.health, 90.0f));
	CHECK(Near(f.bobGame.health, 90.0f));
	CHECK(f.bobGame.givenBack == 0.0f);
}
