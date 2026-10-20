#include "game/Downed.h"

#include "Config.h"
#include "game/DownRules.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
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
			const auto ui = RE::UI::GetSingleton();
			return ui && ui->menuMode > 0;
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

		// Health for getting up; the game stands the player up once the "stay down" flag is off.
		void GetUp(RE::PlayerCharacter* a_player)
		{
			const auto values = RE::ActorValue::GetSingleton();
			SetStayDown(a_player, false);
			if (values && values->health) {
				auto&       owner = static_cast<RE::ActorValueOwner&>(*a_player);
				const float target = owner.GetPermanentActorValue(*values->health) * self.GetRules().reviveHealth;
				const float current = owner.GetActorValue(*values->health);
				if (target > current) {
					owner.RestoreActorValue(*values->health, target - current);
				}
			}
		}

		// The engine's own start of an essential down (Actor, true), as a lethal hit on an essential character runs it
		// (KillImpl): the life state, the bleed-out camera for the player, and the bleed-out event the animation answers.
		// 1.11.240: +C83F80.
		void PutDown(RE::PlayerCharacter* a_player)
		{
			using start_t = void (*)(RE::Actor*, bool);
			static REL::Relocation<start_t> start{ REL::ID{ 2230033 } };
			start(a_player, true);
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
				REX::INFO("Downed: down; a friend can help us up");
				Hud::Notify(Text::Format("down.went_down", { { "key", GiveUpKey() } }));
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
			if (a_step.stayedUp) {
				REX::WARN("Downed: the game kept us up through {} s of putting us down ({}); up it is", DownRules::PUT_DOWN_GIVE_UP.count(), StandUpFacts(a_player));
			}
			if (a_step.revivesBack) {
				REX::INFO("Downed: revives back (the window or the fight is over)");
				Hud::Notify(Text::Get("down.revives_back"));
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
					REX::INFO("Downed: bled out");
				}
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
	}

	void Frame(bool a_withFriends)
	{
		const auto player = Player();
		if (!player) {
			return;
		}
		const bool enabled = self.GetRules().revive && a_withFriends && WorldSync::InWorld();
		const auto now = Clock::now();
		const bool fighting = player->IsInCombat();

		const DownRules::Facts facts{ .enabled = enabled,
			.essential = essential,
			.down = InEssentialDown(player),
			.dead = Dying(player),
			.paused = GamePaused(),
			.inCombat = fighting };
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

		// Only while up: helping is reset meanwhile, so being down for a moment does not count toward it.
		HelpFriends(player, enabled && self.Now() == DownRules::Self::Phase::kUp && !facts.dead, fighting, now);
	}

	void SetRules(const Protocol::SessionRules& a_rules)
	{
		const auto rules = DownRules::Rules::From(a_rules);
		self.SetRules(rules);
		helper.SetRules(rules);
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
		if (!player || !self.Revive(now)) {
			return;
		}
		GetUp(player);
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
		// game would finish the player off. Stand them up instead, as if helped.
		const auto player = Player();
		if (self.Loaded(player && InEssentialDown(player), Clock::now())) {
			SetEssential(player, true);
			GetUp(player);
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
		return std::format("downed: essential={} down={} waits={} downs={} revives={} bleedOuts={} killRetries={} standUps={} putDowns={} helped={} friendsDown={} revivesLeft={} cooldown={}",
			essential, self.IsDown(), self.WaitsForHelp(), self.downs, self.revives, self.bleedOuts, self.killRetries, self.standUps, self.putDowns, helper.helped,
			helper.FriendsDown(), self.RevivesLeft(now), self.InCooldown(now));
	}
}
