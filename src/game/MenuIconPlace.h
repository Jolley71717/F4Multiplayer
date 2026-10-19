#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// The icon over a friend's stand-in while they are in the Pip-Boy or another menu: which one, and where on the HUD it
// goes. Kept free of the game so it can be tested; MenuIcons draws it.
namespace MenuIconPlace
{
	enum class Glyph : std::uint8_t
	{
		kNone,
		kPipboy,  // the Pip-Boy
		kPause,   // two bars: paused (the pause menu, the console, sleeping or waiting)
		kBusy,    // a bubble with three dots: any other menu (a container, a terminal, a workbench, a conversation)
	};

	constexpr Glyph GlyphOf(std::uint8_t a_menu)
	{
		switch (a_menu) {
		case Protocol::MenuKind::kNone:
			return Glyph::kNone;
		case Protocol::MenuKind::kPipboy:
			return Glyph::kPipboy;
		case Protocol::MenuKind::kPaused:
			return Glyph::kPause;
		default:
			return a_menu <= Protocol::MenuKind::kMax ? Glyph::kBusy : Glyph::kNone;
		}
	}

	// The engine's projection of a point onto the screen (HUDMenuUtils::WorldPtToScreenPt3, the one the HUD's own markers
	// use): x from the left edge and y from the bottom edge, 0 to 1 across the screen; z from 0 at the near plane to 1 at
	// the far one, outside that when the point is behind the camera.
	struct Screen
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	// The part of the HUD's stage that is on the screen (Movie::GetVisibleFrameRect), in its own units.
	struct Stage
	{
		float left = 0.0f;
		float top = 0.0f;
		float right = 1280.0f;
		float bottom = 720.0f;
	};

	// Shown no further away than this (game units; about 70 m), and at full size up to NEAR, smaller beyond down to
	// MIN_SCALE.
	inline constexpr float MAX_DISTANCE = 5000.0f;
	inline constexpr float NEAR = 600.0f;
	inline constexpr float MIN_SCALE = 0.45f;
	// The icon is drawn about this many stage units across at full size.
	inline constexpr float SIZE = 26.0f;
	// The spot over the head the icon hangs at (game units above the head node, or above the feet when no head is found).
	inline constexpr float OVER_HEAD = 30.0f;
	inline constexpr float OVER_FEET = 150.0f;

	struct Spot
	{
		bool  shown = false;
		float x = 0.0f;  // on the stage
		float y = 0.0f;
		float scale = 1.0f;
	};

	// Where the icon goes for a friend a_distance away whose spot over the head the engine projected to a_screen: off when
	// it is behind the camera, off the screen, too far, or not a number.
	inline Spot Place(const Screen& a_screen, float a_distance, const Stage& a_stage)
	{
		Spot spot;
		const bool finite = std::isfinite(a_screen.x) && std::isfinite(a_screen.y) && std::isfinite(a_screen.z) && std::isfinite(a_distance);
		if (!finite || a_distance < 0.0f || a_distance > MAX_DISTANCE || a_screen.z <= 0.0f || a_screen.z >= 1.0f || a_screen.x < 0.0f || a_screen.x > 1.0f ||
			a_screen.y < 0.0f || a_screen.y > 1.0f) {
			return spot;
		}
		spot.shown = true;
		spot.x = a_stage.left + a_screen.x * (a_stage.right - a_stage.left);
		spot.y = a_stage.top + (1.0f - a_screen.y) * (a_stage.bottom - a_stage.top);
		spot.scale = a_distance <= NEAR ? 1.0f : std::clamp(NEAR / a_distance, MIN_SCALE, 1.0f);
		return spot;
	}

	// A value written to the HUD again only when it moved this much (stage units) or its scale this much: a friend
	// standing still in their Pip-Boy, and us too, writes nothing frame after frame.
	inline constexpr float MOVE_STEP = 0.5f;
	inline constexpr float SCALE_STEP = 0.02f;

	inline bool Moved(const Spot& a_was, const Spot& a_now)
	{
		return std::abs(a_was.x - a_now.x) >= MOVE_STEP || std::abs(a_was.y - a_now.y) >= MOVE_STEP || std::abs(a_was.scale - a_now.scale) >= SCALE_STEP;
	}
}
