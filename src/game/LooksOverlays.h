#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// LooksMenu's overlays (tattoos, face and body paint: F4EE overlays) carried to friends' stand-ins. Kept free of the game
// so it can be tested; game/LooksMenu.cpp reads and sets them. LooksMenu keeps a character's overlays in its own store,
// by reference and sex, as entries of its Papyrus script Overlays (Overlays#Entry): a template from an overlay mod's json
// (F4SE\Plugins\F4EE\Overlays), the priority it is laid at (one overlay per priority), a tint with alpha and a placement on
// the skin's texture. The player's are read with Overlays.GetAll, alongside the body sliders (BodyMorphs::Reading), and a
// friend's are added to their stand-in with Overlays.Add, then laid onto its 3D with Overlays.Update once every add is
// answered, as the sliders are (BodyMorphs::StandIns).
namespace LooksOverlays
{
	// One entry as Overlays.GetAll answers it: the tint and alpha 0..1, the placement in texture space.
	struct Entry
	{
		std::string  templateId;
		std::int32_t priority = 0;
		float        tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // red, green, blue, alpha
		float        offset[2] = {};
		float        scale[2] = { 1.0f, 1.0f };
	};

	// Whether a template ID can travel: whole (a cut ID would be another template or none), and plain text.
	inline bool Sendable(const std::string& a_id)
	{
		return !a_id.empty() && a_id.size() <= Protocol::MAX_OVERLAY_TEMPLATE &&
		       std::ranges::none_of(a_id, [](const unsigned char c) { return c < 0x20 || c == 0x7F; });
	}

	// A tint channel as LooksMenu saves it: a byte.
	inline std::uint8_t ToByte(float a_value)
	{
		return static_cast<std::uint8_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f));
	}

	inline float FromByte(std::uint8_t a_value)
	{
		return static_cast<float>(a_value) / 255.0f;
	}

	// What a read found, made fit to send: sorted by priority so that two reads of the same character compare equal (a
	// face that differs goes out again and remakes every friend's stand-in), one overlay per priority (LooksMenu lays one
	// at each), none with a template that cannot travel or a placement that is not a number, the tint in bytes and the
	// placement in the range the wire carries; at most MAX_OVERLAYS.
	inline std::vector<Protocol::Overlay> Collect(const std::vector<Entry>& a_read)
	{
		std::vector<Protocol::Overlay> out;
		for (const auto& entry : a_read) {
			const auto finite = [](float a_v) { return std::isfinite(a_v); };
			if (!Sendable(entry.templateId) || !std::ranges::all_of(entry.offset, finite) || !std::ranges::all_of(entry.scale, finite)) {
				continue;
			}
			Protocol::Overlay o;
			o.templateId = entry.templateId;
			o.priority = entry.priority;
			for (std::size_t i = 0; i < 4; ++i) {
				o.tint[i] = std::isfinite(entry.tint[i]) ? ToByte(entry.tint[i]) : 0;
			}
			for (std::size_t i = 0; i < 2; ++i) {
				o.offset[i] = std::clamp(entry.offset[i], -Protocol::MAX_OVERLAY_UV, Protocol::MAX_OVERLAY_UV);
				o.scale[i] = std::clamp(entry.scale[i], -Protocol::MAX_OVERLAY_UV, Protocol::MAX_OVERLAY_UV);
			}
			out.push_back(std::move(o));
		}
		std::ranges::stable_sort(out, {}, &Protocol::Overlay::priority);
		const auto repeats = std::ranges::unique(out, {}, &Protocol::Overlay::priority);
		out.erase(repeats.begin(), repeats.end());
		if (out.size() > Protocol::MAX_OVERLAYS) {
			out.resize(Protocol::MAX_OVERLAYS);
		}
		return out;
	}

	// A friend's overlay as Overlays.Add takes it (the uid is LooksMenu's to give).
	inline Entry ToEntry(const Protocol::Overlay& a_overlay)
	{
		Entry entry;
		entry.templateId = a_overlay.templateId;
		entry.priority = a_overlay.priority;
		for (std::size_t i = 0; i < 4; ++i) {
			entry.tint[i] = FromByte(a_overlay.tint[i]);
		}
		for (std::size_t i = 0; i < 2; ++i) {
			entry.offset[i] = a_overlay.offset[i];
			entry.scale[i] = a_overlay.scale[i];
		}
		return entry;
	}
}
