#pragma once

#include "game/MenuIconPlace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

// A badge over a friend's stand-in while they are in the Pip-Boy (bMenuBadges, off by default; MenuBadges hangs it): a
// model in the world, hung from the stand-in's 3D root over its head and turned to our camera, where the HUD icon
// (MenuIcons) could not be drawn. Kept free of the game so it can be tested.
//
// The model is the Pip-Boy form's own (00021B3B, its world model), loaded through the engine's model database, which
// keeps one node per file and hands that same node to every caller (WeaponFire's throwable does the same; a clone of
// one crashed, docs/GRENADE-IN-HAND.md). A node has one parent, so one badge shows at a time: over the nearest friend in
// their Pip-Boy. Other menu kinds keep only their name tag until there is a model for them.
namespace StandInBadge
{
	using MenuIconPlace::Glyph;

	// Hung within MAX_DISTANCE (about 70 m, as the HUD icon was), and once hung kept up to KEEP_MARGIN further, so a
	// friend at the edge does not blink in and out.
	inline constexpr float MAX_DISTANCE = MenuIconPlace::MAX_DISTANCE;
	inline constexpr float KEEP_MARGIN = 250.0f;

	// The glyphs there is a model for.
	constexpr bool HasModel(Glyph a_glyph)
	{
		return a_glyph == Glyph::kPipboy;
	}

	// A friend whose stand-in could wear the badge this frame.
	struct Candidate
	{
		std::uint32_t standIn = 0;  // its form ID
		Glyph         glyph = Glyph::kNone;
		float         distance = 0.0f;  // from our player
	};

	inline constexpr std::size_t NONE = static_cast<std::size_t>(-1);

	// Which of them wears it: the nearest in a menu there is a model for, within reach. The one wearing it now
	// (a_wearing) is within reach up to KEEP_MARGIN further, and keeps it against one just as near.
	inline std::size_t Pick(std::span<const Candidate> a_candidates, std::uint32_t a_wearing)
	{
		std::size_t best = NONE;
		for (std::size_t i = 0; i < a_candidates.size(); ++i) {
			const auto& candidate = a_candidates[i];
			const bool  wearing = a_wearing && candidate.standIn == a_wearing;
			const float reach = MAX_DISTANCE + (wearing ? KEEP_MARGIN : 0.0f);
			if (!candidate.standIn || !HasModel(candidate.glyph) || !std::isfinite(candidate.distance) || candidate.distance < 0.0f || candidate.distance > reach) {
				continue;
			}
			if (best == NONE || candidate.distance < a_candidates[best].distance || (wearing && candidate.distance == a_candidates[best].distance)) {
				best = i;
			}
		}
		return best;
	}

	// This frame.
	struct Facts
	{
		bool           on = false;     // bMenuBadges
		std::uint32_t  standIn = 0;    // the stand-in Pick chose, 0 for none
		std::uintptr_t root = 0;       // its 3D root now (an address, only compared), 0 while it has none
		bool           model = false;  // the model is loaded and free to hang
	};

	// Where it hangs.
	struct Track
	{
		bool           hung = false;
		std::uint32_t  standIn = 0;
		std::uintptr_t root = 0;
		std::uint32_t  hangs = 0;  // for the status line
		std::uint32_t  takeOffs = 0;
		std::uint32_t  rehangs = 0;
		std::uint32_t  failures = 0;
	};

	enum class Do
	{
		kNothing,  // as it is (hung or not)
		kHang,
		kTakeOff,
		kRehang,  // off the root it hangs from, onto the stand-in and root in Facts
	};

	// It was taken off (by Frame's kTakeOff, or before a stand-in is deleted, a save, a load or the session's end).
	constexpr void TookOff(Track& a_track)
	{
		if (!a_track.hung) {
			return;
		}
		a_track.hung = false;
		a_track.standIn = 0;
		a_track.root = 0;
		++a_track.takeOffs;
	}

	// What becomes of the badge this frame. Hung once, not each frame; taken off when nobody is to wear it (the menu
	// closed, out of reach, gone, turned off, no model); hung again when another stand-in is to wear it, or its 3D was
	// rebuilt (dressed again, in or out of a frame), as the new root is another node.
	constexpr Do Frame(Track& a_track, const Facts& a_facts)
	{
		const bool wanted = a_facts.on && a_facts.standIn && a_facts.root && a_facts.model;
		if (!wanted) {
			if (!a_track.hung) {
				return Do::kNothing;
			}
			TookOff(a_track);
			return Do::kTakeOff;
		}
		if (!a_track.hung) {
			a_track.hung = true;
			a_track.standIn = a_facts.standIn;
			a_track.root = a_facts.root;
			++a_track.hangs;
			return Do::kHang;
		}
		if (a_track.standIn != a_facts.standIn || a_track.root != a_facts.root) {
			a_track.standIn = a_facts.standIn;
			a_track.root = a_facts.root;
			++a_track.rehangs;
			return Do::kRehang;
		}
		return Do::kNothing;
	}

	// The engine did not take it (kHang or kRehang): nothing hangs.
	constexpr void Failed(Track& a_track)
	{
		a_track.hung = false;
		a_track.standIn = 0;
		a_track.root = 0;
		++a_track.failures;
	}

	// How high over the root it hangs, in the root's own units: MenuIconPlace::OVER_HEAD over the head node, or
	// OVER_FEET over the root when there is no head or it is somewhere no head is. Hung from the root, not the head, so
	// it stays level while the stand-in bends over its wrist.
	inline constexpr float MAX_HEAD = 400.0f;  // a head higher than this over the feet is not one (power armor is ~150)

	inline float Height(bool a_head, float a_headZ, float a_rootZ, float a_rootScale)
	{
		const float scale = std::isfinite(a_rootScale) && a_rootScale > 0.0f ? a_rootScale : 1.0f;
		const float head = a_headZ - a_rootZ;
		const bool  found = a_head && std::isfinite(head) && head > 0.0f && head <= MAX_HEAD * scale;
		return (found ? head + MenuIconPlace::OVER_HEAD : MenuIconPlace::OVER_FEET) / scale;
	}

	// A rotation as the engine keeps one (NiMatrix3's rows; a column vector is turned, a child's world rotation is its
	// parent's times its own).
	using Matrix = std::array<std::array<float, 3>, 3>;

	// The turn by a heading a_yaw (radians, clockwise from +Y, as the engine's headings): +Y goes to (sin, cos, 0).
	inline Matrix Turn(float a_yaw)
	{
		const float s = std::sin(a_yaw);
		const float c = std::cos(a_yaw);
		return { { { c, s, 0.0f }, { -s, c, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
	}

	// Its own turn (Turn's heading, in (-pi, pi]) under a root whose world rotation is a_root, so its front (the model's
	// +Y) looks at the camera, a_toCameraX/Y away from it. Read off the root itself rather than the actor's heading, so it
	// holds whatever turns the root. Straight under or over the camera, or for values that are not numbers, it faces
	// where its root does.
	inline float FacingYaw(const Matrix& a_root, float a_toCameraX, float a_toCameraY)
	{
		constexpr float pi = std::numbers::pi_v<float>;
		if (!std::isfinite(a_toCameraX) || !std::isfinite(a_toCameraY) || (std::abs(a_toCameraX) < 1.0f && std::abs(a_toCameraY) < 1.0f)) {
			return 0.0f;
		}
		// The way to the camera in the root's own axes (its rotation's transpose).
		const float x = a_root[0][0] * a_toCameraX + a_root[1][0] * a_toCameraY;
		const float y = a_root[0][1] * a_toCameraX + a_root[1][1] * a_toCameraY;
		if (!std::isfinite(x) || !std::isfinite(y) || (x == 0.0f && y == 0.0f)) {
			return 0.0f;
		}
		const float yaw = std::atan2(x, y);
		return yaw <= -pi ? pi : yaw;
	}

	// Its size: scaled so its bounding sphere is SIZE units across the radius whatever the model's own, within limits.
	// A radius not known yet (0, before the engine's first update of it) or not a number leaves the model's own size.
	inline constexpr float SIZE = 12.0f;
	inline constexpr float MIN_SCALE = 0.25f;
	inline constexpr float MAX_SCALE = 4.0f;

	inline float ScaleFor(float a_radius)
	{
		if (!std::isfinite(a_radius) || a_radius <= 0.0f) {
			return 1.0f;
		}
		return std::clamp(SIZE / a_radius, MIN_SCALE, MAX_SCALE);
	}

	// The model's own radius from what the engine last worked out (its world bound and world scale, ours included).
	inline float OwnRadius(float a_worldRadius, float a_worldScale)
	{
		if (!std::isfinite(a_worldRadius) || !std::isfinite(a_worldScale) || a_worldRadius <= 0.0f || a_worldScale <= 0.0f) {
			return 0.0f;
		}
		return a_worldRadius / a_worldScale;
	}

	// Its local scale under a root of a_rootScale (a stand-in's own scale).
	inline float LocalScale(float a_ownRadius, float a_rootScale)
	{
		const float root = std::isfinite(a_rootScale) && a_rootScale > 0.0f ? a_rootScale : 1.0f;
		return ScaleFor(a_ownRadius) / root;
	}
}
