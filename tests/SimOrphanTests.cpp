// A builder who reloads a save from before they built something: whole sessions (tests/sim/Sim.h), the builder's game
// looking over what the session keeps as theirs after the load (WorkshopAudit, the Sim's world all in memory) and the
// real server taking out what it lacks. Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

using namespace Sim;

namespace
{
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t CHAIR = 0x0001ABCE;
}

// The host reloaded an older save and its friend kept seeing a chair the host no longer had, at every join after, with
// no game able to scrap it (the test rig's session held six for days).
TEST("sim orphans: the host builds a chair, saves, builds a second and reloads: the friend loses the second and keeps the first")
{
	Session s;
	auto&   host = s.Host();
	auto&   vm = s.Player("Vm");
	REQUIRE(host.Join());
	REQUIRE(vm.Join());
	const auto first = host.Build(SANCTUARY, CHAIR, 100.0f, 200.0f);
	s.Settle();
	const auto save = host.SaveGame();
	const auto second = host.Build(SANCTUARY, CHAIR, 300.0f, 200.0f);
	s.Settle();
	REQUIRE((vm.Settlements().contains({ host.Id(), second })));

	host.LoadGame(save);
	s.Settle();
	s.Settle();  // the host's answer to its list, then its relay to the friend
	CHECK((vm.Settlements().contains({ host.Id(), first })));
	CHECK(!(vm.Settlements().contains({ host.Id(), second })));
	CHECK(s.Logged(std::format("player {}'s world no longer has 1 of the pieces", host.Id())));
	CHECK_AGREE({ &host, &vm });

	// Nor does a friend who joins later get it.
	auto& late = s.Player("Late");
	REQUIRE(late.Join());
	s.Settle();
	CHECK((late.Settlements().contains({ host.Id(), first })));
	CHECK(!(late.Settlements().contains({ host.Id(), second })));
	CHECK_AGREE({ &host, &vm, &late });
}

TEST("sim orphans: the same with the friend as the builder: the host loses the second chair and keeps the first")
{
	Session s;
	auto&   host = s.Host();
	auto&   vm = s.Player("Vm");
	REQUIRE(host.Join());
	REQUIRE(vm.Join());
	const auto first = vm.Build(SANCTUARY, CHAIR, 100.0f, 200.0f);
	s.Settle();
	const auto save = vm.SaveGame();
	const auto second = vm.Build(SANCTUARY, CHAIR, 300.0f, 200.0f);
	s.Settle();
	REQUIRE((host.Settlements().contains({ vm.Id(), second })));

	vm.LoadGame(save);
	s.Settle();
	s.Settle();
	CHECK((host.Settlements().contains({ vm.Id(), first })));
	CHECK(!(host.Settlements().contains({ vm.Id(), second })));
	CHECK_AGREE({ &host, &vm });
}

TEST("sim orphans: a builder who reloads with everything still there loses nothing, and an offline builder's pieces stay through the others' reloads")
{
	Session s;
	auto&   host = s.Host();
	auto&   vm = s.Player("Vm");
	REQUIRE(host.Join());
	REQUIRE(vm.Join());
	const auto hostChair = host.Build(SANCTUARY, CHAIR, 100.0f, 200.0f);
	const auto vmChair = vm.Build(SANCTUARY, CHAIR, 300.0f, 200.0f);
	s.Settle();
	const auto vmId = vm.Id();

	host.LoadGame(host.SaveGame());
	s.Settle();
	s.Settle();
	CHECK(!s.Logged("no longer has"));
	CHECK((vm.Settlements().contains({ host.Id(), hostChair })));

	vm.Leave();
	s.Settle();
	host.LoadGame(host.SaveGame());
	s.Settle();
	s.Settle();
	CHECK((host.Settlements().contains({ vmId, vmChair })));  // still shown: the friend's game is the only one that can say
	REQUIRE(vm.Join());
	s.Settle();
	s.Settle();
	CHECK((host.Settlements().contains({ vmId, vmChair })));
	CHECK(!s.Logged("no longer has"));
	CHECK_AGREE({ &host, &vm });
}
