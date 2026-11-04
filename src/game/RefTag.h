#pragma once

#include <cstdint>
#include <format>
#include <string>

// How a reference of ours is named in the log around a save: its form ID, the raw address of the object, and whether its 3D is
// loaded. A use-after-free in the engine's own task queue (the VM crashed 2.5 s after a save, on a freed reference whose 3D was
// set from the loader thread) names the object only by its address, in a register of the crash log: with the address written
// beside the form ID where we placed and deleted it, the crash log says which reference it was.
namespace RefTag
{
	inline std::string Format(std::uint32_t a_id, std::uintptr_t a_address, bool a_has3D)
	{
		return std::format("{:08X} @{:X} 3d={}", a_id, a_address, a_has3D ? "yes" : "no");
	}

	// a_ref is a TESObjectREFR (a template so that this header needs no engine header).
	template <class Ref>
	std::string Of(const Ref* a_ref)
	{
		return a_ref ? Format(a_ref->GetFormID(), reinterpret_cast<std::uintptr_t>(a_ref), a_ref->Get3D() != nullptr) : std::string("none");
	}
}
