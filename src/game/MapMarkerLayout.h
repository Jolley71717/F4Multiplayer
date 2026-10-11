#pragma once

// The engine's MapMarkerData keeps its flags private; this mirror has the same layout (checked against the
// engine type's size) so the name is read through the real TESFullName base and the flags by field, not offset.
namespace MapMarkerLayout
{
	struct Mirror
	{
		void*           vtable;         // 00 (TESFullName is abstract, so its two fields are spelled out)
		RE::BSFixedString name;         // 08
		std::uint8_t    flags;          // 10
		std::uint8_t    originalFlags;  // 11
		RE::MARKER_TYPE type;           // 12
	};
	static_assert(sizeof(Mirror) == sizeof(RE::MapMarkerData));
	static_assert(offsetof(Mirror, name) == 0x08);
	static_assert(offsetof(Mirror, flags) == 0x10);

	inline const Mirror* Of(const RE::MapMarkerData* a_data)
	{
		return reinterpret_cast<const Mirror*>(a_data);
	}
}
