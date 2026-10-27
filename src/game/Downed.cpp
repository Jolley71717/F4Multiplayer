#include "game/Downed.h"

#include "Config.h"
#include "game/AutoSave.h"
#include "game/AutoSaveRules.h"
#include "game/DownRules.h"
#include "game/GameStop.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/RemotePlayers.h"
#include "game/WorldSync.h"
#include "Text.h"

namespace Downed
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using BoolFlag = RE::Actor::BOOL_FLAGS;

		constexpr auto  GIVE_UP_HOLD = 1s;
		constexpr std::uint32_t DEFAULT_GIVE_UP_KEY = 0x76;  // F7, when the teleport key is off

		bool              essential = false;  // we made the player's character essential
		DownRules::Self   self;
		DownRules::Helper helper;
		Clock::time_point giveUpHeldSince{};
		bool              giveUpHeld = false;
		std::optional<Clock::time_point> lastSaveAt;  // OnBeforeSave, for the stand-up log
		std::optional<Clock::time_point> consoleSeenAt;  // the console last seen open
		constexpr auto                   CONSOLE_GRACE = std::chrono::seconds{ 2 };  // a command's effect may come as it closes
		DownRules::Crawl                 crawl;
		std::optional<Clock::time_point> crawledAt;    // the last frame the player could crawl
		bool                             inputSeen = false;  // this down's first movement input is logged (whether the game reads it while down)

		std::vector<std::vector<std::uint8_t>> outgoing;

		RE::PlayerCharacter* Player()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() ? player : nullptr;
		}

		std::int32_t LifeState(RE::Actor* a_actor)
		{
			return static_cast<std::int32_t>(static_cast<RE::ActorState&>(*a_actor).lifeState);
		}

		bool InEssentialDown(RE::Actor* a_actor)
		{
			return DownRules::IsDownState(LifeState(a_actor));
		}

		// A menu has the game stopped (the pause menu, the Pip-Boy): it stands nobody up meanwhile.
		bool GamePaused()
		{
			return GameStop::Now();
		}

		// Dead, or on the way there.
		bool Dying(RE::Actor* a_actor)
		{
			return DownRules::Dead(LifeState(a_actor), a_actor->IsDead(false));
		}

		// Sets the flag on the player's base form directly (not through `setessential`), so the
		// change isn't recorded for the save.
		void SetEssential(RE::PlayerCharacter* a_player, bool a_on)
		{
			const auto npc = a_player->GetNPC();
			if (!npc || a_on == essential) {
				return;
			}
			// A character that is already essential (another mod) is left alone.
			if (a_on && npc->IsEssential()) {
				return;
			}
			if (a_on) {
				npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
			} else {
				npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential);
			}
			essential = a_on;
		}

		// Holding the teleport key gives up (Party sees that hold); F7 when that key is off.
		std::uint32_t GiveUpVk()
		{
			const auto vk = Config::Get().keyTeleport;
			return vk != 0 ? vk : DEFAULT_GIVE_UP_KEY;
		}

		std::string GiveUpKey()
		{
			const auto vk = GiveUpVk();
			return vk >= 0x70 && vk <= 0x7B ? std::format("F{}", vk - 0x6F) : std::format("key {:X}", vk);
		}

		void SetStayDown(RE::PlayerCharacter* a_player, bool a_on)
		{
			if (a_on) {
				a_player->boolFlags.set(BoolFlag::kNoBleedoutRecovery);
			} else {
				a_player->boolFlags.reset(BoolFlag::kNoBleedoutRecovery);
			}
		}

		// The engine's own start (true) or end (false) of an essential down. 1.11.240: +C83F80.
		// Start, as a lethal hit on an essential character runs it (KillImpl): the life state to essential-down (which plays
		// the graph's bleed-out start, and for the player puts on the bleed-out camera and an input layer that takes
		// activate, jump and the view switch), and the character controller's bleed-out flag (other characters pass through).
		// End, as the engine's own recovery runs it once the stay-down flag is off and there is health again; only while the
		// life state is essential-down: back to alive (the graph's bleed-out stop, the camera and the controls back), full
		// health, and the in-bleed-out-animation and controller flags off.
		void EssentialDown(RE::PlayerCharacter* a_player, bool a_start)
		{
			using start_t = void (*)(RE::Actor*, bool);
			static REL::Relocation<start_t> start{ REL::ID{ 2230033 } };
			start(a_player, a_start);
		}

		void PutDown(RE::PlayerCharacter* a_player)
		{
			EssentialDown(a_player, true);
		}

		// The character controller's flag the engine's essential-down start sets and its end clears: a character controller
		// with it lets other characters through (its collision filter reads it).
		constexpr std::uint32_t CONTROLLER_BLEEDOUT = 1u << 27;

		RE::bhkCharacterController* Controller(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			return process && process->middleHigh ? process->middleHigh->charController.get() : nullptr;
		}

		// Where the player's character controller, 3D and first-person 3D are, the camera and the sneak, for the log: a get-up
		// that leaves the player sunk into the ground or crouched is then a look at the lines (as they get up, and settled).
		std::string PoseFacts(RE::PlayerCharacter* a_player)
		{
			const auto& state = static_cast<RE::ActorState&>(*a_player);
			const auto  z = a_player->data.location.z;
			const auto  third = a_player->Get3D(false);
			const auto  first = a_player->firstPerson3D.get();
			const auto  controller = Controller(a_player);
			const auto  camera = RE::PlayerCamera::GetSingleton();
			const int   view = camera && camera->currentState ? static_cast<int>(camera->currentState->id.underlying()) : -1;
			return std::format("life state {}, knock state {}, sneaking {} (stance {}), z {:.1f}, 3D z {}, first-person 3D z {}, camera state {}, in bleed-out animation {}, controller bleed-out flag {}",
				static_cast<std::uint32_t>(state.lifeState), static_cast<std::uint32_t>(state.knockState), a_player->IsSneaking(), static_cast<std::uint32_t>(state.stance), z,
				third ? std::format("{:.1f}", third->world.translate.z) : std::string{ "none" }, first ? std::format("{:.1f}", first->world.translate.z) : std::string{ "none" }, view,
				a_player->boolFlags.all(BoolFlag::kInBleedoutAnimation), controller ? (controller->flags & CONTROLLER_BLEEDOUT) != 0 : false);
		}

		// Helped up: the health the rules give and the engine's own end of the down, at once. Left to the game, its recovery
		// stood the player up a few seconds later, the same way; other paths of the game's can stand them up before that.
		void StandUp(RE::PlayerCharacter* a_player)
		{
			SetStayDown(a_player, false);
			if (const auto values = RE::ActorValue::GetSingleton(); values && values->health) {
				auto&       owner = static_cast<RE::ActorValueOwner&>(*a_player);
				const float target = owner.GetPermanentActorValue(*values->health) * self.GetRules().reviveHealth;
				const float current = owner.GetActorValue(*values->health);
				if (target > current) {
					owner.RestoreActorValue(*values->health, target - current);
				}
			}
			if (InEssentialDown(a_player)) {
				if (self.standUpAsksNow <= 3) {
					REX::INFO("Downed: standing us up (ask {} of this get-up; {})", self.standUpAsksNow, PoseFacts(a_player));
				}
				EssentialDown(a_player, false);
			}
		}

		// Up again after a down, however it ended: what the engine's end of a down takes off and a path of the game's that
		// stood the player up some other way may have left on. The life state is alive by then; the full end (EssentialDown)
		// does nothing then, so the rest of it is done here.
		void FinishGetUp(RE::PlayerCharacter* a_player)
		{
			std::string left;
			if (a_player->boolFlags.all(BoolFlag::kInBleedoutAnimation)) {
				a_player->boolFlags.reset(BoolFlag::kInBleedoutAnimation);
				left += " in-bleed-out-animation flag;";
			}
			if (const auto controller = Controller(a_player); controller && (controller->flags & CONTROLLER_BLEEDOUT) != 0) {
				controller->flags &= ~CONTROLLER_BLEEDOUT;
				left += " controller bleed-out flag;";
			}
			if (!left.empty()) {
				REX::WARN("Downed: up again, but the down left on:{} taken off", left);
			}
			REX::INFO("Downed: up again ({})", PoseFacts(a_player));
		}

		// Up and still in the sneak they went down in: stood, as the graph's own sneak-exit event does it (PlayerCharacter::SetSneaking: the
		// stance and the graph's iIsInSneak), and the graph told the sneak stop an NPC's takes, should it be crouched anyway.
		void Unsneak(RE::PlayerCharacter* a_player)
		{
			static const RE::BSFixedString sneakStop{ "sneakStop" };
			a_player->SetSneaking(false);
			static_cast<RE::IAnimationGraphManagerHolder*>(a_player)->NotifyAnimationGraphImpl(sneakStop);
			REX::INFO("Downed: up again and sneaking; standing us (sneaking now {})", a_player->IsSneaking());
		}

		// Up and standing, though they went down sneaking: the game stood them by the stance alone, its graph left in its
		// sneak, where the sneak key's start is refused until a load. The graph is told its sneak stop; taken, it was in sneak.
		void GraphSneakStop(RE::PlayerCharacter* a_player, bool a_settled)
		{
			static const RE::BSFixedString sneakStop{ "sneakStop" };
			const bool taken = static_cast<RE::IAnimationGraphManagerHolder*>(a_player)->NotifyAnimationGraphImpl(sneakStop);
			if (taken) {
				REX::WARN("Downed: up again ({}), went down sneaking, standing, but the graph was still in its sneak; told it to stop", a_settled ? "settled" : "getting up");
			} else {
				REX::INFO("Downed: up again ({}), went down sneaking, standing; the graph had no sneak to stop", a_settled ? "settled" : "getting up");
			}
		}

		// What the game shows of the player as it stands them up while they wait for help, for the log: which of
		// its paths did it is then a look at the line (the stay-down flag dropped, the character's own essential
		// copy off, a knock-down's get-up, a save a moment before).
		std::string StandUpFacts(RE::PlayerCharacter* a_player)
		{
			const auto& state = static_cast<RE::ActorState&>(*a_player);
			float       health = 0.0f;
			if (const auto values = RE::ActorValue::GetSingleton(); values && values->health) {
				health = static_cast<RE::ActorValueOwner&>(*a_player).GetActorValue(*values->health);
			}
			const auto npc = a_player->GetNPC();
			const auto sinceSave = lastSaveAt ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *lastSaveAt).count() : -1;
			return std::format("life state {}, knock state {}, health {:.1f}, stay-down flag {}, in bleed-out animation {}, essential: base {} own {} protected {}, ms since a save {}",
				static_cast<std::uint32_t>(state.lifeState), static_cast<std::uint32_t>(state.knockState), health,
				a_player->boolFlags.all(BoolFlag::kNoBleedoutRecovery), a_player->boolFlags.all(BoolFlag::kInBleedoutAnimation),
				npc && npc->IsEssential(), a_player->boolFlags.all(BoolFlag::kEssential), a_player->boolFlags.all(BoolFlag::kProtected), sinceSave);
		}

		// The movement input the game's controls read, and what could hold a crawl back, for the log and Describe: whether
		// the game still reads the movement keys and the stick while the player is down is a look at the line (bDownedCrawl).
		std::string InputFacts()
		{
			const auto controls = RE::PlayerControls::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!controls || !player) {
				return "no controls";
			}
			const auto& move = controls->data.moveInputVec;
			return std::format("move input ({:.2f}, {:.2f}), input blocked {}, knock state {}", move.x, move.y, controls->blockPlayerInput,
				static_cast<std::uint32_t>(static_cast<RE::ActorState&>(*player).knockState));
		}

		// The game stands an essential character up with full health; being helped up leaves the player hurt.
		void TrimHealth(RE::PlayerCharacter* a_player)
		{
			if (const auto values = RE::ActorValue::GetSingleton(); values && values->health) {
				auto&       owner = static_cast<RE::ActorValueOwner&>(*a_player);
				const float excess = owner.GetActorValue(*values->health) - owner.GetPermanentActorValue(*values->health) * self.GetRules().reviveHealth;
				if (excess > 0.0f) {
					owner.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *values->health, -excess);
				}
			}
		}

		void Apply(RE::PlayerCharacter* a_player, const DownRules::Step& a_step)
		{
			if (a_step.stoodUp) {
				// As the game left them, before the flags below are set again.
				REX::WARN("Downed: the game stood us up while we wait for help ({}); putting us down again", StandUpFacts(a_player));
			}
			if (a_step.essential) {
				SetEssential(a_player, *a_step.essential);
			}
			if (a_step.stayDown) {
				SetStayDown(a_player, *a_step.stayDown);
			}
			if (a_step.trimHealth) {
				TrimHealth(a_player);
			}
			if (a_step.wentDown) {
				REX::INFO("Downed: down; a friend can help us up (crawl {}; {})", self.MayCrawl(), InputFacts());
				Hud::Notify(Text::Format(self.MayCrawl() ? "down.went_down_crawl" : "down.went_down", { { "key", GiveUpKey() } }));
				// A death loads the newest save, which in a session can be many minutes old: said before it happens.
				const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(self.GetRules().bleedOut).count();
				Hud::Notify(Text::Format(AutoSaveRules::DeathWarning({ .wentDown = true }), { { "n", seconds } }));
				AutoSave::OnWentDown(self.GetRules().saveOnDown);
			}
			if (a_step.secondsLeft >= 0) {
				Hud::Notify(Text::Format("down.seconds_left", { { "n", a_step.secondsLeft } }));
			}
			if (a_step.putDown) {
				PutDown(a_player);
			}
			if (a_step.backDown) {
				REX::INFO("Downed: down again ({} put-down(s) in all)", self.putDowns);
			}
			if (a_step.upByCommand) {
				REX::INFO("Downed: stood up while waiting for help with the console open ({}); up it is", StandUpFacts(a_player));
			}
			if (a_step.stayedUp) {
				REX::WARN("Downed: the game kept us up through {} s of putting us down ({}); up it is", DownRules::PUT_DOWN_GIVE_UP.count(), StandUpFacts(a_player));
			}
			if (a_step.revivesBack) {
				REX::INFO("Downed: revives back (the window or the fight is over)");
				Hud::Notify(Text::Get("down.revives_back"));
			}
			if (a_step.standUp) {
				StandUp(a_player);
			}
			if (a_step.gotUp) {
				FinishGetUp(a_player);
			}
			if (a_step.unsneak) {
				Unsneak(a_player);
			}
			if (a_step.sneakKept) {
				REX::INFO("Downed: up again and sneaking, though we went down standing (the sneak key while down); left so");
			}
			if (a_step.graphSneakStop) {
				GraphSneakStop(a_player, a_step.settled);
			}
			if (a_step.settled) {
				REX::INFO("Downed: the get-up has played out ({})", PoseFacts(a_player));
			}
			if (a_step.kill) {
				// The character can carry its own copy of the essential flag, which a kill may read instead of the base
				// form's: one left on would put the player down again instead of killing them. Not one another mod made
				// essential.
				if (const auto npc = a_player->GetNPC(); npc && !npc->IsEssential()) {
					a_player->boolFlags.reset(BoolFlag::kEssential);
				}
				if (self.killRetries > 0) {
					REX::WARN("Downed: bled out, but still alive and down; killing again ({} so far)", self.killRetries);
				} else {
					REX::INFO("Downed: bled out; the game loads the newest save (the \"Save: loading\" line names it)");
				}
				AutoSave::OnDying();  // no autosave the game still holds is made of the dead player
				RE::Console::ExecuteCommand("player.kill");
			}
		}

		// Stay next to a downed friend's stand-in for a moment to help them up.
		void HelpFriends(RE::PlayerCharacter* a_player, bool a_able, bool a_fighting, Clock::time_point a_now)
		{
			std::vector<DownRules::Helper::Near> near;
			std::vector<RemotePlayers::Info>     friends;
			if (a_able && helper.FriendsDown() > 0) {
				friends = RemotePlayers::List();
				for (const auto& info : friends) {
					if (info.actor && info.actor->GetParentCell()) {
						near.push_back({ info.id, info.actor->data.location.GetDistance(a_player->data.location) });
					}
				}
			}
			const auto step = helper.Frame(a_able, near, a_now, a_fighting);
			const auto nameOf = [&](std::uint32_t a_id) {
				const auto it = std::ranges::find(friends, a_id, &RemotePlayers::Info::id);
				return it != friends.end() ? it->name : std::string{};
			};
			if (step.blocked) {
				Hud::Notify(Text::Format("down.cant_help_fighting", { { "player", nameOf(step.blocked) } }));
			}
			if (step.started) {
				Hud::Notify(Text::Format("down.helping", { { "player", nameOf(step.started) } }));
			}
			if (step.helped) {
				outgoing.push_back(Protocol::Encode(Protocol::Revive{ step.helped }, Protocol::MessageType::kRevive));
				REX::INFO("Downed: helped player {} up", step.helped);
				Hud::Notify(Text::Format("down.helped", { { "player", nameOf(step.helped) } }));
			}
		}

		// How far along the line from a_from to a_to the world has something a crawl stops at (DownRules::CrawlStops), by the
		// engine's own pick through the cell's physics (as Party's SurfaceAlong). Anything else it hits is passed through, our
		// own body first: the rays start inside it. Nothing: nullopt.
		std::optional<float> SolidAlong(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			const auto  line = a_to - a_from;
			const float length = line.Length();
			if (!(length > 0.0f)) {
				return std::nullopt;
			}
			float start = 0.0f;
			for (int tries = 0; tries < 8 && start < length; ++tries) {
				RE::bhkPickData pick;
				pick.SetStartEnd(a_from + line * (start / length), a_to);
				static_cast<void>(a_cell->Pick(pick));  // what it hit is read from the body below
				if (!pick.HasHit()) {
					return std::nullopt;
				}
				const float fraction = pick.GetHitFraction();
				if (!std::isfinite(fraction)) {
					return std::nullopt;
				}
				const float along = start + std::clamp(fraction, 0.0f, 1.0f) * (length - start);
				const auto  body = pick.GetBody();
				const auto  layer = body ? static_cast<std::uint32_t>(RE::CFilter{ body->m_collisionFilterInfo }.GetCollisionLayer()) : 0u;
				if (DownRules::CrawlStops(layer)) {
					return along;
				}
				start = along + 1.0f;
			}
			return std::nullopt;
		}

		// A friend's stand-in within help range: they may be helping us up, and a crawl away would start that over.
		bool FriendNear(RE::PlayerCharacter* a_player)
		{
			for (const auto& info : RemotePlayers::List()) {
				if (info.actor && info.actor->GetParentCell() && info.actor->data.location.GetDistance(a_player->data.location) <= DownRules::HELP_RANGE) {
					return true;
				}
			}
			return false;
		}

		// Down, with the session's crawl rule: the player's movement input turns them and moves them a step, where the world
		// lets it (DownRules::Crawl). Their position goes to friends as it always does; their stand-in lies where they are.
		void CrawlFrame(RE::PlayerCharacter* a_player, bool a_paused, Clock::time_point a_now)
		{
			const auto controls = RE::PlayerControls::GetSingleton();
			const auto input = controls ? controls->data.moveInputVec : RE::NiPoint2{};
			// Whether the game reads the movement keys and the stick while the player is down, rule or not: once a down.
			if (!self.WaitsForHelp()) {
				inputSeen = false;
			} else if (!inputSeen && DownRules::Crawl::Pushed(input.x, input.y)) {
				inputSeen = true;
				REX::INFO("Downed: movement input while down ({})", InputFacts());
			}
			if (!self.MayCrawl()) {
				crawledAt.reset();
				return;
			}
			const float seconds = crawledAt ? std::chrono::duration<float>(a_now - *crawledAt).count() : 0.0f;
			crawledAt = a_now;
			const auto cell = a_player->GetParentCell();
			if (!cell || !a_player->Get3D()) {
				return;
			}
			using CharacterState = RE::IMovementState::CHARACTER_STATE;
			const auto                    state = static_cast<const RE::IMovementState&>(*a_player).DoGetCharacterState();
			const DownRules::Crawl::Facts facts{ .may = true,
				.x = input.x,
				.y = input.y,
				.seconds = seconds,
				.paused = a_paused,
				.inAir = state == CharacterState::kJumping || state == CharacterState::kInAir,
				.swimming = state == CharacterState::kSwimming,
				.inFurniture = static_cast<int>(a_player->DoGetSitSleepState()) != 0,
				.knockedOver = static_cast<RE::ActorState&>(*a_player).knockState != 0,
				.inScene = a_player->GetCurrentScene() != nullptr,
				.friendNear = DownRules::Crawl::Pushed(input.x, input.y) && FriendNear(a_player) };
			const auto move = DownRules::Crawl::Wanted(facts);
			const float heading = DownRules::Crawl::Turned(a_player->data.angle.z, move.turn);
			if (move.turn != 0.0f) {
				a_player->SetHeading(heading);
			}
			if (move.forward == 0.0f) {
				return;
			}
			const auto feet = a_player->data.location;
			const auto way = DownRules::Crawl::Along(heading, move.forward > 0.0f ? 1.0f : -1.0f);
			const auto at = [&](float a_along, float a_up) {
				return RE::NiPoint3{ feet.x + way[0] * a_along, feet.y + way[1] * a_along, feet.z + a_up };
			};
			const float step = std::fabs(move.forward);
			const float reach = step + DownRules::CRAWL_BODY + 1.0f;
			DownRules::Crawl::Probe probe;
			for (const float up : { DownRules::CRAWL_PROBE_LOW, DownRules::CRAWL_PROBE_HIGH }) {
				if (const auto wall = SolidAlong(cell, at(0.0f, up), at(reach, up)); wall && (!probe.wall || *wall < *probe.wall)) {
					probe.wall = wall;
				}
			}
			const auto ground = [&](float a_along) -> std::optional<float> {
				const auto hit = SolidAlong(cell, at(a_along, DownRules::CRAWL_PROBE_LOW), at(a_along, -DownRules::CRAWL_GROUND_REACH));
				return hit ? std::optional<float>{ feet.z + DownRules::CRAWL_PROBE_LOW - *hit } : std::nullopt;
			};
			if (!probe.wall) {  // a wall refuses the step whatever the ground
				probe.groundThere = ground(step);
				probe.groundAhead = ground(DownRules::CRAWL_AHEAD);
			}
			const auto verdict = DownRules::Crawl::Accept(feet.z, move.forward, probe);
			if (crawl.Count(verdict, move.forward)) {
				REX::INFO("Downed: crawl refused ({}; {} step(s) taken, {} refused)", DownRules::RefusedName(verdict.refused), crawl.steps, crawl.refused);
			}
			if (verdict.refused == DownRules::Crawl::Refused::kNone) {
				auto to = at(step, 0.0f);
				to.z = verdict.z;
				a_player->SetPosition(to, true);
			}
		}
	}

	void Frame(bool a_withFriends)
	{
		const auto player = Player();
		if (!player) {
			return;
		}
		// The rules in force: revive turned off while we are down waits until we are up (DownRules::Self::SetRules).
		const bool enabled = self.GetRules().revive && a_withFriends && WorldSync::InWorld();
		const auto now = Clock::now();
		const bool fighting = player->IsInCombat();
		// A stand-up while the console is open, or just after it closed, is the player's own command (DownRules::Facts).
		if (OpenMenus::Open(OpenMenus::Menu::kConsole)) {
			consoleSeenAt = now;
		}
		const bool console = consoleSeenAt && now - *consoleSeenAt < CONSOLE_GRACE;

		const DownRules::Facts facts{ .enabled = enabled,
			.essential = essential,
			.down = InEssentialDown(player),
			.dead = Dying(player),
			.paused = GamePaused(),
			.inCombat = fighting,
			.console = console,
			.knockedOver = static_cast<RE::ActorState&>(*player).knockState != 0,
			.sneaking = player->IsSneaking() };
		Apply(player, self.Frame(facts, now));

		// With the teleport key off, Party doesn't see the hold: watch the fallback key here.
		if (self.WaitsForHelp() && Config::Get().keyTeleport == 0) {
			const bool held = Hotkeys::Held(DEFAULT_GIVE_UP_KEY);
			if (held && !giveUpHeld) {
				giveUpHeldSince = now;
			} else if (held && now - giveUpHeldSince >= GIVE_UP_HOLD) {
				self.GiveUp();
			}
			giveUpHeld = held;
		}

		CrawlFrame(player, facts.paused, now);

		// Only while up: helping is reset meanwhile, so being down for a moment does not count toward it.
		HelpFriends(player, enabled && self.Now() == DownRules::Self::Phase::kUp && !facts.dead, fighting, now);
	}

	void SetRules(const Protocol::SessionRules& a_rules, std::uint64_t a_session)
	{
		const auto rules = DownRules::Rules::From(a_rules);
		self.SetRules(rules, a_session);
		helper.SetRules(rules);
		if (self.RulesWaiting()) {
			REX::INFO("Downed: new rules while we are down; they apply once we are up");
		}
	}

	bool IsDown()
	{
		return self.IsDown();
	}

	bool WaitsForHelp()
	{
		return self.WaitsForHelp();
	}

	void GiveUp()
	{
		self.GiveUp();
	}

	void ApplyRevive(const Protocol::Revive& a_revive)
	{
		const auto player = Player();
		const auto now = Clock::now();
		// Stood up by the next Frame (DownRules::Self's standUp), once a knock-down is over.
		if (!player || !self.Revive(now)) {
			return;
		}
		const auto name = RemotePlayers::NameOf(a_revive.playerId);
		REX::INFO("Downed: helped up by player {}", a_revive.playerId);
		Hud::Notify(Text::Format("down.helped_you", { { "player", name.empty() ? Text::Get("party.a_friend") : name } }));
		// The host's limits, so the next lethal hit is no surprise.
		const auto after = self.AfterRevive(now);
		if (after.lastRevive) {
			REX::INFO("Downed: that was the last revive for now");
			Hud::Notify(Text::Get("down.last_revive"));
		} else if (const int left = self.RevivesLeft(now); left > 0) {
			Hud::Notify(Text::Format("down.revives_left", { { "n", left } }));
		}
		if (after.cooldownSeconds > 0) {
			Hud::Notify(Text::Format("down.cooldown", { { "n", after.cooldownSeconds } }));
		}
		// The next lethal hit may kill outright now: and a death loads the newest save.
		if (const auto warning = AutoSaveRules::DeathWarning({ .lastRevive = after.lastRevive, .cooldownSeconds = after.cooldownSeconds }); !warning.empty()) {
			Hud::Notify(Text::Get(warning));
		}
	}

	void SetFriendDown(std::uint32_t a_id, bool a_down)
	{
		helper.SetFriendDown(a_id, a_down);
	}

	void OnPlayerLeft(std::uint32_t a_id)
	{
		helper.Left(a_id);
	}

	void OnBeforeSave()
	{
		// Neither flag may end up in the save; while down, OnAfterSave sets both again (and Frame, should it not come).
		lastSaveAt = Clock::now();
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			SetEssential(player, false);
			if (self.IsDown()) {
				SetStayDown(player, false);
			}
		}
	}

	void OnAfterSave()
	{
		// At once, not at the next frame of ours: the engine's own update may come first, and with the stay-down flag
		// off it stands a downed player up.
		const auto player = Player();
		if (!player || !self.IsDown()) {
			return;
		}
		SetEssential(player, true);
		if (self.WaitsForHelp()) {
			SetStayDown(player, true);
		}
	}

	void OnGameLoaded()
	{
		// A save made while down (the flags were cleared for it): without the essential flag the
		// game would finish the player off. Stand them up instead, as if helped (the next Frame).
		const auto player = Player();
		if (self.Loaded(player && InEssentialDown(player), Clock::now())) {
			SetEssential(player, true);
			REX::INFO("Downed: loaded a save with the player down; getting them up");
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Reset()
	{
		helper.Clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		const auto now = Clock::now();
		return std::format("downed: essential={} down={} waits={} downs={} revives={} bleedOuts={} killRetries={} standUps={} putDowns={} standUpAsks={} getUps={} unsneaks={} graphSneakStops={} helped={} friendsDown={} revivesLeft={} cooldown={} rulesWaiting={} | crawl: may={} steps={} refused={} last={} distance={:.0f} | {}",
			essential, self.IsDown(), self.WaitsForHelp(), self.downs, self.revives, self.bleedOuts, self.killRetries, self.standUps, self.putDowns, self.standUpAsks, self.getUps, self.unsneaks, self.graphSneakStops, helper.helped,
			helper.FriendsDown(), self.RevivesLeft(now), self.InCooldown(now), self.RulesWaiting(), self.MayCrawl(), crawl.steps, crawl.refused, DownRules::RefusedName(crawl.Last()),
			crawl.distance, InputFacts());
	}
}
