// Power armor frames shared as NPCs are (FrameShare.h, as NpcSync runs it): which of ours are offered and kept, when
// their states go, and where a friend's puts ours; and the claims played against a real server, as NpcSync's scan
// makes them (NpcClaims::Book), with the frame's states relayed from its owner only.

#include "ServerFixture.h"

#include "game/FrameShare.h"
#include "game/NpcClaims.h"

#include <unordered_map>
#include <unordered_set>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using FrameShare::Move;
	using FrameShare::Ours;
	using FrameShare::Share;
	using FrameShare::Spot;
	using Reason = Protocol::ClaimReason;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t MUSEUM_SUIT = 0x0003F2B4;  // a frame reference every game has (placed by a plugin)
	constexpr std::uint32_t MADE_HERE = 0xFF000F96;    // one a game made (a dropped suit)
	constexpr std::uint32_t MUSEUM_CELL = 0x0001A2B3;

	constexpr Spot MUSEUM_ROOF{ 0, COMMONWEALTH, -18000.0f, 78000.0f, 1500.0f };
	constexpr Spot SANCTUARY_STATION{ 0, COMMONWEALTH, -80620.0f, 89560.0f, 3000.0f };

	// Ours standing in the station, loaded.
	constexpr Ours Standing()
	{
		return { .found = true, .frame = true, .loaded = true, .at = SANCTUARY_STATION, .heading = 1.0f };
	}

	// One player's game, as far as frames go: its id, what the server says of each frame, and its claims.
	struct Game
	{
		Fixture&                                         f;
		PeerId                                           peer;
		std::uint32_t                                    id = 0;
		bool                                             hosting = false;
		NpcClaims::Book                                  book;
		std::unordered_map<std::uint32_t, std::uint32_t> owners;
		std::unordered_set<std::uint32_t>                used;

		Game(Fixture& a_f, PeerId a_peer, std::string a_name, bool a_hosting) :
			f(a_f),
			peer(a_peer),
			hosting(a_hosting)
		{
			const auto welcome = f.Join(a_peer, Fixture::MakeHello(std::move(a_name)));
			REQUIRE(welcome);
			id = welcome->playerId;
			Pump(Clock::now());
		}

		[[nodiscard]] std::uint32_t OwnerOf(std::uint32_t a_ref) const
		{
			const auto it = owners.find(a_ref);
			return it == owners.end() ? 0 : it->second;
		}

		// What the server sent: the owners, applied as NpcSync::ApplyOwners does, and the frame states relayed.
		std::vector<Protocol::ActorState> Pump(Clock::time_point a_now)
		{
			std::vector<Protocol::ActorState> relayed;
			Sync(f, peer);
			for (const auto& packet : f.fake->Take(peer)) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kActorOwners)) {
					const auto batch = Protocol::DecodeOwners(packet);
					REQUIRE(batch);
					for (const auto& entry : *batch) {
						book.Answered(entry.refId, entry.playerId, id, a_now);
						if (entry.playerId == 0) {
							owners.erase(entry.refId);
						} else {
							owners[entry.refId] = entry.playerId;
						}
					}
				} else if (packet[0] == Type(MT::kActorStatesRelay)) {
					const auto states = Protocol::DecodeActorStates(packet);
					REQUIRE(states);
					relayed.insert(relayed.end(), states->begin(), states->end());
				}
			}
			return relayed;
		}

		// NpcSync::ScanFrames for one frame of ours at a_now. True if it claimed; the answer is then pumped in.
		bool Scan(std::uint32_t a_ref, const Ours& a_ours, Clock::time_point a_now)
		{
			const auto share = FrameShare::ChooseShare(a_ours);
			if (!FrameShare::Keeps(share) || OwnerOf(a_ref) == id || !FrameShare::Offers(share, hosting, used.contains(a_ref))) {
				book.Expire(a_now);
				return false;
			}
			const auto reason = book.Offer(a_ref, OwnerOf(a_ref), id, false, true, a_now);
			if (reason) {
				f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, *reason } }));
				Pump(a_now);
			}
			book.Expire(a_now);
			return reason.has_value();
		}

		// Its player got into or out of the frame (FrameShare::Used): a takeover claim when another game runs it.
		void Uses(std::uint32_t a_ref, Clock::time_point a_now)
		{
			used.insert(a_ref);
			if (const auto owner = OwnerOf(a_ref); owner != 0 && owner != id) {
				f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, Reason::kInteract } }));
				book.Claimed(a_ref, Reason::kInteract, a_now);
				Pump(a_now);
			}
		}

		// Its game no longer has the frame (its reference is gone): released.
		void Releases(std::uint32_t a_ref)
		{
			f.fake->Deliver(peer, Protocol::EncodeRelease({ a_ref }));
			owners.erase(a_ref);
		}

		// Its game reports where the frame stands.
		void Reports(std::uint32_t a_ref, const Spot& a_at)
		{
			Protocol::ActorState state;
			state.refId = a_ref;
			state.cell = a_at.cell;
			state.worldspace = a_at.worldspace;
			state.x = a_at.x;
			state.y = a_at.y;
			state.z = a_at.z;
			f.fake->Deliver(peer, Protocol::Encode(std::vector<Protocol::ActorState>{ state }, MT::kActorStates));
		}
	};
}

TEST("frame share: a frame is ours to share standing loaded, worn by our player, or ours in a cell since unloaded")
{
	using FrameShare::ChooseShare;
	CHECK(ChooseShare(Standing()) == Share::kStands);
	auto ours = Standing();
	ours.wornHere = true;
	ours.off = true;  // the engine disables it while it is worn
	CHECK(ChooseShare(ours) == Share::kWorn);
	ours = Standing();
	ours.loaded = false;
	CHECK(ChooseShare(ours) == Share::kAway);
	// Not here, made here, not a frame, or disabled by something other than our player's wear: let go.
	CHECK(ChooseShare({}) == Share::kNo);
	ours = Standing();
	ours.found = false;
	CHECK(ChooseShare(ours) == Share::kNo);
	ours = Standing();
	ours.created = true;
	CHECK(ChooseShare(ours) == Share::kNo);
	ours = Standing();
	ours.frame = false;
	CHECK(ChooseShare(ours) == Share::kNo);
	ours = Standing();
	ours.off = true;
	CHECK(ChooseShare(ours) == Share::kNo);
	CHECK(FrameShare::Keeps(Share::kStands));
	CHECK(FrameShare::Keeps(Share::kWorn));
	CHECK(FrameShare::Keeps(Share::kAway));
	CHECK(!FrameShare::Keeps(Share::kNo));
}

TEST("frame share: the host offers every frame standing in its loaded cells, a joiner only one its player used")
{
	using FrameShare::Offers;
	CHECK(Offers(Share::kStands, true, false));
	CHECK(!Offers(Share::kStands, false, false));
	// Used here: offered by anyone, wherever it is now (worn, or in a cell since unloaded).
	CHECK(Offers(Share::kStands, false, true));
	CHECK(Offers(Share::kWorn, false, true));
	CHECK(Offers(Share::kAway, false, true));
	CHECK(Offers(Share::kWorn, true, true));
	// The host's game offers only what stands in its loaded cells: one away, or worn without having been used
	// (never: wearing is a use), is not found by its walk.
	CHECK(!Offers(Share::kAway, true, false));
	CHECK(!Offers(Share::kWorn, true, false));
	CHECK(!Offers(Share::kNo, true, true));
}

TEST("frame share: getting into or out of a frame is a use of it; otherwise the last used one")
{
	using FrameShare::Used;
	// As the session begins: not in one, the last used one is ours to offer.
	CHECK(Used(0, 0, MUSEUM_SUIT) == MUSEUM_SUIT);
	CHECK(Used(0, 0, 0) == 0);
	// Getting in, and out.
	CHECK(Used(0, MUSEUM_SUIT, MUSEUM_SUIT) == MUSEUM_SUIT);
	CHECK(Used(MUSEUM_SUIT, 0, MUSEUM_SUIT) == MUSEUM_SUIT);
	// Still in it: the one we are in (the last used one too).
	CHECK(Used(MUSEUM_SUIT, MUSEUM_SUIT, MUSEUM_SUIT) == MUSEUM_SUIT);
	// Joined while already in one.
	CHECK(Used(0, MUSEUM_SUIT, 0) == MUSEUM_SUIT);
}

TEST("frame share: a frame's state goes once a second, and at once when it moved or turned")
{
	using FrameShare::Due;
	using FrameShare::Moved;
	CHECK(Due(true, 0ms));
	CHECK(!Due(false, 999ms));
	CHECK(Due(false, FrameShare::STATE_INTERVAL));
	// Never sent, moved past a unit, turned past a hundredth of a turn, or in another place: news.
	CHECK(Moved(false, {}, 0.0f, SANCTUARY_STATION, 1.0f));
	CHECK(!Moved(true, SANCTUARY_STATION, 1.0f, SANCTUARY_STATION, 1.0f));
	auto nudged = SANCTUARY_STATION;
	nudged.x += 0.5f;
	CHECK(!Moved(true, SANCTUARY_STATION, 1.0f, nudged, 1.01f));
	nudged.x += 1.0f;
	CHECK(Moved(true, SANCTUARY_STATION, 1.0f, nudged, 1.0f));
	CHECK(Moved(true, SANCTUARY_STATION, 1.0f, SANCTUARY_STATION, 1.1f));
	CHECK(Moved(true, SANCTUARY_STATION, 6.28f, SANCTUARY_STATION, 0.0f) == false);  // a full turn round: the same way
	CHECK(Moved(true, SANCTUARY_STATION, 1.0f, { MUSEUM_CELL, 0, SANCTUARY_STATION.x, SANCTUARY_STATION.y, SANCTUARY_STATION.z }, 1.0f));
}

TEST("frame share: ours is moved to where its owner's game has it: in place when near, through the console when far, unloaded or elsewhere")
{
	using FrameShare::Decide;
	// The 2026-10-12 report: the host's suit in a Sanctuary station, the joiner's copy of that reference still on
	// the museum roof, in the same world but in a cell the joiner has not loaded.
	Ours ours{ .found = true, .frame = true, .loaded = false, .at = MUSEUM_ROOF };
	CHECK(Decide(ours, SANCTUARY_STATION, 1.0f, true) == Move::kBring);
	// Loaded far off in the same world (the joiner stands between the two): brought too.
	ours.loaded = true;
	CHECK(Decide(ours, SANCTUARY_STATION, 1.0f, true) == Move::kBring);
	// Loaded near (the frame they got out of, enabled some units from where they got in): moved in place.
	auto near = Standing();
	near.at.x += 76.0f;
	CHECK(Decide(near, SANCTUARY_STATION, 1.0f, true) == Move::kNudge);
	near.at.x = SANCTUARY_STATION.x + FrameShare::NUDGE_LIMIT;
	CHECK(Decide(near, SANCTUARY_STATION, 1.0f, true) == Move::kNudge);
	near.at.x += 1.0f;
	CHECK(Decide(near, SANCTUARY_STATION, 1.0f, true) == Move::kBring);
	// There already: nothing, in or out of our sight. Turned: moved in place.
	CHECK(Decide(Standing(), SANCTUARY_STATION, 1.0f, true) == Move::kThere);
	CHECK(Decide(Standing(), SANCTUARY_STATION, 1.0f, false) == Move::kThere);
	CHECK(Decide(Standing(), SANCTUARY_STATION, 1.5f, true) == Move::kNudge);
	// In another place (theirs stands in an interior), in view (we are in that interior): brought.
	const Spot inside{ MUSEUM_CELL, 0, 100.0f, 200.0f, 0.0f };
	CHECK(Decide(Standing(), inside, 1.0f, true) == Move::kBring);
	// Out of our sight: left where it is, whatever it would take.
	CHECK(Decide(ours, SANCTUARY_STATION, 1.0f, false) == Move::kOutOfView);
	CHECK(Decide(near, SANCTUARY_STATION, 1.0f, false) == Move::kOutOfView);
	// Not ours to move: none here, another kind of reference, disabled (our player wears it), a quest's, in use.
	CHECK(Decide({}, SANCTUARY_STATION, 1.0f, true) == Move::kMissing);
	auto facts = Standing();
	facts.created = true;
	CHECK(Decide(facts, SANCTUARY_STATION, 1.0f, true) == Move::kMissing);
	facts = Standing();
	facts.frame = false;
	CHECK(Decide(facts, SANCTUARY_STATION, 1.0f, true) == Move::kNotFrame);
	facts = ours;
	facts.off = true;
	CHECK(Decide(facts, SANCTUARY_STATION, 1.0f, true) == Move::kOff);
	facts = ours;
	facts.quest = true;
	CHECK(Decide(facts, SANCTUARY_STATION, 1.0f, true) == Move::kQuest);
	facts = ours;
	facts.inUse = true;
	CHECK(Decide(facts, SANCTUARY_STATION, 1.0f, true) == Move::kInUse);
}

TEST("frame share: the host's game runs a frame nobody used, and only its word on where it stands reaches the others")
{
	Fixture    f;
	Game       host{ f, 1, "Host", true };
	Game       joiner{ f, 2, "Joiner", false };
	const auto t0 = Clock::now();

	// Both have the museum's suit loaded (the host's in a station at Sanctuary, the joiner's on the museum roof): the
	// host's scan offers it, the joiner's does not.
	CHECK(!joiner.Scan(MUSEUM_SUIT, Standing(), t0));
	REQUIRE(host.Scan(MUSEUM_SUIT, Standing(), t0));
	CHECK(host.OwnerOf(MUSEUM_SUIT) == host.id);
	joiner.Pump(t0);
	CHECK(joiner.OwnerOf(MUSEUM_SUIT) == host.id);
	CHECK(!joiner.Scan(MUSEUM_SUIT, Standing(), t0 + 250ms));

	// The host's state for it is relayed to the joiner; the joiner's for the same frame goes nowhere.
	host.Reports(MUSEUM_SUIT, SANCTUARY_STATION);
	joiner.Reports(MUSEUM_SUIT, MUSEUM_ROOF);
	const auto toJoiner = joiner.Pump(t0 + 1s);
	REQUIRE(toJoiner.size() == 1);
	CHECK(toJoiner[0].refId == MUSEUM_SUIT);
	CHECK(toJoiner[0].x == SANCTUARY_STATION.x);
	CHECK(toJoiner[0].y == SANCTUARY_STATION.y);
	CHECK(host.Pump(t0 + 1s).empty());

	// A frame one game made is no reference of the others': never offered.
	CHECK(!joiner.Scan(MADE_HERE, Ours{ .found = true, .created = true, .frame = true, .loaded = true }, t0 + 1s));
	host.Scan(MADE_HERE, Ours{ .found = true, .created = true, .frame = true, .loaded = true }, t0 + 1s);
	CHECK(host.OwnerOf(MADE_HERE) == 0);
}

TEST("frame share: the last player to use a frame runs it")
{
	Fixture    f;
	Game       host{ f, 1, "Host", true };
	Game       joiner{ f, 2, "Joiner", false };
	const auto t0 = Clock::now();
	REQUIRE(host.Scan(MUSEUM_SUIT, Standing(), t0));
	joiner.Pump(t0);
	CHECK(joiner.OwnerOf(MUSEUM_SUIT) == host.id);

	// The joiner gets into their copy of it: a takeover, refused while the host's claim is moments old (the server's
	// cooldown, as for an NPC two players fight), and told who runs it.
	joiner.Uses(MUSEUM_SUIT, t0 + 1s);
	CHECK(joiner.OwnerOf(MUSEUM_SUIT) == host.id);

	// The host walks away from it (its cell unloads, and it never used it): released, and the joiner's game, which
	// used it, offers it on its next scan and has it from then on, wherever it stands.
	host.Releases(MUSEUM_SUIT);
	Sync(f, host.peer);
	joiner.Pump(t0 + 3s);
	CHECK(joiner.OwnerOf(MUSEUM_SUIT) == 0);
	auto worn = Standing();
	worn.wornHere = true;
	worn.off = true;
	REQUIRE(joiner.Scan(MUSEUM_SUIT, worn, t0 + 4s));
	CHECK(joiner.OwnerOf(MUSEUM_SUIT) == joiner.id);
	host.Pump(t0 + 4s);
	CHECK(host.OwnerOf(MUSEUM_SUIT) == joiner.id);
	// Out of it, the joiner's game says where it stands, and the host's game hears.
	joiner.Reports(MUSEUM_SUIT, SANCTUARY_STATION);
	const auto toHost = host.Pump(t0 + 5s);
	REQUIRE(toHost.size() == 1);
	CHECK(toHost[0].refId == MUSEUM_SUIT);
	CHECK(toHost[0].x == SANCTUARY_STATION.x);
	// The host's scan, with the frame standing in its cells again, leaves it to the joiner's game.
	CHECK(!host.Scan(MUSEUM_SUIT, Standing(), t0 + 5s));
	CHECK(host.OwnerOf(MUSEUM_SUIT) == joiner.id);
}
