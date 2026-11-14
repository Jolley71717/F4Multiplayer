// The badge over a friend's stand-in while they are in the Pip-Boy (game/StandInBadge.h): who wears it, when it is hung,
// taken off and hung again, and where it hangs; and that every way a stand-in goes takes it off (game/MenuBadges.cpp).

#include "Test.h"

#include "Protocol.h"
#include "SourceTree.h"
#include "game/StandInBadge.h"

#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using StandInBadge::Candidate;
	using StandInBadge::Do;
	using StandInBadge::Facts;
	using StandInBadge::Glyph;
	using StandInBadge::Track;
	using StandInBadge::NONE;

	constexpr float pi = std::numbers::pi_v<float>;
	constexpr float notANumber = std::numeric_limits<float>::quiet_NaN();
	constexpr float infinite = std::numeric_limits<float>::infinity();

	bool Near(float a_lhs, float a_rhs, float a_by = 0.001f) { return std::abs(a_lhs - a_rhs) < a_by; }

	Facts Wearing(std::uint32_t a_standIn = 0xFF001234, std::uintptr_t a_root = 0x1000) { return { .on = true, .standIn = a_standIn, .root = a_root, .model = true }; }
	Facts Nobody() { return { .on = true }; }

	std::size_t PickOf(std::vector<Candidate> a_candidates, std::uint32_t a_wearing = 0) { return StandInBadge::Pick(a_candidates, a_wearing); }
}

TEST("menu badge: a friend opening the Pip-Boy has it hung once, not each frame, and closing takes it off")
{
	Track track;
	CHECK(StandInBadge::Frame(track, Nobody()) == Do::kNothing);
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kHang);
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kNothing);
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kNothing);
	CHECK(track.hung);
	CHECK(StandInBadge::Frame(track, Nobody()) == Do::kTakeOff);
	CHECK(StandInBadge::Frame(track, Nobody()) == Do::kNothing);
	CHECK(!track.hung);
	CHECK(track.hangs == 1);
	CHECK(track.takeOffs == 1);
	CHECK(track.rehangs == 0);
}

TEST("menu badge: another stand-in, or the same one's 3D rebuilt, has it hung again")
{
	Track track;
	CHECK(StandInBadge::Frame(track, Wearing(1, 0x1000)) == Do::kHang);
	CHECK(StandInBadge::Frame(track, Wearing(1, 0x2000)) == Do::kRehang);  // dressed again, into its frame...
	CHECK(StandInBadge::Frame(track, Wearing(1, 0x2000)) == Do::kNothing);
	CHECK(StandInBadge::Frame(track, Wearing(2, 0x2000)) == Do::kRehang);  // placed afresh (a new form ID)
	CHECK(track.standIn == 2);
	CHECK(track.rehangs == 2);
	CHECK(track.hangs == 1);
	// Its 3D gone (a loading screen, a cell change): off, and on again once it is back.
	CHECK(StandInBadge::Frame(track, Wearing(2, 0)) == Do::kTakeOff);
	CHECK(StandInBadge::Frame(track, Wearing(2, 0x3000)) == Do::kHang);
}

TEST("menu badge: one taken off by something else on the same root is hung again, not left off until the menu closes")
{
	Track track;
	CHECK(StandInBadge::Frame(track, Wearing(1, 0x1000)) == Do::kHang);
	auto lost = Wearing(1, 0x1000);
	lost.lost = true;
	CHECK(StandInBadge::Frame(track, lost) == Do::kRehang);
	CHECK(StandInBadge::Frame(track, Wearing(1, 0x1000)) == Do::kNothing);
	CHECK(track.rehangs == 1);
	CHECK(track.lost == 1);
	// Lost on a frame it moves to another stand-in anyway: one rehang, counted as the move.
	auto moved = Wearing(2, 0x2000);
	moved.lost = true;
	CHECK(StandInBadge::Frame(track, moved) == Do::kRehang);
	CHECK(track.rehangs == 2);
	CHECK(track.lost == 1);
	// Lost as the menu closes: just off.
	auto closed = Nobody();
	closed.lost = true;
	CHECK(StandInBadge::Frame(track, closed) == Do::kTakeOff);
	CHECK(StandInBadge::Frame(track, closed) == Do::kNothing);
	CHECK(track.hangs == 1);
}

TEST("menu badge: off, or with no model, nothing is hung, and turning it off takes it off")
{
	Track track;
	auto  off = Wearing();
	off.on = false;
	CHECK(StandInBadge::Frame(track, off) == Do::kNothing);
	auto noModel = Wearing();
	noModel.model = false;
	CHECK(StandInBadge::Frame(track, noModel) == Do::kNothing);
	CHECK(track.hangs == 0);
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kHang);
	CHECK(StandInBadge::Frame(track, off) == Do::kTakeOff);
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kHang);
	CHECK(StandInBadge::Frame(track, noModel) == Do::kTakeOff);
}

TEST("menu badge: one the engine did not take is tried again, and one taken off outside a frame is not taken off twice")
{
	Track track;
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kHang);
	StandInBadge::Failed(track);
	CHECK(!track.hung);
	CHECK(track.failures == 1);
	CHECK(StandInBadge::Frame(track, Wearing()) == Do::kHang);
	// Deleted with its stand-in (or before a save): off once, and the next frame has nothing to take off.
	StandInBadge::TookOff(track);
	StandInBadge::TookOff(track);
	CHECK(track.takeOffs == 1);
	CHECK(StandInBadge::Frame(track, Nobody()) == Do::kNothing);
	CHECK(track.takeOffs == 1);
}

TEST("menu badge: the nearest friend in the Pip-Boy wears it, and only in the Pip-Boy for now")
{
	CHECK(PickOf({}) == NONE);
	CHECK(PickOf({ { 1, Glyph::kPipboy, 900.0f }, { 2, Glyph::kPipboy, 300.0f }, { 3, Glyph::kPipboy, 600.0f } }) == 1);
	// Paused or in another menu: their name tag says so, and no badge (there is no model for those yet).
	CHECK(PickOf({ { 1, Glyph::kPause, 100.0f }, { 2, Glyph::kBusy, 100.0f }, { 3, Glyph::kNone, 100.0f } }) == NONE);
	CHECK(PickOf({ { 1, Glyph::kBusy, 100.0f }, { 2, Glyph::kPipboy, 800.0f } }) == 1);
	CHECK(!StandInBadge::HasModel(Glyph::kPause));
	CHECK(!StandInBadge::HasModel(Glyph::kBusy));
	CHECK(!StandInBadge::HasModel(Glyph::kNone));
	CHECK(StandInBadge::HasModel(Glyph::kPipboy));
	CHECK(StandInBadge::HasModel(MenuIconPlace::GlyphOf(Protocol::MenuKind::kPipboy)));
	// No stand-in, or a distance that is not one: never.
	CHECK(PickOf({ { 0, Glyph::kPipboy, 100.0f } }) == NONE);
	CHECK(PickOf({ { 1, Glyph::kPipboy, notANumber }, { 2, Glyph::kPipboy, -5.0f }, { 3, Glyph::kPipboy, infinite } }) == NONE);
	// The one wearing it keeps it against one just as near; one nearer takes it.
	CHECK(PickOf({ { 1, Glyph::kPipboy, 500.0f }, { 2, Glyph::kPipboy, 500.0f } }, 2) == 1);
	CHECK(PickOf({ { 1, Glyph::kPipboy, 499.0f }, { 2, Glyph::kPipboy, 500.0f } }, 2) == 0);
}

TEST("menu badge: beyond reach it comes off, a little further for the one wearing it, and back on nearer")
{
	constexpr float edge = StandInBadge::MAX_DISTANCE;
	CHECK(PickOf({ { 1, Glyph::kPipboy, edge } }) == 0);
	CHECK(PickOf({ { 1, Glyph::kPipboy, edge + 1.0f } }) == NONE);
	CHECK(PickOf({ { 1, Glyph::kPipboy, edge + 1.0f } }, 1) == 0);  // wearing it: not off at the very edge
	CHECK(PickOf({ { 1, Glyph::kPipboy, edge + StandInBadge::KEEP_MARGIN } }, 1) == 0);
	CHECK(PickOf({ { 1, Glyph::kPipboy, edge + StandInBadge::KEEP_MARGIN + 1.0f } }, 1) == NONE);
	CHECK(PickOf({ { 1, Glyph::kPipboy, edge + 1.0f } }, 2) == NONE);  // the margin is the wearer's only

	// Walked off and back, frame by frame.
	Track track;
	const auto frame = [&](float a_distance) {
		const std::vector<Candidate> candidates{ { 7, Glyph::kPipboy, a_distance } };
		const auto pick = StandInBadge::Pick(candidates, track.hung ? track.standIn : 0);
		return StandInBadge::Frame(track, pick == NONE ? Nobody() : Wearing(7));
	};
	CHECK(frame(1000.0f) == Do::kHang);
	CHECK(frame(edge + 100.0f) == Do::kNothing);
	CHECK(frame(edge + StandInBadge::KEEP_MARGIN + 50.0f) == Do::kTakeOff);
	CHECK(frame(edge + 100.0f) == Do::kNothing);  // not back on until within reach
	CHECK(frame(edge - 10.0f) == Do::kHang);
}

TEST("menu badge: it turns to face the camera from every side, the turn kept within half a circle each way")
{
	// A root facing north (+Y).
	const auto north = StandInBadge::Turn(0.0f);
	CHECK(Near(StandInBadge::FacingYaw(north, 0.0f, 500.0f), 0.0f));
	CHECK(Near(StandInBadge::FacingYaw(north, 500.0f, 0.0f), pi / 2));
	CHECK(Near(StandInBadge::FacingYaw(north, 0.0f, -500.0f), pi));
	CHECK(Near(StandInBadge::FacingYaw(north, -500.0f, 0.0f), -pi / 2));
	// Facing east, the camera north: a quarter turn back.
	CHECK(Near(StandInBadge::FacingYaw(StandInBadge::Turn(pi / 2), 0.0f, 500.0f), -pi / 2));
	// Within half a circle each way, whatever the heading (the engine's can be any angle).
	for (const float heading : { -7.0f, -3.1f, -pi, 0.0f, 3.1f, pi, 7.0f, 100.0f }) {
		for (const auto& [x, y] : { std::pair{ 0.0f, 1.0f }, std::pair{ 1.0f, 0.0f }, std::pair{ 0.0f, -1.0f }, std::pair{ -1.0f, 0.0f }, std::pair{ -1.0f, -0.001f } }) {
			const auto actor = StandInBadge::Turn(heading);
			const float yaw = StandInBadge::FacingYaw(actor, 400.0f * x, 400.0f * y);
			CHECK(yaw > -pi);
			CHECK(yaw <= pi);
			// Turned by its root, then by its own turn: its front (+Y) points at the camera.
			const auto badge = StandInBadge::Turn(yaw);
			const float fx = actor[0][0] * badge[0][1] + actor[0][1] * badge[1][1];
			const float fy = actor[1][0] * badge[0][1] + actor[1][1] * badge[1][1];
			if (!Near(fx, x, 0.01f) || !Near(fy, y, 0.01f)) {
				Test::Fail(__FILE__, __LINE__, std::format("heading {} camera ({}, {}): the front points at ({}, {})", heading, x, y, fx, fy));
			}
		}
	}
	// Straight under the camera, a way to it that is not a number, or a root that is no rotation: as the root faces.
	CHECK(StandInBadge::FacingYaw(north, -0.2f, 0.1f) == 0.0f);
	CHECK(StandInBadge::FacingYaw(north, notANumber, 10.0f) == 0.0f);
	CHECK(StandInBadge::FacingYaw(north, 10.0f, infinite) == 0.0f);
	StandInBadge::Matrix broken{};
	CHECK(StandInBadge::FacingYaw(broken, 10.0f, 10.0f) == 0.0f);
	broken[0][0] = notANumber;
	CHECK(StandInBadge::FacingYaw(broken, 10.0f, 10.0f) == 0.0f);
}

TEST("menu badge: the turn sends the front (+Y) as the engine's heading does")
{
	const auto east = StandInBadge::Turn(pi / 2);
	CHECK(Near(east[0][1], 1.0f));  // +Y to +X
	CHECK(Near(east[1][1], 0.0f));
	CHECK(Near(east[2][2], 1.0f));
	const auto none = StandInBadge::Turn(0.0f);
	CHECK(Near(none[0][0], 1.0f));
	CHECK(Near(none[1][1], 1.0f));
	CHECK(Near(none[0][1], 0.0f));
}

TEST("menu badge: it hangs over the head, or over the feet with no head found, in the root's own units")
{
	using MenuIconPlace::OVER_FEET;
	using MenuIconPlace::OVER_HEAD;
	CHECK(Near(StandInBadge::Height(true, 1120.0f, 1000.0f, 1.0f), 120.0f + OVER_HEAD));
	CHECK(Near(StandInBadge::Height(false, 0.0f, 1000.0f, 1.0f), OVER_FEET));
	// A stand-in at twice the size: the head is twice as high over the feet, and as many root units.
	CHECK(Near(StandInBadge::Height(true, 1240.0f, 1000.0f, 2.0f), (240.0f + OVER_HEAD) / 2.0f));
	// A head under the feet, far over them or not a number is none.
	CHECK(Near(StandInBadge::Height(true, 990.0f, 1000.0f, 1.0f), OVER_FEET));
	CHECK(Near(StandInBadge::Height(true, 1000.0f + StandInBadge::MAX_HEAD + 1.0f, 1000.0f, 1.0f), OVER_FEET));
	CHECK(Near(StandInBadge::Height(true, notANumber, 1000.0f, 1.0f), OVER_FEET));
	// A root scale that is no scale is taken as 1.
	CHECK(Near(StandInBadge::Height(true, 1120.0f, 1000.0f, 0.0f), 120.0f + OVER_HEAD));
	CHECK(Near(StandInBadge::Height(false, 0.0f, 1000.0f, notANumber), OVER_FEET));
}

TEST("menu badge: its size comes from the model's own, within limits, and stays as it is for a radius not known")
{
	CHECK(Near(StandInBadge::ScaleFor(StandInBadge::SIZE), 1.0f));
	CHECK(Near(StandInBadge::ScaleFor(StandInBadge::SIZE * 2.0f), 0.5f));
	CHECK(Near(StandInBadge::ScaleFor(1e9f), StandInBadge::MIN_SCALE));
	CHECK(Near(StandInBadge::ScaleFor(0.001f), StandInBadge::MAX_SCALE));
	for (const float unknown : { 0.0f, -3.0f, notANumber, infinite }) {
		CHECK(StandInBadge::ScaleFor(unknown) == 1.0f);
	}
	CHECK(Near(StandInBadge::OwnRadius(30.0f, 2.0f), 15.0f));
	CHECK(StandInBadge::OwnRadius(30.0f, 0.0f) == 0.0f);
	CHECK(StandInBadge::OwnRadius(notANumber, 1.0f) == 0.0f);
	CHECK(StandInBadge::OwnRadius(0.0f, 1.0f) == 0.0f);
	// Under a root at twice the size, half as much of its own: the same size in the world.
	CHECK(Near(StandInBadge::LocalScale(StandInBadge::SIZE, 2.0f), 0.5f));
	CHECK(Near(StandInBadge::LocalScale(StandInBadge::SIZE, notANumber), 1.0f));
	// Worked out again each frame from what the engine last drew, it does not creep: the world radius it gives back is
	// the same own radius.
	const float own = 20.0f;
	const float scale = StandInBadge::LocalScale(own, 1.3f);
	CHECK(Near(StandInBadge::OwnRadius(own * scale * 1.3f, scale * 1.3f), own));
}

TEST("menu badge: a model with no collision hangs as it is; with collision it is refused, or copied with the dev switch")
{
	using StandInBadge::ModelState;
	using StandInBadge::Scan;
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = 12 }, false) == ModelState::kShared);
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = 12 }, true) == ModelState::kShared);  // no copy needed
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = 12, .collisions = 1 }, false) == ModelState::kCollision);
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = 12, .collisions = 1 }, true) == ModelState::kCopy);
	// Not walked all through: refused whatever was found so far and whatever the switch, and told apart from collision.
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = StandInBadge::MAX_NODES, .capped = true }, false) == ModelState::kNodeCap);
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = StandInBadge::MAX_NODES, .capped = true }, true) == ModelState::kNodeCap);
	CHECK(StandInBadge::ModelStateFor(Scan{ .nodes = StandInBadge::MAX_NODES, .collisions = 3, .capped = true }, true) == ModelState::kNodeCap);
}

TEST("menu badge: collision comes off our copy only where the copy alone holds it and it is that node's own")
{
	CHECK(StandInBadge::MayTakeOff(1, true));
	CHECK(!StandInBadge::MayTakeOff(2, true));  // shared with the database's node, or held by something else
	CHECK(!StandInBadge::MayTakeOff(1, false));  // names another node as its own
	CHECK(!StandInBadge::MayTakeOff(0, true));  // a count that cannot be: left alone
}

TEST("menu badge: a copy that failed or still has collision is refused, and each refusal says which")
{
	using StandInBadge::ModelState;
	using StandInBadge::Scan;
	CHECK(StandInBadge::CopyStateFor(true, 0, Scan{ .nodes = 12 }) == ModelState::kCopy);
	CHECK(StandInBadge::CopyStateFor(false, 0, Scan{}) == ModelState::kCopyFailed);
	CHECK(StandInBadge::CopyStateFor(true, 1, Scan{ .nodes = 12, .collisions = 1 }) == ModelState::kCopyFailed);
	CHECK(StandInBadge::CopyStateFor(true, 1, Scan{ .nodes = 12 }) == ModelState::kCopyFailed);  // one left, though none seen after
	CHECK(StandInBadge::CopyStateFor(true, 0, Scan{ .nodes = 12, .collisions = 1 }) == ModelState::kCopyCollision);
	CHECK(StandInBadge::CopyStateFor(true, 0, Scan{ .nodes = StandInBadge::MAX_NODES, .capped = true }) == ModelState::kNodeCap);

	// Only the database's node with no collision and our clean copy are hung; every state reads differently.
	const std::vector<ModelState> states{ ModelState::kNotAsked, ModelState::kNoForm, ModelState::kNoWorldModel, ModelState::kLoadFailed,
		ModelState::kShared, ModelState::kCollision, ModelState::kNodeCap, ModelState::kCopyFailed, ModelState::kCopyCollision, ModelState::kCopy };
	for (std::size_t i = 0; i < states.size(); ++i) {
		CHECK(StandInBadge::Usable(states[i]) == (states[i] == ModelState::kShared || states[i] == ModelState::kCopy));
		CHECK(!StandInBadge::Name(states[i]).empty());
		CHECK(StandInBadge::Name(states[i]) != "?");
		for (std::size_t j = i + 1; j < states.size(); ++j) {
			CHECK(StandInBadge::Name(states[i]) != StandInBadge::Name(states[j]));
		}
	}
}

namespace
{
	std::size_t Count(std::string_view a_code, std::string_view a_call)
	{
		std::size_t count = 0;
		for (auto at = a_code.find(a_call); at != std::string_view::npos; at = a_code.find(a_call, at + a_call.size())) {
			++count;
		}
		return count;
	}
}

// The badge holds a stand-in's 3D root while it hangs: every place the grenade in a stand-in's hand comes off (its
// deletion, a save or a load, the session's end) takes the badge off too. (NpcSync drops the grenade of our copies of
// friends' NPCs, which never wear the badge.)
TEST("menu badge: every way a stand-in goes that takes the grenade out of its hand takes the badge off too")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	std::size_t drops = 0;
	std::size_t dropAlls = 0;
	for (const auto& [file, code] : SourceTree::PluginCode(*root)) {
		if (file.starts_with("src/game/WeaponFire") || file.starts_with("src/game/MenuBadges")) {
			continue;  // where they are written
		}
		const auto one = file == "src/game/RemotePlayers.cpp" ? Count(code, "WeaponFire::DropHandThrowable(") : 0;
		const auto all = Count(code, "WeaponFire::DropHandThrowables(");
		if (Count(code, "MenuBadges::Drop(") != one) {
			Test::Fail(__FILE__, __LINE__, std::format("{} drops a stand-in's grenade {} time(s) but its badge (MenuBadges::Drop) not as often", file, one));
		}
		if (Count(code, "MenuBadges::DropAll(") != all) {
			Test::Fail(__FILE__, __LINE__, std::format("{} drops every grenade {} time(s) but not the badge (MenuBadges::DropAll) as often", file, all));
		}
		drops += one;
		dropAlls += all;
	}
	CHECK(drops >= 1);     // a stand-in deleted
	CHECK(dropAlls >= 2);  // before a save or a load, and the session's end
}

namespace
{
	std::string BadgeCode()
	{
		const auto root = SourceTree::Root();
		if (!root) {
			return {};
		}
		for (const auto& [file, code] : SourceTree::PluginCode(*root)) {
			if (file == "src/game/MenuBadges.cpp") {
				return std::string{ code };
			}
		}
		return {};
	}
}

// The model is the database's one node, which the engine clones whenever it shows a Pip-Boy (the inventory's preview, one
// lying in the world): a copy takes its transform as it is then, so only our own node's transform is ever set. (With
// `menubadges copy on` the model is our own copy instead; its transform is left alone all the same.)
TEST("menu badge: the shared Pip-Boy model's transform is never changed, only that of our own node it hangs under")
{
	const auto code = BadgeCode();
	REQUIRE(!code.empty());
	for (auto at = code.find("->local"); at != std::string::npos; at = code.find("->local", at + 1)) {
		const auto start = code.rfind('\n', at);
		const auto line = std::string_view{ code }.substr(start == std::string::npos ? 0 : start + 1, at - (start == std::string::npos ? 0 : start + 1));
		if (!line.ends_with("holder")) {
			Test::Fail(__FILE__, __LINE__, std::format("MenuBadges.cpp touches a transform other than our node's: '{}->local'", line));
		}
	}
	CHECK(Count(code, "model->world =") == 0);
	CHECK(Count(code, "badge->world =") == 0);
	CHECK(Count(code, "new RE::NiNode(") == 1);
}

TEST("menu badge: our node is placed before it goes on the root, so the first frame does not draw it where it last was")
{
	const auto code = BadgeCode();
	REQUIRE(!code.empty());
	const auto place = code.find("Place(a_actor, root);");
	const auto attach = code.find("root->AttachChild(holder.get()");
	REQUIRE(place != std::string::npos);
	REQUIRE(attach != std::string::npos);
	CHECK(place < attach);
	CHECK(code.find("holder->Update(", attach) != std::string::npos);
}

// The copy is made as the engine clones a node: CreateClone and then ProcessClone, both on the source. Round 3 of the
// grenade in the hand called ProcessClone on the clone, with a zero-filled cloning process, and crashed in
// BSGeometry::ProcessClone (docs/GRENADE-IN-HAND.md).
TEST("menu badge: our copy is cloned as the engine clones, ProcessClone on the source after CreateClone")
{
	const auto code = BadgeCode();
	REQUIRE(!code.empty());
	const auto create = code.find("a_source->CreateClone(cloning)");
	const auto process = code.find("a_source->ProcessClone(cloning)");
	REQUIRE(create != std::string::npos);
	REQUIRE(process != std::string::npos);
	CHECK(create < process);
	CHECK(Count(code, "->CreateClone(") == 1);
	CHECK(Count(code, "->ProcessClone(") == 1);
	CHECK(Count(code, "cloning.copyType = RE::NiCloningProcess::CopyType::kCopyExact;") == 1);
	CHECK(Count(code, "cloning.appendChar = ") == 1);  // '$', as the engine's constructor (the scan blanks literals)
	CHECK(Count(code, "cloning.scale = { 1.0f, 1.0f, 1.0f };") == 1);
}

// Collision comes off our copy alone, never the database's node, which the engine shares with every Pip-Boy it shows
// (one lying in the world keeps its collision); and one copy is made per load of the model, never per hang, so hanging
// it again does not pile up copies. The copy stays a dev switch, off at every start.
TEST("menu badge: collision is taken off our copy only, one copy per load of the model, and the copy switch starts off")
{
	const auto code = BadgeCode();
	REQUIRE(!code.empty());
	CHECK(Count(code, "TakeOffCollision(") == 2);  // written, and called once
	CHECK(Count(code, "TakeOffCollision(copy.get())") == 1);
	CHECK(Count(code, "std::exchange(a_object->collisionObject,") == 1);
	CHECK(Count(code, "collisionObject.reset(") == 0);
	CHECK(Count(code, "collisionObject =") == 0);
	CHECK(Count(code, "SetSceneGraphObject(") == 0);
	// The one write to a collision pointer is inside TakeOffCollision.
	const auto taker = code.find("std::size_t TakeOffCollision(");
	const auto write = code.find("std::exchange(a_object->collisionObject,");
	const auto next = code.find("RE::NiNode* Model()");
	REQUIRE(taker != std::string::npos);
	REQUIRE(next != std::string::npos);
	CHECK(taker < write);
	CHECK(write < next);
	CHECK(Count(code, "CopyOf(") == 2);  // written, and called once, in Model, which asks once until ForgetModel
	CHECK(code.find("CopyOf(loaded.get())") > next);
	CHECK(Count(code, "std::atomic<bool> copyWanted{ false };") == 1);
	CHECK(Count(code, "copyWanted.store(") == 1);  // SetCopy, from `menubadges copy`
	// A model let go only with nothing hung (ForgetModel after the badge came off).
	const auto forget = code.find("ForgetModel();");
	REQUIRE(forget != std::string::npos);
	const auto tookOff = code.rfind("StandInBadge::TookOff(track);", forget);
	CHECK(tookOff != std::string::npos);
}
