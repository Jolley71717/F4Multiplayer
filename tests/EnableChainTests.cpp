#include "Test.h"

#include "game/EnableChain.h"

#include <array>

// What is switched on so that a reference a friend's game has on comes on here (NpcSync::SwitchOn, WorldSync's switch-on
// before a kill): the engine ignores an enable of a reference with an enable parent, so the top of its chain goes on, but
// only the game's own parent, as its plugin left it, switching no more than MAX_CHILDREN references.
namespace
{
	using EnableChain::Link;
	using EnableChain::Verdict;

	constexpr std::uint32_t MIRELURK = 0x000C3DB4;  // one of Salem's LvlMirelurkCrab, under Gun Run's marker
	constexpr std::uint32_t ENABLER = 0x000C3D6F;   // MirelurkQuestEnabler, off until DN083_Barney's stage 30
	constexpr std::uint32_t MARKER = 0x000E4610;

	// A link to a parent of the game's own files, as its plugin left it, switching a_children references.
	constexpr Link Vanilla(std::uint32_t a_parent, bool a_off, std::uint32_t a_children = 1)
	{
		Link link;
		link.parent = a_parent;
		link.parentOff = a_off;
		link.official = true;
		link.unchanged = true;
		link.children = a_children;
		return link;
	}

	constexpr Link Opposite(std::uint32_t a_parent, bool a_off)
	{
		auto link = Vanilla(a_parent, a_off);
		link.opposite = true;
		return link;
	}

	constexpr Link NotFound()
	{
		auto link = Vanilla(0, false, 0);
		link.official = false;
		link.unchanged = false;
		return link;
	}
}

TEST("enable chain: a reference with no enable parent is switched on itself")
{
	const auto plan = EnableChain::Decide(MIRELURK, {}, true);
	CHECK(plan.verdict == Verdict::kSelf);
	CHECK(plan.target == MIRELURK);
	CHECK(EnableChain::SwitchesOn(plan));
	// The story setting does not matter for one with no parent: its own check (MaySwitchOn) comes before.
	CHECK(EnableChain::Decide(MIRELURK, {}, false).verdict == Verdict::kSelf);
}

TEST("enable chain: Salem's mirelurk is switched on through Gun Run's marker")
{
	const std::array links{ Vanilla(ENABLER, true, 48) };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParent);
	CHECK(plan.target == ENABLER);
	CHECK(plan.children == 48u);
	CHECK(EnableChain::SwitchesOn(plan));
}

TEST("enable chain: the top of a longer chain is switched on")
{
	const std::array links{ Vanilla(MARKER, true), Vanilla(ENABLER, true) };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParent);
	CHECK(plan.target == ENABLER);
	CHECK(plan.children == 2u);  // what every parent of the chain switches
	// The links between are on or off as the top is: only the top is read.
	const std::array mixed{ Vanilla(MARKER, false), Vanilla(ENABLER, true) };
	CHECK(EnableChain::Decide(MIRELURK, mixed, true).target == ENABLER);
}

TEST("enable chain: a chain whose middle link is off while the top is on switches nothing")
{
	// Something else keeps the middle off (or it is still turning): the top is on, and switching it on does nothing.
	const std::array links{ Vanilla(MARKER, true), Vanilla(0x000E4611, true), Vanilla(ENABLER, false) };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParentOn);
	CHECK(plan.target == ENABLER);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: nothing is switched on when the top is on already")
{
	const std::array links{ Vanilla(ENABLER, false) };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParentOn);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: never a parent the opposite way, which would have to go off")
{
	const std::array links{ Opposite(ENABLER, false) };
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kOpposite);
	// Higher up the chain too.
	const std::array higher{ Vanilla(MARKER, true), Opposite(ENABLER, false) };
	const auto       plan = EnableChain::Decide(MIRELURK, higher, true);
	CHECK(plan.verdict == Verdict::kOpposite);
	CHECK(plan.target == ENABLER);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: nothing when a parent is not found here, or is deleted")
{
	const std::array links{ NotFound() };
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kNoParent);
	const std::array higher{ Vanilla(MARKER, true), NotFound() };
	CHECK(!EnableChain::SwitchesOn(EnableChain::Decide(MIRELURK, higher, true)));
}

TEST("enable chain: with everyone on their own story no parent is switched on")
{
	const std::array links{ Vanilla(ENABLER, true) };
	const auto       plan = EnableChain::Decide(MIRELURK, links, false);
	CHECK(plan.verdict == Verdict::kOwnStory);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: a parent our save changed is left off: our game switched it off, or our story is ahead")
{
	auto changed = Vanilla(ENABLER, true);
	changed.unchanged = false;
	const std::array links{ changed };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kChanged);
	CHECK(plan.target == ENABLER);
	CHECK(!EnableChain::SwitchesOn(plan));
	// Only the top's own state counts: the links below it follow it.
	auto middle = Vanilla(MARKER, true);
	middle.unchanged = false;
	const std::array chain{ middle, Vanilla(ENABLER, true) };
	CHECK(EnableChain::Decide(MIRELURK, chain, true).verdict == Verdict::kParent);
}

TEST("enable chain: a link or a parent of a plugin outside the game's own files is left alone")
{
	// A mod only our game has that hides a vanilla NPC behind a marker of its own (or overrides the NPC to name one).
	auto modded = Vanilla(ENABLER, true);
	modded.official = false;
	const std::array links{ modded };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kUnofficial);
	CHECK(plan.target == ENABLER);
	CHECK(!EnableChain::SwitchesOn(plan));
	// Anywhere along the chain.
	auto below = Vanilla(MARKER, true);
	below.official = false;
	const std::array chain{ below, Vanilla(ENABLER, true) };
	CHECK(EnableChain::Decide(MIRELURK, chain, true).verdict == Verdict::kUnofficial);
	// A default link (as the engine side fills it when it reads nothing) is never official.
	const std::array unknown{ Link{ ENABLER, false, true } };
	CHECK(EnableChain::Decide(MIRELURK, unknown, true).verdict == Verdict::kUnofficial);
}

TEST("enable chain: a dead actor as a parent is not brought back with its children")
{
	auto dead = Vanilla(ENABLER, true);
	dead.parentDead = true;
	const std::array links{ dead };
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kParentDead);
	auto deadBelow = Vanilla(MARKER, true);
	deadBelow.parentDead = true;
	const std::array chain{ deadBelow, Vanilla(ENABLER, true) };
	CHECK(EnableChain::Decide(MIRELURK, chain, true).verdict == Verdict::kParentDead);
}

TEST("enable chain: a parent that switches more than MAX_CHILDREN references is left off, counted across the chain")
{
	const std::array atCap{ Vanilla(ENABLER, true, EnableChain::MAX_CHILDREN) };
	CHECK(EnableChain::Decide(MIRELURK, atCap, true).verdict == Verdict::kParent);
	const std::array over{ Vanilla(ENABLER, true, EnableChain::MAX_CHILDREN + 1) };
	const auto       plan = EnableChain::Decide(MIRELURK, over, true);
	CHECK(plan.verdict == Verdict::kTooMany);
	CHECK(plan.children == EnableChain::MAX_CHILDREN + 1);
	CHECK(!EnableChain::SwitchesOn(plan));
	const std::array chain{ Vanilla(MARKER, true, 40), Vanilla(ENABLER, true, 40) };
	CHECK(EnableChain::Decide(MIRELURK, chain, true).verdict == Verdict::kTooMany);
	// A parent on already is that, whatever it switches.
	const std::array onBig{ Vanilla(ENABLER, false, 500) };
	CHECK(EnableChain::Decide(MIRELURK, onBig, true).verdict == Verdict::kParentOn);
}

TEST("enable chain: a chain longer than the limit is left alone")
{
	std::array<Link, EnableChain::MAX_DEPTH + 1> links{};
	for (auto& link : links) {
		link = Vanilla(MARKER, true);
	}
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kTooDeep);
	CHECK(EnableChain::Decide(MIRELURK, std::span{ links.data(), EnableChain::MAX_DEPTH }, true).verdict == Verdict::kParent);
}

TEST("enable chain: every verdict has a name for the log")
{
	for (const auto verdict : { Verdict::kSelf, Verdict::kParent, Verdict::kParentOn, Verdict::kOpposite, Verdict::kNoParent, Verdict::kTooDeep, Verdict::kOwnStory,
			 Verdict::kUnofficial, Verdict::kChanged, Verdict::kParentDead, Verdict::kTooMany }) {
		CHECK(EnableChain::VerdictName(verdict) != "?");
	}
}
