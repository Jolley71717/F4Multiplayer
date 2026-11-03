#include "Test.h"

#include "game/EnableChain.h"

#include <array>

// What is switched on so that a reference a friend's game has on comes on here (NpcSync::SwitchOn, WorldSync's switch-on
// before a kill): the engine ignores an enable of a reference with an enable parent, so the top of its chain goes on.
namespace
{
	using EnableChain::Link;
	using EnableChain::Verdict;

	constexpr std::uint32_t MIRELURK = 0x000C3DB4;  // one of Salem's LvlMirelurkCrab, under Gun Run's marker
	constexpr std::uint32_t ENABLER = 0x000C3D6F;   // MirelurkQuestEnabler, off until DN083_Barney's stage 30
	constexpr std::uint32_t MARKER = 0x000E4610;
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
	const std::array links{ Link{ ENABLER, false, true } };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParent);
	CHECK(plan.target == ENABLER);
	CHECK(EnableChain::SwitchesOn(plan));
}

TEST("enable chain: the top of a longer chain is switched on")
{
	const std::array links{ Link{ MARKER, false, true }, Link{ ENABLER, false, true } };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParent);
	CHECK(plan.target == ENABLER);
	// The links between are on or off as the top is: only the top is read.
	const std::array mixed{ Link{ MARKER, false, false }, Link{ ENABLER, false, true } };
	CHECK(EnableChain::Decide(MIRELURK, mixed, true).target == ENABLER);
}

TEST("enable chain: nothing is switched on when the top is on already")
{
	const std::array links{ Link{ ENABLER, false, false } };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParentOn);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: never a parent the opposite way, which would have to go off")
{
	const std::array links{ Link{ ENABLER, true, false } };
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kOpposite);
	// Higher up the chain too.
	const std::array higher{ Link{ MARKER, false, true }, Link{ ENABLER, true, false } };
	const auto       plan = EnableChain::Decide(MIRELURK, higher, true);
	CHECK(plan.verdict == Verdict::kOpposite);
	CHECK(plan.target == ENABLER);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: nothing when a parent is not found here")
{
	const std::array links{ Link{ 0, false, false } };
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kNoParent);
	const std::array higher{ Link{ MARKER, false, true }, Link{ 0, false, false } };
	CHECK(!EnableChain::SwitchesOn(EnableChain::Decide(MIRELURK, higher, true)));
}

TEST("enable chain: with everyone on their own story no parent is switched on")
{
	const std::array links{ Link{ ENABLER, false, true } };
	const auto       plan = EnableChain::Decide(MIRELURK, links, false);
	CHECK(plan.verdict == Verdict::kOwnStory);
	CHECK(!EnableChain::SwitchesOn(plan));
}

TEST("enable chain: a chain longer than the limit is left alone")
{
	std::array<Link, EnableChain::MAX_DEPTH + 1> links{};
	for (auto& link : links) {
		link = { MARKER, false, true };
	}
	CHECK(EnableChain::Decide(MIRELURK, links, true).verdict == Verdict::kTooDeep);
	CHECK(EnableChain::Decide(MIRELURK, std::span{ links.data(), EnableChain::MAX_DEPTH }, true).verdict == Verdict::kParent);
}

TEST("enable chain: every verdict has a name for the log")
{
	for (const auto verdict : { Verdict::kSelf, Verdict::kParent, Verdict::kParentOn, Verdict::kOpposite, Verdict::kNoParent, Verdict::kTooDeep, Verdict::kOwnStory }) {
		CHECK(EnableChain::VerdictName(verdict) != "?");
	}
}
