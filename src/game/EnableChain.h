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

	// How many references a chain's parents may switch between them (ExtraEnableStateChildren) for the top to be switched
	// on: Gun Run's marker switches 48 in Fallout4.esm. One that switches a whole scene or settlement is left alone.
	constexpr std::uint32_t MAX_CHILDREN = 64;

	// BGSSaveLoadGame's change flags for a form (what our save records as changed on it): bit 0, CHANGE_FORM_FLAGS. It
	// says only that some flag of the form was set since the game started, not that its enable state changed: the
	// engine records it for a reference it merely loads (TESObjectREFR::Load, +4F2760, sets an in-game form flag
	// through +313560, which records CHANGE_FORM_FLAGS whether or not anything changed), and for a change of any of seven
	// form flags. Saves bear it out: untouched persistent references whose only change is this bit, their flags as their
	// plugin has them. Salem's marker had it in a fresh game at Gun Run's stage 5, before any script touches it (stage 30
	// enables it), so every switch-on through it was refused.
	constexpr std::uint32_t CHANGE_FORM_FLAGS = 0x1;
	constexpr std::uint32_t CHANGES_UNKNOWN = 0xFFFFFFFF;  // the game's save bookkeeping wasn't there to ask

	// One link up a reference's chain, nearest first: its enable parent as the engine finds it here.
	//
	// The friend's NPC running proves only that the friend's chain is on, not that ours is off because our game is behind.
	// Ours may be off on purpose: a plugin only our game has hides a vanilla NPC by giving it an enable parent that stays
	// off (sessions may have different mod lists), or our story is ahead and a quest of ours switched the scene's marker
	// off again. So a parent is switched on only if the link and the parent are the game's own (official: Fallout4.esm and
	// its add-ons, no plugin overriding either), no script of the game's own ever turns it off (storyTurnsOff), and the
	// top is off as its plugin has it (AsItsPluginLeftIt).
	enum class PluginState : std::uint8_t
	{
		kUnknown,  // not loaded with the game's data (a temporary reference, loaded with its cell): its plugin's state isn't known
		kOff,      // off when the game's data was loaded, before any save: its plugin has it off
		kOn,       // on then: its plugin has it on
	};

	struct Link
	{
		std::uint32_t parent = 0;                      // the parent's form id; 0: the engine has none it can find here (or it is deleted)
		bool          opposite = false;                // OPPOSITE: switching the parent on would switch the child off
		bool          parentOff = false;               // the parent is switched off here
		bool          official = false;                // the child's record that names the parent, and the parent's, are the game's own files'
		bool          storyTurnsOff = false;           // a script of the game's own turns the parent off somewhere (DisabledParents.h)
		PluginState   plugin = PluginState::kUnknown;  // the parent's state as its plugin has it
		std::uint32_t changeFlags = CHANGES_UNKNOWN;   // what our save records as changed on the parent (CHANGE_FORM_FLAGS ...)
		bool          parentDead = false;              // the parent is an actor, dead here: it would come back as a corpse with its children
		std::uint32_t children = 0;                    // the references the parent switches here (ExtraEnableStateChildren)
	};

	// Whether a parent that is off here is off as its plugin left it, not because our game switched it off: its plugin
	// has it off (the state it had when the game's data was loaded), or, where that isn't known (a reference not loaded
	// with the game's data: a temporary one, loaded with its cell), our save records no change of its flags at all.
	// The change flags alone can't tell (CHANGE_FORM_FLAGS): references the engine only loaded carry them. One its plugin
	// has on is refused whatever its change flags say (a disable made while the engine records no changes is one too):
	// off here and on in its plugin, a quest of ours or a script switched it off. One its plugin has off that our story
	// switched on and off again is not told apart here (nothing in the game records which way a reference was switched
	// before): the game's own scripts that ever turn a parent off are listed instead (storyTurnsOff).
	constexpr bool AsItsPluginLeftIt(const Link& a_link)
	{
		switch (a_link.plugin) {
		case PluginState::kOff:
			return true;
		case PluginState::kOn:
			return false;
		case PluginState::kUnknown:
			break;
		}
		return (a_link.changeFlags & CHANGE_FORM_FLAGS) == 0;
	}

	enum class Verdict
	{
		kSelf,         // no enable parent: the reference itself is switched on
		kParent,       // the parent at the top of the chain is switched on, and with it the reference
		kParentOn,     // the top of the chain is on here already: something else keeps it off; nothing done
		kOpposite,     // a link is the opposite way: its parent would have to go off; nothing done
		kNoParent,     // the engine names a parent it cannot find here; nothing done
		kTooDeep,      // a chain longer than MAX_DEPTH; nothing done
		kOwnStory,     // everyone on their own story: a parent may switch on characters of ours too; nothing done
		kUnofficial,   // a link or a parent from a plugin outside the game's own files, or overridden by one; nothing done
		kChanged,      // the top is off here but on in its plugin (or that isn't known and our save changed its flags): taken as our game having switched it off; nothing done
		kParentDead,   // a parent is a dead actor; nothing done
		kTooMany,      // the chain's parents switch more than MAX_CHILDREN references; nothing done
		kStoryTurnsOff,  // a script of the game's own turns a parent off again somewhere (a quest's later stage): left to our own story; nothing done
	};

	struct Plan
	{
		Verdict       verdict = Verdict::kSelf;
		std::uint32_t target = 0;    // what is switched on (kSelf, kParent), else the link that stopped it, or 0
		std::uint32_t children = 0;  // the references the chain's parents switch here (when the walk got to the top)
		bool          atTop = false;                   // the walk got to the top: the two below are its, for the log
		std::uint32_t changeFlags = CHANGES_UNKNOWN;   // the top's change flags in our save
		PluginState   plugin = PluginState::kUnknown;  // the top's state as its plugin has it
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
		std::uint32_t children = 0;
		for (const auto& link : a_links) {
			if (link.parent == 0) {
				return { Verdict::kNoParent, 0 };
			}
			if (link.opposite) {
				return { Verdict::kOpposite, link.parent };
			}
			if (link.parentDead) {
				return { Verdict::kParentDead, link.parent };
			}
			if (!link.official) {
				return { Verdict::kUnofficial, link.parent };
			}
			// A marker its plugin has off that a quest turns on at one stage and off at a later one (the airport's
			// Brotherhood, BoSAirportGuards): off here may be our story past it, the friend's game only behind.
			if (link.storyTurnsOff) {
				return { Verdict::kStoryTurnsOff, link.parent };
			}
			children += link.children;
		}
		// Only the top is read for on or off: the links below it are on or off as it is.
		const auto& top = a_links.back();
		const auto  at = [&](Verdict a_verdict) { return Plan{ a_verdict, top.parent, children, true, top.changeFlags, top.plugin }; };
		if (!top.parentOff) {
			return at(Verdict::kParentOn);
		}
		if (!AsItsPluginLeftIt(top)) {
			return at(Verdict::kChanged);
		}
		if (children > MAX_CHILDREN) {
			return at(Verdict::kTooMany);
		}
		return at(Verdict::kParent);
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
		case Verdict::kUnofficial:
			return "nothing: its enable parent, or the link to it, is a plugin's, not the game's own";
		case Verdict::kChanged:
			return "nothing: our game seems to have switched its enable parent off (on in its plugin, or its plugin's state not known and our save changed its flags)";
		case Verdict::kParentDead:
			return "nothing: its enable parent is dead here";
		case Verdict::kTooMany:
			return "nothing: its enable parent switches too many references";
		case Verdict::kStoryTurnsOff:
			return "nothing: a quest of the game turns its enable parent off again later; left to our own story";
		}
		return "?";
	}
}
