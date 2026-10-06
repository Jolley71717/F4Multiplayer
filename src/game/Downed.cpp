#include "game/Downed.h"

#include "Config.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
#include "game/RemotePlayers.h"
#include "game/WorldSync.h"

namespace Downed
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using BoolFlag = RE::Actor::BOOL_FLAGS;

		constexpr auto  BLEEDOUT_TIME = 45s;
		constexpr auto  HELP_TIME = 2s;          // standing next to a downed friend this long helps them up
		constexpr auto  HELP_RETRY = 3s;         // before helping the same friend again
		constexpr float HELP_RANGE = 180.0f;     // game units (about 2.5 m)
		constexpr float REVIVE_HEALTH = 0.3f;    // of maximum
		constexpr auto  GET_UP_TIME = 20s;       // after being helped, the game stands the player up by then
		constexpr auto  GIVE_UP_HOLD = 1s;
		constexpr std::uint32_t DEFAULT_GIVE_UP_KEY = 0x76;  // F7, when the teleport key is off

		bool              essential = false;  // we made the player's character essential
		bool              down = false;
		bool              giveUp = false;
		bool              revived = false;  // a friend helped us; we're getting up
		Clock::time_point revivedAt{};
		Clock::time_point giveUpHeldSince{};
		bool              giveUpHeld = false;
		Clock::time_point downSince{};
		int               reminders = 0;

		std::set<std::uint32_t>                friendsDown;
		std::uint32_t                          helping = 0;
		Clock::time_point                      helpSince{};
		Clock::time_point                      helpAgain{};
		std::vector<std::vector<std::uint8_t>> outgoing;

		std::uint32_t downs = 0;
		std::uint32_t revives = 0;
		std::uint32_t bleedOuts = 0;
		std::uint32_t helped = 0;

		RE::PlayerCharacter* Player()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() ? player : nullptr;
		}

		bool InEssentialDown(RE::Actor* a_actor)
		{
			const auto state = static_cast<RE::ACTOR_LIFE_STATE>(static_cast<RE::ActorState&>(*a_actor).lifeState);
			return state == RE::ACTOR_LIFE_STATE::kEssentialDown || state == RE::ACTOR_LIFE_STATE::kBleedout;
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
				const float target = owner.GetPermanentActorValue(*values->health) * REVIVE_HEALTH;
				const float current = owner.GetActorValue(*values->health);
				if (target > current) {
					owner.RestoreActorValue(*values->health, target - current);
				}
			}
			down = true;
			revived = true;
			revivedAt = Clock::now();
		}

		void BleedOut(RE::PlayerCharacter* a_player)
		{
			down = false;
			revived = false;
			SetStayDown(a_player, false);
			SetEssential(a_player, false);
			++bleedOuts;
			RE::Console::ExecuteCommand("player.kill");
		}

		// Stay next to a downed friend's stand-in for HELP_TIME to help them up.
		void HelpFriends(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			std::uint32_t near = 0;
			std::string   name;
			if (!friendsDown.empty() && !a_player->IsDead(false) && a_now >= helpAgain) {
				for (const auto& info : RemotePlayers::List()) {
					if (info.actor && friendsDown.contains(info.id) && info.actor->GetParentCell() &&
						info.actor->data.location.GetDistance(a_player->data.location) <= HELP_RANGE) {
						near = info.id;
						name = info.name;
						break;
					}
				}
			}
			if (near != helping) {
				helping = near;
				helpSince = a_now;
				if (near) {
					Hud::Notify(std::format("Helping {} up...", name));
				}
			} else if (near && a_now - helpSince >= HELP_TIME) {
				outgoing.push_back(Protocol::Encode(Protocol::Revive{ near }, Protocol::MessageType::kRevive));
				Hud::Notify(std::format("You helped {} up", name));
				++helped;
				helping = 0;
				helpAgain = a_now + HELP_RETRY;
			}
		}
	}

	void Frame(bool a_withFriends)
	{
		const auto player = Player();
		if (!player) {
			return;
		}
		const bool enabled = Config::Get().revive && a_withFriends && WorldSync::InWorld();
		const auto now = Clock::now();

		if (!down) {
			// Not while dying: making a dying character essential again could stop the death.
			if (!player->IsDead(false) || !enabled) {
				SetEssential(player, enabled);
			}
			if (essential && InEssentialDown(player)) {
				down = true;
				giveUp = false;
				revived = false;
				downSince = now;
				reminders = 0;
				++downs;
				// The game would stand the player up again by itself after a while.
				SetStayDown(player, true);
				Hud::Notify(std::format("You're down! A friend can help you up. Hold {} to give up", GiveUpKey()));
			}
		} else if (!InEssentialDown(player)) {
			down = false;  // up again
			SetStayDown(player, false);
			// The game stands an essential character up with full health; being helped up
			// leaves the player hurt.
			if (const auto values = RE::ActorValue::GetSingleton(); revived && values && values->health) {
				auto&       owner = static_cast<RE::ActorValueOwner&>(*player);
				const float excess = owner.GetActorValue(*values->health) - owner.GetPermanentActorValue(*values->health) * REVIVE_HEALTH;
				if (excess > 0.0f) {
					owner.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *values->health, -excess);
				}
			}
			revived = false;
		} else if (revived) {
			// Helped up: the game stands the player up on its own. If it somehow doesn't, they die.
			if (now - revivedAt >= GET_UP_TIME) {
				BleedOut(player);
			}
		} else if (giveUp || !enabled || now - downSince >= BLEEDOUT_TIME) {
			BleedOut(player);
		} else {
			// A save clears both flags (OnBeforeSave); still down, so set them again.
			SetEssential(player, true);
			SetStayDown(player, true);
			// With the teleport key off, Party doesn't see the hold: watch the fallback key here.
			if (Config::Get().keyTeleport == 0) {
				const bool held = Hotkeys::Held(DEFAULT_GIVE_UP_KEY);
				if (held && !giveUpHeld) {
					giveUpHeldSince = now;
				} else if (held && now - giveUpHeldSince >= GIVE_UP_HOLD) {
					giveUp = true;
				}
				giveUpHeld = held;
			}
			const auto left = std::chrono::duration_cast<std::chrono::seconds>(BLEEDOUT_TIME - (now - downSince)).count();
			if ((reminders == 0 && left <= 30) || (reminders == 1 && left <= 10)) {
				++reminders;
				Hud::Notify(std::format("{} seconds left to be helped up", left));
			}
		}

		if (!down && enabled) {
			HelpFriends(player, now);
		}
	}

	bool IsDown()
	{
		return down;
	}

	void GiveUp()
	{
		giveUp = down;
	}

	void ApplyRevive(const Protocol::Revive& a_revive)
	{
		const auto player = Player();
		if (!down || revived || !player) {
			return;
		}
		GetUp(player);
		++revives;
		const auto name = RemotePlayers::NameOf(a_revive.playerId);
		Hud::Notify(std::format("{} helped you up", name.empty() ? "A friend" : name));
	}

	void SetFriendDown(std::uint32_t a_id, bool a_down)
	{
		if (a_down) {
			friendsDown.insert(a_id);
		} else {
			friendsDown.erase(a_id);
		}
	}

	void OnPlayerLeft(std::uint32_t a_id)
	{
		friendsDown.erase(a_id);
	}

	void OnBeforeSave()
	{
		// Neither flag may end up in the save; while down, Frame sets both again.
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			SetEssential(player, false);
			if (down) {
				SetStayDown(player, false);
			}
		}
	}

	void OnGameLoaded()
	{
		down = false;
		giveUp = false;
		revived = false;
		// A save made while down (the flags were cleared for it): without the essential flag the
		// game would finish the player off. Stand them up instead, as if helped.
		const auto player = Player();
		if (player && InEssentialDown(player)) {
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
		friendsDown.clear();
		helping = 0;
		outgoing.clear();
	}

	std::string Describe()
	{
		return std::format("downed: essential={} down={} downs={} revives={} bleedOuts={} helped={} friendsDown={}",
			essential, down, downs, revives, bleedOuts, helped, friendsDown.size());
	}
}
