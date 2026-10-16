#include "Test.h"
#include "game/VoicePath.h"

TEST("VoicePath: a line's voice file is under its plugin and voice type, named for its INFO and response")
{
	CHECK(VoicePath::Build("Fallout4.esm", "NPCFEllie", 0x0002A2B3, 1) == "Sound\\Voice\\Fallout4.esm\\NPCFEllie\\0002A2B3_1.fuz");
	CHECK(VoicePath::Build("Fallout4.esm", "PlayerVoiceFemale01", 0x000A1B2C, 3) == "Sound\\Voice\\Fallout4.esm\\PlayerVoiceFemale01\\000A1B2C_3.fuz");
	CHECK(VoicePath::Build("Fallout4.esm", "NPCFEllie", 0x0002A2B3, 12, "wav") == "Sound\\Voice\\Fallout4.esm\\NPCFEllie\\0002A2B3_12.wav");
}

TEST("VoicePath: the INFO id is the one its own plugin gives it")
{
	CHECK(VoicePath::LocalId(0x0002A2B3, false) == 0x0002A2B3);
	CHECK(VoicePath::LocalId(0x0401C9F4, false) == 0x0001C9F4);  // DLCCoast.esm loaded fourth
	CHECK(VoicePath::LocalId(0xFE012ABC, true) == 0x00000ABC);   // a light plugin
	CHECK(VoicePath::Build("DLCCoast.esm", "DLC03VoiceType", VoicePath::LocalId(0x0401C9F4, false), 2) ==
		  "Sound\\Voice\\DLCCoast.esm\\DLC03VoiceType\\0001C9F4_2.fuz");
}

TEST("VoicePath: no path without a response, an INFO or names that stay one folder each")
{
	CHECK(VoicePath::Build("Fallout4.esm", "NPCFEllie", 0x0002A2B3, 0).empty());
	CHECK(VoicePath::Build("Fallout4.esm", "NPCFEllie", 0, 1).empty());
	CHECK(VoicePath::Build("", "NPCFEllie", 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("Fallout4.esm", "", 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("..", "NPCFEllie", 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("Fallout4.esm", "..\\..\\Interface", 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("Fallout4.esm", "a/b", 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("C:", "NPCFEllie", 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("Fallout4.esm", std::string("NPC\nFEllie"), 0x0002A2B3, 1).empty());
	CHECK(VoicePath::Build("Fallout4.esm", "NPCFEllie", 0x0002A2B3, 1, "fuz\\x").empty());
	CHECK(VoicePath::Build("Fallout4.esm", std::string(129, 'a'), 0x0002A2B3, 1).empty());
}
