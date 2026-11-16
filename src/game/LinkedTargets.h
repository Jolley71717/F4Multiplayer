#pragma once

#include <cstdint>

// The one way the plugin reads where a linked-ref entry (ExtraLinkedRef) points. The field the entry calls a pointer
// may hold the target's form ID instead (LinkedRefs.h says when), and reading it as a pointer crashed the host's game
// in a wsinfo scan near Sanctuary (2026-11-16) as it had at the Red Rocket terminal (0.7.1). Nothing here ever reads
// through the field unguarded: an ID is looked up, a pointer is believed only if it names itself (LinkedRefs::Resolve).
// Every reader of an entry's target goes through these; the engine's own GetLinkedRef is safe to call too.
namespace LinkedTargets
{
	// The reference the entry links to, if it is in memory; nullptr for none, a target not loaded, or a stale pointer.
	[[nodiscard]] RE::TESObjectREFR* Target(const RE::ExtraLinkedRef::LinkedRef& a_link);

	// The form ID the entry names, for logs and for comparing: the ID the engine holds there (whether or not its
	// reference is in memory), or the ID of the reference its pointer resolves to; 0 for none or a stale pointer.
	[[nodiscard]] std::uint32_t TargetId(const RE::ExtraLinkedRef::LinkedRef& a_link);

	// The first entry of a reference's linked refs under the keyword with this form ID, read as above (nullptr, 0: none).
	[[nodiscard]] RE::TESObjectREFR* Under(const RE::TESObjectREFR& a_ref, std::uint32_t a_keyword);
	[[nodiscard]] std::uint32_t      IdUnder(const RE::TESObjectREFR& a_ref, std::uint32_t a_keyword);
}
