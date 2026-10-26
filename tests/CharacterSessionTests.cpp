// A session belongs to the characters that played it: the host's saved session to the host's character, and
// a friend's joining to the character they joined with. Players load other saves, start new characters and
// come back; each test plays them against the real Server through the steps Session takes (PlayerGame.h).

#include "PlayerGame.h"

#include <filesystem>
#include <string>

using namespace TestGame;

namespace
{
	constexpr std::uint64_t HOST_NEW = 0x00A1B2C3D4E5F603;    // a character the host starts later
	constexpr std::uint64_t FRIEND_NEW = 0x0071E2D3C4B5A603;  // one the friend starts for co-op

	std::filesystem::path FileOf(const Machine& a_machine, std::uint64_t a_character)
	{
		return a_machine.folder.path / CharacterBinding::FileName(a_character, false);
	}

	bool Says(const std::vector<std::string>& a_lines, std::size_t a_index, std::string_view a_text)
	{
		return a_index < a_lines.size() && a_lines[a_index].find(a_text) != std::string::npos;
	}
}

TEST("character session: the host loading their solo character hosts its own session; a friend let back into it leaves, and the co-op session waits as it was")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();  // in the world, out of the opening: hosting starts
	REQUIRE(host.welcome);
	const auto coopSession = host.welcome->sessionId;
	me.JoinFriend();
	REQUIRE(me.welcome);
	me.Send(Kill(RAIDER));
	host.Loot(-3);
	host.Process();
	me.Process();
	REQUIRE(me.stimpaks == 7);
	const auto hostCoop = host.MakeSave();

	// A death reload of the same character changes nothing.
	host.Load(hostCoop);
	host.Tick();
	CHECK(host.hosting);
	CHECK(me.welcome);

	// The host loads their solo save with bHost still on: hosting stops at once, and the friend's connection goes.
	host.Load({ HOST_SOLO });
	CHECK(!host.hosting);
	CHECK(!me.connected);
	CHECK(!me.Retry());  // nothing to reach

	// A second later it starts again, with the solo character's own session.
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.welcome->sessionId != coopSession);
	CHECK(host.stimpaks == STIMPAKS_IN_CHEST);

	// The friend's game reconnects by itself and is let into that other session: it leaves, and takes nothing of it.
	CHECK(!me.Retry());
	CHECK(!me.joining);
	REQUIRE(!me.notices.empty());
	CHECK(me.notices.back().find("another session") != std::string::npos);
	CHECK(me.MakeSave().session == coopSession);
	host.Loot(-1);  // in the solo world
	host.Process();
	CHECK(me.stimpaks == 7);

	// Back to the co-op character: its session as it was, and the friend joins it again from the menu.
	host.Load(hostCoop);
	CHECK(!host.hosting);
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.welcome->sessionId == coopSession);
	CHECK(host.stimpaks == 7);
	me.JoinFriend();
	REQUIRE(me.welcome);
	CHECK(me.welcome->sessionId == coopSession);
	CHECK(me.stimpaks == 7);
	CHECK(me.applied == 1);

	const auto coopFile = SessionFile::Load(FileOf(machine, HOST_COOP));
	const auto soloFile = SessionFile::Load(FileOf(machine, HOST_SOLO));
	REQUIRE(coopFile);
	REQUIRE(soloFile);
	CHECK(coopFile->deadActors == std::vector<std::uint32_t>{ RAIDER });
	CHECK(soloFile->deadActors.empty());
	CHECK(soloFile->containerChanges.size() == 1);
	CHECK(machine.said.empty());  // no old shared file: nothing to say
}

TEST("character session: a host who walks into Vault 111 later keeps hosting; a new game stops it until the new character is named and out of the opening")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();
	REQUIRE(host.welcome);
	const auto coopSession = host.welcome->sessionId;
	me.JoinFriend();
	REQUIRE(me.welcome);

	// An alternate start never finishes the vault's quest: the host's later visit for the Cryolator is "the opening".
	host.inOpening = true;
	host.Tick();
	CHECK(host.hosting);
	CHECK(me.welcome);
	host.LeaveOpening();

	host.NewGame();
	CHECK(!host.hosting);
	CHECK(!me.connected);
	host.Tick();  // in the opening, with the last character's ID still
	CHECK(!host.hosting);
	host.NameCharacter(HOST_NEW);
	host.Tick();
	CHECK(!host.hosting);
	host.LeaveOpening();
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.welcome->sessionId != coopSession);
	host.StopHosting();
	CHECK(std::filesystem::exists(FileOf(machine, HOST_NEW)));
}

TEST("character session: a friend who loads their solo save leaves before the session's loot is asked for, and the solo save never records the session")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();
	me.JoinFriend();
	REQUIRE(me.welcome);
	host.Loot(-3);
	host.Process();
	me.Process();
	REQUIRE(me.stimpaks == 7);
	const auto coopSave = me.MakeSave();

	// A death reload of the co-op character stays joined.
	me.Load(coopSave);
	me.Process();
	CHECK(me.welcome);
	CHECK(me.stimpaks == 7);

	me.Load({ FRIEND_SOLO });
	CHECK(!me.joining);
	CHECK(!me.connected);
	CHECK(me.stimpaks == STIMPAKS_IN_CHEST);
	CHECK(me.MakeSave().session == 0);  // the solo world holds none of the session's loot, and says so
	host.Loot(-2);
	host.Process();
	CHECK(!me.Retry());
	CHECK(me.stimpaks == STIMPAKS_IN_CHEST);

	// Back on the co-op save and joined again from the menu: the loot since comes, once.
	me.Load(coopSave);
	me.JoinFriend();
	REQUIRE(me.welcome);
	CHECK(me.stimpaks == 5);
	CHECK(me.applied == 2);
}

TEST("character session: a friend waiting for the host who loads their solo save meanwhile doesn't join the session with it")
{
	TempFile file;
	Machine  machine;
	machine.Start(HostedOptions(file));  // the host's game is still loading
	Game host{ machine, HOST, HOST_COOP };
	Game me{ machine, FRIEND, FRIEND_COOP };
	me.JoinFriend();
	CHECK(!me.welcome);
	REQUIRE(me.joining);

	me.Load({ FRIEND_SOLO });  // to play alone while waiting
	CHECK(!me.joining);
	host.JoinOwnServer();
	REQUIRE(host.welcome);
	CHECK(!me.Retry());
	CHECK(me.MakeSave().session == 0);
}

TEST("character session: a friend who starts a new character for co-op stays joined, plays the session as that character once named, and leaves on loading the old one")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();
	me.JoinFriend();
	REQUIRE(me.welcome);

	me.NewGame();
	CHECK(me.joining);
	CHECK(me.welcome);
	me.Tick();
	me.NameCharacter(FRIEND_NEW);
	me.Tick();  // the name is confirmed in the opening: the new character is the one in the session
	me.LeaveOpening();
	me.Tick();
	host.Loot(-3);
	host.Process();
	me.Process();
	CHECK(me.stimpaks == 7);

	// A reconnect is still the new character's, and so is its own save.
	me.ConnectionLost();
	REQUIRE(me.Retry());
	const auto newSave = me.MakeSave();
	me.Load(newSave);
	CHECK(me.welcome);

	me.Load({ FRIEND_COOP, 7, me.loot.Point().sessionId, 1, {} });
	CHECK(!me.joining);
}

TEST("character session: an alternate-start host doesn't host until named; the previous character's file is untouched and the new one gets its own")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();
	REQUIRE(host.welcome);
	const auto coopSession = host.welcome->sessionId;
	me.JoinFriend();
	REQUIRE(me.welcome);
	me.Send(Kill(RAIDER));
	me.Process();
	host.StopHosting();  // the co-op session is written
	const auto coopPath = FileOf(machine, HOST_COOP);
	REQUIRE(std::filesystem::exists(coopPath));
	const auto coopTime = std::filesystem::last_write_time(coopPath);

	// New Game with an alternate start: out of Vault 111 from the first frame, with the co-op character's ID still.
	host.AlternateStart();
	CHECK(host.InOpening());
	host.Tick();
	host.Tick();
	CHECK(!host.hosting);
	CHECK(!machine.server);

	// Named in the mod's room: hosting starts, with the new character's own session.
	host.NameCharacter(HOST_NEW);
	CHECK(!host.InOpening());
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.welcome->sessionId != coopSession);
	host.StopHosting();
	CHECK(std::filesystem::last_write_time(coopPath) == coopTime);
	const auto coopFile = SessionFile::Load(coopPath);
	const auto newFile = SessionFile::Load(FileOf(machine, HOST_NEW));
	REQUIRE(coopFile);
	REQUIRE(newFile);
	CHECK(coopFile->sessionId == coopSession);
	CHECK(coopFile->deadActors == std::vector<std::uint32_t>{ RAIDER });
	CHECK(newFile->deadActors.empty());
}

TEST("character session: a friend with an alternate start who joins before naming plays as the named character, and its own save loads without leaving")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();
	REQUIRE(host.welcome);

	me.AlternateStart();
	CHECK(me.InOpening());  // held, as in Vault 111
	me.JoinFriend();
	REQUIRE(me.welcome);
	me.Tick();
	me.NameCharacter(FRIEND_NEW);
	CHECK(!me.InOpening());
	me.Tick();
	host.Loot(-3);
	host.Process();
	me.Process();
	CHECK(me.stimpaks == 7);

	const auto newSave = me.MakeSave();
	me.Load(newSave);
	CHECK(me.joining);
	CHECK(me.welcome);
	me.Tick();
	CHECK(me.joining);

	me.Load({ FRIEND_COOP, 7, me.loot.Point().sessionId, 1, {} });
	CHECK(!me.joining);
}

TEST("character session: an unnamed alternate start that walks into Vault 111 and out again still doesn't host under the previous character's ID")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	host.wantHost = true;
	host.Tick();
	REQUIRE(host.welcome);
	host.StopHosting();  // the co-op session is written
	const auto coopPath = FileOf(machine, HOST_COOP);
	REQUIRE(std::filesystem::exists(coopPath));
	const auto coopTime = std::filesystem::last_write_time(coopPath);

	// A start that never asks for a name, then the trip from Sanctuary to loot the vault: Out of Time never started.
	host.AlternateStart();
	host.Tick();
	host.EnterVault();
	CHECK(host.InOpening());
	host.Tick();
	host.Tick();
	CHECK(!host.hosting);
	host.LeaveVault();
	CHECK(host.InOpening());  // still unnamed: the vault's places are not the start that would end it
	host.Tick();
	host.Tick();
	CHECK(!host.hosting);
	CHECK(!machine.server);
	CHECK(std::filesystem::last_write_time(coopPath) == coopTime);

	// Named later: its own session.
	host.NameCharacter(HOST_NEW);
	host.Tick();
	CHECK(host.hosting);
	host.StopHosting();
	CHECK(std::filesystem::exists(FileOf(machine, HOST_NEW)));
	CHECK(std::filesystem::last_write_time(coopPath) == coopTime);
}

TEST("character session: a friend's unnamed alternate start stays held after a walk through Vault 111, and its name still binds it")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	Game    me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;
	host.Tick();
	REQUIRE(host.welcome);

	me.AlternateStart();
	me.JoinFriend();
	REQUIRE(me.welcome);
	me.EnterVault();
	me.Tick();
	me.LeaveVault();
	me.Tick();
	CHECK(me.InOpening());
	me.NameCharacter(FRIEND_NEW);
	me.Tick();
	CHECK(!me.InOpening());
	me.Load(me.MakeSave());
	CHECK(me.joining);
	me.Load({ FRIEND_COOP, 7, me.loot.Point().sessionId, 1, {} });
	CHECK(!me.joining);
}

TEST("character session: a Vault 111 New Game is let out with the vault even if naming never changed the ID")
{
	Machine machine;
	Game    host{ machine, HOST, HOST_COOP };
	host.wantHost = true;
	host.NewGame();
	host.Tick();
	CHECK(!host.hosting);
	host.LeaveOpening();  // named in the vault, the ID as it was
	CHECK(!host.InOpening());
	host.Tick();
	CHECK(host.hosting);  // as before the unnamed hold
}

TEST("character session: the dev channel's coc character hosts unnamed as before")
{
	Machine machine;
	Game    host{ machine, HOST, 0 };
	host.wantHost = true;
	host.devChannel = true;
	host.AlternateStart();  // coc from the main menu: no name ever asked
	CHECK(!host.InOpening());
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.hosting);
	host.StopHosting();
	CHECK(std::filesystem::exists(machine.folder.path / CharacterBinding::FileName(0, true)));
}

TEST("character session: the old shared session file is taken over only by the character whose save played in it")
{
	Machine machine;
	REQUIRE(SessionFile::Save(machine.folder.path / CharacterBinding::LEGACY_FILE, PlayedSession()));  // written by 0.8.5
	Game host{ machine, HOST, HOST_SOLO };
	Game me{ machine, FRIEND, FRIEND_COOP };
	host.wantHost = true;

	// The solo character's save has no record of that session: it starts its own, and the old file waits.
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.welcome->sessionId != 0x5E55);
	CHECK(Says(machine.said, 0, "another save's session"));
	CHECK(std::filesystem::exists(machine.folder.path / CharacterBinding::LEGACY_FILE));

	// The co-op character's save was played in it (its co-save record names the session): it carries on.
	host.Load({ HOST_COOP, 7, 0x5E55, 1, {} });
	host.Tick();
	REQUIRE(host.welcome);
	CHECK(host.welcome->sessionId == 0x5E55);
	CHECK(Says(machine.said, 1, "continuing the saved co-op session"));
	CHECK(!std::filesystem::exists(machine.folder.path / CharacterBinding::LEGACY_FILE));
	CHECK(std::filesystem::exists(FileOf(machine, HOST_COOP)));

	// The friend's co-op save too: in, as the player they were, with nothing of the old session taken twice.
	me.Load({ FRIEND_COOP, 7, 0x5E55, 1, {} });
	me.JoinFriend();
	REQUIRE(me.welcome);
	CHECK(me.welcome->playerId == 2);
	CHECK(me.stimpaks == 7);
	CHECK(me.applied == 0);
}
