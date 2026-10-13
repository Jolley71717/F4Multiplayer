// The console commands the NPC sync runs in a frame, and how many copies it takes over from their own AI in one
// (CommandMeter, as NpcSync::UpdateMirrors, StopMirroring and Frame use it: TryRegister gates each takeover,
// Released counts each hand-back's evp). Taking a copy over, handing it back and dressing it happen in the game, so
// the tests model that part (the loop over the mirrors here is the tests' own, not UpdateMirrors): a friend's game
// runs a settlement's settlers against a real server, and what the server relays (owners, states, gear) drives our
// game's frames, dressed by GearPlan with a made-up slot table.

#include "ServerFixture.h"

#include "game/CommandMeter.h"
#include "game/GearPlan.h"

#include <map>
#include <set>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using CommandMeter::Clock;

	constexpr std::uint32_t FIRST_SETTLER = 0x0002A001;
	constexpr std::uint32_t SETTLERS = 40;
	constexpr std::uint32_t WORLD = 0x0000003C;
	constexpr auto          FRAME = 16ms;

	constexpr std::uint32_t COMBAT_SUIT = 0x00012001;  // body
	constexpr std::uint32_t HAT = 0x00012002;          // head
	constexpr std::uint32_t GLASSES = 0x00012003;      // eyes
	constexpr std::uint32_t RIFLE = 0x00012004;        // hands
	const std::vector<std::uint32_t> OUTFIT{ COMBAT_SUIT, HAT, GLASSES, RIFLE };

	GearPlan::Slots SlotsOf(std::uint32_t a_item)
	{
		switch (a_item) {
		case COMBAT_SUIT:
			return GearPlan::Slots{ 1 } << 3;
		case HAT:
			return GearPlan::Slots{ 1 } << 0;
		case GLASSES:
			return GearPlan::Slots{ 1 } << 17;
		case RIFLE:
			return GearPlan::WEAPON_SLOT;
		default:
			return 0;
		}
	}

	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	// One of our copies of a friend's NPC (NpcSync's Mirror, and the actor it stands for).
	struct Mirror
	{
		bool                              registered = false;
		std::size_t                       snapshots = 0;
		std::optional<std::vector<std::uint32_t>> gear;  // the owner's last report (NpcSync's knownGear)
		std::set<std::uint32_t>           carried{ COMBAT_SUIT, HAT, GLASSES, RIFLE };
		std::vector<std::uint32_t>        worn;
	};

	// Our game copying a friend's settlement.
	struct Game
	{
		CommandMeter::Meter               meter;
		std::map<std::uint32_t, Mirror>   mirrors;
		std::uint32_t                     logLines = 0;
		std::uint32_t                     mostTakenInAFrame = 0;
		std::uint32_t                     frames = 0;

		// What the server sent: owners make mirrors, states their snapshots, gear reports their knownGear.
		void Apply(Fixture& a_f, PeerId a_peer, std::uint32_t a_owner)
		{
			Sync(a_f, a_peer);
			for (const auto& packet : a_f.fake->Take(a_peer)) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kActorOwners)) {
					const auto owners = Protocol::DecodeOwners(packet);
					REQUIRE(owners);
					for (const auto& owner : *owners) {
						if (owner.playerId == a_owner) {
							mirrors.try_emplace(owner.refId);
						}
					}
				} else if (packet[0] == Type(MT::kActorStatesRelay)) {
					const auto states = Protocol::DecodeActorStates(packet);
					REQUIRE(states);
					for (const auto& state : *states) {
						if (const auto it = mirrors.find(state.refId); it != mirrors.end()) {
							++it->second.snapshots;
						}
					}
				} else if (packet[0] == Type(MT::kNpcEquipment)) {
					const auto gear = Protocol::DecodeNpcEquipment(packet);
					REQUIRE(gear);
					if (const auto it = mirrors.find(gear->refId); it != mirrors.end()) {
						it->second.gear = gear->items;
					}
				}
			}
		}

		// NpcSync::Frame: the last frame's commands are tallied, then UpdateMirrors takes copies over, a few a frame,
		// dressing each (Equipment::Wear's equips).
		void Frame(Clock::time_point a_now)
		{
			++frames;
			if (meter.EndFrame(a_now)) {
				++logLines;
			}
			std::uint32_t taken = 0;
			for (auto& [id, mirror] : mirrors) {
				if (mirror.registered || mirror.snapshots == 0) {
					continue;
				}
				if (!meter.TryRegister()) {
					continue;  // next frame; its snapshots stay
				}
				mirror.registered = true;
				++taken;
				if (mirror.gear) {
					const auto plan = GearPlan::Equip(mirror.worn, *mirror.gear, false, [&](std::uint32_t a_item) { return mirror.carried.contains(a_item); },
						[](std::uint32_t) { return false; }, SlotsOf);
					meter.Count(static_cast<std::uint32_t>(plan.size()));
					mirror.worn.insert(mirror.worn.end(), plan.begin(), plan.end());
				}
			}
			mostTakenInAFrame = (std::max)(mostTakenInAFrame, taken);
		}

		// The owner's states stopped (a pause menu in their game): every copy goes back to its own AI, an evp each.
		void OwnerWentQuiet()
		{
			for (auto& [id, mirror] : mirrors) {
				if (mirror.registered) {
					mirror.registered = false;
					meter.Released();
				}
			}
		}

		[[nodiscard]] std::size_t Registered() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(mirrors, [](const auto& a_entry) { return a_entry.second.registered; }));
		}

		Clock::time_point RunUntilAllTaken(Clock::time_point a_now)
		{
			for (int i = 0; i < 100 && Registered() < mirrors.size(); ++i) {
				Frame(a_now);
				a_now += FRAME;
			}
			Frame(a_now);  // the frame that tallies the last takeovers
			return a_now + FRAME;
		}
	};

	// Alice's game runs a settlement's settlers: it claims them, reports their gear and sends their states.
	void AliceRunsTheSettlement(Fixture& a_f)
	{
		std::vector<Protocol::ActorClaim> claims;
		std::vector<Protocol::ActorState> states;
		for (std::uint32_t i = 0; i < SETTLERS; ++i) {
			claims.push_back({ FIRST_SETTLER + i, Protocol::ClaimReason::kUnowned });
			Protocol::ActorState state;
			state.refId = FIRST_SETTLER + i;
			state.worldspace = WORLD;
			state.x = 100.0f * static_cast<float>(i);
			states.push_back(state);
		}
		a_f.fake->Deliver(1, Protocol::Encode(claims));
		for (const auto& claim : claims) {
			a_f.fake->Deliver(1, Protocol::Encode(Protocol::NpcEquipment{ 0, claim.refId, OUTFIT }, MT::kReportNpcEquipment));
		}
		a_f.fake->Deliver(1, Protocol::Encode(states, MT::kActorStates));
		Sync(a_f, 1);
	}
}

TEST("npc commands: arriving at a settlement a friend runs, its 40 settlers are taken over a few a frame, each dressed, with no busy frame")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	AliceRunsTheSettlement(f);

	Game bob;
	bob.Apply(f, 2, alice->playerId);
	REQUIRE(bob.mirrors.size() == SETTLERS);

	bob.RunUntilAllTaken(Clock::now());
	CHECK(bob.Registered() == SETTLERS);
	CHECK(bob.mostTakenInAFrame == CommandMeter::MAX_REGISTRATIONS_PER_FRAME);
	CHECK(bob.meter.Registrations() == SETTLERS);
	// Every copy wears the friend's outfit from its own pack, and no frame ran more commands than a busy one.
	CHECK(std::ranges::all_of(bob.mirrors, [](const auto& a_entry) { return a_entry.second.worn.size() == OUTFIT.size(); }));
	CHECK(bob.meter.Most() == CommandMeter::MAX_REGISTRATIONS_PER_FRAME * OUTFIT.size());
	CHECK(bob.meter.BusyFrames() == 0);
	CHECK(bob.logLines == 0);
}

// The meter's busy-frame log limit, with the hand-backs modelled: StopMirroring's own path is engine code.
TEST("npc commands: a friend's pause menu hands every copy back in one frame: logged once, and again only 10 s later")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	AliceRunsTheSettlement(f);
	Game bob;
	bob.Apply(f, 2, alice->playerId);
	auto now = bob.RunUntilAllTaken(Clock::now());

	// Alice opens her Pip-Boy: 40 evps in one frame, then her copies come back and are taken over again, already
	// dressed (no command for gear they wear).
	bob.OwnerWentQuiet();
	now = bob.RunUntilAllTaken(now);
	CHECK(bob.meter.BusyFrames() == 1);
	CHECK(bob.logLines == 1);
	CHECK(bob.meter.Releases() == SETTLERS);
	CHECK(bob.meter.Registrations() == 2 * SETTLERS);

	// Again a few seconds later: counted, not logged.
	bob.OwnerWentQuiet();
	now = bob.RunUntilAllTaken(now + 3s);
	CHECK(bob.meter.BusyFrames() == 2);
	CHECK(bob.logLines == 1);

	bob.OwnerWentQuiet();
	bob.RunUntilAllTaken(now + CommandMeter::LOG_INTERVAL);
	CHECK(bob.meter.BusyFrames() == 3);
	CHECK(bob.logLines == 2);
}

TEST("command meter: a frame is busy past 32 commands, and takeovers start again each frame")
{
	CommandMeter::Meter meter;
	const auto          t0 = Clock::now();
	meter.Count(CommandMeter::BUSY_FRAME);
	CHECK(!meter.EndFrame(t0));
	CHECK(meter.Last() == CommandMeter::BUSY_FRAME);
	meter.Count(CommandMeter::BUSY_FRAME + 1);
	CHECK(meter.EndFrame(t0 + 1s));
	CHECK(meter.Most() == CommandMeter::BUSY_FRAME + 1);
	CHECK(!meter.EndFrame(t0 + 2s));
	CHECK(meter.Last() == 0);

	for (std::uint32_t i = 0; i < CommandMeter::MAX_REGISTRATIONS_PER_FRAME; ++i) {
		REQUIRE(meter.TryRegister());
	}
	CHECK(!meter.TryRegister());
	CHECK(meter.Registrations() == CommandMeter::MAX_REGISTRATIONS_PER_FRAME);  // a refused one isn't counted
	meter.EndFrame(t0 + 3s);
	CHECK(meter.TryRegister());

	// A hand-back is a command (its evp) in the frame it happens.
	meter.EndFrame(t0 + 4s);
	meter.Released();
	meter.Released();
	meter.EndFrame(t0 + 5s);
	CHECK(meter.Last() == 2);
	CHECK(meter.Releases() == 2);
}
