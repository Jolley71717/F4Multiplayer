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
	inline constexpr std::size_t MAX_WATCHED = 512;
	inline constexpr double KNOCK_BURST = 8.0;  // rests our game reports: a few at once, then KNOCKS_PER_SECOND
	inline constexpr double KNOCKS_PER_SECOND = 3.0;
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
	inline constexpr std::size_t BLAST_MAX_REPORTS = 16;                       // rests one blast may have reported (the server's burst is 24)
	inline constexpr auto        BLAST_DEFER = std::chrono::seconds(6);        // a rest the budget had no room for waits that long

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

	// The blasts our scans saw lately, and which objects they reach.
	class BlastBook
	{
	public:
		struct Reach
		{
			std::uint32_t serial = 0;      // the nearest blast of ours that reaches it (0: none)
			bool          theirs = false;  // a friend's blast reaches it too: not ours to report, whatever else moved it
		};

		// An explosion seen at a_at (a_key: its handle, the same only while it is the same explosion). Whether it is new.
		bool Note(std::uint32_t a_key, const Vec3& a_at, float a_radius, BlastRole a_role, Clock::time_point a_now)
		{
			std::erase_if(blasts, [&](const Blast& a_blast) { return a_now - a_blast.noted >= BLAST_KEY_MEMORY; });
			if (!Finite(a_at) || !std::isfinite(a_radius) || a_radius <= 0.0f || blasts.size() >= MAX_BLASTS) {
				return false;
			}
			if (std::ranges::any_of(blasts, [&](const Blast& a_blast) { return a_blast.key == a_key; })) {
				return false;
			}
			blasts.push_back({ .key = a_key, .serial = ++lastSerial, .at = a_at, .radius = (std::min)(a_radius, BLAST_MAX_RADIUS), .role = a_role, .noted = a_now });
			return true;
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
		};

		std::vector<Blast> blasts;
		std::uint32_t      lastSerial = 0;
	};

	// The objects our game sees moving, by a scan of those near our player (a few times a second). One moved with no
	// hand on it and come to rest is reported when our game is the one to say who moved it, whether or not a friend has
	// it loaded now (the session keeps where it rests, for a game that loads it later): our player did (walked into it,
	// shot it: byUs), an NPC our game runs did (byNpc: one of ours stood by it), or a blast of ours threw it (blast:
	// BlastBook's serial). The friend's game reports the ones their player, their NPCs and their blasts move; an object a
	// friend's blast reaches (theirs) is never ours, whatever else moved it. What a friend's move put somewhere (Placed), and what is in anyone's hands or flying from
	// them (busy), is never ours to report either. A blast is reported for at most BLAST_MAX_REPORTS objects; one the
	// budget has no room for yet waits BLAST_DEFER for it.
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

		std::vector<Protocol::ObjectRest> Scan(std::span<const Seen> a_seen, Clock::time_point a_now)
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
				w.seenAt = a_now;
				if (seen.busy) {
					w.rest = w.last = seen.pose;
					w.moving = false;
					w.owed = false;
					w.quietUntil = (std::max)(w.quietUntil, a_now + QUIET_AFTER_PLACED);
					continue;
				}
				if (!w.moving) {
					if (Distance(seen.pose.position, w.rest.position) > MOVE_DISTANCE || Turned(seen.pose.angles, w.rest.angles) > MOVE_TURN) {
						w.moving = true;
						w.owed = false;
						w.byUs = seen.byUs;
						w.byNpc = seen.byNpc;
						w.blast = seen.blast;
						w.theirs = seen.theirs;
						w.movingSince = w.stillSince = a_now;
						w.last = seen.pose;
					} else if (w.owed) {
						// Came to rest while the budget had no room: say so now if it has, or give up.
						if (a_now >= w.owedUntil) {
							w.owed = false;
						} else if (a_now >= w.quietUntil && Take(w.blast, a_now)) {
							out.push_back(Rest(seen));
							w.owed = false;
						}
					}
					continue;
				}
				w.byUs = w.byUs || seen.byUs;
				w.byNpc = w.byNpc || seen.byNpc;
				w.theirs = w.theirs || seen.theirs;
				if (w.blast == 0) {
					w.blast = seen.blast;
				}
				if (Distance(seen.pose.position, w.last.position) > STILL_DISTANCE || Turned(seen.pose.angles, w.last.angles) > STILL_TURN) {
					w.stillSince = a_now;
				}
				w.last = seen.pose;
				if (a_now - w.stillSince < SETTLE_TIME && a_now - w.movingSince < MOVING_GIVE_UP) {
					continue;
				}
				w.moving = false;
				w.rest = seen.pose;
				if (!w.theirs && (w.byUs || w.byNpc || w.blast != 0) && a_now >= w.quietUntil) {
					if (Take(w.blast, a_now)) {
						out.push_back(Rest(seen));
					} else if (w.blast != 0 && BlastRoom(w.blast)) {
						// A blast moves dozens at once: the rest come as the budget fills again.
						w.owed = true;
						w.owedUntil = a_now + BLAST_DEFER;
					}
				}
				w.byUs = w.byNpc = w.theirs = false;
				if (!w.owed) {
					w.blast = 0;
				}
			}
			std::erase_if(watched, [&](const auto& a_entry) { return a_now - a_entry.second.seenAt >= FORGET_AFTER; });
			return out;
		}

		// A friend's move put the object at a_pose: where it rests now, and what our game sees of it settling there (its
		// own physics) is not ours to report for a while.
		void Placed(std::uint32_t a_ref, const Pose& a_pose, Clock::time_point a_now)
		{
			auto& w = watched[a_ref];
			w.rest = w.last = a_pose;
			w.moving = false;
			w.owed = false;
			w.byUs = w.byNpc = w.theirs = false;
			w.blast = 0;
			w.seenAt = a_now;
			w.quietUntil = a_now + QUIET_AFTER_PLACED;
		}

		[[nodiscard]] std::size_t Watching() const { return watched.size(); }
		[[nodiscard]] std::size_t Moving() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(watched, [](const auto& a_entry) { return a_entry.second.moving; }));
		}
		[[nodiscard]] std::uint64_t Reported() const { return reported; }

		void Reset()
		{
			watched.clear();
			blastReports.clear();
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
			bool              owed = false;  // at rest, to be reported when the budget has room
			std::uint32_t     blast = 0;
			Clock::time_point movingSince{};
			Clock::time_point stillSince{};
			Clock::time_point seenAt{};
			Clock::time_point quietUntil{};
			Clock::time_point owedUntil{};
		};

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
