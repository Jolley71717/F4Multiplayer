#include "Test.h"

#include "game/DisabledParents.h"
#include "game/EnableChain.h"

#include <algorithm>
#include <array>
#include <iterator>

// What is switched on so that a reference a friend's game has on comes on here (NpcSync::SwitchOn, WorldSync's switch-on
// before a kill): the engine ignores an enable of a reference with an enable parent, so the top of its chain goes on, but
// only the game's own parent, as its plugin left it, switching no more than MAX_CHILDREN references.
namespace
{
	using EnableChain::Link;
	using EnableChain::PluginState;
	using EnableChain::Verdict;

	constexpr std::uint32_t MIRELURK = 0x000C3DB4;  // one of Salem's LvlMirelurkCrab, under Gun Run's marker
	constexpr std::uint32_t ENABLER = 0x000C3D6F;   // MirelurkQuestEnabler, off until DN083_Barney's stage 30
	constexpr std::uint32_t MARKER = 0x000E4610;

	// A link to a parent of the game's own files, as its plugin left it (off here only if off there), switching
	// a_children references.
	constexpr Link Vanilla(std::uint32_t a_parent, bool a_off, std::uint32_t a_children = 1)
	{
		Link link;
		link.parent = a_parent;
		link.parentOff = a_off;
		link.official = true;
		link.plugin = a_off ? PluginState::kOff : PluginState::kOn;
		link.changeFlags = 0;
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
		link.changeFlags = EnableChain::CHANGES_UNKNOWN;
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

TEST("enable chain: Salem's marker as a fresh game has it, its flags recorded as changed by the engine, is switched on")
{
	// The rig's game: Gun Run at stage 5, nobody had touched the marker, and our save recorded CHANGE_FORM_FLAGS on it
	// all the same (the engine records it for a reference it only loads). Off here and off in its plugin: as it was left.
	auto marker = Vanilla(ENABLER, true, 48);
	marker.changeFlags = EnableChain::CHANGE_FORM_FLAGS;
	const std::array links{ marker };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParent);
	CHECK(plan.target == ENABLER);
	CHECK(EnableChain::SwitchesOn(plan));
	// Other change flags beside it change nothing either.
	marker.changeFlags = 0x80000001;
	const std::array more{ marker };
	CHECK(EnableChain::Decide(MIRELURK, more, true).verdict == Verdict::kParent);
}

TEST("enable chain: a parent off here that its plugin has on is left off: our game switched it off, or our story is ahead")
{
	auto changed = Vanilla(ENABLER, true);
	changed.plugin = PluginState::kOn;
	changed.changeFlags = EnableChain::CHANGE_FORM_FLAGS;
	const std::array links{ changed };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kChanged);
	CHECK(plan.target == ENABLER);
	CHECK(!EnableChain::SwitchesOn(plan));
	// Only the top's own state counts: the links below it follow it.
	auto middle = Vanilla(MARKER, true);
	middle.plugin = PluginState::kOn;
	middle.changeFlags = EnableChain::CHANGE_FORM_FLAGS;
	const std::array chain{ middle, Vanilla(ENABLER, true) };
	CHECK(EnableChain::Decide(MIRELURK, chain, true).verdict == Verdict::kParent);
	// Its change flags don't matter: a reference loaded with the game's data that was on then is on in its plugin, so
	// off here our game switched it off (a disable made while the engine records no changes leaves no flag).
	changed.changeFlags = 0;
	const std::array unrecorded{ changed };
	CHECK(EnableChain::Decide(MIRELURK, unrecorded, true).verdict == Verdict::kChanged);
	changed.changeFlags = EnableChain::CHANGES_UNKNOWN;
	const std::array noSave{ changed };
	CHECK(EnableChain::Decide(MIRELURK, noSave, true).verdict == Verdict::kChanged);
}

TEST("enable chain: a parent its plugin has off is switched on with no save bookkeeping to ask")
{
	// Its plugin's state is known: the change flags aren't needed (at 58f28da0 this was refused).
	auto marker = Vanilla(ENABLER, true, 48);
	marker.changeFlags = EnableChain::CHANGES_UNKNOWN;
	const std::array links{ marker };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.verdict == Verdict::kParent);
	CHECK(plan.target == ENABLER);
	CHECK(EnableChain::SwitchesOn(plan));
}

TEST("enable chain: a parent a script of the game's own turns off again is left to our own story")
{
	// BoSAirportGuards (0019C046): off in Fallout4.esm, BoSEnable's stage 10 turns it on, its stage 200 and Inst307Fight's
	// stage 10 off. A game past the Brotherhood's end at the airport has it off; a friend's still there runs its guards.
	constexpr std::uint32_t AIRPORT = 0x0019C046;
	auto airport = Vanilla(AIRPORT, true, 12);
	airport.storyTurnsOff = true;
	const std::array links{ airport };
	const auto       plan = EnableChain::Decide(0x0019C04A, links, true);
	CHECK(plan.verdict == Verdict::kStoryTurnsOff);
	CHECK(plan.target == AIRPORT);
	CHECK(!EnableChain::SwitchesOn(plan));
	// Anywhere along the chain.
	const std::array chain{ airport, Vanilla(ENABLER, true) };
	CHECK(EnableChain::Decide(MIRELURK, chain, true).verdict == Verdict::kStoryTurnsOff);
	const std::array below{ Vanilla(MARKER, true), airport };
	CHECK(EnableChain::Decide(MIRELURK, below, true).verdict == Verdict::kStoryTurnsOff);
}

TEST("enable chain: where its plugin's state isn't known, only a parent our save records no flag change of is switched on")
{
	// A reference not loaded with the game's data (a temporary one) is not in the list of those off in their plugins.
	auto unknown = Vanilla(ENABLER, true);
	unknown.plugin = PluginState::kUnknown;
	unknown.changeFlags = 0;
	const std::array untouched{ unknown };
	CHECK(EnableChain::Decide(MIRELURK, untouched, true).verdict == Verdict::kParent);
	unknown.changeFlags = EnableChain::CHANGE_FORM_FLAGS;
	const std::array changed{ unknown };
	CHECK(EnableChain::Decide(MIRELURK, changed, true).verdict == Verdict::kChanged);
	// Other change flags (a move, its inventory) say nothing of its enable state.
	unknown.changeFlags = 0x2;
	const std::array moved{ unknown };
	CHECK(EnableChain::Decide(MIRELURK, moved, true).verdict == Verdict::kParent);
	// No save bookkeeping to ask: nothing is known, so nothing is switched on.
	unknown.changeFlags = EnableChain::CHANGES_UNKNOWN;
	const std::array noSave{ unknown };
	CHECK(EnableChain::Decide(MIRELURK, noSave, true).verdict == Verdict::kChanged);
}

TEST("enable chain: the plan carries the top's change flags and plugin state for the log, once the walk got to the top")
{
	auto marker = Vanilla(ENABLER, true, 48);
	marker.changeFlags = 0x1;
	const std::array links{ marker };
	const auto       plan = EnableChain::Decide(MIRELURK, links, true);
	CHECK(plan.atTop);
	CHECK(plan.changeFlags == 0x1u);
	CHECK(plan.plugin == PluginState::kOff);
	auto onHere = Vanilla(ENABLER, false);
	onHere.changeFlags = 0x5;
	const std::array on{ onHere };
	const auto       onPlan = EnableChain::Decide(MIRELURK, on, true);
	CHECK(onPlan.verdict == Verdict::kParentOn);
	CHECK(onPlan.atTop);
	CHECK(onPlan.changeFlags == 0x5u);
	CHECK(onPlan.plugin == PluginState::kOn);
	// Stopped below the top: nothing of the top to say.
	const std::array opposite{ Opposite(ENABLER, false) };
	CHECK(!EnableChain::Decide(MIRELURK, opposite, true).atTop);
	CHECK(!EnableChain::Decide(MIRELURK, {}, true).atTop);
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
			 Verdict::kUnofficial, Verdict::kChanged, Verdict::kParentDead, Verdict::kTooMany, Verdict::kStoryTurnsOff }) {
		CHECK(EnableChain::VerdictName(verdict) != "?");
	}
}

// DisabledParents.h, generated by tools/questscan.ps1 -DisabledParents from the game's own files.
TEST("enable chain: the table of parents the game's scripts turn off is as the scan wrote it, sorted, one entry each")
{
	static_assert(std::size(DisabledParents::PARENTS) == DisabledParents::SCANNED, "DisabledParents.h was changed by hand: run tools/questscan.ps1 -DisabledParents");
	const auto less = [](const DisabledParents::Entry& a, const DisabledParents::Entry& b) { return a.file != b.file ? a.file < b.file : a.id < b.id; };
	CHECK(std::ranges::is_sorted(DisabledParents::PARENTS, less));
	CHECK(std::ranges::adjacent_find(DisabledParents::PARENTS, [](const auto& a, const auto& b) { return a.file == b.file && a.id == b.id; }) == std::end(DisabledParents::PARENTS));
	for (const auto& entry : DisabledParents::PARENTS) {
		CHECK(entry.file < DisabledParents::FILES.size());
		CHECK((entry.id & 0xFF000000) == 0);
	}
}

TEST("enable chain: the markers quests turn on and off again are in the table, Gun Run's is not")
{
	// What the review of the change found by its own read of Fallout4.esm and its scripts: each off in the plugin, turned
	// on by a quest stage and off by a later one.
	for (const std::uint32_t id : {
			 0x0019C046u,  // BoSAirportGuards: BoSEnable 10 on; BoSEnable 200, Inst307Fight 10 off
			 0x001096BEu,  // DN130_80_RR302BoSSoldiersEnableTempMarker: DN130 80 on, 90 off
			 0x0003A75Cu,  // DN130_50_BoSSoldiersEnableTempMarker: DN130 50 on, 80 off, 90 on
			 0x00222C62u,  // DN136_FleeingScientistEnableMarkerREF: DN136_Attack 10 on, 2000 off
			 0x0015D3A4u,  // DN084_BoS303_RoofEnableMarker: 11 on, 500 off
			 0x0015D382u,  // DN084_Inst303_RoofEnableMarker: 12 on, 500 off
			 0x000F8E1Eu,  // DN084_Inst303_DescentAmbushEnableMarker: 228 on, 391 off
			 0x0019661Cu,  // RR303GloryDeathEnabler
			 0x00153344u,  // RRLiamFuneralEnableMarker
			 0x001A7BD4u,  // BoS101POI_EnableTemp
			 0x00248653u,  // DN048_EnableExt
		 }) {
		CHECK(DisabledParents::Contains("Fallout4.esm", id));
	}
	// MirelurkQuestEnabler: only DN083_Barney's stage 30 names it, to turn it on.
	CHECK(!DisabledParents::Contains("Fallout4.esm", 0x000C3D6F));
	// DmndSecurityEnableMarker, on in its plugin (the rig's negative case: refused as kChanged, not through the table).
	CHECK(!DisabledParents::Contains("Fallout4.esm", 0x000E1AC8));
	// By the plugin that made it, in any case, its load order's index ignored.
	CHECK(DisabledParents::Contains("FALLOUT4.ESM", 0x0119C046));
	CHECK(!DisabledParents::Contains("SomeMod.esp", 0x0019C046));
	CHECK(!DisabledParents::Contains("DLCCoast.esm", 0x0019C046));
	CHECK(DisabledParents::FileIndex("dlccoast.esm") == 3u);
	CHECK(DisabledParents::FileIndex("Fallout4.esp") == DisabledParents::FILES.size());
}
