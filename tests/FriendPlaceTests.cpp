#include "Test.h"

#include "FormTranslate.h"
#include "ProtocolSamples.h"
#include "game/FriendPlace.h"

#include <map>
#include <optional>
#include <vector>

// A friend who goes somewhere our game does not have (a DLC or mod): the server relays their state, the form
// translation at our edge leaves it out (it names a cell or worldspace we cannot name), and Session tells RemotePlayers
// whose it was. They are then nowhere we can reach, not where their last readable state put them; the first state of
// theirs that we can read puts them back. The steps below are what Session::Receive and RemotePlayers::TakeState do, on
// the real packets and the real translation.
namespace
{
	using namespace ProtocolSamples;
	using FormTranslate::Result;

	// Ours: Fallout4.esm, Shared.esp. The session's: Fallout4.esm, Shared.esp, Theirs.esp (we lack it).
	const FormIds::Mapper MISSING(Protocol::ComparedLoadOrder({ "Fallout4.esm", "Shared.esp" }, {}), Protocol::ComparedLoadOrder({ "Fallout4.esm", "Shared.esp", "Theirs.esp" }, {}));
	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;  // Fallout4.esm: the same in every game
	constexpr std::uint32_t SESSION_THEIRS = 0x02000800;  // a worldspace of Theirs.esp, in the session's numbering
	constexpr std::uint32_t SESSION_THEIRS_CELL = 0x02000801;

	PlayerState At(std::uint32_t a_seq, std::uint32_t a_cell, std::uint32_t a_worldspace, float a_x)
	{
		auto state = MakeState(a_seq);
		state.cell = a_cell;
		state.worldspace = a_worldspace;
		state.x = a_x;
		return state;
	}

	// What our game knows of one friend, as RemotePlayers keeps it.
	struct Friend
	{
		std::optional<PlayerState> last;
		FriendPlace::Track         place;

		std::optional<PlayerState> Where() const { return place.Where(last); }
	};

	// One batch from the server, through our edge, then into what we keep of each friend.
	Result Receive(std::map<std::uint32_t, Friend>& a_friends, const std::vector<RemoteState>& a_batch)
	{
		const auto packet = Encode(PlayerStates{ 1, a_batch });
		auto       result = FormTranslate::Translate(packet, MISSING, FormTranslate::Way::kToLocal);
		const auto batch = DecodePlayerStates(result.kind == Result::Kind::kChanged ? result.packet : packet);
		REQUIRE(batch);
		for (const auto id : result.placeless) {
			a_friends[id].place.Unplaced();
		}
		for (const auto& entry : batch->players) {
			a_friends[entry.playerId].last = entry.state;
			a_friends[entry.playerId].place.Placed();
		}
		return result;
	}
}

TEST("friend place: a friend's state in a place we lack is named, and the others' states come through")
{
	std::map<std::uint32_t, Friend> friends;
	const auto result = Receive(friends, { { 2, At(1, 0, COMMONWEALTH, 100.0f) }, { 3, At(1, 0, SESSION_THEIRS, 5.0f) }, { 4, At(1, SESSION_THEIRS_CELL, 0, 6.0f) } });
	CHECK(result.placeless == std::vector<std::uint32_t>({ 3, 4 }));
	CHECK(result.entries == 2);
	CHECK(friends[2].Where().has_value());
	CHECK(friends[2].Where()->x == 100.0f);
	CHECK(!friends[3].Where().has_value());
	CHECK(!friends[4].Where().has_value());

	// Nothing named when nothing is out: the same load order is not decoded at all.
	const FormIds::Mapper same(Protocol::ComparedLoadOrder({ "Fallout4.esm" }, {}), Protocol::ComparedLoadOrder({ "Fallout4.esm" }, {}));
	CHECK(FormTranslate::Translate(Encode(PlayerStates{ 1, { { 2, At(1, 0, COMMONWEALTH, 1.0f) } } }), same, FormTranslate::Way::kToLocal).placeless.empty());
}

TEST("friend place: a friend who goes to a place we lack is no longer where they last were, and back at once when they return")
{
	std::map<std::uint32_t, Friend> friends;
	Receive(friends, { { 2, At(1, 0, COMMONWEALTH, 100.0f) } });
	REQUIRE(friends[2].Where());
	CHECK(friends[2].Where()->worldspace == COMMONWEALTH);
	CHECK(friends[2].Where()->x == 100.0f);

	// They go through the door into the DLC: every state from then on names it.
	for (std::uint32_t seq = 2; seq < 6; ++seq) {
		Receive(friends, { { 2, At(seq, 0, SESSION_THEIRS, 9000.0f + static_cast<float>(seq)) } });
		CHECK(!friends[2].Where());
		CHECK(friends[2].place.beyond);
	}
	// What was last readable is kept, but not given out as where they are.
	CHECK(friends[2].last.has_value());
	CHECK(friends[2].last->x == 100.0f);

	// Back in the Commonwealth: the first state puts them there, at its place.
	Receive(friends, { { 2, At(6, 0, COMMONWEALTH, 250.0f) } });
	REQUIRE(friends[2].Where());
	CHECK(friends[2].Where()->x == 250.0f);
	CHECK(!friends[2].place.beyond);
}

TEST("friend place: a friend who goes between two places we lack stays out of reach, and one who goes to another we have is reachable")
{
	std::map<std::uint32_t, Friend> friends;
	Receive(friends, { { 2, At(1, 0, SESSION_THEIRS, 1.0f) } });
	CHECK(!friends[2].Where());
	Receive(friends, { { 2, At(2, SESSION_THEIRS_CELL, 0, 2.0f) } });  // an interior of the same DLC
	CHECK(!friends[2].Where());
	Receive(friends, { { 2, At(3, 0x0001A2B3, 0, 3.0f) } });  // an interior of the base game
	REQUIRE(friends[2].Where());
	CHECK(friends[2].Where()->cell == 0x0001A2B3u);
}

TEST("friend place: states that stop coming change nothing, that is not a place we lack")
{
	std::map<std::uint32_t, Friend> friends;
	Receive(friends, { { 2, At(1, 0, COMMONWEALTH, 100.0f) } });
	// A lagging or a lost friend: later batches hold other players, or nothing of theirs.
	Receive(friends, { { 3, At(1, 0, COMMONWEALTH, 5.0f) } });
	Receive(friends, {});
	CHECK(!friends[2].place.beyond);
	REQUIRE(friends[2].Where());
	CHECK(friends[2].Where()->x == 100.0f);

	// A friend we have heard nothing from has no place at all (still loading), reachable or not.
	const FriendPlace::Track fresh;
	CHECK(!fresh.beyond);
	CHECK(!fresh.Where(std::optional<PlayerState>{}));
}
