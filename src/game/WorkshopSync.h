#pragma once

#include "Protocol.h"

// Shared settlement building: what a player places, moves or scraps in workshop mode is copied
// into the other players' games (a plain copy of the object: it isn't part of their workshop, so
// they can't scrap or power it; the builder keeps that). A power wire's copy is drawn from its
// builder's shape; a piece the settlement came with, moved, moves in the others' games instead.
// Copies are removed when the session ends; what a player kept in a copy of a container comes to
// them first. The host also reports the settlement junk its save had scrapped before the session,
// and what it built before the session (BuiltWalk).
// Main thread only.
namespace WorkshopSync
{
	// Hooks the workshop's placed/moved/scrapped events. Call once the game is up; no-op after.
	void Install();

	// Places copies (and scraps pieces of the world) that were waiting for their cell to load and, on
	// the host (a_host), walks the loaded cells for junk scrapped, and pieces built, before the session. Call every frame
	// while in a session.
	void Frame(bool a_host);

	// The reports to send now: no more than SendPacer::WORKSHOP_PER_SECOND a second, the rest waiting their turn
	// in order. a_sending: false while they can't be (not in a session): all of them are dropped, and a piece of
	// the world they reported scrapped is reported again when the walk finds it.
	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending);

	// Another player built, moved or scrapped something: in line behind what came before, and placed a
	// few a frame (Frame).
	void Apply(const Protocol::WorkshopItem& a_item);

	// Whether the reference is a settlement's workbench (whatever its state): what a friend's build or scrap
	// must name as its workbench, and a container no friend's loot changes.
	[[nodiscard]] bool IsWorkbenchAnchor(const RE::TESObjectREFR* a_ref);

	// The workbench the reference is linked to as one of its settlement's workshop items (a piece built there), if any.
	[[nodiscard]] RE::TESObjectREFR* WorkbenchOf(const RE::TESObjectREFR& a_ref);

	// Dev: whether workshop mode lets the player scrap the reference (the game's own test).
	[[nodiscard]] bool WorkshopModeScraps(RE::TESObjectREFR& a_ref);

	// Dev: reports an existing reference as if the player had just placed (or scrapped) it.
	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op);

	// Removes every copy (leaving the session).
	void Reset();

	// A save was loaded: the copies made before it are let go (the session sends its building again),
	// and its cells are walked again for scrapped junk.
	void OnGameLoaded();

	// The loaded save is another character's world (the session is left): the copies were placed in the world before
	// the load, and their handles name nothing of ours now (or something else). Forgotten, not removed.
	void OnWorldGone();

	[[nodiscard]] std::string Describe();

	// Dev: what the reference is to the workshop: its base's type, whether it is one of our copies or a workbench,
	// its linked references, power links and wire (spline) data, and which extra data it carries.
	[[nodiscard]] std::string DescribeRef(RE::TESObjectREFR* a_ref);

	// Dev: the workbenches the walk found (or this one), with whether the player stands in each one's
	// build area; for any other reference, what the walk makes of it.
	[[nodiscard]] std::string DescribeBenches(RE::TESObjectREFR* a_ref = nullptr);

	// ---- Power (see WorkshopPower.h) -----------------------------------------------------------------
	//
	// Our copies of friends' pieces can't be picked in workshop mode (so no wire reaches one), and none is ever left on
	// one of our power grids: one found there is taken off (every few seconds, before every save, and before it is
	// deleted). Our own pieces' power is sent to the others, and their copies here show what their builder's game says.

	// Which of another player's pieces their game powers.
	void ApplyPower(const Protocol::PowerStates& a_power);

	// Someone joined: all our pieces' power goes to them.
	void OnPlayerJoined();

	// Someone left: their copies keep what they show, and are not shown again until their game says so (main thread).
	void OnPlayerLeft(std::uint32_t a_playerId);

	// A save is about to be written: no grid of ours may name a copy (main thread).
	void BeforeSave();

	// Whether the reference carries the keyword every workshop copy is given (the game's WorkshopBlockInteractionKeyword,
	// since 0.9.0, saved with the copy): a power armor frame made at runtime that has it is a copy an older version's
	// workshop sync made of a frame (FrameShare::Ghost), never a suit of the player's own.
	[[nodiscard]] bool IsCopyMarked(const RE::TESObjectREFR& a_ref);

	// Whether the reference holds anything in our game.
	[[nodiscard]] bool HoldsAnything(RE::TESObjectREFR* a_ref);

	// Dev: whether copies are kept out of workshop mode's reach (off: to try the repair), and the audit run now.
	void                      SetBlockCopies(bool a_block);
	[[nodiscard]] std::string AuditNow();
	// Dev: the reference shown powered (or not) as a friend's copy is; false if the game didn't take it.
	bool                      ShowPoweredForDev(RE::TESObjectREFR* a_ref, bool a_on);
	[[nodiscard]] std::string DescribePower();
	// Dev: each copy here, and whose piece it is: "copy=builder:theirRef".
	[[nodiscard]] std::string DescribeCopies();
	// Dev: the power grids of this workbench (or of every one seen), with any copy or deleted reference they name.
	[[nodiscard]] std::string DescribeGrids(RE::TESObjectREFR* a_bench = nullptr);
}
