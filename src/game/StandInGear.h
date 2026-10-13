#pragma once

#include "game/ArmorFrames.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

// What a friend wears and holds, as their stand-in is dressed in it. Kept free of the game so it can be tested.
// Their game reports their gear (once a second, when it changed: a gun swapped, a hat taken off, the last grenade of a
// kind thrown). Every report used to strip the stand-in and dress it again from scratch, and have it put its gun away
// and draw it again: the friend's shots went silent for a second or more mid-fight over a hat or a grenade, and a game
// that sent the same list over and over had everyone else strip and redress its stand-in every frame.
// Now a stand-in is dressed afresh only when it needs it (new, in or out of its power armor frame, or the engine put
// the NPC's own outfit back on it); a change while it is dressed is put on over what it wears, at most once a
// CHANGE_GAP, and the gun is drawn again only when the weapons they hold changed.
namespace StandInGear
{
	using Clock = std::chrono::steady_clock;

	// Changes to a dressed stand-in are put on at most this often (their game reports once a second): reports closer
	// together are put on together.
	constexpr auto CHANGE_GAP = std::chrono::seconds(1);
	// The game sometimes puts the NPC's own outfit back on a stand-in (when its inventory is set up late, or its 3D
	// reloads). What a dressed stand-in wears is looked at this often, and it is dressed afresh when it is not what it
	// was dressed in, at most MAX_FIXES times in a row: one the engine keeps undressing is left as it is.
	constexpr auto CHECK_INTERVAL = std::chrono::seconds(3);
	constexpr int  MAX_FIXES = 3;

	struct Gear
	{
		std::vector<std::uint32_t> items;            // what they wear and hold (base forms), as last reported
		bool                       has = false;      // reported at all
		bool                       dressed = false;  // the stand-in was dressed afresh for what it wears now
		bool                       changed = false;  // they reported a change since: put on over what it wears
		std::vector<std::uint32_t> worn;             // what it was dressed in last
		Clock::time_point          wornAt{};         // when
		ArmorFrames::Dress         dress = ArmorFrames::Dress::kClothes;  // how the stand-in was dressed last frame
		Clock::time_point          checkAt{};        // when what it wears is looked at next
		int                        fixes = 0;        // dressed afresh in a row for wearing something else
	};

	// A report from their game. False when it is what they have on already, in whatever order: nothing to do.
	inline bool Report(Gear& a_gear, std::vector<std::uint32_t> a_items)
	{
		std::ranges::sort(a_items);
		a_items.erase(std::unique(a_items.begin(), a_items.end()), a_items.end());
		if (a_gear.has && a_gear.items == a_items) {
			return false;
		}
		a_gear.items = std::move(a_items);
		a_gear.has = true;
		// Back to what the stand-in has on (a change and its undoing within the gap) is nothing to put on.
		a_gear.changed = a_gear.dressed && a_gear.items != a_gear.worn;
		a_gear.fixes = 0;
		return true;
	}

	// The stand-in is to be dressed afresh: a new one, one that got into or out of its frame, or one the engine dressed
	// in its own outfit again.
	inline void Redress(Gear& a_gear)
	{
		a_gear.dressed = false;
	}

	// A new stand-in was placed for them: out of any frame, dressed afresh as soon as its 3D is in.
	inline void Placed(Gear& a_gear)
	{
		Redress(a_gear);
		a_gear.dress = ArmorFrames::Dress::kClothes;
		a_gear.fixes = 0;
	}

	enum class Step
	{
		kNone,
		kDress,   // dressed afresh (Equipment::Apply, or WearInArmor in its frame), its gun drawn again
		kChange,  // their change put on over what it wears (Equipment::Wear, or WearInArmor in its frame)
	};

	inline Step Next(const Gear& a_gear, Clock::time_point a_now)
	{
		if (!a_gear.has) {
			return Step::kNone;
		}
		if (!a_gear.dressed) {
			return Step::kDress;
		}
		return a_gear.changed && a_now - a_gear.wornAt >= CHANGE_GAP ? Step::kChange : Step::kNone;
	}

	// The weapons among a_items that are held in the hand (a_inHand: not a grenade or mine, which the owner's game counts
	// as worn while it has one), sorted.
	template <class InHand>
	std::vector<std::uint32_t> HandWeapons(const std::vector<std::uint32_t>& a_items, InHand&& a_inHand)
	{
		std::vector<std::uint32_t> out;
		for (const auto item : a_items) {
			if (a_inHand(item)) {
				out.push_back(item);
			}
		}
		std::ranges::sort(out);
		return out;
	}

	// The step done. Whether the stand-in puts its gun away and draws it again (Puppets::RedrawWeapon): always when
	// dressed afresh (its weapon was taken off it), and after a change only when the weapons in hand changed.
	template <class InHand>
	bool Done(Gear& a_gear, Step a_step, Clock::time_point a_now, InHand&& a_inHand)
	{
		const bool redraw = a_step == Step::kDress || HandWeapons(a_gear.worn, a_inHand) != HandWeapons(a_gear.items, a_inHand);
		a_gear.worn = a_gear.items;
		a_gear.wornAt = a_now;
		a_gear.dressed = true;
		a_gear.changed = false;
		return redraw;
	}

	// How their gear goes on the stand-in this frame. Each is given what they have on, as the stand-in is dressed
	// (ArmorFrames::Wears).
	enum class Put
	{
		kNone,
		kApply,        // emptied and dressed (Equipment::Apply)
		kWear,         // put on over what it wears (Equipment::Wear)
		kWearInArmor,  // in its frame: their pieces and weapons put on, its inventory left as it is (Equipment::WearInArmor)
	};

	struct Dressing
	{
		Put  put = Put::kNone;
		bool change = false;  // a change they reported, put on over what it wore
		bool redraw = false;  // then its gun is put away and drawn again (Puppets::RedrawWeapon)
		bool refit = false;   // the check found it in something else than it was dressed in: dressed afresh next frame
	};

	// One frame of dressing a stand-in, dressed as the engine has it (ArmorFrames::ChooseDress), with or without its 3D.
	// a_inHand: whether an item is a weapon held in the hand (see HandWeapons). a_wears(items): whether the stand-in wears
	// those items, dressed so; asked only when the check is due. RemotePlayers::Update does what this returns.
	// A new dress (into or out of its frame) is dressed afresh; waiting for the power armor skeleton nothing is put on or
	// looked at; without its 3D neither. A dressing due (afresh, or a change after the gap) comes before the check, and
	// the check looks for what it was dressed in, not for a change still waiting for the gap.
	template <class InHand, class Wears>
	Dressing Dress(Gear& a_gear, ArmorFrames::Dress a_dress, bool a_has3D, Clock::time_point a_now, InHand&& a_inHand, Wears&& a_wears)
	{
		Dressing out;
		if (a_dress != a_gear.dress) {
			a_gear.dress = a_dress;
			Redress(a_gear);
		}
		if (a_dress == ArmorFrames::Dress::kWait || !a_has3D) {
			return out;
		}
		if (const auto step = Next(a_gear, a_now); step != Step::kNone) {
			out.put = a_dress == ArmorFrames::Dress::kArmor ? Put::kWearInArmor : step == Step::kDress ? Put::kApply : Put::kWear;
			out.change = step == Step::kChange;
			out.redraw = Done(a_gear, step, a_now, a_inHand);
			a_gear.checkAt = a_now + CHECK_INTERVAL;
			return out;
		}
		if (a_gear.dressed && a_now >= a_gear.checkAt) {
			a_gear.checkAt = a_now + CHECK_INTERVAL;
			if (a_wears(a_gear.worn)) {
				a_gear.fixes = 0;
			} else if (a_gear.fixes < MAX_FIXES) {
				++a_gear.fixes;
				Redress(a_gear);
				out.refit = true;
			}
		}
		return out;
	}
}
