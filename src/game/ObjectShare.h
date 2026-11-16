#pragma once

#include "Protocol.h"
#include "TokenBucket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Loose objects a player moves by hand or knocks about, shared (src/game/MovedObjects.cpp runs these in the game). Kept
// free of the game so the rules can be tested (tests/MovedObjectsTests.cpp):
// - HolderFrame: an object's pose in its holder's own frame, and back. A held object travels in its holder's frame, so
//   each game draws it in front of its own stand-in of the holder, however far behind or ahead of the holder's own
//   place that stand-in is drawn: the friend sees it in the stand-in's hands as they walk and turn, never trailing.
// - Carry: what our player holds, sent at HOLD_HZ while it moves; their let-go (with how it left their hands) and where
//   it came to rest once still.
// - Carried: what friends hold, drawn eased towards their newest hold (Smooth), stale holds dropped.
// - KnockWatch: objects near our player that something moved with no hand on them (our player walking into them, our
//   shots); where they came to rest, when our player moved them, on a budget.
// - Judge: what a friend's message may do in our world.
namespace ObjectShare
{
	using Clock = std::chrono::steady_clock;
	using Vec3 = std::array<float, 3>;

	inline constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;

	// An angle brought into [-pi, pi).
	inline float Wrap(float a_angle)
	{
		if (!std::isfinite(a_angle)) {
			return 0.0f;
		}
		a_angle = std::fmod(a_angle + std::numbers::pi_v<float>, TWO_PI);
		if (a_angle < 0.0f) {
			a_angle += TWO_PI;
		}
		return a_angle - std::numbers::pi_v<float>;
	}

	inline bool Finite(const Vec3& a_v)
	{
		return std::ranges::all_of(a_v, [](float a_x) { return std::isfinite(a_x); });
	}

	inline float Distance(const Vec3& a_a, const Vec3& a_b)
	{
		const float dx = a_a[0] - a_b[0], dy = a_a[1] - a_b[1], dz = a_a[2] - a_b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	// The largest of the three angles' differences, the short way round.
	inline float Turned(const Vec3& a_a, const Vec3& a_b)
	{
		float most = 0.0f;
		for (std::size_t i = 0; i < 3; ++i) {
			most = (std::max)(most, std::fabs(Wrap(a_a[i] - a_b[i])));
		}
		return most;
	}

	// Where an object is: its space (an interior cell, or a worldspace), place and angles (the reference's, radians).
	struct Pose
	{
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		Vec3          position{};
		Vec3          angles{};

		bool operator==(const Pose&) const = default;
	};

	// Where a player is: their feet and heading (radians; 0 faces +y, a quarter turn faces +x, as the game has it).
	struct Holder
	{
		Vec3  position{};
		float heading = 0.0f;
	};

	// --- rotations ------------------------------------------------------------------------------------------------
	//
	// A reference's angles (x, y, z) are its 3D's rotation R = Rz(z) Ry(y) Rx(x), the rows of R being the object's own
	// axes in the world (row 1 is where it faces: a heading h faces (sin h, cos h)). So the object turned with its holder
	// by a heading d is R Rz(d): measured in the game, carrying a tilted tire round (its x and y angles change as it
	// turns, and R's third column stays put). The game's NiMatrix3::ToEulerAnglesXYZ gives these angles back.

	using Mat3 = std::array<Vec3, 3>;  // rows

	inline Mat3 Multiply(const Mat3& a_a, const Mat3& a_b)
	{
		Mat3 out{};
		for (std::size_t r = 0; r < 3; ++r) {
			for (std::size_t c = 0; c < 3; ++c) {
				out[r][c] = a_a[r][0] * a_b[0][c] + a_a[r][1] * a_b[1][c] + a_a[r][2] * a_b[2][c];
			}
		}
		return out;
	}

	// Rz(a_heading): a player's own rotation at that heading.
	inline Mat3 Yaw(float a_heading)
	{
		const float c = std::cos(a_heading), s = std::sin(a_heading);
		return { Vec3{ c, -s, 0.0f }, Vec3{ s, c, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f } };
	}

	// R = Rz(z) Ry(y) Rx(x).
	inline Mat3 FromAngles(const Vec3& a_angles)
	{
		const float cx = std::cos(a_angles[0]), sx = std::sin(a_angles[0]);
		const float cy = std::cos(a_angles[1]), sy = std::sin(a_angles[1]);
		const Mat3  x{ Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, cx, -sx }, Vec3{ 0.0f, sx, cx } };
		const Mat3  y{ Vec3{ cy, 0.0f, sy }, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ -sy, 0.0f, cy } };
		return Multiply(Multiply(Yaw(a_angles[2]), y), x);
	}

	// The angles of a rotation (FromAngles undone), each in [-pi, pi]; y in [-pi/2, pi/2].
	inline Vec3 ToAngles(const Mat3& a_m)
	{
		const float sy = std::clamp(-a_m[2][0], -1.0f, 1.0f);
		const float y = std::asin(sy);
		if (std::fabs(sy) < 0.99999f) {
			return { std::atan2(a_m[2][1], a_m[2][2]), y, std::atan2(a_m[1][0], a_m[0][0]) };
		}
		return { 0.0f, y, std::atan2(-a_m[0][1], a_m[1][1]) };  // pitched straight up or down: x folded into z
	}

	// --- the holder's frame ---------------------------------------------------------------------------------------

	struct Relative
	{
		Vec3 offset{};    // to the holder's right, ahead, up
		Vec3 rotation{};  // the object's angles in the holder's frame: its rotation turned back by their heading

		bool operator==(const Relative&) const = default;
	};

	// An object at a_position with a_angles, in a_holder's frame.
	inline Relative ToHolderFrame(const Vec3& a_position, const Vec3& a_angles, const Holder& a_holder)
	{
		const float c = std::cos(a_holder.heading), s = std::sin(a_holder.heading);
		const float dx = a_position[0] - a_holder.position[0], dy = a_position[1] - a_holder.position[1];
		return {
			.offset = { dx * c - dy * s, dx * s + dy * c, a_position[2] - a_holder.position[2] },
			.rotation = ToAngles(Multiply(FromAngles(a_angles), Yaw(-a_holder.heading))),
		};
	}

	// The world place and angles of an object held at a_relative by a holder at a_holder.
	inline std::pair<Vec3, Vec3> FromHolderFrame(const Relative& a_relative, const Holder& a_holder)
	{
		const float c = std::cos(a_holder.heading), s = std::sin(a_holder.heading);
		const auto& o = a_relative.offset;
		return {
			Vec3{ a_holder.position[0] + o[0] * c + o[1] * s, a_holder.position[1] - o[0] * s + o[1] * c, a_holder.position[2] + o[2] },
			ToAngles(Multiply(FromAngles(a_relative.rotation), Yaw(a_holder.heading))),
		};
	}

	// --- what our player holds --------------------------------------------------------------------------------------

	inline constexpr auto  HOLD_INTERVAL = std::chrono::milliseconds(66);   // 15 a second while it moves in their hands
	inline constexpr auto  HOLD_KEEPALIVE = std::chrono::milliseconds(500);  // and twice a second while it doesn't
	inline constexpr float HOLD_MOVED = 0.5f;                                // units, in the holder's frame
	inline constexpr float HOLD_TURNED = 0.01f;                              // radians
	inline constexpr auto  LAUNCH_WINDOW = std::chrono::milliseconds(80);    // its flight's start, after the let-go
	inline constexpr auto  SETTLE_TIME = std::chrono::milliseconds(600);     // still that long: come to rest
	inline constexpr float STILL_DISTANCE = 1.0f;                            // units between two looks
	inline constexpr float STILL_TURN = 0.02f;                               // radians between two looks
	inline constexpr auto  REST_GIVE_UP = std::chrono::seconds(10);          // then where it is is where it rests
	inline constexpr std::size_t MAX_SETTLING = 8;                           // objects let go of, still moving

	// What our player holds and has let go of. Once a frame: what they hold now (0: nothing) and where they are; for each
	// object it watches (Watched), where it is (none: gone from our world). Out come holds, let-gos and rests to send.
	class Carry
	{
	public:
		using PoseOf = std::function<std::optional<Pose>(std::uint32_t)>;

		// The objects whose place it needs this frame: the one held and those let go of, still moving.
		[[nodiscard]] std::vector<std::uint32_t> Watched() const
		{
			std::vector<std::uint32_t> out;
			if (held) {
				out.push_back(held->refId);
			}
			for (const auto& flying : settling) {
				out.push_back(flying.refId);
			}
			return out;
		}

		void Frame(std::uint32_t a_held, const Holder& a_holder, const PoseOf& a_poseOf, Clock::time_point a_now)
		{
			if (held && held->refId != a_held) {
				LetGo(a_poseOf, a_now);
			}
			if (refused && refused != a_held) {
				refused = 0;  // they let go of what a friend holds: nothing of theirs to say
			}
			if (a_held && a_held != refused) {
				if (!held) {
					std::erase_if(settling, [&](const Settling& a_s) { return a_s.refId == a_held; });
					held = Held{ .refId = a_held };
				}
				if (const auto pose = a_poseOf(a_held)) {
					Track(*held, *pose, a_holder, a_now);
				}
			}
			Settle(a_poseOf, a_now);
		}

		// The server says a friend holds what our player holds (they grabbed it first): nothing more of it is sent until
		// our player lets go of it, and that let-go says nothing.
		void Refused(std::uint32_t a_ref)
		{
			if (held && held->refId == a_ref) {
				held.reset();
				refused = a_ref;
			}
		}

		// A friend's move of the object: our own let-go of it (if any) is over; their game says where it rests.
		void TakenOver(std::uint32_t a_ref)
		{
			std::erase_if(settling, [&](const Settling& a_s) { return a_s.refId == a_ref; });
		}

		[[nodiscard]] std::uint32_t Holding() const { return held ? held->refId : 0; }
		[[nodiscard]] bool          IsRefused(std::uint32_t a_ref) const { return a_ref != 0 && refused == a_ref; }
		[[nodiscard]] bool          IsSettling(std::uint32_t a_ref) const
		{
			return std::ranges::any_of(settling, [&](const auto& a_s) { return a_s.refId == a_ref; });
		}

		std::vector<Protocol::ObjectHold> TakeHolds() { return std::exchange(holds, {}); }
		std::vector<Protocol::ObjectRest> TakeRests() { return std::exchange(rests, {}); }

		void Reset()
		{
			held.reset();
			settling.clear();
			refused = 0;
			holds.clear();
			rests.clear();
		}

	private:
		struct Held
		{
			std::uint32_t           refId = 0;
			std::optional<Relative> sent;
			Clock::time_point       sentAt{};
			Pose                    last{};  // its place in the world, as last seen
			bool                    seen = false;
		};

		struct Settling
		{
			std::uint32_t     refId = 0;
			std::uint32_t     sequence = 0;
			Clock::time_point letGoAt{};
			bool              launched = false;  // its let-go went
			Pose              first{};           // its first place after the let-go
			Clock::time_point firstAt{};
			Pose              last{};
			Clock::time_point lastAt{};
			Clock::time_point stillSince{};
			bool              seen = false;
		};

		void Track(Held& a_held, const Pose& a_pose, const Holder& a_holder, Clock::time_point a_now)
		{
			if (!Finite(a_pose.position) || !Finite(a_pose.angles) || !Finite(a_holder.position) || !std::isfinite(a_holder.heading)) {
				return;
			}
			a_held.last = a_pose;
			a_held.seen = true;
			const auto relative = ToHolderFrame(a_pose.position, a_pose.angles, a_holder);
			if (!Protocol::WithinLength(relative.offset, Protocol::MAX_HOLD_OFFSET)) {
				return;  // not in their hands (a frame where the game moved them and not it yet)
			}
			if (a_held.sent) {
				const auto since = a_now - a_held.sentAt;
				if (since < HOLD_INTERVAL) {
					return;
				}
				const bool still = Distance(relative.offset, a_held.sent->offset) < HOLD_MOVED && Turned(relative.rotation, a_held.sent->rotation) < HOLD_TURNED;
				if (still && since < HOLD_KEEPALIVE) {
					return;
				}
			}
			a_held.sent = relative;
			a_held.sentAt = a_now;
			holds.push_back({ .playerId = 0, .refId = a_held.refId, .sequence = ++sequence, .offset = relative.offset, .rotation = relative.rotation });
		}

		void LetGo(const PoseOf& a_poseOf, Clock::time_point a_now)
		{
			const auto was = *std::exchange(held, std::nullopt);
			if (!was.sent) {
				return;  // nothing of it went: nobody saw it held
			}
			if (settling.size() >= MAX_SETTLING) {
				settling.erase(settling.begin());
			}
			Settling s{ .refId = was.refId, .sequence = sequence, .letGoAt = a_now };
			if (const auto pose = a_poseOf(was.refId); pose && Finite(pose->position) && Finite(pose->angles)) {
				s.first = s.last = *pose;
				s.firstAt = s.lastAt = s.stillSince = a_now;
				s.seen = true;
			} else if (was.seen) {
				s.first = s.last = was.last;
				s.firstAt = s.lastAt = s.stillSince = a_now;
				s.seen = true;
			}
			settling.push_back(s);
		}

		static Protocol::ObjectRest Message(const Settling& a_s, const Pose& a_pose, std::uint8_t a_kind)
		{
			Protocol::ObjectRest rest{ .refId = a_s.refId, .kind = a_kind, .sequence = a_s.sequence, .cell = a_pose.cell, .worldspace = a_pose.worldspace,
				.position = a_pose.position };
			for (std::size_t i = 0; i < 3; ++i) {
				rest.rotation[i] = Wrap(a_pose.angles[i]);
			}
			return rest;
		}

		void Settle(const PoseOf& a_poseOf, Clock::time_point a_now)
		{
			for (auto it = settling.begin(); it != settling.end();) {
				auto&      s = *it;
				const auto pose = a_poseOf(s.refId);
				if (!pose || !Finite(pose->position) || !Finite(pose->angles)) {
					if (s.launched || !s.seen) {
						it = settling.erase(it);  // gone from our world (picked up, its cell unloaded)
						continue;
					}
				} else if (!s.seen) {
					s.first = s.last = *pose;
					s.firstAt = s.lastAt = s.stillSince = a_now;
					s.seen = true;
				} else {
					if (Distance(pose->position, s.last.position) > STILL_DISTANCE || Turned(pose->angles, s.last.angles) > STILL_TURN) {
						s.stillSince = a_now;
					}
					s.last = *pose;
					s.lastAt = a_now;
				}
				if (!s.launched && s.seen && (a_now - s.letGoAt >= LAUNCH_WINDOW || !pose)) {
					auto       letGo = Message(s, s.first, Protocol::ObjectRestKind::kLetGo);
					const auto dt = std::chrono::duration<float>(s.lastAt - s.firstAt).count();
					if (dt >= 0.01f) {
						for (std::size_t i = 0; i < 3; ++i) {
							letGo.velocity[i] = (s.last.position[i] - s.first.position[i]) / dt;
						}
					}
					if (!Protocol::WithinLength(letGo.velocity, Protocol::MAX_OBJECT_SPEED)) {
						letGo.velocity = {};
					}
					rests.push_back(letGo);
					s.launched = true;
				}
				if (s.launched && (a_now - s.stillSince >= SETTLE_TIME || a_now - s.letGoAt >= REST_GIVE_UP)) {
					rests.push_back(Message(s, s.last, Protocol::ObjectRestKind::kRest));
					it = settling.erase(it);
					continue;
				}
				++it;
			}
		}

		std::optional<Held>                held;
		std::vector<Settling>              settling;
		std::uint32_t                      refused = 0;
		std::uint32_t                      sequence = 0;
		std::vector<Protocol::ObjectHold> holds;
		std::vector<Protocol::ObjectRest> rests;
	};

	// --- what friends hold ------------------------------------------------------------------------------------------

	inline constexpr auto  HELD_TIMEOUT = std::chrono::milliseconds(1500);  // no hold for that long: they let go somehow
	inline constexpr float SMOOTH_SECONDS = 0.06f;                          // the drawn pose's time constant
	inline constexpr auto  LETGO_MEMORY = std::chrono::seconds(5);          // late holds of a let-go are dropped that long

	// a_from eased towards a_to over a_seconds (angles the short way round), in time constant SMOOTH_SECONDS.
	inline Relative Smooth(const Relative& a_from, const Relative& a_to, float a_seconds)
	{
		if (!(a_seconds > 0.0f)) {
			return a_from;
		}
		const float k = 1.0f - std::exp(-a_seconds / SMOOTH_SECONDS);
		Relative    out;
		for (std::size_t i = 0; i < 3; ++i) {
			out.offset[i] = a_from.offset[i] + (a_to.offset[i] - a_from.offset[i]) * k;
			out.rotation[i] = Wrap(a_from.rotation[i] + Wrap(a_to.rotation[i] - a_from.rotation[i]) * k);
		}
		return out;
	}

	class Carried
	{
	public:
		struct Entry
		{
			std::uint32_t     holder = 0;
			std::uint32_t     sequence = 0;
			Relative          target{};
			Relative          drawn{};
			Clock::time_point heardAt{};
			bool              placed = false;  // drawn in its holder's hands at least once (MovedObjects took it over)
		};

		// A friend's hold. False when it is older than what we have of them (or than their let-go), or another friend
		// holds the object (the server lets only one: an old holder's last holds can still be on the way).
		bool OnHold(const Protocol::ObjectHold& a_hold, Clock::time_point a_now)
		{
			Prune(a_now);
			if (const auto gone = letGo.find(a_hold.refId); gone != letGo.end() && gone->second.holder == a_hold.playerId && !Newer(a_hold.sequence, gone->second.sequence)) {
				return false;
			}
			const Relative relative{ a_hold.offset, a_hold.rotation };
			auto           it = held.find(a_hold.refId);
			if (it != held.end()) {
				auto& e = it->second;
				if (e.holder == a_hold.playerId && !Newer(a_hold.sequence, e.sequence)) {
					return false;
				}
				// Another friend's: they took it after the holder's let-go, which has not come yet (the server passes an
				// object on only once its holder let go of it). Theirs now.
				e.holder = a_hold.playerId;
				e.sequence = a_hold.sequence;
				e.target = relative;
				e.heardAt = a_now;
				return true;
			}
			held.emplace(a_hold.refId, Entry{ .holder = a_hold.playerId, .sequence = a_hold.sequence, .target = relative, .drawn = relative, .heardAt = a_now });
			return true;
		}

		// A friend let go of it (or it came to rest after they moved it): no longer in anyone's hands. Their holds up to
		// the let-go's sequence that come after it are dropped. Whether it was in their hands here.
		bool OnLetGo(const Protocol::ObjectRest& a_rest, Clock::time_point a_now)
		{
			if (a_rest.kind == Protocol::ObjectRestKind::kLetGo) {
				letGo[a_rest.refId] = { a_rest.playerId, a_rest.sequence, a_now };
			}
			const auto it = held.find(a_rest.refId);
			if (it == held.end() || it->second.holder != a_rest.playerId) {
				return false;
			}
			held.erase(it);
			return true;
		}

		// The pose to draw a held object at this frame, a_seconds after the last: eased towards its newest hold.
		std::optional<Relative> Step(std::uint32_t a_ref, float a_seconds)
		{
			const auto it = held.find(a_ref);
			if (it == held.end()) {
				return std::nullopt;
			}
			it->second.drawn = Smooth(it->second.drawn, it->second.target, (std::min)(a_seconds, 0.25f));
			return it->second.drawn;
		}

		// Held objects not heard of for HELD_TIMEOUT (their game paused, gone, or their let-go lost): no longer held.
		std::vector<std::uint32_t> TakeStale(Clock::time_point a_now)
		{
			std::vector<std::uint32_t> out;
			for (auto it = held.begin(); it != held.end();) {
				if (a_now - it->second.heardAt >= HELD_TIMEOUT) {
					out.push_back(it->first);
					it = held.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		// A friend left: what they held is held by nobody.
		std::vector<std::uint32_t> Leave(std::uint32_t a_holder)
		{
			std::vector<std::uint32_t> out;
			for (auto it = held.begin(); it != held.end();) {
				if (it->second.holder == a_holder) {
					out.push_back(it->first);
					it = held.erase(it);
				} else {
					++it;
				}
			}
			return out;
		}

		// Our game can't draw it (unloaded, picked up, ours to hold now): forgotten until their next hold.
		void Drop(std::uint32_t a_ref) { held.erase(a_ref); }

		[[nodiscard]] const Entry* Find(std::uint32_t a_ref) const
		{
			const auto it = held.find(a_ref);
			return it == held.end() ? nullptr : &it->second;
		}
		Entry* Find(std::uint32_t a_ref)
		{
			const auto it = held.find(a_ref);
			return it == held.end() ? nullptr : &it->second;
		}

		[[nodiscard]] std::vector<std::uint32_t> Refs() const
		{
			std::vector<std::uint32_t> out;
			for (const auto& [ref, entry] : held) {
				out.push_back(ref);
			}
			return out;
		}
		[[nodiscard]] std::size_t Size() const { return held.size(); }

		void Reset()
		{
			held.clear();
			letGo.clear();
		}

	private:
		struct LetGo
		{
			std::uint32_t     holder = 0;
			std::uint32_t     sequence = 0;
			Clock::time_point at{};
		};

		static bool Newer(std::uint32_t a_seq, std::uint32_t a_than)
		{
			return a_seq != a_than && static_cast<std::uint32_t>(a_seq - a_than) < 0x80000000u;
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(letGo, [&](const auto& a_entry) { return a_now - a_entry.second.at >= LETGO_MEMORY; });
		}

		std::unordered_map<std::uint32_t, Entry> held;
		std::unordered_map<std::uint32_t, LetGo> letGo;
	};

	// --- objects knocked about --------------------------------------------------------------------------------------

	inline constexpr float  MOVE_DISTANCE = 4.0f;    // units from where it rested: moved
	inline constexpr float  MOVE_TURN = 0.07f;       // radians (4 degrees)
	inline constexpr auto   MOVING_GIVE_UP = std::chrono::seconds(15);
	inline constexpr auto   FORGET_AFTER = std::chrono::seconds(3);  // not seen in a scan that long
	inline constexpr auto   QUIET_AFTER_PLACED = std::chrono::seconds(4);  // a friend's move: ours of it are theirs
	inline constexpr auto   FOLLOW_ON = std::chrono::seconds(10);  // ours, at rest, moves again this soon: it rolled on, still ours
	inline constexpr int    ECHO_MAX = 3;  // rests said for one object in ECHO_WINDOW that our physics moved on from a friend's place (two games whose physics never agree would trade it for ever)
	inline constexpr auto   ECHO_WINDOW = std::chrono::seconds(120);
	inline constexpr int    FOLLOW_ON_MAX = 8;  // rests in a row that are ours only by the follow-on: past it, a thing that never stops (swinging) is let go
	inline constexpr std::size_t MAX_WATCHED = 512;
	inline constexpr double KNOCK_BURST = 8.0;  // rests our game reports: a few at once, then KNOCKS_PER_SECOND
	inline constexpr double KNOCKS_PER_SECOND = 3.0;
	// What a friend's blast moved in our game alone (their game said no rest of it) is put back where it lay.
	inline constexpr float       PUT_BACK_DISTANCE = 20.0f;                    // our copy ended this far from where it lay, at least
	inline constexpr auto        PUT_BACK_AFTER = std::chrono::seconds(5);     // ... and has rested that long
	inline constexpr auto        PUT_BACK_QUIET = std::chrono::seconds(4);     // ... with no rest that long from the friend whose blast threw it: their rests are in
	inline constexpr auto        PUT_BACK_GIVE_UP = std::chrono::seconds(45);  // not put back by then: left where it lies
	inline constexpr std::size_t PUT_BACKS_A_SCAN = 16;
	inline constexpr float  NEAR_DISTANCE = 160.0f;  // our player this close (across) walked into it
	inline constexpr float  NEAR_HEIGHT = 200.0f;

	// Whether our player at a_player is close enough to an object at a_object to have walked into it.
	inline bool Near(const Vec3& a_player, const Vec3& a_object)
	{
		const float dx = a_player[0] - a_object[0], dy = a_player[1] - a_object[1];
		return dx * dx + dy * dy <= NEAR_DISTANCE * NEAR_DISTANCE && std::fabs(a_object[2] - a_player[2]) <= NEAR_HEIGHT;
	}

	// An NPC our game runs: one of its own that close (across), alive, walked or fell into an object and shoved it. Wider
	// than Near: its body is bigger round than our player's feet.
	inline constexpr float       NPC_NEAR_DISTANCE = 200.0f;
	inline constexpr float       NPC_NEAR_HEIGHT = 200.0f;
	inline constexpr std::size_t MAX_SHOVERS = 64;  // NPCs of ours looked at in one scan (a crowd's)

	// Whether an NPC at a_npc is close enough to an object at a_object to have shoved it.
	inline bool NpcNear(const Vec3& a_npc, const Vec3& a_object)
	{
		const float dx = a_npc[0] - a_object[0], dy = a_npc[1] - a_object[1];
		return dx * dx + dy * dy <= NPC_NEAR_DISTANCE * NPC_NEAR_DISTANCE && std::fabs(a_object[2] - a_npc[2]) <= NPC_NEAR_HEIGHT;
	}

	// Whether our game answers for an actor (an NPC that shoves, or the cause of a blast): the session knows it (a_id,
	// NpcSync::SessionId: 0 is never shared) and the server made our player its owner. Another player's NPC, one nobody
	// runs yet and one that is not shared are not ours. So the game that runs an NPC is the only one that reports what it
	// shoves, and two games never report the same object.
	constexpr bool Answers(std::uint32_t a_id, std::uint32_t a_owner, std::uint32_t a_local)
	{
		return a_id != 0 && a_local != 0 && a_owner == a_local;
	}

	// --- blasts ---------------------------------------------------------------------------------------------------------

	inline constexpr auto        BLAST_REACH_FOR = std::chrono::seconds(4);    // a blast's reach holds that long after our scan first sees it
	inline constexpr auto        BLAST_KEY_MEMORY = std::chrono::seconds(20);  // ... and the explosion is not noted again while it lingers
	inline constexpr float       BLAST_MARGIN = 128.0f;                        // objects beyond its radius are thrown by it still (bodies, not centres)
	inline constexpr float       BLAST_MAX_RADIUS = 1500.0f;                   // a nuke's reach is the scan's
	inline constexpr std::size_t MAX_BLASTS = 16;
	// Rests one blast may have reported. A grenade in a cluttered room throws 20 to 40 objects, and the ones nearest it fly
	// furthest and come to rest last: a cap of 16 left exactly those unsaid (every rig throw said 16 to 18 rests), so the
	// friend's game kept its copies where its own copy of the explosion threw them. The budget (8 at once, 3 a second,
	// under the server's 24 and 6) paces them; the cap only bounds a nuke's.
	inline constexpr std::size_t BLAST_MAX_REPORTS = 64;
	inline constexpr auto        BLAST_DEFER = std::chrono::seconds(24);  // a rest the budget had no room for waits that long: the 64th is said within it

	// Whose blast it is. The cause decides, so the game that threw the grenade (or shot the barrel, or runs the NPC that
	// did) is the one that reports what it threw about; every other game sets off the same explosion on its own, and what
	// that one throws about is the first game's to say.
	enum class BlastRole : std::uint8_t
	{
		kOurs,    // our player's, or an NPC we run's
		kTheirs,  // a friend's or their NPC's, or one set off here for a friend's throw
	};

	// What caused an explosion, as our game knows it.
	enum class Cause : std::uint8_t
	{
		kNobody,   // a hazard, a trap, a script: no game claims what it throws about
		kPlayer,   // our player
		kStandIn,  // a friend's stand-in
		kNpc,      // an NPC (ours or not: a_npcAnswered)
	};

	// a_placed: our game set the explosion off for a friend's throw. No role: nobody's.
	inline std::optional<BlastRole> RoleOf(bool a_placed, Cause a_cause, bool a_npcAnswered)
	{
		if (a_placed) {
			return BlastRole::kTheirs;
		}
		switch (a_cause) {
		case Cause::kPlayer:
			return BlastRole::kOurs;
		case Cause::kStandIn:
			return BlastRole::kTheirs;
		case Cause::kNpc:
			return a_npcAnswered ? BlastRole::kOurs : BlastRole::kTheirs;
		default:
			return std::nullopt;
		}
	}

	// The friends' blasts that reach one object (BlastBook's serials), MAX_THEIR_BLASTS at most: with more reaching it
	// the set says so (more), and nothing is made of it (KnockWatch::Due puts no such object back).
	inline constexpr std::size_t MAX_THEIR_BLASTS = 6;

	struct TheirBlasts
	{
		std::array<std::uint32_t, MAX_THEIR_BLASTS> serials{};
		std::uint8_t                                count = 0;
		bool                                        more = false;

		void Add(std::uint32_t a_serial)
		{
			const auto end = serials.begin() + count;
			if (a_serial == 0 || std::find(serials.begin(), end, a_serial) != end) {
				return;
			}
			if (count >= MAX_THEIR_BLASTS) {
				more = true;
				return;
			}
			serials[count++] = a_serial;
		}

		void Add(const TheirBlasts& a_other)
		{
			for (const auto serial : a_other.Serials()) {
				Add(serial);
			}
			more = more || a_other.more;
		}

		[[nodiscard]] std::span<const std::uint32_t> Serials() const { return std::span<const std::uint32_t>(serials.data(), count); }
	};

	// The blasts our scans saw lately, and which objects they reach.
	class BlastBook
	{
	public:
		struct Reach
		{
			std::uint32_t serial = 0;      // the nearest blast of ours that reaches it (0: none)
			bool          theirs = false;  // a friend's blast reaches it: not ours to report, whatever else moved it, unless a blast of ours reaches it too
			TheirBlasts   their;           // ... which of the friends' blasts
		};

		// An explosion seen at a_at (a_key: its handle, the same only while it is the same explosion). Whether it is new.
		// a_from, for a friend's blast: the player whose game answers for what it throws about, so the one that says
		// where each object it saw move came to rest (who threw it, or whose game runs the NPC that did; 0: not known,
		// or nobody's).
		bool Note(std::uint32_t a_key, const Vec3& a_at, float a_radius, BlastRole a_role, Clock::time_point a_now, std::uint32_t a_from = 0)
		{
			std::erase_if(blasts, [&](const Blast& a_blast) { return a_now - a_blast.noted >= BLAST_KEY_MEMORY; });
			if (!Finite(a_at) || !std::isfinite(a_radius) || a_radius <= 0.0f || blasts.size() >= MAX_BLASTS) {
				return false;
			}
			if (std::ranges::any_of(blasts, [&](const Blast& a_blast) { return a_blast.key == a_key; })) {
				return false;
			}
			blasts.push_back({ .key = a_key, .serial = ++lastSerial, .at = a_at, .radius = (std::min)(a_radius, BLAST_MAX_RADIUS), .role = a_role, .noted = a_now,
				.from = a_role == BlastRole::kTheirs ? a_from : 0 });
			return true;
		}

		// The player whose game answers for the friend's blast a_serial (0: not known, nobody's, or a blast forgotten).
		[[nodiscard]] std::uint32_t From(std::uint32_t a_serial) const
		{
			const auto it = std::ranges::find_if(blasts, [&](const Blast& a_blast) { return a_blast.serial == a_serial; });
			return it != blasts.end() ? it->from : 0;
		}

		[[nodiscard]] Reach Reaching(const Vec3& a_object, Clock::time_point a_now) const
		{
			Reach reach;
			float nearest = (std::numeric_limits<float>::max)();
			for (const auto& blast : blasts) {
				const float d = Distance(a_object, blast.at);
				if (a_now - blast.noted >= BLAST_REACH_FOR || d > blast.radius + BLAST_MARGIN) {
					continue;
				}
				if (blast.role == BlastRole::kTheirs) {
					reach.theirs = true;
					reach.their.Add(blast.serial);
				} else if (d < nearest) {
					nearest = d;
					reach.serial = blast.serial;
				}
			}
			return reach;
		}

		[[nodiscard]] std::size_t Size() const { return blasts.size(); }
		void                      Reset() { blasts.clear(); }

	private:
		struct Blast
		{
			std::uint32_t     key = 0;
			std::uint32_t     serial = 0;
			Vec3              at{};
			float             radius = 0.0f;
			BlastRole         role = BlastRole::kOurs;
			Clock::time_point noted{};
			std::uint32_t     from = 0;  // a friend's: the player whose game answers for it (0: not known)
		};

		std::vector<Blast> blasts;
		std::uint32_t      lastSerial = 0;
	};

	// The objects our game sees moving, by a scan of those near our player (a few times a second). One moved with no
	// hand on it and come to rest is reported when our game is the one to say who moved it, whether or not a friend has
	// it loaded now (the session keeps where it rests, for a game that loads it later): our player did (walked into it,
	// shot it: byUs), an NPC our game runs did (byNpc: one of ours stood by it), or a blast of ours threw it (blast:
	// BlastBook's serial). The friend's game reports the ones their player, their NPCs and their blasts move; an object a
	// friend's blast reaches (theirs) is never ours, whatever else moved it, unless a blast of ours reached it as well (both
	// players threw at one pile: with theirs alone deciding, each game left every object to the other and no rest was said
	// at all; now both say it, and the server keeps the first). One their blast moved here that their game says nothing of,
	// while it says where other things that same blast threw came to rest, is put back where it lay (Due). What a friend's
	// move put somewhere (Placed), and what is in anyone's hands or flying from
	// them (busy), is never ours to report either. A blast is reported for at most BLAST_MAX_REPORTS objects; one the
	// budget has no room for yet waits BLAST_DEFER for it.
	//
	// It rests when it moved less than STILL_DISTANCE (and turned less than STILL_TURN) for SETTLE_TIME, or after
	// MOVING_GIVE_UP. A ball rolls on after that now and then: slowing on a rise, then rolling back, or still rolling when
	// we gave up. Whatever moved it is long gone by then (our player walked on, the hit is old), so one of ours that moves
	// again within FOLLOW_ON of the rest our game reported is still ours (the follow-on), and is reported again where it
	// truly rests. Nothing new is looked at for it: the same scan sees it. A friend's move of it (Placed), a hand on it
	// (busy) or a friend's blast (theirs) ends the follow-on, so a friend's place never echoes back.
	//
	// A friend standing by it changes nothing: each game reports what its own player, NPCs and blasts moved and what rolled
	// on from its own rests, and when two games claim one rest the server settles it (ObjectMovers::Book: the first rest
	// holds for SETTLED, a second player's is refused and answered with the first), so both end with the same copy. Their
	// rest puts our copy where their game has it, but our world is not theirs: put down in our player's body, or in
	// their stand-in's where their player is not, it is pushed out, and lies somewhere their game knows nothing of. So a
	// copy their place put down that our player, an NPC of ours or a blast of ours moves on is ours again, the friend's
	// blast's reach notwithstanding (their blast's throw of it ended with their rest), said once the quiet after their
	// place (QUIET_AFTER_PLACED, longer than the server's SETTLED) is over: never crossed with their rest on the way, so
	// no two games fight over it, and at most one rest per QUIET_AFTER_PLACED per object goes back, ECHO_MAX per
	// ECHO_WINDOW (two games whose physics never agree on where it lies would trade it for ever). A copy that only
	// settles on its own after their place, with nothing of ours by it, is not ours to speak of.
	class KnockWatch
	{
	public:
		struct Seen
		{
			std::uint32_t refId = 0;
			Pose          pose{};
			bool          byUs = false;
			bool          busy = false;
			bool          byNpc = false;
			std::uint32_t blast = 0;
			bool          theirs = false;
		};

		// a_blasts: the blasts our scans saw lately. What a blast reaches is judged from where the object lay before it
		// moved as well as from where it is now: a blast flings an object far past its own reach (and past our player's
		// near range) between two scans, so the scan that first sees it move finds it already out of reach of what moved
		// it. The claim is made when it starts moving and held until it rests (and its follow-on after).
		std::vector<Protocol::ObjectRest> Scan(std::span<const Seen> a_seen, Clock::time_point a_now, const BlastBook* a_blasts = nullptr)
		{
			std::vector<Protocol::ObjectRest> out;
			for (const auto& seen : a_seen) {
				if (!Finite(seen.pose.position) || !Finite(seen.pose.angles)) {
					continue;
				}
				auto it = watched.find(seen.refId);
				if (it == watched.end()) {
					if (watched.size() >= MAX_WATCHED) {
						continue;
					}
					watched.emplace(seen.refId, Watch{ .rest = seen.pose, .last = seen.pose, .seenAt = a_now });
					continue;
				}
				auto& w = it->second;
				std::uint32_t blast = seen.blast;
				bool          reachedByTheirs = seen.theirs;
				TheirBlasts   reached;  // the friends' blasts that reach it
				if (a_blasts) {
					for (const Vec3* from : { &seen.pose.position, static_cast<const Vec3*>(&w.rest.position) }) {
						const auto reach = a_blasts->Reaching(*from, a_now);
						blast = blast != 0 ? blast : reach.serial;
						reachedByTheirs = reachedByTheirs || reach.theirs;
						reached.Add(reach.their);
					}
				}
				if (a_now - w.seenAt >= FORGET_AFTER) {
					// Not looked at for a while (a loading screen, a cell that unloaded and came back): whatever made it ours
					// is long gone.
					w.again = false;
					w.followUntil = {};
					w.follows = 0;
					w.back.reset();
				}
				w.seenAt = a_now;
				if (seen.busy) {
					w.rest = w.last = seen.pose;
					w.moving = false;
					w.owed = false;
					w.back.reset();  // a hand on it: where it goes now is its holder's to say
					w.again = false;
					w.followUntil = {};
					w.follows = 0;
					w.quietUntil = (std::max)(w.quietUntil, a_now + QUIET_AFTER_PLACED);
					continue;
				}
				// A friend's blast reaching it makes the move theirs, unless their place put it here just now (quiet): their
				// blast's throw of it ended with their rest, and what moves it on from where they put it is not the blast.
				const bool theirs = reachedByTheirs && a_now >= w.quietUntil;
				if (!w.moving) {
					if (Distance(seen.pose.position, w.rest.position) > MOVE_DISTANCE || Turned(seen.pose.angles, w.rest.angles) > MOVE_TURN) {
						// One of ours still owed (unsaid: the budget had no room yet) that moves on, hit by another can the
						// blast threw, is still ours: its claim was made, and the blast's reach may be over by now.
						const bool wasOwed = w.owed && a_now < w.owedUntil;
						w.moving = true;
						w.owed = false;
						w.byUs = seen.byUs;
						w.byNpc = seen.byNpc;
						w.blast = blast;
						w.theirs = theirs;
						w.their = {};
						w.unplaced = false;
						if (theirs) {
							w.their = reached;
							Know(reached, a_blasts);
						}
						w.again = a_now < w.followUntil || wasOwed;
						w.movingSince = w.stillSince = a_now;
						w.last = seen.pose;
					} else if (w.owed) {
						// Came to rest while the budget had no room: say so now if it has, or give up.
						if (a_now >= w.owedUntil) {
							w.owed = false;
							++dropped;
						} else if (a_now >= w.quietUntil && Take(w.blast, a_now)) {
							out.push_back(Rest(seen));
							w.owed = false;
							w.followUntil = a_now + FOLLOW_ON;
						}
					}
					continue;
				}
				w.byUs = w.byUs || seen.byUs;
				w.byNpc = w.byNpc || seen.byNpc;
				w.theirs = w.theirs || theirs;
				if (theirs) {
					w.their.Add(reached);
					Know(reached, a_blasts);
				}
				if (w.blast == 0) {
					w.blast = blast;
				}
				// Still: within STILL_DISTANCE of where it was when it last moved, not merely of the last look, so a slow
				// roll (under a unit a look) is moving, not at rest.
				if (Distance(seen.pose.position, w.last.position) > STILL_DISTANCE || Turned(seen.pose.angles, w.last.angles) > STILL_TURN) {
					w.stillSince = a_now;
					w.last = seen.pose;
				}
				if (a_now - w.stillSince < SETTLE_TIME && a_now - w.movingSince < MOVING_GIVE_UP) {
					continue;
				}
				const Pose before = w.rest;
				w.moving = false;
				w.rest = w.last = seen.pose;
				// Ours because something said so (fresh), or only by the follow-on, and not for ever: a thing that swings or
				// jitters for hours is let go after FOLLOW_ON_MAX rests in a row that nothing but the follow-on claimed.
				const bool fresh = w.byUs || w.byNpc || w.blast != 0;
				// A friend's blast reached it and no blast of ours did: theirs to say. One that a blast of each reached (both
				// players threw at the pile) is said by both games, and the server keeps the first rest (ObjectMovers::Book).
				const bool theirsToSay = w.theirs && w.blast == 0;
				const bool ours = !theirsToSay && (fresh || (w.again && w.follows < FOLLOW_ON_MAX));
				if (ours) {
					w.back.reset();  // said (or owed) where it lies now
				} else {
					if (theirsToSay && w.unplaced) {
						// Their game named a rest for this move, one ours could not put it at (RestHeard): theirs moved too,
						// so ours is not one to put back.
						w.back.reset();
					} else if (theirsToSay) {
						// Their game says where it rests. If it says nothing (their copy never moved), ours goes back here,
						// once every blast of theirs that threw it is known to be reported (Due). One waiting to go back
						// that another blast throws on waits for that one's proof as well.
						if (!w.back) {
							w.back = before;
							w.backSince = w.movingSince;
							w.backBy = {};
						}
						w.backBy.Add(w.their);
					}
					w.backRested = a_now;  // one waiting to go back that rolled on waits from here
				}
				if (ours) {
					// Ours: if it moves again soon, that is still ours (the follow-on), reported or owed.
					w.follows = fresh ? 0 : w.follows + 1;
					w.followUntil = a_now + FOLLOW_ON;
					if (a_now < w.quietUntil) {
						// A friend's place put it down just now and something of ours moved it on from there (our player's
						// body pushed it out, say): said where it lies once the quiet is over, if it still lies there. At
						// most ECHO_MAX times in ECHO_WINDOW: if our game and theirs each move it on from the other's rest
						// for ever (the physics of the two never agree on where it lies), the trade stops.
						if (a_now - w.echoSince >= ECHO_WINDOW) {
							w.echoes = 0;
						}
						if (w.echoes < ECHO_MAX) {
							if (w.echoes++ == 0) {
								w.echoSince = a_now;
							}
							w.owed = true;
							w.owedUntil = w.quietUntil + BLAST_DEFER;
						}
					} else if (Take(w.blast, a_now)) {
						out.push_back(Rest(seen));
					} else if (BlastRoom(w.blast)) {
						// A blast moves dozens at once: the rest come as the budget fills again. So does any other rest of
						// ours that comes while the budget is empty (a can rolling on from its rest, by the follow-on):
						// dropped, it was never said, and the friend's copy stayed where their own physics put it.
						w.owed = true;
						w.owedUntil = a_now + BLAST_DEFER;
					} else {
						++dropped;  // past the blast's cap
					}
				}
				w.byUs = w.byNpc = w.theirs = w.again = w.unplaced = false;
				w.their = {};
				if (!w.owed) {
					w.blast = 0;
				}
			}
			std::erase_if(watched, [&](const auto& a_entry) { return a_now - a_entry.second.seenAt >= FORGET_AFTER; });
			return out;
		}

		// A friend's move put the object at a_pose: where it rests now, and what our game sees of it settling there (its
		// own physics) is not ours to report; what something of ours moves it on to is said once the quiet is over.
		// a_restFrom: the player whose game said this rest, when it is one a game said as the object came to rest there
		// (a live kRest); 0 for every other place (an object in a friend's hands drawn where they hold it, a let-go, a
		// catch-up, one that waited for our game to load it). Only a rest a game said is word of a blast (Heard, Due):
		// with every place counted, a friend carrying a can kept every put-back waiting until it was given up.
		void Placed(std::uint32_t a_ref, const Pose& a_pose, Clock::time_point a_now, std::uint32_t a_restFrom = 0)
		{
			auto& w = watched[a_ref];
			Heard(w, a_restFrom, a_now);
			Settle(w, a_pose, a_now);
		}

		// A friend's game named a rest (or a let-go) for the object that ours did not put it at (too far from where ours
		// has it, another space, not loaded here). a_restFrom as for Placed. Their copy did move, so ours is not one their
		// game said nothing of: it is not put back where it lay, now or when it comes to rest. The rest is still word of
		// the blast that threw it.
		void RestHeard(std::uint32_t a_ref, std::uint32_t a_restFrom, Clock::time_point a_now)
		{
			const auto it = watched.find(a_ref);
			if (it == watched.end()) {
				Heard(Watch{}, a_restFrom, a_now);
				return;
			}
			auto& w = it->second;
			Heard(w, a_restFrom, a_now);
			w.back.reset();
			w.unplaced = w.moving;
		}

		struct Back
		{
			std::uint32_t refId = 0;
			Pose          pose{};  // where it lay before their blast moved our copy
		};

		// The objects to put back where they lay, at most a_max a call (after Scan, with the same a_now). A friend's blast
		// goes off in our game too, and our physics throws what it reaches its own way; their game says where each one it
		// saw move came to rest, and our copy is put there (Placed). One their game says nothing of did not move in theirs
		// (a can at the blast's edge that only our copy of the explosion caught): our copy, more than PUT_BACK_DISTANCE from
		// where it lay, goes back there, once it has rested PUT_BACK_AFTER and no rest of theirs has come for
		// PUT_BACK_QUIET (their rests come 3 a second while any are owed, so a gap that long is the end of them).
		//
		// Only when their game is known to be saying what that very blast threw. A game that says nothing at all of a
		// blast (its player too far to look at the pile, an NPC's grenade a long way from the game that runs it) is not
		// one whose silence means anything, and what that blast threw here stays where it lies. So the proof is kept for
		// each blast (BlastBook's serial) and is the word of one friend only: the player whose game answers for the blast
		// (BlastBook::From) has said a rest, since ours started moving, for an object that this blast, and no other of the
		// friends', moved here (Heard). Before, one time stood for every friend and every blast: the rests a friend said
		// for their own grenade let the cans their NPC's grenade threw here, far from their player and unreported, go
		// back, and with three players any friend's rests did for any other's blast. A rest of another player's, one for
		// another blast's objects, a held object's place and a catch-up prove nothing; a blast nobody is known to answer
		// for (a trap's, an NPC nobody runs) is never proven. An object more than one of the friends' blasts threw goes
		// back only when each of them is proven, and the quiet is each answering friend's own.
		//
		// A rest of theirs that comes later still (theirs rolled on) is applied as any other: theirs wins. Not while ours
		// moves, was not looked at in this scan, or has a hand on it; one not put back within PUT_BACK_GIVE_UP of its rest
		// is left. The caller places each and says so (PutBack).
		//
		// Known limits. A blast whose reach lies partly beyond the answering game's own scan: that game says the rests it
		// sees, the blast is proven, and its silence about what lay beyond its scan is read as "did not move". Blasts so
		// close together that each object is in reach of more than one (a cluster's children): no rest is word of any one
		// of them, so none is proven and nothing they threw goes back. A blast that threw one set of objects in their game
		// and another in ours, with none in both: their rests are for objects ours never moved, word of no blast, so what
		// ours threw stays where it lies.
		[[nodiscard]] std::vector<Back> Due(Clock::time_point a_now, std::size_t a_max = PUT_BACKS_A_SCAN)
		{
			std::vector<Back> out;
			for (auto& [ref, w] : watched) {
				if (!w.back || w.moving) {
					continue;
				}
				if (a_now - w.backRested >= PUT_BACK_GIVE_UP || Distance(w.rest.position, w.back->position) <= PUT_BACK_DISTANCE) {
					w.back.reset();
					continue;
				}
				if (w.seenAt != a_now || a_now - w.backRested < PUT_BACK_AFTER || !Proven(w.backBy, w.backSince, a_now)) {
					continue;
				}
				if (out.size() >= a_max) {
					break;
				}
				out.push_back({ .refId = ref, .pose = *w.back });
				w.back.reset();
			}
			return out;
		}

		// Our game put a_ref back at a_pose (Due): where it rests now, and quiet as after a friend's place, so what our
		// physics does with it there is not reported unless something of ours moves it on.
		void PutBack(std::uint32_t a_ref, const Pose& a_pose, Clock::time_point a_now)
		{
			if (const auto it = watched.find(a_ref); it != watched.end()) {
				Settle(it->second, a_pose, a_now);
				++putBack;
			}
		}

		// The objects of ours the scan has not finished with, at most a_max: moving and ours so far (our player, an NPC of
		// ours or a blast of ours set them going, no friend's blast did); at rest and owed (the blast's budget had no room
		// yet); at rest within the follow-on of a rest of ours (it may roll on). What the scan looks at wherever they lie,
		// past its own radius round our player, until each is said or Scan gives up on it (MOVING_GIVE_UP, owedUntil,
		// followUntil), so how far one is flung does not end the watch on it.
		[[nodiscard]] std::vector<std::uint32_t> Tracking(std::size_t a_max, Clock::time_point a_now) const
		{
			std::vector<std::uint32_t> out;
			const auto                 ours = [&](const Watch& a_w) {
				return a_w.moving ? (!a_w.theirs || a_w.blast != 0) && (a_w.byUs || a_w.byNpc || a_w.blast != 0) : (a_w.owed && a_now < a_w.owedUntil) || a_now < a_w.followUntil;
			};
			for (const auto& [ref, w] : watched) {
				if (out.size() >= a_max) {
					return out;
				}
				if (ours(w)) {
					out.push_back(ref);
				}
			}
			// Then, in the room left, what a friend's blast set going here and what waits to be put back (Due): looked at
			// until it rests and goes back, or is given up.
			for (const auto& [ref, w] : watched) {
				if (out.size() >= a_max) {
					break;
				}
				if (!ours(w) && ((w.moving && w.theirs) || w.back)) {
					out.push_back(ref);
				}
			}
			return out;
		}

		[[nodiscard]] std::size_t Watching() const { return watched.size(); }
		[[nodiscard]] std::size_t Moving() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(watched, [](const auto& a_entry) { return a_entry.second.moving; }));
		}
		[[nodiscard]] std::uint64_t Reported() const { return reported; }
		// Rests of ours never said (past a blast's cap, or still owed at owedUntil): each leaves the friends' copies where
		// their own physics put them.
		[[nodiscard]] std::uint64_t Dropped() const { return dropped; }
		// Objects a friend's blast moved in our game alone, put back where they lay (PutBack).
		[[nodiscard]] std::uint64_t PutBacks() const { return putBack; }

		void Reset()
		{
			watched.clear();
			blastReports.clear();
			proofs.clear();
			lastRest.clear();
		}

	private:
		struct Watch
		{
			Pose              rest{};
			Pose              last{};
			bool              moving = false;
			bool              byUs = false;
			bool              byNpc = false;
			bool              theirs = false;
			bool              owed = false;   // at rest, to be reported when the budget has room, or a friend's place's quiet is over
			bool              again = false;  // moving on after a rest of ours (the follow-on): ours
			std::uint32_t     blast = 0;
			Clock::time_point movingSince{};
			Clock::time_point stillSince{};
			Clock::time_point seenAt{};
			Clock::time_point quietUntil{};
			Clock::time_point owedUntil{};
			Clock::time_point followUntil{};  // ours if it moves again before then
			int               follows = 0;    // rests in a row that only the follow-on made ours
			int               echoes = 0;     // rests owed for moving a friend's place on, counted from echoSince
			Clock::time_point echoSince{};
			std::optional<Pose> back;        // where it lay before a friend's blast moved it here: put back there if their game says no rest of it (Due)
			Clock::time_point backSince{};   // ... when it started moving
			Clock::time_point backRested{};  // ... and when it last came to rest
			TheirBlasts       backBy;        // ... and the friends' blasts that threw it: each has to be proven (Due)
			TheirBlasts       their;         // moving: the friends' blasts that reached it in this move
			bool              unplaced = false;  // moving: their game named a rest for it that ours did not put it at (RestHeard)
		};

		// What is known of a friend's blast that moved something here.
		struct Proof
		{
			std::uint32_t                    from = 0;  // the player whose game answers for it (0: not known: never proven)
			std::optional<Clock::time_point> heard;     // when that player last said a rest for an object it alone moved here
		};

		static constexpr std::size_t MAX_PROOFS = 128;  // blasts remembered (a guard): the older half goes past it
		static constexpr std::size_t MAX_RESTERS = 64;  // players remembered for their last rest (a guard)

		// The friends' blasts in a_their have moved something here: who answers for each is kept from the blast book,
		// which forgets a blast long before what it threw is put back.
		void Know(const TheirBlasts& a_their, const BlastBook* a_blasts)
		{
			if (!a_blasts) {
				return;
			}
			for (const auto serial : a_their.Serials()) {
				if (proofs.contains(serial)) {
					continue;
				}
				if (proofs.size() >= MAX_PROOFS) {
					std::erase_if(proofs, [&](const auto& a_entry) { return a_entry.first + MAX_PROOFS / 2 <= serial; });
					if (proofs.size() >= MAX_PROOFS) {
						proofs.clear();
					}
				}
				proofs.emplace(serial, Proof{ .from = a_blasts->From(serial) });
			}
		}

		// Player a_from's game said a rest for the object a_w watches (0: no rest a game said, nothing is made of it).
		// It is word of a blast when that blast, and no other of the friends', moved the object here (moving now, or at
		// rest and waiting to go back), and a_from is the player whose game answers for it.
		void Heard(const Watch& a_w, std::uint32_t a_from, Clock::time_point a_now)
		{
			if (a_from == 0) {
				return;
			}
			if (lastRest.size() >= MAX_RESTERS && !lastRest.contains(a_from)) {
				lastRest.clear();
			}
			lastRest[a_from] = a_now;
			const TheirBlasts* by = nullptr;
			if (a_w.moving && a_w.theirs) {
				by = &a_w.their;
			} else if (a_w.back) {
				by = &a_w.backBy;
			}
			if (!by || by->count != 1 || by->more) {
				return;
			}
			if (const auto it = proofs.find(by->serials[0]); it != proofs.end() && it->second.from == a_from) {
				it->second.heard = a_now;
			}
		}

		// Whether every blast in a_by is known to be reported by the game that answers for it: that player said a rest
		// for something the blast alone moved here since a_since, and has said no rest at all for PUT_BACK_QUIET.
		[[nodiscard]] bool Proven(const TheirBlasts& a_by, Clock::time_point a_since, Clock::time_point a_now) const
		{
			if (a_by.count == 0 || a_by.more) {
				return false;
			}
			for (const auto serial : a_by.Serials()) {
				const auto it = proofs.find(serial);
				if (it == proofs.end() || it->second.from == 0 || !it->second.heard || *it->second.heard < a_since) {
					return false;
				}
				if (const auto rest = lastRest.find(it->second.from); rest != lastRest.end() && a_now - rest->second < PUT_BACK_QUIET) {
					return false;
				}
			}
			return true;
		}

		// a_w rests at a_pose, put there by a friend's move or by our put-back.
		static void Settle(Watch& a_w, const Pose& a_pose, Clock::time_point a_now)
		{
			a_w.rest = a_w.last = a_pose;
			a_w.moving = false;
			a_w.owed = false;
			a_w.byUs = a_w.byNpc = a_w.theirs = a_w.again = a_w.unplaced = false;
			a_w.their = {};
			a_w.blast = 0;
			a_w.seenAt = a_now;
			a_w.quietUntil = a_now + QUIET_AFTER_PLACED;
			a_w.followUntil = {};  // theirs now: our physics moving it on is not ours to report
			a_w.follows = 0;
			a_w.back.reset();
		}

		// Whether the blast (0: none) may have another rest reported.
		[[nodiscard]] bool BlastRoom(std::uint32_t a_blast) const
		{
			if (a_blast == 0) {
				return true;
			}
			const auto it = blastReports.find(a_blast);
			return it == blastReports.end() || it->second < BLAST_MAX_REPORTS;
		}

		// A rest to report, by the budget (the same as every knock's) and the blast's own cap.
		bool Take(std::uint32_t a_blast, Clock::time_point a_now)
		{
			if (!BlastRoom(a_blast) || !budget.Take(a_now)) {
				return false;
			}
			if (a_blast != 0) {
				if (blastReports.size() >= 64 && !blastReports.contains(a_blast)) {
					blastReports.clear();  // old blasts' counts: a blast does not last this many others
				}
				++blastReports[a_blast];
			}
			return true;
		}

		Protocol::ObjectRest Rest(const Seen& a_seen)
		{
			Protocol::ObjectRest rest{ .refId = a_seen.refId, .kind = Protocol::ObjectRestKind::kRest, .cell = a_seen.pose.cell, .worldspace = a_seen.pose.worldspace,
				.position = a_seen.pose.position };
			for (std::size_t i = 0; i < 3; ++i) {
				rest.rotation[i] = Wrap(a_seen.pose.angles[i]);
			}
			++reported;
			return rest;
		}

		std::unordered_map<std::uint32_t, Watch>       watched;
		std::unordered_map<std::uint32_t, std::size_t> blastReports;  // by BlastBook's serial: rests reported for each
		TokenBucket                                    budget{ KNOCK_BURST, KNOCKS_PER_SECOND };
		std::uint64_t                                  reported = 0;
		std::uint64_t                                  dropped = 0;
		std::uint64_t                                  putBack = 0;
		std::unordered_map<std::uint32_t, Proof>             proofs;    // by BlastBook's serial: the friends' blasts that moved something here
		std::unordered_map<std::uint32_t, Clock::time_point> lastRest;  // by player: when their game last said a rest
	};

	// --- what a friend's message may do ---------------------------------------------------------------------------

	inline constexpr float HOLD_REACH = 2500.0f;   // a held object this close to its holder's stand-in here, at most
	inline constexpr float APPLY_RANGE = 8000.0f;  // a moved object this close to our player, at most
	inline constexpr float MAX_JUMP = 4096.0f;     // from where our game has it: a throw goes far, never across the map

	// What our game knows of the object a friend's message names.
	struct Facts
	{
		bool  loaded = false;      // a loose object of ours (MovedObjects::Movable), loaded with its 3D
		bool  sameSpace = false;   // in the space the message says (a rest), or the holder's stand-in's (a hold)
		bool  oursHeld = false;    // in our player's hands
		float fromUs = 0.0f;       // its distance from our player
		float fromPlace = 0.0f;    // from where the message puts it (a rest), or from the holder's stand-in (a hold)
	};

	enum class Verdict : std::uint8_t
	{
		kApply,
		kNotHere,    // not loaded here (or not a loose object), in another space, or too far from us
		kOurs,       // in our player's hands: the server says who holds it, and our game lets go if it is a friend
		kTooFar,     // the message puts it further than the game could have moved it
	};

	inline Verdict JudgeHold(const Facts& a_facts)
	{
		if (!a_facts.loaded || !a_facts.sameSpace || !(a_facts.fromUs <= APPLY_RANGE)) {
			return Verdict::kNotHere;
		}
		if (a_facts.oursHeld) {
			return Verdict::kOurs;
		}
		return a_facts.fromPlace <= HOLD_REACH ? Verdict::kApply : Verdict::kTooFar;
	}

	inline Verdict JudgeRest(const Facts& a_facts)
	{
		if (!a_facts.loaded || !a_facts.sameSpace || !(a_facts.fromUs <= APPLY_RANGE)) {
			return Verdict::kNotHere;
		}
		if (a_facts.oursHeld) {
			return Verdict::kOurs;
		}
		return a_facts.fromPlace <= MAX_JUMP ? Verdict::kApply : Verdict::kTooFar;
	}

	// A place the session kept (a catch-up after joining or loading, or a friend's rest that came while our game didn't
	// have the object): put there once our game has the object loaded, in that space, and the place is within APPLY_RANGE
	// of our player (so the ground there is loaded too: put down beyond it, the object would fall through), however far
	// from where our save had it (it was carried there, out of our sight). Not while someone moves it (a_busy: in a
	// friend's hands, flying from them, or settling from our own let-go), nor while our player holds it: that move says
	// where it goes.
	inline Verdict JudgePlace(const Facts& a_facts, bool a_busy)
	{
		if (!a_facts.loaded || !a_facts.sameSpace || !(a_facts.fromUs <= APPLY_RANGE)) {
			return Verdict::kNotHere;
		}
		return a_facts.oursHeld || a_busy ? Verdict::kOurs : Verdict::kApply;
	}

	// How far a friend's move takes the object (Facts::fromPlace: to a_to, in that space), from where our copy lies or
	// from a place of the session's still waiting to be put down here, whichever is nearer. While a place waits, our copy
	// lies where our save had it (a catch-up that came while loading, or one not near enough yet): the friend moves it
	// from where the session has it, which may be across the map from our copy. Judged from our copy alone, that move was
	// dropped as too far, and so was the place it replaced, so neither went down.
	inline float FromWhereItLies(const Vec3& a_copy, const Protocol::ObjectRest* a_waiting, std::uint32_t a_cell, std::uint32_t a_worldspace, const Vec3& a_to)
	{
		const float fromCopy = Distance(a_copy, a_to);
		if (!a_waiting || a_waiting->cell != a_cell || a_waiting->worldspace != a_worldspace) {
			return fromCopy;
		}
		return (std::min)(fromCopy, Distance(a_waiting->position, a_to));
	}

	// What waits for the object after a friend's move our game did not apply (a_verdict, JudgeRest's), where a_waited is
	// what waited before it: their rest, when our game doesn't have the object near yet, or when it is too far even from
	// the place that waited (the session keeps it there now; our copy only lies where our save had it); else what waited,
	// so a let-go not applied here keeps the place its rest is judged from. Nothing while our player holds it: that move
	// is the newer. A rest too far from our copy with no place waiting is a made-up jump: dropped.
	inline std::optional<Protocol::ObjectRest> StillWaiting(Verdict a_verdict, const Protocol::ObjectRest& a_move, const Protocol::ObjectRest* a_waited)
	{
		if (a_verdict == Verdict::kApply || a_verdict == Verdict::kOurs) {
			return std::nullopt;
		}
		if (a_move.kind == Protocol::ObjectRestKind::kRest && (a_verdict == Verdict::kNotHere || (a_verdict == Verdict::kTooFar && a_waited))) {
			return a_move;
		}
		return a_waited ? std::optional{ *a_waited } : std::nullopt;
	}

	// The session's places our game could not put an object at yet (not loaded, in another space, being moved), one each,
	// the newest. A newer move of the object (ours or a friend's, applied) ends its wait. At most MAX: a flood of one
	// friend's rests for objects we never load (a joiner's made-up ones) drops the rests waiting of the friend with the most,
	// the flooder's, and keeps the catch-ups (playerId 0, at most the session's MAX_PLACES) and everyone else's, so the
	// host's places, kept or live, still go down when we reach their cells.
	class WaitingPlaces
	{
	public:
		static constexpr std::size_t MAX = 4096;

		void Add(const Protocol::ObjectRest& a_rest)
		{
			if (a_rest.kind != Protocol::ObjectRestKind::kRest) {
				return;  // flying: where it comes to rest follows
			}
			if (places.size() >= MAX && !places.contains(a_rest.refId)) {
				std::unordered_map<std::uint32_t, std::size_t> byPlayer;
				for (const auto& [ref, rest] : places) {
					if (rest.playerId != 0) {
						++byPlayer[rest.playerId];
					}
				}
				const auto most = std::ranges::max_element(byPlayer, {}, [](const auto& a_count) { return a_count.second; });
				if (most != byPlayer.end()) {
					std::erase_if(places, [player = most->first](const auto& a_place) { return a_place.second.playerId == player; });
				}
				if (places.size() >= MAX) {
					places.clear();
				}
			}
			places[a_rest.refId] = a_rest;
		}

		[[nodiscard]] const Protocol::ObjectRest* Find(std::uint32_t a_ref) const
		{
			const auto it = places.find(a_ref);
			return it == places.end() ? nullptr : &it->second;
		}

		void Erase(std::uint32_t a_ref) { places.erase(a_ref); }
		[[nodiscard]] bool        Empty() const { return places.empty(); }
		[[nodiscard]] std::size_t Size() const { return places.size(); }

		[[nodiscard]] std::vector<std::uint32_t> Refs() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(places.size());
			for (const auto& [ref, rest] : places) {
				out.push_back(ref);
			}
			return out;
		}

		void Clear() { places.clear(); }

	private:
		std::unordered_map<std::uint32_t, Protocol::ObjectRest> places;
	};

	// Whether a pose may be written into the game: finite, in the world, angles a turn or so.
	inline bool Writable(const Vec3& a_position, const Vec3& a_angles)
	{
		return std::ranges::all_of(a_position, Protocol::InWorld) && Protocol::ObjectAngles(a_angles);
	}

	// A friend's launch velocity, as far as our game takes it: none when it is not finite, at most MAX_OBJECT_SPEED.
	inline Vec3 Launch(const Vec3& a_velocity)
	{
		if (!Finite(a_velocity)) {
			return {};
		}
		const float length = std::sqrt(a_velocity[0] * a_velocity[0] + a_velocity[1] * a_velocity[1] + a_velocity[2] * a_velocity[2]);
		if (length <= Protocol::MAX_OBJECT_SPEED) {
			return a_velocity;
		}
		const float k = Protocol::MAX_OBJECT_SPEED / length;
		return { a_velocity[0] * k, a_velocity[1] * k, a_velocity[2] * k };
	}
}
