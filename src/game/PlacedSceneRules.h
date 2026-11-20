#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// A town's scene played once for the session (PlacedScenes.h has the game's side). Diamond City's synth accusation
// (FFDiamondCity12): a run-once misc quest the story manager starts as a game's player comes into Diamond City
// (DiamondCitySpecialSceneNode, on the change of location, once IntroSceneDiamondCitySalon's 10 is done and its own 10
// is not). Its 10 switches on Kyle, who holds his brother Riley at gunpoint, placed NPCs every game has, and makes two
// guards at markers; two trigger boxes in the market set 15 and 20 as that game's own player walks in, which start its
// scenes; security shoots Kyle (30); the quest stops once that player leaves Diamond City (300), and its shutdown stage
// (1000) deletes Kyle, Riley and the guards. Every game used to play its own run over its own copies of the two.
//
// Now the run is the session's: the game whose claim (15, or 20) the server kept first runs it (the runner) and holds
// Kyle and Riley (NpcSync claims them as it does a conversation's speaker), its guards shared calm as a radiant's are.
// Every other game follows: it marks the quest's stages done without their scripts (its triggers then never fire, and the
// story manager never starts the quest there, its 10 being done), and shows the runner's Kyle and Riley as it shows any
// NPC a friend's game runs. A game whose own run had started (both players came in) yields: its stages are marked the same
// way, its own guards removed for the runner's, and its run is stopped once the runner's ends. A follower that never ran
// the quest switches Kyle and Riley off once the runner's run ends (its shutdown deleted them there) and keeps them off.
// The runner sets a claim's stage itself when a friend's stand-in walks into its trigger's area, since a friend's game,
// following, sets none. A late arrival gets the runner's stages from the session's state and follows the same way.
//
// The stages go as any quest's (kReportQuestStage, kQuestStage): no protocol change. A game without this ignores them (the
// quest is no shared one), and plays its own run, as before.
// Kept free of the game so it can be tested (tests/PlacedSceneTests.cpp).
namespace PlacedSceneRules
{
	// Where a trigger box stands, as the ground under it: a circle around its centre, as wide as the box's narrower side
	// (both are boxes near square, turned about 93 degrees), and a band of height around it that takes a player's feet
	// under the box (the trigger is their body's, which stands above them).
	struct Area
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float radius = 0.0f;
		float below = 0.0f;  // feet this far under the centre are in
		float above = 0.0f;  // ... and this far over it
	};

	// A stage the quest's trigger sets as its player walks in, once a stage before it is done: the stage that makes a game
	// the runner (the first of them kept by the server).
	struct Claim
	{
		std::uint16_t stage = 0;
		std::uint16_t prereq = 0;
		Area          area{};
	};

	struct Event
	{
		std::uint8_t                 file = 0;        // StageRules::File (0: Fallout4.esm)
		std::uint32_t                quest = 0;       // the quest's local id
		std::uint32_t                worldspace = 0;  // the triggers' worldspace, a local id of the same file
		std::array<Claim, 2>         claims{};
		std::uint16_t                shutdown = 0;  // the stage the quest runs as it stops (its INDX flag 4)
		std::array<std::uint16_t, 2> ends{};         // stages that end the run: its player left, or it stopped
		std::array<std::uint32_t, 2> actors{};       // the placed NPCs it runs (local ids of the same file)
		std::string_view             name;
	};

	// Read from Fallout4.esm: the quest's INDX, its triggers' XPRM and DATA (0015DFF3 sets 15, 0015378E sets 20, each
	// once 10 is done), and the ACHRs its unique aliases name (cell DiamondCityOrigin).
	inline constexpr Event EVENTS[] = {
		{ 0, 0x14BF48, 0x000F94,
			{ { { 15, 10, { 2010.5f, 1196.8f, -1231.1f, 2595.0f, 600.0f, 322.0f } },
				{ 20, 10, { 1883.4f, 1111.4f, -1419.5f, 815.0f, 300.0f, 134.0f } } } },
			1000, { 300, 1000 }, { 0x14BF68, 0x14BF65 }, "Diamond City's synth accusation" },
	};

	// A friend's stand-in in a claim's area this long (and still there) has the runner set the claim's stage: a passing
	// stand-in sets none, and the friend's own game, if it runs the quest, sets its own first.
	inline constexpr auto FRIEND_IN_AREA = std::chrono::seconds(3);

	constexpr const Event* Find(std::uint8_t a_file, std::uint32_t a_quest)
	{
		for (const auto& event : EVENTS) {
			if (event.file == a_file && event.quest == a_quest) {
				return &event;
			}
		}
		return nullptr;
	}

	constexpr bool IsClaim(const Event& a_event, std::uint16_t a_stage)
	{
		return std::ranges::any_of(a_event.claims, [&](const Claim& a_claim) { return a_claim.stage == a_stage; });
	}

	constexpr bool IsEnd(const Event& a_event, std::uint16_t a_stage)
	{
		return std::ranges::contains(a_event.ends, a_stage);
	}

	constexpr bool IsActor(const Event& a_event, std::uint32_t a_local)
	{
		return std::ranges::contains(a_event.actors, a_local);
	}

	constexpr bool Inside(const Area& a_area, float a_x, float a_y, float a_z)
	{
		const float dx = a_x - a_area.x;
		const float dy = a_y - a_area.y;
		return dx * dx + dy * dy <= a_area.radius * a_area.radius && a_z >= a_area.z - a_area.below && a_z <= a_area.z + a_area.above;
	}

	// What our game's quest is, read from it.
	struct Facts
	{
		bool running = false;    // the quest runs here
		bool claimDone = false;  // one of its claims' stages is done here
		bool mirror = false;     // a friend's run marked it (IsMirror)
	};

	// Whether the stages done here are a friend's run's marks: every stage of the quest but its shutdown done, which no
	// game's own run does (its 25, Kyle unloaded, and its 30, security's shot, are two ends of the scene that never both run).
	constexpr bool IsMirror(std::span<const std::uint16_t> a_stages, std::span<const std::uint16_t> a_done, std::uint16_t a_shutdown)
	{
		bool any = false;
		for (const auto stage : a_stages) {
			if (stage == a_shutdown) {
				continue;
			}
			if (!std::ranges::contains(a_done, stage)) {
				return false;
			}
			any = true;
		}
		return any;
	}

	enum class Role : std::uint8_t
	{
		kNone,      // no run here yet, or one nobody claimed: our trigger may still make us the runner
		kRunner,    // our game's run is the session's
		kFollower,  // a friend's run is, and our quest never ran (or ran and stopped before it)
		kYielded,   // a friend's run is, and our own run still goes, hollow, until theirs ends
	};

	constexpr Role RoleOf(const Facts& a_facts)
	{
		if (a_facts.mirror) {
			return a_facts.running ? Role::kYielded : Role::kFollower;
		}
		return a_facts.running && a_facts.claimDone ? Role::kRunner : Role::kNone;
	}

	constexpr std::string_view Name(Role a_role)
	{
		switch (a_role) {
		case Role::kRunner:
			return "runner";
		case Role::kFollower:
			return "follower";
		case Role::kYielded:
			return "yielded";
		default:
			return "none";
		}
	}

	// What our game does with a friend's stage of the quest.
	enum class Act : std::uint8_t
	{
		kIgnore,
		kMark,         // marked done here, no script (we follow their run)
		kFollow,       // their claim, and our quest doesn't run: follow (mark every stage but the shutdown)
		kYield,        // their claim, while our own run goes: yield (mark the same; our guards go, theirs stand)
		kEndFollowed,  // their run ended, and we never ran it: Kyle and Riley off, and kept off
		kEndYielded,   // their run ended, and our hollow run still goes: stopped (its shutdown deletes ours)
	};

	// a_live: it came as the friend's game set it, not with the session's state (a catch-up). A catch-up's claim never
	// takes over a run of ours whose claim is done (the server kept theirs from some other time: a session file).
	constexpr Act OnFriendStage(const Event& a_event, const Facts& a_facts, std::uint16_t a_stage, bool a_live)
	{
		const auto role = RoleOf(a_facts);
		if (role == Role::kRunner && !(IsClaim(a_event, a_stage) && a_live)) {
			return Act::kIgnore;
		}
		if (IsEnd(a_event, a_stage)) {
			switch (role) {
			case Role::kFollower:
				return Act::kEndFollowed;
			case Role::kYielded:
				return Act::kEndYielded;
			default:
				return Act::kIgnore;
			}
		}
		if (role == Role::kFollower || role == Role::kYielded) {
			return Act::kMark;
		}
		if (!IsClaim(a_event, a_stage)) {
			return Act::kIgnore;  // a stage before anyone's claim: each game's own start (10)
		}
		if (!a_facts.running) {
			return Act::kFollow;
		}
		return a_live || !a_facts.claimDone ? Act::kYield : Act::kIgnore;
	}

	// Whether our game reports its stages of the quest: once a claim of ours is done, while it is no friend's run's marks
	// (a run of ours that ended too: the session's state then has it for a friend who joins later).
	constexpr bool Reports(const Facts& a_facts)
	{
		return a_facts.claimDone && !a_facts.mirror;
	}

	// Whether our game holds the run's actors and shares the actors its start made: our run is the session's.
	constexpr bool Runs(const Facts& a_facts)
	{
		return RoleOf(a_facts) == Role::kRunner;
	}

	// The claim whose stage our game sets for a friend's stand-in in its area: our quest runs and is no friend's run's,
	// the claim's prereq is done and its stage is not. a_done: whether a stage is done here; a_friendIn: whether a friend's
	// stand-in has stood in the area of the claim at that index for FRIEND_IN_AREA.
	template <class Done, class FriendIn>
	constexpr std::optional<std::uint16_t> ClaimFor(const Event& a_event, const Facts& a_facts, Done&& a_done, FriendIn&& a_friendIn)
	{
		if (!a_facts.running || a_facts.mirror) {
			return std::nullopt;
		}
		for (std::size_t i = 0; i < a_event.claims.size(); ++i) {
			const auto& claim = a_event.claims[i];
			if (a_done(claim.prereq) && !a_done(claim.stage) && a_friendIn(i)) {
				return claim.stage;
			}
		}
		return std::nullopt;
	}
}
