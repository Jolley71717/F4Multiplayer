#include "game/FriendBot.h"

#include "Config.h"
#include "game/Downed.h"
#include "game/FriendBotPlan.h"
#include "game/GameStop.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/PackageRelease.h"
#include "game/PadPlan.h"
#include "game/RemotePlayers.h"
#include "game/VirtualPad.h"

#include <array>
#include <cmath>

namespace FriendBot
{
	namespace
	{
		using Clock = FriendBotPlan::Clock;
		using Plan = FriendBotPlan::Bot;

		// ---- the engine's packages, as it makes them for the player itself ----
		//
		// Read from Fallout4.exe 1.11.240 (the Address Library ids; offsets from the image base):
		//  - Game.SetPlayerAIDriven is a native (+112A010) that jumps to PlayerCharacter::SetAIControlledPackage
		//    (+D6F180, CommonLibF4's). On, it switches the player's MovementControllerNPC (Actor+318) to the AI (+DC8540) and
		//    sets aiControlledPackage; off, it switches it back (+DC85E0).
		//  - +D55460 (2232897) walks the player through a door (PlayerCharacter::walkThroughDoor, +D30): the same switch,
		//    then a Travel package made on the spot (CreatePackageByType(6), packFlags |= 6), a location near the door
		//    (a PackageLocation made on the stack, its type 0 "near a reference" and the door, copied in by
		//    TESPackage::SetLocation), a target (a PackageTarget made on the stack, copied in by TESPackage::SetTarget), and
		//    Actor::PutCreatedPackage(package, false, true, false).
		//  - +CF0EB0 (2231555, AIProcess) makes a Follow package the same way: CreatePackageByType(1), the procedure set
		//    (+758B20), and the target's type 0 "a specific reference" with the actor to follow (its handle).
		//  - Actor::InitiateDoNothingPackage (+C69BF0) is the same call shape for DoNothing.
		// The bot makes its packages exactly so: Follow (or Travel) with the host's stand-in as the target and the place
		// near it as the location; Travel near a door for a load door.
		constexpr REL::ID CREATE_PACKAGE{ 2211661 };    // TESPackage* CreatePackageByType(PACKAGE_PROCEDURE_TYPE) +757DB0
		constexpr REL::ID SET_PROCEDURE{ 2211680 };     // TESPackage::SetProcedureType(type) +758B20
		constexpr REL::ID SET_LOCATION{ 2211681 };      // TESPackage::SetLocation(const PackageLocation&) +759030
		constexpr REL::ID SET_TARGET{ 2211683 };        // TESPackage::SetTarget(const PackageTarget&) +759410
		constexpr REL::ID LOCATION_CTOR{ 2211912 };     // PackageLocation::PackageLocation() +764980
		constexpr REL::ID LOCATION_DTOR{ 2211914 };     // PackageLocation::~PackageLocation() +764A80
		constexpr REL::ID LOCATION_SET_TYPE{ 2211916 }; // PackageLocation::SetType(type) +764AB0
		constexpr REL::ID LOCATION_SET_REF{ 2211923 };  // PackageLocation::SetNearReference(ref) +764CC0
		constexpr REL::ID TARGET_CTOR{ 2211947 };       // PackageTarget::PackageTarget() +767410
		constexpr REL::ID TARGET_DTOR{ 2716753 };       // PackageTarget::~PackageTarget() +767590
		constexpr REL::ID TARGET_SET_TYPE{ 2211959 };   // PackageTarget::SetType(type) +7677D0
		constexpr REL::ID TARGET_SET_REF{ 2211960 };    // PackageTarget::SetSpecificRef(ref) +767850
		// The clear Puppets uses for a made package (AIProcess::ClearCurrentDataforProcess, ClearCurrentPackage).
		constexpr REL::ID CLEAR_PACKAGE_DATA{ 2232435 };
		constexpr REL::ID CLEAR_CURRENT_PACKAGE{ 2231582 };

		constexpr std::int32_t PROCEDURE_FOLLOW = 1;  // RE::PTYPE::kFollow
		constexpr std::int32_t PROCEDURE_TRAVEL = 6;  // RE::PTYPE::kTravel
		constexpr std::uint32_t MADE_FLAGS = 6;        // packFlags the engine's own player and follow packages get
		constexpr std::int32_t LOCATION_NEAR_REF = 0;
		constexpr std::int32_t TARGET_SPECIFIC_REF = 0;

		// The stack objects the engine copies from: a PackageLocation is 0x20 bytes (a vtable, its type at +8, its
		// reference's handle at +18), a PackageTarget 0x18 (its type at +0, the handle at +8). Room to spare.
		struct alignas(16) Scratch
		{
			std::array<std::byte, 0x40> bytes{};
		};

		RE::TESPackage* MakePackage(std::int32_t a_procedure, RE::TESObjectREFR* a_near, RE::Actor* a_target)
		{
			static REL::Relocation<RE::TESPackage* (*)(std::int32_t)>                 create{ CREATE_PACKAGE };
			static REL::Relocation<void (*)(RE::TESPackage*, std::int32_t)>           setProcedure{ SET_PROCEDURE };
			static REL::Relocation<void (*)(RE::TESPackage*, const void*)>            setLocation{ SET_LOCATION };
			static REL::Relocation<void (*)(RE::TESPackage*, const void*)>            setTarget{ SET_TARGET };
			static REL::Relocation<void* (*)(void*)>                                   locationCtor{ LOCATION_CTOR };
			static REL::Relocation<void (*)(void*)>                                    locationDtor{ LOCATION_DTOR };
			static REL::Relocation<void (*)(void*, std::int32_t)>                     locationType{ LOCATION_SET_TYPE };
			static REL::Relocation<void (*)(void*, RE::TESObjectREFR*)>               locationRef{ LOCATION_SET_REF };
			static REL::Relocation<void* (*)(void*)>                                   targetCtor{ TARGET_CTOR };
			static REL::Relocation<void (*)(void*)>                                    targetDtor{ TARGET_DTOR };
			static REL::Relocation<void (*)(void*, std::int32_t)>                     targetType{ TARGET_SET_TYPE };
			static REL::Relocation<void (*)(void*, RE::TESObjectREFR*)>               targetRef{ TARGET_SET_REF };

			const auto package = create(a_procedure);
			if (!package) {
				return nullptr;
			}
			if (a_procedure == PROCEDURE_FOLLOW) {
				setProcedure(package, a_procedure);  // as AIProcess's own Follow package (+CF0F61)
			}
			package->data.packFlags |= MADE_FLAGS;
			Scratch location;
			locationCtor(location.bytes.data());
			locationType(location.bytes.data(), LOCATION_NEAR_REF);
			locationRef(location.bytes.data(), a_near);
			setLocation(package, location.bytes.data());
			locationDtor(location.bytes.data());
			Scratch target;
			targetCtor(target.bytes.data());
			if (a_target) {
				targetType(target.bytes.data(), TARGET_SPECIFIC_REF);
				targetRef(target.bytes.data(), a_target);
			}
			setTarget(package, target.bytes.data());
			targetDtor(target.bytes.data());
			return package;
		}

		// ---- the bot ----

		Plan                                bot;
		bool                                aiDriven = false;        // we set the player AI-driven and have not let go
		std::uint32_t                       madePackage = 0;         // the form id of the package we gave the player
		std::optional<FriendBotPlan::Door>  pendingDoor;             // the door PickDoor found, for the next frame's Facts
		std::optional<std::uint32_t>        target;                  // the enemy picked, looked for every TARGET_EVERY
		Clock::time_point                   nextTargetLook{};
		Clock::time_point                   nextStatus{};
		std::string                         lastLine = "none yet";
		std::uint32_t                       packagesMade = 0;
		std::uint32_t                       doorsUsed = 0;
		std::uint32_t                       aimFrames = 0;
		std::uint32_t                       fireFrames = 0;

		constexpr auto TARGET_EVERY = std::chrono::milliseconds(250);
		constexpr auto STATUS_EVERY = std::chrono::seconds(5);
		constexpr float EYE_HEIGHT = 120.0f;   // the player's eyes above their feet (DevCommands' turnto)
		constexpr float CHEST_HEIGHT = 60.0f;  // what is aimed at above the target's feet

		void Log(std::string a_line)
		{
			if (a_line.empty()) {
				return;
			}
			REX::INFO("FriendBot: {}", a_line);
			lastLine = std::move(a_line);
		}

		FriendBotPlan::Place PlaceOf(const RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return {};
			}
			if (a_cell->IsInterior()) {
				return { a_cell->GetFormID(), 0 };
			}
			return { 0, a_cell->worldSpace ? a_cell->worldSpace->GetFormID() : 0 };
		}

		FriendBotPlan::Point PointOf(const RE::NiPoint3& a_at)
		{
			return { a_at.x, a_at.y, a_at.z };
		}

		RE::TESObjectCELL* CellOf(const RE::TESObjectREFR& a_ref)
		{
			const auto cell = a_ref.GetParentCell();
			return cell ? cell : a_ref.GetSaveParentCell();
		}

		// The player we follow: the one asked for, or the other player with the lowest id (the host in a game of two).
		std::optional<RemotePlayers::Info> Host()
		{
			std::optional<RemotePlayers::Info> found;
			for (auto& info : RemotePlayers::List()) {
				if (bot.Player() != 0 ? info.id == bot.Player() : (!found || info.id < found->id)) {
					found = std::move(info);
				}
			}
			return found;
		}

		RE::Actor* LiveActor(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsDeleted() && !a_actor->IsDisabled() && a_actor->Get3D() && a_actor->GetParentCell() ? a_actor : nullptr;
		}

		// A menu that stops the game (GameStop: the UI's menu count) or takes the player's hands (OpenMenus).
		bool MenuTakesControl()
		{
			using M = OpenMenus::Menu;
			return GameStop::Now() || OpenMenus::AnyOpen({ M::kConsole, M::kDialogue, M::kLooks, M::kPause, M::kPipboy, M::kPowerArmorMod, M::kVats, M::kWorkshop,
										  M::kBarter, M::kBook, M::kContainer, M::kCooking, M::kExamine, M::kHolotape, M::kLevelUp, M::kLockpicking, M::kMessageBox,
										  M::kSitWait, M::kSleepWait, M::kTerminal, M::kTerminalHolotape, M::kCaravan, M::kRobotMod, M::kMultiplayer });
		}

		bool Loading()
		{
			return Hud::LoadingScreenUp() || OpenMenus::Open(OpenMenus::Menu::kFader);
		}

		// Drops the package we gave the player, as Puppets drops a mirror's do-nothing package (the engine's own clear),
		// if it is still the current one.
		void DropMadePackage(RE::PlayerCharacter* a_player)
		{
			const auto process = a_player ? a_player->currentProcess : nullptr;
			const auto package = process ? process->currentPackage.package : nullptr;
			if (!package || madePackage == 0 || package->GetFormID() != madePackage || !PackageRelease::IsMadeForm(madePackage)) {
				madePackage = 0;
				return;
			}
			static REL::Relocation<void (*)(RE::AIProcess*, RE::Actor*)>  clearData{ CLEAR_PACKAGE_DATA };
			static REL::Relocation<void (*)(RE::AIProcess*, RE::Actor&)> clearPackage{ CLEAR_CURRENT_PACKAGE };
			a_player->EndInterruptPackage(false);
			if (process->currentPackage.package == package) {
				clearData(process, a_player);
				clearPackage(process, *a_player);
			}
			madePackage = 0;
		}

		void Release(std::string_view a_why)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (aiDriven && player) {
				player->SetAIControlledPackage(false);
				DropMadePackage(player);
				REX::INFO("FriendBot: let go of the player ({})", a_why);
			}
			aiDriven = false;
			VirtualPad::LetGoAim();
		}

		bool Drive(std::int32_t a_procedure, RE::TESObjectREFR* a_near, RE::Actor* a_target, std::string_view a_what)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->currentProcess || !a_near) {
				return false;
			}
			VirtualPad::LetGoAim();
			const auto package = MakePackage(a_procedure, a_near, a_target);
			if (!package) {
				Log(std::format("could not make a {} package", a_what));
				return false;
			}
			if (!aiDriven) {
				player->SetAIControlledPackage(true);
				aiDriven = true;
			}
			player->PutCreatedPackage(package, false, true, false);
			madePackage = package->GetFormID();
			++packagesMade;
			const auto current = player->currentProcess->currentPackage.package;
			if (packagesMade <= 20 || packagesMade % 50 == 0) {
				REX::INFO("FriendBot: {} package {:08X} to {:08X} (the player's current package now {:08X}, {} made)", a_what, madePackage, a_near->GetFormID(),
					current ? current->GetFormID() : 0, packagesMade);
			}
			return true;
		}

		// The load doors near where the host was last seen, and the one they took.
		std::optional<FriendBotPlan::Door> FindDoor(const FriendBotPlan::Place& a_hostNow, const FriendBotPlan::Point& a_lastSeen)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return std::nullopt;
			}
			std::vector<FriendBotPlan::Door> doors;
			tes->ForEachReferenceInRange(RE::NiPoint3{ a_lastSeen.x, a_lastSeen.y, a_lastSeen.z }, 4000.0f, [&](RE::TESObjectREFR* a_ref) {
				const auto teleport = a_ref && a_ref->extraList ? a_ref->extraList->GetByType<RE::ExtraTeleport>() : nullptr;
				const auto data = teleport ? teleport->teleportData : nullptr;
				if (data && !a_ref->IsDisabled()) {
					const auto linked = data->linkedDoor.get();
					doors.push_back({ a_ref->GetFormID(), PointOf(a_ref->data.location), linked ? PlaceOf(CellOf(*linked)) : FriendBotPlan::Place{} });
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			const auto pick = FriendBotPlan::PickDoor(doors, a_hostNow, a_lastSeen);
			Log(pick ? std::format("{} load doors near where the host was last seen; picked {:08X}", doors.size(), doors[*pick].ref) :
			           std::format("{} load doors near where the host was last seen; none leads where they went (cell {:08X} world {:08X})", doors.size(),
						   a_hostNow.cell, a_hostNow.worldspace));
			return pick ? std::optional{ doors[*pick] } : std::nullopt;
		}

		// The enemy to fight (FriendBotPlan::PickTarget), from the actors the engine runs at full AI.
		std::optional<std::uint32_t> LookForTarget(RE::PlayerCharacter* a_player, RE::Actor* a_host)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return std::nullopt;
			}
			std::vector<FriendBotPlan::Candidate> candidates;
			const auto&                           me = a_player->data.location;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor == a_player || actor == a_host || RemotePlayers::PlayerIdFor(actor->GetFormID()) != 0 || !LiveActor(actor)) {
					continue;
				}
				const auto& at = actor->data.location;
				const float toUs = std::hypot(at.x - me.x, at.y - me.y, at.z - me.z);
				const float toHost = a_host ? std::hypot(at.x - a_host->data.location.x, at.y - a_host->data.location.y, at.z - a_host->data.location.z) : toUs;
				if (std::min(toUs, toHost) > FriendBotPlan::FIGHT_RANGE) {
					continue;
				}
				FriendBotPlan::Candidate c;
				c.ref = actor->GetFormID();
				c.toUs = toUs;
				c.toHost = toHost;
				c.alive = !actor->IsDead(false);
				c.hostile = c.alive && actor->GetHostileToActor(a_player);
				c.inCombat = c.hostile && actor->IsInCombat();
				if (c.inCombat) {
					bool picked = false;
					c.seen = a_player->HasLOSToTarget(actor, &picked);
				}
				candidates.push_back(c);
			}
			const auto pick = FriendBotPlan::PickTarget(candidates);
			return pick ? std::optional{ candidates[*pick].ref } : std::nullopt;
		}

		// One frame of a fight: the right stick toward the target's chest, the trigger held once on it.
		void Aim(RE::PlayerCharacter* a_player)
		{
			const auto actor = target ? RE::TESForm::GetFormByID<RE::Actor>(*target) : nullptr;
			if (!actor || !LiveActor(actor)) {
				VirtualPad::LetGoAim();
				return;
			}
			const auto& me = a_player->data.location;
			const auto& to = actor->data.location;
			const float dx = to.x - me.x;
			const float dy = to.y - me.y;
			const float yaw = a_player->data.angle.z * PadPlan::TO_DEGREES;
			const float pitch = a_player->data.angle.x * PadPlan::TO_DEGREES;
			const float yawOff = PadPlan::AngleDiff(PadPlan::Bearing(dx, dy), yaw);
			const float pitchOff = FriendBotPlan::PitchTo((to.z + CHEST_HEIGHT) - (me.z + EYE_HEIGHT), std::hypot(dx, dy)) - pitch;
			const auto  aim = FriendBotPlan::AimAt(yawOff, pitchOff);
			VirtualPad::HoldAim(aim.rightX, aim.rightY, aim.fire ? 1.0f : 0.0f);
			++aimFrames;
			if (aim.fire) {
				++fireFrames;
			}
		}

		std::string Status()
		{
			const auto host = Host();
			const auto player = RE::PlayerCharacter::GetSingleton();
			float      distance = -1.0f;
			if (host && host->actor && player) {
				const auto& a = host->actor->data.location;
				const auto& b = player->data.location;
				distance = std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
			}
			const auto& door = bot.DoorTaken();
			return std::format("friendbot={} state={}{} mode={} host={} hostDist={:.0f} aiDriven={} package={:08X} made={} door={} doorsUsed={} target={} aim={} fire={} last=\"{}\"",
				Name(bot.Wanted()), Name(bot.Now()), bot.Pause().empty() ? std::string{} : std::format(" (\"{}\")", bot.Pause()), Name(bot.Package()),
				host ? std::format("{}", host->id) : std::string{ "none" }, distance, aiDriven, madePackage, packagesMade,
				door ? std::format("{:08X}", door->ref) : std::string{ "none" }, doorsUsed, target ? std::format("{:08X}", *target) : std::string{ "none" }, aimFrames,
				fireFrames, lastLine);
		}
	}

	std::string Command(std::string_view a_args)
	{
		if (!Config::Get().devChannel) {
			return "error: the friend bot is dev channel only";
		}
		auto parsed = FriendBotPlan::Parse(a_args);
		if (const auto error = std::get_if<std::string>(&parsed)) {
			return *error;
		}
		const auto& command = std::get<FriendBotPlan::Command>(parsed);
		using Kind = FriendBotPlan::Command::Kind;
		switch (command.kind) {
		case Kind::kStatus:
			return Status();
		case Kind::kOn:
			Log(bot.TurnOn(command.player));
			break;
		case Kind::kOff:
			Log(bot.TurnOff());
			Release("off");
			pendingDoor.reset();
			target.reset();
			break;
		case Kind::kWait:
			Log(bot.Wait());
			break;
		case Kind::kFollow:
			Log(bot.Follow());
			break;
		case Kind::kMode:
			Log(bot.SetMode(command.mode));
			break;
		}
		return Status();
	}

	void BeforeSaveOrLoad()
	{
		if (aiDriven) {
			Release("before a save or load");
		}
		bot.Renew();
		pendingDoor.reset();
		target.reset();
	}

	void Frame()
	{
		if (!bot.On() && !aiDriven) {
			return;
		}
		const auto now = Clock::now();
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;

		FriendBotPlan::Facts facts;
		facts.inGame = player && cell;
		facts.loading = Loading();
		facts.menu = facts.inGame && MenuTakesControl();
		facts.dialogue = facts.inGame && RE::IsPlayerInDialogue();
		facts.down = facts.inGame && (player->IsDead(false) || Downed::IsDown());
		facts.ourPlace = PlaceOf(cell);
		if (player) {
			facts.us = PointOf(player->data.location);
		}
		RE::Actor* hostActor = nullptr;
		if (const auto host = Host()) {
			facts.hostKnown = true;
			if (const auto state = RemotePlayers::LastStateOf(host->id)) {
				facts.hostPlace = { state->cell, state->worldspace };
			}
			hostActor = LiveActor(host->actor);
			facts.hostHere = hostActor && RemotePlayers::PositionOf(host->id).has_value();
			if (facts.hostHere) {
				facts.hostActor = hostActor->GetFormID();
				facts.hostAt = PointOf(hostActor->data.location);
			}
		}
		if (facts.inGame && !facts.loading && now >= nextTargetLook) {
			nextTargetLook = now + TARGET_EVERY;
			target = LookForTarget(player, facts.hostHere ? hostActor : nullptr);
		}
		facts.target = target;
		facts.door = pendingDoor;

		const auto decision = bot.Step(now, facts);
		Log(decision.log);
		using Action = FriendBotPlan::Action;
		switch (decision.action) {
		case Action::kNone:
			break;
		case Action::kRelease:
			Release(FriendBotPlan::Name(bot.Now()));
			break;
		case Action::kPickDoor:
			Release("the host went through a door");
			pendingDoor = FindDoor(facts.hostPlace, bot.LastSeen());
			break;
		case Action::kFollowHost:
			if (hostActor) {
				Drive(bot.Package() == FriendBotPlan::Mode::kFollow ? PROCEDURE_FOLLOW : PROCEDURE_TRAVEL, hostActor, hostActor, Name(bot.Package()));
			}
			break;
		case Action::kTravelToDoor:
			pendingDoor.reset();
			if (const auto& door = bot.DoorTaken()) {
				if (const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(door->ref)) {
					Drive(PROCEDURE_TRAVEL, ref, nullptr, "door travel");
				}
			}
			break;
		case Action::kUseDoor:
			Release("at the door");
			if (const auto& door = bot.DoorTaken()) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(door->ref);
				const bool done = ref && player && ref->ActivateRef(player, nullptr, 1, false, false, false);
				++doorsUsed;
				Log(std::format("activated door {:08X}: {}", door->ref, done ? "accepted" : "refused"));
			}
			break;
		case Action::kFight:
			if (aiDriven) {
				Release("a fight");
			}
			if (player) {
				Aim(player);
			}
			break;
		}

		if (bot.On() && now >= nextStatus) {
			nextStatus = now + STATUS_EVERY;
			REX::INFO("FriendBot: status {}", Status());
		}
	}
}
