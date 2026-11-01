// A builder who reloads a save from before they built something: whole sessions (tests/sim/Sim.h), the builder's game
// looking over what the session keeps as theirs after the load (WorkshopAudit, the Sim's world all in memory) and the
// real server taking out what it lacks. Each test names what the players would see go wrong if it failed.

#include "sim/Sim.h"

using namespace Sim;

namespace
{
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t CHAIR = 0x0001ABCE;
	// Long enough for a game to look twice at what the session keeps as its own (WorkshopAudit::GONE_LOOKS) and be heard.
	constexpr std::chrono::seconds LOOKS{ 6 };
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
	s.Play(LOOKS);  // its game looks over what it built, twice, and its answer is relayed
	CHECK((vm.Settlements().contains({ host.Id(), first })));
	CHECK((!(vm.Settlements().contains({ host.Id(), second }))));
	CHECK(s.Logged(std::format("player {}'s world no longer has 1 of the pieces", host.Id())));
	CHECK_AGREE({ &host, &vm });

	// Nor does a friend who joins later get it.
	auto& late = s.Player("Late");
	REQUIRE(late.Join());
	s.Settle();
	CHECK((late.Settlements().contains({ host.Id(), first })));
	CHECK((!(late.Settlements().contains({ host.Id(), second }))));
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
	s.Play(LOOKS);  // its game looks over what it built, twice, and its answer is relayed
	CHECK((host.Settlements().contains({ vm.Id(), first })));
	CHECK((!(host.Settlements().contains({ vm.Id(), second }))));
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
	s.Play(LOOKS);  // its game looks over what it built, twice, and its answer is relayed
	CHECK(!s.Logged("no longer has"));
	CHECK((vm.Settlements().contains({ host.Id(), hostChair })));

	vm.Leave();
	s.Settle();
	host.LoadGame(host.SaveGame());
	s.Play(LOOKS);  // its game looks over what it built, twice, and its answer is relayed
	CHECK((host.Settlements().contains({ vmId, vmChair })));  // still shown: the friend's game is the only one that can say
	REQUIRE(vm.Join());
	s.Play(LOOKS);  // its game looks over what it built, twice, and its answer is relayed
	CHECK((host.Settlements().contains({ vmId, vmChair })));
	CHECK(!s.Logged("no longer has"));
	CHECK_AGREE({ &host, &vm });
}

// The player ID is the PC's: a friend who built with one character and came back with another had the first's list sent
// to a world with none of it, and every piece went for everyone, for good (the test rig's VM lost four chairs this way).
TEST("sim orphans: a friend who built with one character and joins with another loses none of the first's pieces, and gets them back as the first")
{
	Session s;
	auto&   host = s.Host();
	auto&   vm = s.Player("Vm");
	REQUIRE(host.Join());
	REQUIRE(vm.Join());
	const auto first = vm.Build(SANCTUARY, CHAIR, 100.0f, 200.0f);
	const auto second = vm.Build(SANCTUARY, CHAIR, 300.0f, 200.0f);
	s.Settle();
	const auto firstCharacter = vm.SaveGame();
	const auto vmId = vm.Id();

	vm.Leave();
	vm.NewCharacter();  // a new game, nothing built
	REQUIRE(vm.Join());
	CHECK(vm.Id() == vmId);  // the same PC
	s.Play(LOOKS);
	CHECK(!s.Logged("no longer has"));
	CHECK((host.Settlements().contains({ vmId, first })));
	CHECK((host.Settlements().contains({ vmId, second })));
	// What the second builds is its own; the first's two stay.
	const auto third = vm.Build(SANCTUARY, CHAIR, 500.0f, 200.0f);
	s.Settle();
	vm.LoadGame(vm.SaveGame());
	s.Play(LOOKS);
	CHECK(!s.Logged("no longer has"));
	CHECK((host.Settlements().contains({ vmId, third })));

	// Back as the first: its world has its two, the second's chair isn't asked about.
	vm.Leave();
	vm.LoadGame(firstCharacter);
	REQUIRE(vm.Join());
	s.Play(LOOKS);
	CHECK(!s.Logged("no longer has"));
	CHECK((host.Settlements().contains({ vmId, first })));
	CHECK((host.Settlements().contains({ vmId, second })));
	CHECK((host.Settlements().contains({ vmId, third })));
}

TEST("sim orphans: a chair taken out by an older save comes back for everyone when the newer save is loaded")
{
	Session s;
	auto&   host = s.Host();
	auto&   vm = s.Player("Vm");
	REQUIRE(host.Join());
	REQUIRE(vm.Join());
	const auto first = host.Build(SANCTUARY, CHAIR, 100.0f, 200.0f);
	s.Settle();
	const auto older = host.SaveGame();
	const auto second = host.Build(SANCTUARY, CHAIR, 300.0f, 200.0f);
	s.Settle();
	const auto newer = host.SaveGame();

	host.LoadGame(older);
	s.Play(LOOKS);
	REQUIRE(!(vm.Settlements().contains({ host.Id(), second })));

	host.LoadGame(newer);
	s.Play(LOOKS);
	CHECK((vm.Settlements().contains({ host.Id(), first })));
	CHECK((vm.Settlements().contains({ host.Id(), second })));
	CHECK(s.Logged("has " + std::format("{:08X}", second) + " again"));
	CHECK_AGREE({ &host, &vm });

	auto& late = s.Player("Late");
	REQUIRE(late.Join());
	s.Settle();
	CHECK((late.Settlements().contains({ host.Id(), second })));
}
