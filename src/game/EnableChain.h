#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// What to switch on so that a reference switched off here comes on, when a friend's game has it on (NpcSync::SwitchOn:
// their copy of the NPC runs; WorldSync: it died there). A reference with an enable parent (ExtraEnableStateParent: a
// quest's enable marker, an ambush's) is on or off as its parent is, and the engine ignores an enable of the reference
// itself: the console's `enable` (and Papyrus' Enable) returns at once for one with a parent, logging "Enable is being
// called on reference ... even though it has enable state parent ... This is not valid behavior and will be ignored."
// So the parent at the top of its chain is switched on instead, as the friend's game did: Gun Run's (DN083_Barney)
// stage 30 enables MirelurkQuestEnabler (000C3D6F), the parent of Salem's mirelurks, their ambush spots and eggs, in
// the game of the player who got there; in the other game each stayed off, the console's enable did nothing, and the
// friend fought a mirelurk nobody else could see (a Nexus report on 0.10.1). Kept free of the game so it can be tested.
namespace EnableChain
{
	// ExtraEnableStateParent's flags (the byte at +0x18 of the extra, after BSExtraData): bit 0, the reference is on
	// while its parent is off (as the engine reads it at +280D74: movzx eax, byte [rax+18h]; and al, 1).
	constexpr std::uint8_t OPPOSITE = 0x01;

	// How far up a chain is followed (a parent's parent ...). Vanilla chains are one or two links long.
	constexpr std::size_t MAX_DEPTH = 8;

	// One link up a reference's chain, nearest first: its enable parent as the engine finds it here.
	struct Link
	{
		std::uint32_t parent = 0;         // the parent's form id; 0: the engine has none it can find here
		bool          opposite = false;   // OPPOSITE: switching the parent on would switch the child off
		bool          parentOff = false;  // the parent is switched off here
	};

	enum class Verdict
	{
		kSelf,         // no enable parent: the reference itself is switched on
		kParent,       // the parent at the top of the chain is switched on, and with it the reference
		kParentOn,     // the top of the chain is on here already: something else keeps it off; nothing done
		kOpposite,     // a link is the opposite way: its parent would have to go off; nothing done
		kNoParent,     // the engine names a parent it cannot find here; nothing done
		kTooDeep,      // a chain longer than MAX_DEPTH; nothing done
		kOwnStory,     // everyone on their own story: a parent may switch on characters of ours too; nothing done
	};

	struct Plan
	{
		Verdict       verdict = Verdict::kSelf;
		std::uint32_t target = 0;  // what is switched on (kSelf, kParent), else the link that stopped it, or 0
	};

	// a_self: the reference; a_links: its chain upward, nearest first, ending with a parent that has none of its own (or
	// at the link that stops the walk: one the opposite way, or one whose parent cannot be found), at most MAX_DEPTH + 1
	// long (longer than MAX_DEPTH: too deep). With everyone on their own story (a_sharedStory false) a parent is never
	// switched on: one marker often switches a whole scene, characters of our own story among them, and the reference's
	// own story check (RemoteEdits::MaySwitchOn) does not see those.
	constexpr Plan Decide(std::uint32_t a_self, std::span<const Link> a_links, bool a_sharedStory)
	{
		if (a_links.empty()) {
			return { Verdict::kSelf, a_self };
		}
		if (!a_sharedStory) {
			return { Verdict::kOwnStory, a_links.front().parent };
		}
		if (a_links.size() > MAX_DEPTH) {
			return { Verdict::kTooDeep, 0 };
		}
		for (const auto& link : a_links) {
			if (link.parent == 0) {
				return { Verdict::kNoParent, 0 };
			}
			if (link.opposite) {
				return { Verdict::kOpposite, link.parent };
			}
		}
		const auto& top = a_links.back();
		return { top.parentOff ? Verdict::kParent : Verdict::kParentOn, top.parent };
	}

	// Whether the plan switches something on.
	constexpr bool SwitchesOn(const Plan& a_plan)
	{
		return a_plan.verdict == Verdict::kSelf || a_plan.verdict == Verdict::kParent;
	}

	// For the log.
	constexpr std::string_view VerdictName(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kSelf:
			return "itself";
		case Verdict::kParent:
			return "its enable parent";
		case Verdict::kParentOn:
			return "nothing: its enable parent is on here already";
		case Verdict::kOpposite:
			return "nothing: its enable parent would have to go off";
		case Verdict::kNoParent:
			return "nothing: its enable parent is not found here";
		case Verdict::kTooDeep:
			return "nothing: its chain of enable parents is too long";
		case Verdict::kOwnStory:
			return "nothing: an enable parent switches it, and everyone is on their own story";
		}
		return "?";
	}
}
