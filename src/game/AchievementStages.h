#pragma once

#include <cstddef>
#include <cstdint>

// The shared quests' stages that award a Steam achievement (Game.AddAchievement): in their own fragment (a row whose
// award is itself: MQ102 10 "War Never Changes", the story's and the factions' milestones, the add-ons' main quests')
// or by stage sets alone, every award stage their fragment sets, of their own quest or another, or sets one that does,
// and so on (MQ104 205 sets its 210, MQ201 150 sets MQ106 2000, the Brotherhood quests' stage 0s set BoS200 200; a
// stage that reaches several has a row for each: BoSM04 1 sets BoS200 200 and MQ206 1200). A caught-up stage set here
// marks its awards' stages too (AchievementGate::Marks::CaughtUp): the game sets those stages from this one's fragment,
// and their fragments make the calls. Generated with tools/questscan.ps1 -Safe -Achievements from the quest fragment
// scripts of Fallout4.esm, DLCCoast.esm, DLCRobot.esm and DLCNukaWorld.esm (the stage sets of each fragment's own code,
// followed as far as they go; the ids after the colon are the game's NEW_ACHIEVEMENT_<id>_0). Left out, 191 stages that
// reach an award only through a script function, the game's own doing from its own state, not the stage's: a companion
// set (FollowersScript, the Achievements quest's 320), a settlement's settlers and happiness (WorkshopParentScript),
// the Minutemen's recruitment and ally count (MinRecruitQuestScript, MinutemenCentralScript), Far Harbor's settlement
// jobs, Nuka-World's raids and outposts. The endings' MQ302 1000 and Inst308 200 are reached from the faction ending
// quests' last stages by stage sets (rows) and those from the leader's closing conversation, which no stage set makes.
// Sorted by file, quest, stage and award; the files are StageRules::File's.
namespace AchievementStages
{
	struct Reach
	{
		std::uint8_t  file;   // StageRules::File
		std::uint32_t quest;  // form ID in its file
		std::uint16_t stage;
		std::uint8_t  awardFile;
		std::uint32_t awardQuest;
		std::uint16_t awardStage;
	};

	inline constexpr Reach REACHES[] = {
		{ 0, 0x01CC2A, 10, 0, 0x01CC2A, 10 },  // MQ102 10: 1
		{ 0, 0x01F25E, 205, 0, 0x01F25E, 210 },  // MQ104 205 -> MQ104 210: 3
		{ 0, 0x01F25E, 210, 0, 0x01F25E, 210 },  // MQ104 210: 3
		{ 0, 0x0229E7, 2000, 0, 0x0229E7, 2000 },  // MQ106 2000: 4
		{ 0, 0x0229E8, 100, 0, 0x0229E7, 2000 },  // MQ201 100 -> MQ106 2000: 4
		{ 0, 0x0229E8, 110, 0, 0x0229E7, 2000 },  // MQ201 110 -> MQ106 2000: 4
		{ 0, 0x0229E8, 150, 0, 0x0229E7, 2000 },  // MQ201 150 -> MQ106 2000: 4
		{ 0, 0x0229E9, 0, 0, 0x0229E7, 2000 },  // MQ202 0 -> MQ106 2000: 4
		{ 0, 0x0229E9, 320, 0, 0x0229E9, 320 },  // MQ202 320: 5
		{ 0, 0x0229EA, 0, 0, 0x0229E7, 2000 },  // MQ203 0 -> MQ106 2000: 4
		{ 0, 0x0229EB, 100, 0, 0x0229EB, 100 },  // MQ205 100: 6
		{ 0, 0x0229EC, 5, 0, 0x0229EC, 200 },  // MQ207 5 -> MQ207 200: 9
		{ 0, 0x0229EC, 200, 0, 0x0229EC, 200 },  // MQ207 200: 9
		{ 0, 0x0229EE, 0, 0, 0x0229EB, 100 },  // MQ302 0 -> MQ205 100: 6
		{ 0, 0x0229EE, 1, 0, 0x0229EB, 100 },  // MQ302 1 -> MQ205 100: 6
		{ 0, 0x0229EE, 2, 0, 0x0229EB, 100 },  // MQ302 2 -> MQ205 100: 6
		{ 0, 0x0229EE, 2, 0, 0x0229EC, 200 },  // MQ302 2 -> MQ207 200: 9
		{ 0, 0x0229EE, 2, 0, 0x05DEE4, 2000 },  // MQ302 2 -> Min01 2000: 13
		{ 0, 0x0229EE, 3, 0, 0x0229EB, 100 },  // MQ302 3 -> MQ205 100: 6
		{ 0, 0x0229EE, 3, 0, 0x06FA37, 800 },  // MQ302 3 -> RR102 800: 19
		{ 0, 0x0229EE, 1000, 0, 0x0229EE, 1000 },  // MQ302 1000: 8,50
		{ 0, 0x02B4E9, 350, 0, 0x02B4E9, 350 },  // Inst303 350: 10
		{ 0, 0x02BF20, 200, 0, 0x02BF20, 200 },  // BoS200 200: 16
		{ 0, 0x02BF21, 0, 0, 0x02BF20, 200 },  // BoS201 0 -> BoS200 200: 16
		{ 0, 0x03A457, 0, 0, 0x05DEE4, 2000 },  // Min02 0 -> Min01 2000: 13
		{ 0, 0x03A457, 650, 0, 0x03A457, 650 },  // Min02 650: 14
		{ 0, 0x043275, 1, 0, 0x06FA37, 800 },  // RR303 1 -> RR102 800: 19
		{ 0, 0x043275, 100, 0, 0x0B2D48, 1100 },  // RR303 100 -> RR201 1100: 20
		{ 0, 0x043275, 1300, 0, 0x043275, 1300 },  // RR303 1300: 21
		{ 0, 0x043275, 10200, 0, 0x0B2D48, 1100 },  // RR303 10200 -> RR201 1100: 20
		{ 0, 0x04402C, 0, 0, 0x02BF20, 200 },  // BoSM02 0 -> BoS200 200: 16
		{ 0, 0x04402C, 1, 0, 0x02BF20, 200 },  // BoSM02 1 -> BoS200 200: 16
		{ 0, 0x04402C, 2, 0, 0x02BF20, 200 },  // BoSM02 2 -> BoS200 200: 16
		{ 0, 0x04402C, 3, 0, 0x02BF20, 200 },  // BoSM02 3 -> BoS200 200: 16
		{ 0, 0x04402C, 4, 0, 0x02BF20, 200 },  // BoSM02 4 -> BoS200 200: 16
		{ 0, 0x04402C, 5, 0, 0x02BF20, 200 },  // BoSM02 5 -> BoS200 200: 16
		{ 0, 0x04402C, 6, 0, 0x02BF20, 200 },  // BoSM02 6 -> BoS200 200: 16
		{ 0, 0x04402C, 7, 0, 0x02BF20, 200 },  // BoSM02 7 -> BoS200 200: 16
		{ 0, 0x04402C, 8, 0, 0x02BF20, 200 },  // BoSM02 8 -> BoS200 200: 16
		{ 0, 0x04402C, 9, 0, 0x02BF20, 200 },  // BoSM02 9 -> BoS200 200: 16
		{ 0, 0x05DEE4, 1500, 0, 0x05DEE4, 1500 },  // Min01 1500: 22
		{ 0, 0x05DEE4, 2000, 0, 0x05DEE4, 2000 },  // Min01 2000: 13
		{ 0, 0x064EC7, 0, 0, 0x02BF20, 200 },  // BoSR01 0 -> BoS200 200: 16
		{ 0, 0x06B500, 15, 0, 0x0229E9, 320 },  // MQ204 15 -> MQ202 320: 5
		{ 0, 0x06FA37, 800, 0, 0x06FA37, 800 },  // RR102 800: 19
		{ 0, 0x07CC61, 10, 0, 0x06FA37, 800 },  // RR301 10 -> RR102 800: 19
		{ 0, 0x0A274B, 200, 0, 0x0A274B, 200 },  // Inst305 200: 11
		{ 0, 0x0AA778, 0, 0, 0x05DEE4, 2000 },  // Min03 0 -> Min01 2000: 13
		{ 0, 0x0AA778, 980, 0, 0x0AA778, 980 },  // Min03 980: 15
		{ 0, 0x0AA778, 1000, 0, 0x0AA778, 980 },  // Min03 1000 -> Min03 980: 15
		{ 0, 0x0ADCE5, 0, 0, 0x02BF20, 200 },  // BoSM04 0 -> BoS200 200: 16
		{ 0, 0x0ADCE5, 0, 0, 0x0B1752, 1200 },  // BoSM04 0 -> MQ206 1200: 7
		{ 0, 0x0ADCE5, 1, 0, 0x02BF20, 200 },  // BoSM04 1 -> BoS200 200: 16
		{ 0, 0x0ADCE5, 1, 0, 0x0B1752, 1200 },  // BoSM04 1 -> MQ206 1200: 7
		{ 0, 0x0ADCE5, 2, 0, 0x02BF20, 200 },  // BoSM04 2 -> BoS200 200: 16
		{ 0, 0x0ADCE5, 2, 0, 0x0B1752, 1200 },  // BoSM04 2 -> MQ206 1200: 7
		{ 0, 0x0ADCE5, 3, 0, 0x02BF20, 200 },  // BoSM04 3 -> BoS200 200: 16
		{ 0, 0x0ADCE5, 3, 0, 0x0B1752, 1200 },  // BoSM04 3 -> MQ206 1200: 7
		{ 0, 0x0ADCE5, 4, 0, 0x02BF20, 200 },  // BoSM04 4 -> BoS200 200: 16
		{ 0, 0x0ADCE5, 4, 0, 0x0B1752, 1200 },  // BoSM04 4 -> MQ206 1200: 7
		{ 0, 0x0B1752, 0, 0, 0x0229EB, 100 },  // MQ206 0 -> MQ205 100: 6
		{ 0, 0x0B1752, 1050, 0, 0x0B1752, 1200 },  // MQ206 1050 -> MQ206 1200: 7
		{ 0, 0x0B1752, 1200, 0, 0x0B1752, 1200 },  // MQ206 1200: 7
		{ 0, 0x0B1D79, 0, 0, 0x02BF20, 200 },  // BoSM01 0 -> BoS200 200: 16
		{ 0, 0x0B1D79, 1, 0, 0x02BF20, 200 },  // BoSM01 1 -> BoS200 200: 16
		{ 0, 0x0B2D48, 1100, 0, 0x0B2D48, 1100 },  // RR201 1100: 20
		{ 0, 0x0B9F9D, 255, 0, 0x0B9F9D, 255 },  // BoS302 255: 17
		{ 0, 0x0BAD00, 50, 0, 0x0BAD00, 200 },  // Inst308 50 -> Inst308 200: 12,50
		{ 0, 0x0BAD00, 200, 0, 0x0BAD00, 200 },  // Inst308 200: 12,50
		{ 0, 0x0BDB04, 0, 0, 0x05DEE4, 2000 },  // MinDefendCastle 0 -> Min01 2000: 13
		{ 0, 0x0BDB04, 0, 0, 0x0AA778, 980 },  // MinDefendCastle 0 -> Min03 980: 15
		{ 0, 0x0C30DC, 0, 0, 0x02BF20, 200 },  // BosR02 0 -> BoS200 200: 16
		{ 0, 0x0C8675, 0, 0, 0x02BF20, 200 },  // BoSR03 0 -> BoS200 200: 16
		{ 0, 0x0C8675, 0, 0, 0x0B1752, 1200 },  // BoSR03 0 -> MQ206 1200: 7
		{ 0, 0x0CF3E2, 0, 0, 0x02BF20, 200 },  // BoSR04 0 -> BoS200 200: 16
		{ 0, 0x0D1EB2, 0, 0, 0x02BF20, 200 },  // BoSR05 0 -> BoS200 200: 16
		{ 0, 0x0DDFDB, 0, 0, 0x0229EB, 100 },  // Min301 0 -> MQ205 100: 6
		{ 0, 0x0DDFDB, 0, 0, 0x0229EC, 200 },  // Min301 0 -> MQ207 200: 9
		{ 0, 0x0DDFDB, 0, 0, 0x05DEE4, 2000 },  // Min301 0 -> Min01 2000: 13
		{ 0, 0x0DDFDB, 1, 0, 0x0229EB, 100 },  // Min301 1 -> MQ205 100: 6
		{ 0, 0x0DDFDB, 1, 0, 0x0229EC, 200 },  // Min301 1 -> MQ207 200: 9
		{ 0, 0x0DDFDB, 1, 0, 0x05DEE4, 2000 },  // Min301 1 -> Min01 2000: 13
		{ 0, 0x0DFB3C, 0, 0, 0x0229EB, 100 },  // MinDestBoS 0 -> MQ205 100: 6
		{ 0, 0x0DFB3C, 0, 0, 0x0229EC, 200 },  // MinDestBoS 0 -> MQ207 200: 9
		{ 0, 0x0DFB3C, 0, 0, 0x0229EE, 1000 },  // MinDestBoS 0 -> MQ302 1000: 8,50
		{ 0, 0x0DFB3C, 0, 0, 0x05DEE4, 2000 },  // MinDestBoS 0 -> Min01 2000: 13
		{ 0, 0x0FDC8C, 0, 0, 0x0B9F9D, 255 },  // BoS303 0 -> BoS302 255: 17
		{ 0, 0x0FDC8C, 1, 0, 0x0B9F9D, 255 },  // BoS303 1 -> BoS302 255: 17
		{ 0, 0x0FDC8C, 2, 0, 0x0B9F9D, 255 },  // BoS303 2 -> BoS302 255: 17
		{ 0, 0x0FDC8C, 3, 0, 0x0B9F9D, 255 },  // BoS303 3 -> BoS302 255: 17
		{ 0, 0x107A1C, 0, 0, 0x0B9F9D, 255 },  // BoS302B 0 -> BoS302 255: 17
		{ 0, 0x10C64A, 1910, 0, 0x0229EE, 1000 },  // MQ302Min 1910 -> MQ302 1000: 8,50
		{ 0, 0x10C64A, 2000, 0, 0x0229EE, 1000 },  // MQ302Min 2000 -> MQ302 1000: 8,50
		{ 0, 0x10C64B, 1000, 0, 0x0229EE, 1000 },  // MQ302BoS 1000 -> MQ302 1000: 8,50
		{ 0, 0x10C64C, 1000, 0, 0x0229EE, 1000 },  // MQ302RR 1000 -> MQ302 1000: 8,50
		{ 0, 0x142FD3, 1, 0, 0x06FA37, 800 },  // RR304 1 -> RR102 800: 19
		{ 0, 0x142FD3, 20, 0, 0x06FA37, 800 },  // RR304 20 -> RR102 800: 19
		{ 0, 0x142FD3, 20, 0, 0x0B2D48, 1100 },  // RR304 20 -> RR201 1100: 20
		{ 0, 0x142FD3, 25, 0, 0x06FA37, 800 },  // RR304 25 -> RR102 800: 19
		{ 0, 0x142FD3, 25, 0, 0x0B2D48, 1100 },  // RR304 25 -> RR201 1100: 20
		{ 0, 0x142FD3, 30, 0, 0x06FA37, 800 },  // RR304 30 -> RR102 800: 19
		{ 0, 0x142FD3, 200, 0, 0x0B2D48, 1100 },  // RR304 200 -> RR201 1100: 20
		{ 0, 0x153307, 0, 0, 0x06FA37, 800 },  // DialogueRailroadLiamFuneral 0 -> RR102 800: 19
		{ 0, 0x173ED9, 0, 0, 0x0B9F9D, 255 },  // BoS304 0 -> BoS302 255: 17
		{ 0, 0x173ED9, 1, 0, 0x0B9F9D, 255 },  // BoS304 1 -> BoS302 255: 17
		{ 0, 0x173ED9, 2, 0, 0x0B9F9D, 255 },  // BoS304 2 -> BoS302 255: 17
		{ 0, 0x173ED9, 255, 0, 0x173ED9, 255 },  // BoS304 255: 18
		{ 0, 0x1A001C, 1000, 0, 0x1A001C, 1000 },  // Min00 1000: 2
		{ 0, 0x215CC7, 10, 0, 0x0229EE, 1000 },  // MQ302Post 10 -> MQ302 1000: 8,50
		{ 1, 0x001B3F, 500, 1, 0x001B3F, 500 },  // DLC03MQ01 500: 59
		{ 1, 0x001B40, 650, 1, 0x057152, 67 },  // DLC03MQ02 650 -> DLC03_Achievements 67: 67
		{ 1, 0x001B40, 675, 1, 0x057152, 67 },  // DLC03MQ02 675 -> DLC03_Achievements 67: 67
		{ 1, 0x001B41, 1000, 1, 0x001B41, 1000 },  // DLC03MQ03 1000: 60
		{ 1, 0x001B42, 1, 1, 0x001B41, 1000 },  // DLC03MQ04 1 -> DLC03MQ03 1000: 60
		{ 1, 0x001B42, 15, 1, 0x001B41, 1000 },  // DLC03MQ04 15 -> DLC03MQ03 1000: 60
		{ 1, 0x001B43, 125, 1, 0x001B43, 125 },  // DLC03MQ05 125: 61
		{ 1, 0x001B43, 725, 1, 0x001B43, 725 },  // DLC03MQ05 725: 61
		{ 1, 0x001B43, 1000, 1, 0x001B43, 1000 },  // DLC03MQ05 1000: 61
		{ 1, 0x001B44, 249, 1, 0x001B44, 540 },  // DLC03MQ06 249 -> DLC03MQ06 540: 62
		{ 1, 0x001B44, 250, 1, 0x001B44, 250 },  // DLC03MQ06 250: 62
		{ 1, 0x001B44, 500, 1, 0x001B44, 535 },  // DLC03MQ06 500 -> DLC03MQ06 535: 62
		{ 1, 0x001B44, 530, 1, 0x001B44, 530 },  // DLC03MQ06 530: 62
		{ 1, 0x001B44, 535, 1, 0x001B44, 535 },  // DLC03MQ06 535: 62
		{ 1, 0x001B44, 540, 1, 0x001B44, 540 },  // DLC03MQ06 540: 62
		{ 1, 0x001B44, 550, 1, 0x001B44, 555 },  // DLC03MQ06 550 -> DLC03MQ06 555: 62
		{ 1, 0x001B44, 555, 1, 0x001B44, 555 },  // DLC03MQ06 555: 62
		{ 1, 0x004F2C, 500, 1, 0x004F2C, 500 },  // DLC03MQPostQuest 500: 63
		{ 1, 0x00A991, 200, 1, 0x057152, 67 },  // DLC03FarHarborS01 200 -> DLC03_Achievements 67: 67
		{ 1, 0x00AB19, 150, 1, 0x057152, 67 },  // DLC03FarHarborS01Defend 150 -> DLC03_Achievements 67: 67
		{ 1, 0x00AB19, 200, 1, 0x057152, 67 },  // DLC03FarHarborS01Defend 200 -> DLC03_Achievements 67: 67
		{ 1, 0x00AEC3, 400, 1, 0x057152, 67 },  // DLC03FarHarborS02 400 -> DLC03_Achievements 67: 67
		{ 1, 0x00AEC3, 425, 1, 0x057152, 67 },  // DLC03FarHarborS02 425 -> DLC03_Achievements 67: 67
		{ 1, 0x015366, 150, 1, 0x001B44, 250 },  // DLC03MQ06_NukeHandling 150 -> DLC03MQ06 250: 62
		{ 1, 0x01AC2B, 50, 1, 0x001B44, 535 },  // DLC03MQ06_WindFarmHandling 50 -> DLC03MQ06 535: 62
		{ 1, 0x02127F, 290, 1, 0x001B44, 555 },  // DLC03MQ06a 290 -> DLC03MQ06 555: 62
		{ 1, 0x02127F, 300, 1, 0x001B44, 555 },  // DLC03MQ06a 300 -> DLC03MQ06 555: 62
		{ 1, 0x023ACB, 220, 1, 0x057152, 67 },  // DLC03FarHarborFF01 220 -> DLC03_Achievements 67: 67
		{ 1, 0x02BBDB, 10, 1, 0x001B41, 1000 },  // DLC03DialogueKasumiAcadia 10 -> DLC03MQ03 1000: 60
		{ 2, 0x000801, 0, 0, 0x01CC2A, 10 },  // DLC01MQ02 0 -> MQ102 10: 1
		{ 2, 0x000801, 0, 2, 0x000806, 3000 },  // DLC01MQ02 0 -> DLC01MQ01 3000: 51
		{ 2, 0x000806, 3000, 2, 0x000806, 3000 },  // DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 0, 2, 0x000806, 3000 },  // DLC01Lair 0 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 0, 2, 0x002833, 255 },  // DLC01Lair 0 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 1, 2, 0x000806, 3000 },  // DLC01Lair 1 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 1, 2, 0x002833, 255 },  // DLC01Lair 1 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 3, 2, 0x000806, 3000 },  // DLC01Lair 3 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 3, 2, 0x002833, 255 },  // DLC01Lair 3 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 4, 2, 0x000806, 3000 },  // DLC01Lair 4 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 4, 2, 0x002833, 255 },  // DLC01Lair 4 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 5, 2, 0x000806, 3000 },  // DLC01Lair 5 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 5, 2, 0x002833, 255 },  // DLC01Lair 5 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 6, 2, 0x000806, 3000 },  // DLC01Lair 6 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 6, 2, 0x002833, 255 },  // DLC01Lair 6 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 7, 2, 0x000806, 3000 },  // DLC01Lair 7 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 7, 2, 0x002833, 255 },  // DLC01Lair 7 -> DLC01MQ04 255: 52
		{ 2, 0x0008BB, 8, 2, 0x000806, 3000 },  // DLC01Lair 8 -> DLC01MQ01 3000: 51
		{ 2, 0x0008BB, 8, 2, 0x002833, 255 },  // DLC01Lair 8 -> DLC01MQ04 255: 52
		{ 2, 0x0010F5, 0, 0, 0x01CC2A, 10 },  // DLC01MQ05 0 -> MQ102 10: 1
		{ 2, 0x0010F5, 0, 2, 0x000806, 3000 },  // DLC01MQ05 0 -> DLC01MQ01 3000: 51
		{ 2, 0x0010F5, 0, 2, 0x002833, 255 },  // DLC01MQ05 0 -> DLC01MQ04 255: 52
		{ 2, 0x0010F5, 1100, 2, 0x0010F5, 1100 },  // DLC01MQ05 1100: 53,54
		{ 2, 0x002833, 0, 0, 0x01CC2A, 10 },  // DLC01MQ04 0 -> MQ102 10: 1
		{ 2, 0x002833, 0, 2, 0x000806, 3000 },  // DLC01MQ04 0 -> DLC01MQ01 3000: 51
		{ 2, 0x002833, 1, 0, 0x01CC2A, 10 },  // DLC01MQ04 1 -> MQ102 10: 1
		{ 2, 0x002833, 1, 2, 0x000806, 3000 },  // DLC01MQ04 1 -> DLC01MQ01 3000: 51
		{ 2, 0x002833, 2, 0, 0x01CC2A, 10 },  // DLC01MQ04 2 -> MQ102 10: 1
		{ 2, 0x002833, 2, 2, 0x000806, 3000 },  // DLC01MQ04 2 -> DLC01MQ01 3000: 51
		{ 2, 0x002833, 3, 0, 0x01CC2A, 10 },  // DLC01MQ04 3 -> MQ102 10: 1
		{ 2, 0x002833, 3, 2, 0x000806, 3000 },  // DLC01MQ04 3 -> DLC01MQ01 3000: 51
		{ 2, 0x002833, 255, 2, 0x002833, 255 },  // DLC01MQ04 255: 52
		{ 3, 0x000801, 1000, 3, 0x000801, 1000 },  // DLC04MQ01 1000: 81
		{ 3, 0x000802, 0, 3, 0x000801, 1000 },  // DLC04MQ02 0 -> DLC04MQ01 1000: 81
		{ 3, 0x000803, 655, 3, 0x000803, 655 },  // DLC04MQ03 655: 82
		{ 3, 0x000804, 1, 3, 0x000801, 1000 },  // DLC04MQ04 1 -> DLC04MQ01 1000: 81
		{ 3, 0x000804, 2, 3, 0x000801, 1000 },  // DLC04MQ04 2 -> DLC04MQ01 1000: 81
		{ 3, 0x000804, 3, 3, 0x000801, 1000 },  // DLC04MQ04 3 -> DLC04MQ01 1000: 81
		{ 3, 0x000804, 4, 3, 0x000801, 1000 },  // DLC04MQ04 4 -> DLC04MQ01 1000: 81
		{ 3, 0x000804, 500, 3, 0x000804, 500 },  // DLC04MQ04 500: 83
		{ 3, 0x000805, 255, 3, 0x000805, 255 },  // DLC04MQ05 255: 84
	};
}
