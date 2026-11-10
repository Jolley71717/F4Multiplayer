// One owner per companion, played against a real server (common/CompanionBook.h in Server.cpp, protocol 65): the first
// game to report recruiting an NPC keeps it, the second is told whose it is, a dismissal frees it, an owner who leaves
// has it held for a rejoin, and the AI claims follow the book (the owner's game runs its companion).

#include "ServerFixture.h"

#include "CompanionBook.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Reason = Protocol::ClaimReason;
	using State = Protocol::CompanionState;

	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t PIPER = 0x0002F1F8;

	auto Recruit(std::uint32_t a_ref) { return Protocol::Encode(Protocol::CompanionReport{ a_ref, true }); }
	auto LetGo(std::uint32_t a_ref) { return Protocol::Encode(Protocol::CompanionReport{ a_ref, false }); }
	auto Claim(std::uint32_t a_ref, Reason a_reason) { return Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } }); }

	// The owner's words a_peer received, in order.
	std::vector<Protocol::CompanionOwner> Words(Fixture& a_f, PeerId a_peer)
	{
		std::vector<Protocol::CompanionOwner> out;
		for (const auto& packet : Received(a_f, a_peer)) {
			if (!packet.empty() && packet[0] == Type(MT::kCompanionOwner)) {
				if (const auto word = Protocol::DecodeCompanionOwner(packet)) {
					out.push_back(*word);
				}
			}
		}
		return out;
	}

	// The last word about a_ref among a_words (nothing: none).
	std::optional<Protocol::CompanionOwner> LastWord(const std::vector<Protocol::CompanionOwner>& a_words, std::uint32_t a_ref)
	{
		std::optional<Protocol::CompanionOwner> last;
		for (const auto& word : a_words) {
			if (word.refId == a_ref) {
				last = word;
			}
		}
		return last;
	}

	// The last word about a_ref a_peer received (nothing: none). It takes everything the peer received: for two NPCs at
	// once, take Words first.
	std::optional<Protocol::CompanionOwner> LastWord(Fixture& a_f, PeerId a_peer, std::uint32_t a_ref)
	{
		return LastWord(Words(a_f, a_peer), a_ref);
	}

	// Who the server says runs a_ref, by the owner lists a_peer received last (nothing: no list named it).
	std::optional<std::uint32_t> RunnerSaid(Fixture& a_f, PeerId a_peer, std::uint32_t a_ref)
	{
		std::optional<std::uint32_t> last;
		for (const auto& packet : Received(a_f, a_peer)) {
			if (packet.empty() || packet[0] != Type(MT::kActorOwners)) {
				continue;
			}
			if (const auto owners = Protocol::DecodeOwners(packet)) {
				for (const auto& owner : *owners) {
					if (owner.refId == a_ref) {
						last = owner.playerId;
					}
				}
			}
		}
		return last;
	}
}

TEST("server companions: the first to recruit keeps the companion; the second is told whose it is, and everyone hears the owner")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	const auto carol = JoinAs(f, 3, "Carol", 0x33);
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(1, Recruit(DOGMEAT));
	auto word = LastWord(f, 1, DOGMEAT);
	REQUIRE(word);
	CHECK(word->playerId == host && word->state == State::kOwner);
	// Bob and Carol hear whose it is.
	for (const PeerId peer : { 2, 3 }) {
		word = LastWord(f, peer, DOGMEAT);
		REQUIRE(word);
		CHECK(word->playerId == host && word->state == State::kOwner);
	}
	CHECK(f.LoggedSoon(std::format("server: {:08X} is player {}'s companion", DOGMEAT, host)));

	// Bob recruits Dogmeat too: refused, told it is the host's; nobody else hears anything new.
	f.fake->Deliver(2, Recruit(DOGMEAT));
	word = LastWord(f, 2, DOGMEAT);
	REQUIRE(word);
	CHECK(word->playerId == host && word->state == State::kOwner);
	CHECK(f.LoggedSoon(std::format("server: player {} recruited {:08X}: refused, it is player {}'s companion", bob, DOGMEAT, host)));
	CHECK(Words(f, 1).empty());
	CHECK(Words(f, 3).empty());
	(void)carol;
}

TEST("server companions: the owner letting the companion go frees it for everyone, and a non-owner's let-go changes nothing")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Recruit(DOGMEAT));
	Clear(f, { 1, 2 });

	f.fake->Deliver(2, LetGo(DOGMEAT));  // not his to let go
	CHECK(Words(f, 1).empty());
	CHECK(Words(f, 2).empty());
	f.fake->Deliver(2, Recruit(DOGMEAT));
	CHECK(LastWord(f, 2, DOGMEAT)->playerId == host);

	f.fake->Deliver(1, LetGo(DOGMEAT));
	for (const PeerId peer : { 1, 2 }) {
		const auto word = LastWord(f, peer, DOGMEAT);
		REQUIRE(word);
		CHECK(word->state == State::kFree && word->playerId == 0);
	}
	CHECK(f.LoggedSoon(std::format("server: player {} let {:08X} go", host, DOGMEAT)));
	// Free: Bob's next recruit is granted.
	f.fake->Deliver(2, Recruit(DOGMEAT));
	const auto word = LastWord(f, 2, DOGMEAT);
	REQUIRE(word);
	CHECK(word->playerId == bob && word->state == State::kOwner);
	CHECK(LastWord(f, 1, DOGMEAT)->playerId == bob);
}

TEST("server companions: an owner who drops out has the companion held, a rejoin finds it theirs, and after the hold it is free")
{
	Fixture    f;
	f.HoldClock();
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Recruit(DOGMEAT));
	f.fake->Deliver(2, Recruit(PIPER));
	Clear(f, { 1, 2 });

	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));
	auto word = LastWord(f, 1, DOGMEAT);
	REQUIRE(word);
	CHECK(word->state == State::kHeld && word->playerId == bob);
	CHECK(f.LoggedSoon(std::format("server: {:08X} is held for player {}, who left, for 5 min", DOGMEAT, bob)));

	// The host's recruit while held: refused, worded as held for Bob.
	f.fake->Deliver(1, Recruit(DOGMEAT));
	word = LastWord(f, 1, DOGMEAT);
	REQUIRE(word);
	CHECK(word->state == State::kHeld && word->playerId == bob);
	CHECK(f.LoggedSoon("(held for them while they are away)"));

	// Bob's game comes back (the same identity, a new connection): both are his again under his new id, the host hears
	// it, and his welcome lists them as his.
	f.Advance(1min);
	const auto bobAgain = JoinAs(f, 3, "Bob", 0x22);  // the server keeps a returning identity's id
	const auto hostHeard = Words(f, 1);
	const auto bobHeard = Words(f, 3);
	for (const auto ref : { DOGMEAT, PIPER }) {
		word = LastWord(hostHeard, ref);
		REQUIRE(word);
		CHECK(word->state == State::kOwner && word->playerId == bobAgain);
		word = LastWord(bobHeard, ref);
		REQUIRE(word);
		CHECK(word->state == State::kOwner && word->playerId == bobAgain);
	}
	// His game's report after its load: his, and nothing new for the host.
	Clear(f, { 1, 3 });
	f.fake->Deliver(3, Recruit(DOGMEAT));
	CHECK(LastWord(f, 3, DOGMEAT)->playerId == bobAgain);
	CHECK(Words(f, 1).empty());

	// Gone again, and gone for the hold: free, and everyone hears.
	f.fake->Disconnect(3);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bobAgain)));
	Clear(f, { 1 });
	f.Advance(CompanionBook::HOLD - 1s);
	Sync(f, 1);
	CHECK(Words(f, 1).empty());
	f.Advance(2s);
	CHECK(f.LoggedSoon(std::format("server: {:08X} is nobody's companion now", DOGMEAT)));
	const auto freed = Words(f, 1);
	for (const auto ref : { DOGMEAT, PIPER }) {
		word = LastWord(freed, ref);
		REQUIRE(word);
		CHECK(word->state == State::kFree);
	}
	f.fake->Deliver(1, Recruit(DOGMEAT));
	CHECK(LastWord(f, 1, DOGMEAT)->playerId == host);
}

TEST("server companions: an owner with no identity frees the companion as they leave")
{
	Fixture    f;
	JoinAs(f, 1, "Host");
	const auto bob = JoinAs(f, 2, "Bob");  // identity 0
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Recruit(DOGMEAT));
	Clear(f, { 1, 2 });
	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));
	const auto word = LastWord(f, 1, DOGMEAT);
	REQUIRE(word);
	CHECK(word->state == State::kFree);
}

TEST("server companions: a joiner is told every companion's owner after the welcome")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	f.fake->Deliver(1, Recruit(DOGMEAT));
	f.fake->Deliver(2, Recruit(PIPER));
	Clear(f, { 1, 2 });
	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));

	JoinAs(f, 3, "Carol", 0x33);
	const auto listed = Words(f, 3);
	const auto dog = LastWord(listed, DOGMEAT);
	const auto piper = LastWord(listed, PIPER);
	REQUIRE(dog && piper);
	CHECK(dog->playerId == host && dog->state == State::kOwner);
	CHECK(piper->playerId == bob && piper->state == State::kHeld);
}

TEST("server companions: a player keeps at most the book's share; past it the report is answered full")
{
	Fixture f;
	JoinAs(f, 1, "Host", 0x11);
	Clear(f, { 1 });
	for (std::uint32_t i = 0; i < CompanionBook::MAX_PER_PLAYER; ++i) {
		f.fake->Deliver(1, Recruit(0x00010000 + i));
	}
	f.fake->Deliver(1, Recruit(0x00020000));
	const auto word = LastWord(f, 1, 0x00020000);
	REQUIRE(word);
	CHECK(word->state == State::kFull && word->playerId == 0);
	CHECK(f.LoggedSoon("refused, they have 4 companions already"));
}

TEST("server companions: the owner's game runs its companion, whoever ran it before; a non-owner's companion claim is a plain offer")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	// Bob's game runs the dog (it is loaded there, nobody's). The host recruits him: the host's game takes him over with
	// its companion claim, as a companion claim takes over a plain runner.
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kUnowned));
	CHECK(RunnerSaid(f, 2, DOGMEAT) == bob);
	f.fake->Deliver(1, Recruit(DOGMEAT));
	f.fake->Deliver(1, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 1, DOGMEAT) == host);

	// Bob's game says the dog follows him too (its save from before, or its copy's scripts): a plain offer while the
	// host runs him, so the host keeps him; it never takes him over.
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 2, DOGMEAT) == host);

	// The host's game lets the dog go (unloaded): Bob's companion claim runs him, as a plain runner (nobody else's AI to
	// run him), and the host's companion claim takes him back at once, cooldown or not.
	f.fake->Deliver(1, Protocol::EncodeRelease(std::vector<std::uint32_t>{ DOGMEAT }));
	CHECK(RunnerSaid(f, 1, DOGMEAT) == 0);
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 2, DOGMEAT) == bob);
	f.fake->Deliver(1, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 1, DOGMEAT) == host);

	// Had Bob's claim been taken for a companion's, the host's would be refused (two companion claims: the first keeps).
	// It wasn't: Bob is told the host runs him, and a second companion claim of Bob's changes nothing.
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 2, DOGMEAT) == host);
}

TEST("server companions: a companion claim from the owner of a held companion takes it over from whoever runs it, and nobody else's does")
{
	Fixture    f;
	f.HoldClock();
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Recruit(DOGMEAT));
	f.fake->Deliver(2, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 2, DOGMEAT) == bob);
	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));
	// Held for Bob; the host's game runs him meanwhile (its companion claim is a plain offer: granted, as nobody runs him).
	f.fake->Deliver(1, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 1, DOGMEAT) == host);
	// Bob is back: his companion claim takes the dog from the host's game.
	f.Advance(10s);
	const auto bobAgain = JoinAs(f, 3, "Bob", 0x22);
	Clear(f, { 1, 3 });
	f.fake->Deliver(3, Claim(DOGMEAT, Reason::kCompanion));
	CHECK(RunnerSaid(f, 3, DOGMEAT) == bobAgain);
	CHECK(RunnerSaid(f, 1, DOGMEAT) == bobAgain);
}

TEST("server companions: a bad report is dropped, and a report before the welcome is nothing")
{
	Fixture f;
	JoinAs(f, 1, "Host", 0x11);
	Clear(f, { 1 });
	f.fake->Deliver(1, Protocol::Encode(Protocol::CompanionReport{ Protocol::PLAYER_REF_ID, true }));
	f.fake->Deliver(1, Protocol::Encode(Protocol::CompanionReport{ 0, true }));
	CHECK(Words(f, 1).empty());
	f.fake->Connect(2);
	f.fake->Deliver(2, Recruit(DOGMEAT));  // no hello
	f.fake->Deliver(1, Recruit(DOGMEAT));
	const auto word = LastWord(f, 1, DOGMEAT);
	REQUIRE(word);
	CHECK(word->playerId == 1 && word->state == State::kOwner);
	CHECK(f.LogLines("refused").empty());
}

TEST("server companions: a rejoin hands a held companion back, and the owner's game letting it go (its save doesn't hold it) frees it for everyone")
{
	// The game's side of it: a word that an NPC is ours that the aliases don't hold is reported let go (CompanionRules);
	// the server frees it, and the next to recruit gets it.
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });
	f.fake->Deliver(2, Recruit(DOGMEAT));
	Clear(f, { 1, 2 });
	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));
	Clear(f, { 1 });

	const auto bobAgain = JoinAs(f, 3, "Bob", 0x22);
	CHECK(LastWord(f, 3, DOGMEAT)->playerId == bobAgain);
	CHECK(LastWord(f, 1, DOGMEAT)->playerId == bobAgain);
	f.fake->Deliver(3, LetGo(DOGMEAT));
	for (const PeerId peer : { 1, 3 }) {
		const auto word = LastWord(f, peer, DOGMEAT);
		REQUIRE(word);
		CHECK(word->state == State::kFree);
	}
	CHECK(f.LoggedSoon(std::format("server: player {} let {:08X} go", bobAgain, DOGMEAT)));
	f.fake->Deliver(1, Recruit(DOGMEAT));
	const auto word = LastWord(f, 1, DOGMEAT);
	REQUIRE(word);
	CHECK(word->playerId == host && word->state == State::kOwner);
}
