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

#include <cmath>

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

	f.bobGame.Shoot(15.0f);
	f.Pass();
	CHECK(Near(f.hostGame.health, 75.0f));  // 15 healed back on: "my shots do nothing", or the fire did nothing
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

TEST("health share: the book reports nothing on first sight, nothing under a step, and at most once every throttle")
{
	HealthShare::Book book;
	const Clock::time_point t0{};
	CHECK(!book.Due(RAIDER, 1.0f, t0));  // where it stands
	CHECK(!book.Due(RAIDER, 1.0f - HealthShare::STEP / 2, t0 + FRAME));
	CHECK(book.Due(RAIDER, 0.9f, t0 + 2 * FRAME));  // the first change goes at once
	CHECK(!book.Due(RAIDER, 0.8f, t0 + 3 * FRAME));  // then waits out the throttle
	CHECK(book.Due(RAIDER, 0.8f, t0 + 2 * FRAME + HealthShare::THROTTLE));
	CHECK(book.Shared() == 2);

	// A hit's report is what the friends have: the same health is not sent again.
	book.Sent(RAIDER, 0.5f, t0 + std::chrono::seconds{ 5 });
	CHECK(!book.Due(RAIDER, 0.5f, t0 + std::chrono::seconds{ 6 }));
	CHECK(book.Due(RAIDER, 0.4f, t0 + std::chrono::seconds{ 6 }));

	book.Prune([](std::uint32_t) { return false; });
	CHECK(book.Size() == 0);
	CHECK(!book.Due(RAIDER, 0.1f, t0 + std::chrono::seconds{ 7 }));  // seen anew
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
