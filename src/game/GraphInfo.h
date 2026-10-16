#pragma once

#include <span>
#include <string>
#include <string_view>

// What a reference's animation graph is and what it holds, for the dev command anim and the log: the behaviour project
// its race builds (an actor in power armor is given the power armor race, whose project is the power armor one),
// whether it has an animation graph manager at all and how many graphs that holds, the locomotion variables the
// stand-ins are driven by (Puppets::DriveAnimation) as the graph reads them back, its leg bone, and the puppet's own
// state. A variable shows as "?" when the graph cannot read it: every one of them with "manager=none" or "graphs=0"
// means there is no graph to read (nothing it is sent plays either), with a graph there it means its project has no
// variable by that name.
namespace GraphInfo
{
	// a_more: further variable names to read (a b or i first letter reads it as a bool or an int).
	std::string Describe(RE::TESObjectREFR* a_ref, std::span<const std::string_view> a_more = {});
}
