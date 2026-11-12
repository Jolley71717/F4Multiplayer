#pragma once

#include "Protocol.h"
#include "game/ResourceMirror.h"
#include "game/WorkshopNumbers.h"

#include <optional>
#include <string>
#include <string_view>

// A friend's workshop pieces counted in our settlement's food, water, defense and power: the engine's side of it
// (ResourceMirror.h is the game-free half; WorkshopSync runs it, as it runs the power mirror).
//
// A settlement's numbers are actor values on its workbench. The game's own recount (the workbench's RecalculateResources,
// run as pieces are built, scrapped, assigned or powered) sets each value's base to the sum of what every piece linked to
// the workbench counts for, and its damage modifier to minus the sum of those that can't produce now (an unassigned crop,
// an unpowered purifier): the rig, 2026-11-04, docs/DESIGN.md "A friend's pieces count in your settlement". It leaves the
// permanent and temporary modifiers be, and the Pip-Boy and the workshop scripts read the value with both (GetValue), but
// for beds: the Pip-Boy shows a settlement's beds, and the daily update counts them, by the base alone. So friends' pieces
// go on the workbench as its temporary modifier (scripts' ModValue writes the permanent one), and beds not at all yet.
// Main thread only.
namespace WorkshopResources
{
	// The workbench actor value each kind is (beds: resourceBed, which the mirror doesn't put on workbenches).
	[[nodiscard]] RE::ActorValueInfo* ValueOf(ResourceMirror::Kind a_kind);

	// The engine's own test of whether a workshop piece counts now (its CanProduceForWorkshop condition, which the
	// workbench's recount goes by): none if the game has no such function.
	[[nodiscard]] std::optional<bool> CanProduce(RE::TESObjectREFR& a_ref);

	// What the piece counts for in its workbench's numbers now, as the recount counts it: each value, if it can produce.
	[[nodiscard]] ResourceMirror::Values Counts(RE::TESObjectREFR& a_ref);
	// Whether it counts for anything at all, producing or not (a piece worth watching: a crop, a pump, a bed, a turret, a
	// generator, a light).
	[[nodiscard]] bool CountsForAnything(RE::TESObjectREFR& a_ref);

	// Adds a_delta to the workbench's value of that kind, on its temporary modifier. False if it is no workbench here.
	bool AddToBench(std::uint32_t a_bench, ResourceMirror::Kind a_kind, float a_delta);
	// The workbench's temporary modifier of that kind (none: no workbench here).
	[[nodiscard]] std::optional<float> TemporaryOf(std::uint32_t a_bench, ResourceMirror::Kind a_kind);

	// What the workshop bar would show at that workbench now: the engine's own getters, the ones the bar's fill calls
	// (WorkshopNumbers.h; the host's game tells its friends these). The workbench loaded or not: they read its values.
	[[nodiscard]] WorkshopNumbers::Read ReadFigures(RE::TESObjectREFR& a_bench);

	// ---- Dev -----------------------------------------------------------------------------------------

	// The settlement values of the reference (a workbench or a piece): each one's value, base and modifiers.
	[[nodiscard]] std::string DescribeValues(RE::TESObjectREFR* a_ref);
	// Adds a_delta to one of the reference's values (a_av: food water safety beds happiness power powerreq, or an AVIF's
	// hex ID), on its permanent, temporary or damage modifier, or its base.
	[[nodiscard]] std::string ModValue(RE::TESObjectREFR* a_ref, std::string_view a_av, std::string_view a_kind, float a_delta);
	// The Pip-Boy's workshop data as it holds it now.
	[[nodiscard]] std::string DescribePipboy(std::string_view a_filter);
	// The engine's CanProduceForWorkshop on the reference (its condition function, as a quest condition runs it), and its
	// values; or, for a workbench, each of its workshop items that counts for something and the sums, all of them and
	// those the engine says produce, beside the workbench's own values.
	[[nodiscard]] std::string DescribeProduce(RE::TESObjectREFR* a_ref);
	[[nodiscard]] std::string SumPieces(RE::TESObjectREFR* a_bench);
	// The buildable objects (a recipe makes them) whose name holds the text.
	[[nodiscard]] std::string FindBuildable(std::string_view a_text);
}
