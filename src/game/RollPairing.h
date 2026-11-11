#pragma once

#include "Protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Which NPC a roll held for the resolver is for (src/game/RollHook.cpp, docs/SHARED-ROLLS.md).
//
// RollHook decides a reference's roll in the resolver's call of IsLeveledCreatureResolved and holds it for its thread;
// the next CopyFromTemplateForms on that thread writes it into the NPC being built. Between the two the resolver does
// enough (the reference's 3D let go, a new NPC made and copied from the original, its leveled lists read) that another
// CopyFromTemplateForms on the same thread can't be ruled out from the engine's code. One that is another NPC's must not
// take the roll: that NPC would be built with this reference's gear.
//
// So a held roll names its owner, and only that owner takes it:
// - the call: the resolver's own array of templates, on its stack frame (found from the frame as the hook sees it), is
//   the array its CopyFromTemplateForms is given. 0 when the frame isn't known (its bytes not as 1.11.240 has them):
//   not checked then.
// - the NPC: what the resolver builds from (its default template and the 13 slots' templates, as the original NPC has
//   them) is what the new NPC holds once copied from the original, when CopyFromTemplateForms is called on it.
// Anything else leaves the roll held for its owner, and is built as the engine rolled it.
//
// Pure, so it can be tested without the game: tests/RollPairingTests.cpp.
namespace RollPairing
{
	// The default template (baseTemplateForm), then the 13 slots' (templateForms): their addresses.
	inline constexpr std::size_t SOURCES = 1 + Protocol::IDENTITY_TEMPLATES;
	using Sources = std::array<std::uintptr_t, SOURCES>;

	struct Owner
	{
		std::uintptr_t call = 0;  // the resolver's array of templates; 0 = not known
		Sources        sources{};
	};

	enum class Verdict : std::uint8_t
	{
		kNone,       // nothing held
		kOwner,      // its owner: the roll is taken
		kOtherCall,  // another array than the resolver's: another caller's CopyFromTemplateForms
		kOtherNpc,   // an NPC built from other templates than the reference's
	};

	[[nodiscard]] constexpr const char* Name(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kNone:
			return "nothing held";
		case Verdict::kOwner:
			return "its owner";
		case Verdict::kOtherCall:
			return "another call";
		case Verdict::kOtherNpc:
			return "another NPC";
		}
		return "?";
	}

	[[nodiscard]] constexpr Verdict Judge(const Owner& a_held, const Owner& a_seen)
	{
		if (a_held.call != 0 && a_seen.call != a_held.call) {
			return Verdict::kOtherCall;
		}
		if (a_seen.sources != a_held.sources) {
			return Verdict::kOtherNpc;
		}
		return Verdict::kOwner;
	}

	// The roll held on one thread. Constant-initialized when T is (a thread_local with no dynamic init).
	template <class T>
	class Held
	{
	public:
		struct Take
		{
			Verdict verdict = Verdict::kNone;
			T       value{};
		};

		void Arm(const Owner& a_owner, const T& a_value)
		{
			owner = a_owner;
			value = a_value;
			armed = true;
		}

		// Drops what is held (a new resolve on this thread); true when something was.
		bool Disarm()
		{
			const bool was = armed;
			armed = false;
			return was;
		}

		[[nodiscard]] bool Armed() const { return armed; }
		[[nodiscard]] const T& Value() const { return value; }

		// A CopyFromTemplateForms as a_seen: the roll if it is the owner's (it is no longer held then), else the verdict
		// alone, the roll still held for its owner.
		Take TakeFor(const Owner& a_seen)
		{
			if (!armed) {
				return {};
			}
			const auto verdict = Judge(owner, a_seen);
			if (verdict != Verdict::kOwner) {
				return { verdict, T{} };
			}
			armed = false;
			return { verdict, value };
		}

	private:
		Owner owner{};
		T     value{};
		bool  armed = false;
	};

	// The resolver's array of templates from the hook's return-address slot, as 1.11.240's resolver lays out its frame:
	// six pushes, rbp = rsp - 0x78, sub rsp, 0x178, the array at rbp - 0x20, no push before its call at +0xCB. So the
	// array is 0xE0 above rsp at that call, which is 8 above the slot the call pushed the return address into.
	inline constexpr std::uintptr_t FRAME_TO_TEMPLATES = 8 + 0xE0;

	[[nodiscard]] constexpr std::uintptr_t TemplatesOf(std::uintptr_t a_returnSlot)
	{
		return a_returnSlot + FRAME_TO_TEMPLATES;
	}
}
