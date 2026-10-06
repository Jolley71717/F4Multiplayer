#include "game/Party.h"

#include "Config.h"
#include "game/Downed.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
#include "game/MapShare.h"
#include "game/QuestSync.h"
#include "game/RemotePlayers.h"
#include "game/WorldSync.h"

namespace Party
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto STATUS_INTERVAL = 2s;
		constexpr auto STATUS_MIN_GAP = 250ms;
		constexpr auto HEARTBEAT_INTERVAL = 1s;
		constexpr auto UNSTABLE_AFTER = 4s;  // no heartbeat answer for this long
		constexpr auto KILL_XP_WINDOW = 2s;  // XP gained this soon after a kill counts as kill XP
		constexpr auto UNCLAIMED_XP_WINDOW = 5s;  // ... or this soon before its death event arrives
		// Kill XP waits this long before it's reported: a quest stage or a discovery seen in the
		// meantime means some of it was quest or discovery XP, which isn't shared (the quest
		// reaches the others by itself).
		constexpr auto XP_HOLD = 2500ms;
		constexpr auto XP_NOTICE_DELAY = 3s;
		constexpr auto TELEPORT_TIMEOUT = 30s;
		constexpr std::uint32_t HIGH_PING_MS = 400;
		constexpr float UNITS_TO_METERS = 0.0142875f;

		struct Known
		{
			Protocol::PlayerStatus status;
			bool                   hasStatus = false;
		};

		struct PendingTeleport
		{
			std::uint32_t     cell = 0;
			std::uint32_t     worldspace = 0;
			RE::NiPoint3      position;
			Clock::time_point deadline{};
		};

		std::uint32_t                          localId = 0;
		std::map<std::uint32_t, Known>         known;
		std::vector<std::vector<std::uint8_t>> outgoing;

		std::optional<Protocol::PlayerStatus> sentStatus;
		Clock::time_point                     nextStatus{};
		Clock::time_point                     lastStatusSent{};

		std::uint32_t                                       heartbeatToken = 0;
		std::map<std::uint32_t, Clock::time_point>          heartbeatsSent;
		Clock::time_point                                   nextHeartbeat{};
		Clock::time_point                                   lastAck{};
		std::uint32_t                                       roundTripMs = 0;
		bool                                                unstable = false;
		int                                                 slowAnswers = 0;
		Clock::time_point                                   nextPingWarning{};

		std::optional<float> lastXp;
		float                xpCredit = 0.0f;  // shares we applied, not to be reported as our own XP
		Clock::time_point    killWindowEnd{};
		float                unclaimedXp = 0.0f;  // XP that came before its kill was seen
		Clock::time_point    unclaimedSince{};
		Clock::time_point    unclaimedUntil{};
		float                heldXp = 0.0f;  // kill XP waiting for XP_HOLD
		Clock::time_point    heldSince{};
		std::uint32_t        xpWithheld = 0;
		float                xpNotice = 0.0f;
		Clock::time_point    xpNoticeAt{};

		std::uint32_t                  teleportTarget = 0;
		std::optional<PendingTeleport> pendingTeleport;

		Clock::time_point killFeedWindow{};
		int               killFeedShown = 0;

		RE::PlayerCharacter* Player()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() ? player : nullptr;
		}

		// Where an actor is, as in states: (interior cell, 0) or (0, worldspace).
		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::Actor* a_actor)
		{
			const auto cell = a_actor ? a_actor->GetParentCell() : nullptr;
			if (!cell) {
				return { 0, 0 };
			}
			if (cell->IsInterior()) {
				return { cell->GetFormID(), 0 };
			}
			return { 0, cell->worldSpace ? cell->worldSpace->GetFormID() : 0 };
		}

		std::string NameOfForm(std::uint32_t a_id)
		{
			const auto form = a_id ? RE::TESForm::GetFormByID(a_id) : nullptr;
			return form ? std::string{ RE::TESFullName::GetFullName(*form) } : std::string{};
		}

		std::string PlaceName(std::uint32_t a_location, std::uint32_t a_cell)
		{
			auto name = NameOfForm(a_cell);
			if (name.empty()) {
				name = NameOfForm(a_location);
			}
			return name;
		}

		std::string Bearing(float a_dx, float a_dy)
		{
			static constexpr std::array names{ "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
			float degrees = std::atan2(a_dx, a_dy) * 180.0f / std::numbers::pi_v<float>;
			if (degrees < 0.0f) {
				degrees += 360.0f;
			}
			return names[static_cast<std::size_t>(std::lround(degrees / 45.0f)) % names.size()];
		}

		// "230 m NE", or "" when they're somewhere else.
		std::string DistanceTo(std::uint32_t a_cell, std::uint32_t a_worldspace, float a_x, float a_y)
		{
			const auto player = Player();
			if (!player || SpaceOf(player) != std::pair{ a_cell, a_worldspace }) {
				return {};
			}
			const float dx = a_x - player->data.location.x;
			const float dy = a_y - player->data.location.y;
			const float meters = std::sqrt(dx * dx + dy * dy) * UNITS_TO_METERS;
			if (meters < 5.0f) {
				return "right here";
			}
			return std::format("{:.0f} m {}", meters, Bearing(dx, dy));
		}

		std::string Describe(const RemotePlayers::Info& a_player)
		{
			std::string out = a_player.name;
			const auto  it = known.find(a_player.id);
			if (it != known.end() && it->second.hasStatus) {
				const auto& status = it->second.status;
				out += status.downed ? " (down)" : status.health == 0 ? " (dead)" : std::format(" ({}% HP, level {})", status.health, status.level);
				if (auto place = PlaceName(status.location, status.cell); !place.empty()) {
					out += " - " + place;
				}
			}
			if (a_player.state) {
				if (auto distance = DistanceTo(a_player.state->cell, a_player.state->worldspace, a_player.state->x, a_player.state->y); !distance.empty()) {
					out += ", " + distance;
				}
			} else {
				out += ", loading";
			}
			return out;
		}

		std::optional<Protocol::PlayerStatus> SampleStatus()
		{
			const auto player = Player();
			const auto values = RE::ActorValue::GetSingleton();
			if (!player || !values || !values->health) {
				return std::nullopt;
			}
			const auto& owner = static_cast<const RE::ActorValueOwner&>(*player);
			const float maximum = owner.GetPermanentActorValue(*values->health);
			const float current = owner.GetActorValue(*values->health);
			Protocol::PlayerStatus status;
			status.location = player->currentLocation ? player->currentLocation->GetFormID() : 0;
			status.cell = SpaceOf(player).first;
			if (!player->IsDead(false) && maximum > 0.0f) {
				status.health = static_cast<std::uint8_t>(std::clamp(std::lround(current / maximum * 100.0f), 1L, 100L));
			}
			status.level = static_cast<std::uint16_t>((std::max)(player->GetLevel(), std::int16_t{ 0 }));
			status.downed = Downed::IsDown();
			return status;
		}

		void SendStatus(Clock::time_point a_now)
		{
			const auto status = SampleStatus();
			if (!status) {
				return;
			}
			const bool changed = !sentStatus || *sentStatus != *status;
			if ((changed && a_now - lastStatusSent >= STATUS_MIN_GAP) || a_now >= nextStatus) {
				outgoing.push_back(Protocol::Encode(*status, Protocol::MessageType::kReportStatus));
				sentStatus = status;
				lastStatusSent = a_now;
				nextStatus = a_now + STATUS_INTERVAL;
			}
		}

		void Heartbeat(Clock::time_point a_now)
		{
			// Loading screens stall the main thread; answers that waited meanwhile aren't the
			// connection's fault.
			static Clock::time_point lastFrame{};
			if (a_now - lastFrame > 1s || !WorldSync::InWorld()) {
				lastAck = (std::max)(lastAck, a_now - 1s);
			}
			lastFrame = a_now;
			if (lastAck == Clock::time_point{}) {
				lastAck = a_now;  // start the clock with the session
			}

			if (a_now >= nextHeartbeat) {
				nextHeartbeat = a_now + HEARTBEAT_INTERVAL;
				heartbeatsSent[++heartbeatToken] = a_now;
				while (heartbeatsSent.size() > 10) {
					heartbeatsSent.erase(heartbeatsSent.begin());
				}
				outgoing.push_back(Protocol::Encode(Protocol::Heartbeat{ heartbeatToken }, Protocol::MessageType::kHeartbeat));
			}
			if (!unstable && a_now - lastAck > UNSTABLE_AFTER) {
				unstable = true;
				Hud::Notify("Multiplayer: connection to the host is unstable");
			}
		}

		const RE::ActorValueInfo* ExperienceInfo()
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values ? values->experience : nullptr;
		}

		// Reports XP we got for kills, so the others get their share.
		void TrackXp(Clock::time_point a_now)
		{
			const auto player = Player();
			const auto info = ExperienceInfo();
			if (!player || !info) {
				return;
			}
			// The kill (death event) and its XP can come in either order: the death event waits
			// for the victim's next AI update.
			const auto hold = [&](float a_xp, Clock::time_point a_since) {
				if (heldXp <= 0.0f) {
					heldSince = a_since;
				}
				heldXp += a_xp;
			};
			if (a_now >= unclaimedUntil) {
				unclaimedXp = 0.0f;
			}
			if (WorldSync::TakePlayerKills() > 0) {
				killWindowEnd = a_now + KILL_XP_WINDOW;
				if (unclaimedXp >= 1.0f) {
					hold(unclaimedXp, unclaimedSince);
				}
				unclaimedXp = 0.0f;
			}
			const float xp = static_cast<const RE::ActorValueOwner&>(*player).GetActorValue(*info);
			if (lastXp && xp > *lastXp) {
				float gain = xp - *lastXp;
				const float credited = (std::min)(gain, xpCredit);
				xpCredit -= credited;
				gain -= credited;
				if (gain >= 1.0f && a_now < killWindowEnd) {
					hold(gain, a_now);
				} else if (gain >= 1.0f) {
					if (unclaimedXp <= 0.0f) {
						unclaimedSince = a_now;
					}
					unclaimedXp += gain;
					unclaimedUntil = a_now + UNCLAIMED_XP_WINDOW;
				}
			}
			lastXp = xp;

			if (heldXp > 0.0f && a_now - heldSince >= XP_HOLD) {
				// The quest and map checks poll (every 0.5 s and 2 s), so look a little before too.
				const auto from = heldSince - 1s;
				if (QuestSync::LastStageChange() >= from || MapShare::LastDiscovery() >= from) {
					++xpWithheld;
				} else if (heldXp >= 1.0f) {
					outgoing.push_back(Protocol::Encode(Protocol::XpGain{ 0, (std::min)(heldXp, Protocol::MAX_XP_SHARE) }, Protocol::MessageType::kReportXp));
				}
				heldXp = 0.0f;
			}

			if (xpNotice >= 1.0f && a_now >= xpNoticeAt) {
				Hud::Notify(std::format("+{:.0f} XP from your party's kills", xpNotice));
				xpNotice = 0.0f;
			}
		}

		void FinishTeleport(Clock::time_point a_now)
		{
			const auto player = Player();
			if (!pendingTeleport || !player) {
				return;
			}
			if (a_now > pendingTeleport->deadline) {
				pendingTeleport.reset();
				return;
			}
			if (SpaceOf(player) == std::pair{ pendingTeleport->cell, pendingTeleport->worldspace } && player->Get3D()) {
				player->SetPosition(pendingTeleport->position, true);
				pendingTeleport.reset();
			}
		}

		void HandleHotkeys()
		{
			for (const auto action : Hotkeys::Poll()) {
				if (localId == 0) {
					Hud::Notify("Multiplayer: not in a session");
					continue;
				}
				// While down, holding the teleport key gives up instead.
				if (action == Hotkeys::Action::kTeleportGo && Downed::IsDown()) {
					Downed::GiveUp();
					continue;
				}
				switch (action) {
				case Hotkeys::Action::kPlayerList:
					ShowPlayerList();
					break;
				case Hotkeys::Action::kTeleportPick:
					PickTeleportTarget();
					break;
				case Hotkeys::Action::kTeleportGo:
					TeleportToTarget();
					break;
				case Hotkeys::Action::kPing:
					SendPing();
					break;
				}
			}
		}

		std::optional<RemotePlayers::Info> FindPlayer(std::uint32_t a_id)
		{
			for (auto& info : RemotePlayers::List()) {
				if (info.id == a_id) {
					return info;
				}
			}
			return std::nullopt;
		}

		std::string KeyName(std::uint32_t a_vk)
		{
			return a_vk >= 0x70 && a_vk <= 0x7B ? std::format("F{}", a_vk - 0x6F) : std::format("key {:X}", a_vk);
		}
	}

	void SetLocalPlayer(std::uint32_t a_id)
	{
		localId = a_id;
	}

	void Frame()
	{
		const auto now = Clock::now();
		HandleHotkeys();
		FinishTeleport(now);
		Downed::Frame(localId != 0 && !RemotePlayers::List().empty());
		if (localId == 0) {
			return;
		}
		SendStatus(now);
		Heartbeat(now);
		TrackXp(now);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		auto packets = std::exchange(outgoing, {});
		for (auto& packet : Downed::TakeOutgoing()) {
			packets.push_back(std::move(packet));
		}
		return packets;
	}

	void ApplyStatus(const Protocol::PlayerStatus& a_status)
	{
		auto&      entry = known[a_status.playerId];
		const auto name = RemotePlayers::NameOf(a_status.playerId);
		if (entry.hasStatus) {
			const auto& before = entry.status;
			const bool  wasDead = before.health == 0 && !before.downed;
			if (a_status.downed && !before.downed) {
				// Holding the teleport key goes to them.
				teleportTarget = a_status.playerId;
				const auto target = FindPlayer(a_status.playerId);
				const auto where = target && target->state ? DistanceTo(target->state->cell, target->state->worldspace, target->state->x, target->state->y) : std::string{};
				Hud::Notify(where.empty() ?
								std::format("{} is down! Hold {} to teleport to them and help them up", name, KeyName(Config::Get().keyTeleport)) :
								std::format("{} is down! Get to them to help them up ({})", name, where));
			} else if (a_status.health == 0 && !a_status.downed && !wasDead) {
				Hud::Notify(name + " died");
			} else if (before.downed && !a_status.downed && a_status.health > 0) {
				Hud::Notify(name + " is back up");
			}
		}
		entry.status = a_status;
		entry.hasStatus = true;
		RemotePlayers::SetHealth(a_status.playerId, a_status.health, a_status.downed);
		Downed::SetFriendDown(a_status.playerId, a_status.downed);
	}

	void ApplyPing(const Protocol::Ping& a_ping)
	{
		const auto name = RemotePlayers::NameOf(a_ping.playerId);
		auto       where = DistanceTo(a_ping.cell, a_ping.worldspace, a_ping.x, a_ping.y);
		if (where.empty()) {
			const auto it = known.find(a_ping.playerId);
			const auto place = it != known.end() ? PlaceName(it->second.status.location, a_ping.cell) : NameOfForm(a_ping.cell);
			where = place.empty() ? "somewhere else" : "at " + place;
		}
		Hud::Notify(std::format("{}: over here! ({})", name.empty() ? "A friend" : name, where));
	}

	void ApplyXp(const Protocol::XpGain& a_gain)
	{
		const auto player = Player();
		const float share = a_gain.xp * Config::Get().xpShare;
		if (!player || player->IsDead(false) || share < 1.0f || !WorldSync::InWorld()) {
			return;
		}
		const auto amount = std::lround(share);
		RE::Console::ExecuteCommand(std::format("player.modav experience {}", amount).c_str());
		xpCredit += static_cast<float>(amount);
		if (xpNotice < 1.0f) {
			xpNoticeAt = Clock::now() + XP_NOTICE_DELAY;
		}
		xpNotice += static_cast<float>(amount);
	}

	void ApplyHeartbeatAck(const Protocol::Heartbeat& a_beat)
	{
		const auto now = Clock::now();
		const auto it = heartbeatsSent.find(a_beat.token);
		if (it == heartbeatsSent.end()) {
			return;
		}
		roundTripMs = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count());
		heartbeatsSent.erase(heartbeatsSent.begin(), std::next(it));
		lastAck = now;
		if (unstable) {
			unstable = false;
			Hud::Notify("Multiplayer: connection restored");
		}
		slowAnswers = roundTripMs > HIGH_PING_MS ? slowAnswers + 1 : 0;
		if (slowAnswers >= 5 && now >= nextPingWarning) {
			nextPingWarning = now + 60s;
			Hud::Notify(std::format("Multiplayer: high ping to the host ({} ms)", roundTripMs));
		}
	}

	void OnActorDied(const Protocol::ActorDeath& a_death)
	{
		if (!a_death.killed) {
			return;
		}
		const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_death.refId);
		const auto base = ref ? ref->GetObjectReference() : nullptr;
		const auto victim = base ? std::string{ RE::TESFullName::GetFullName(*base) } : std::string{};
		const auto killer = RemotePlayers::NameOf(a_death.playerId);
		if (victim.empty() || killer.empty()) {
			return;
		}
		// At most 3 kill messages per 5 s, so a big fight doesn't bury everything else.
		const auto now = Clock::now();
		if (now >= killFeedWindow) {
			killFeedWindow = now + 5s;
			killFeedShown = 0;
		}
		if (++killFeedShown <= 3) {
			Hud::Notify(std::format("{} killed {}", killer, victim));
		}
	}

	void OnPlayerLeft(std::uint32_t a_id)
	{
		known.erase(a_id);
		Downed::OnPlayerLeft(a_id);
		if (teleportTarget == a_id) {
			teleportTarget = 0;
		}
	}

	void ShowPlayerList()
	{
		const auto players = RemotePlayers::List();
		Hud::Notify(players.empty() ? "Multiplayer: nobody else is here yet" :
		                              std::format("Party: {} other player{}{}", players.size(), players.size() == 1 ? "" : "s",
											  roundTripMs ? std::format(" (ping {} ms)", roundTripMs) : ""));
		for (const auto& player : players) {
			Hud::Notify(Describe(player));
		}
	}

	void PickTeleportTarget()
	{
		const auto players = RemotePlayers::List();
		if (players.empty()) {
			Hud::Notify("Multiplayer: nobody to teleport to");
			return;
		}
		// The next player after the current pick (players are sorted by ID).
		auto next = std::ranges::find_if(players, [](const auto& a_player) { return a_player.id > teleportTarget; });
		if (next == players.end()) {
			next = players.begin();
		}
		teleportTarget = next->id;
		Hud::Notify(std::format("Teleport to {}? Hold {} to go", Describe(*next), KeyName(Config::Get().keyTeleport)));
	}

	void TeleportToTarget()
	{
		auto target = teleportTarget ? FindPlayer(teleportTarget) : std::nullopt;
		if (!target) {
			const auto players = RemotePlayers::List();
			if (players.size() != 1) {
				Hud::Notify(players.empty() ? "Multiplayer: nobody to teleport to" :
				                              std::format("Tap {} to choose who to teleport to", KeyName(Config::Get().keyTeleport)));
				return;
			}
			target = players.front();
		}
		const auto player = Player();
		if (!player || player->IsDead(false)) {
			return;
		}
		if (!target->state) {
			Hud::Notify(std::format("Can't find {} right now", target->name));
			return;
		}
		const auto& state = *target->state;
		const RE::NiPoint3 position{ state.x, state.y, state.z + 10.0f };

		// Same place: go straight there. Elsewhere: load their cell or worldspace first.
		if (SpaceOf(player) == std::pair{ state.cell, state.worldspace }) {
			player->SetPosition(position, true);
			Hud::Notify(std::format("Teleported to {}", target->name));
			return;
		}
		if (const auto cell = state.cell ? RE::TESForm::GetFormByID<RE::TESObjectCELL>(state.cell) : nullptr) {
			const std::string editorId = cell->GetFormEditorID();
			if (editorId.empty()) {
				Hud::Notify(std::format("Can't teleport into {}'s location", target->name));
				return;
			}
			RE::Console::ExecuteCommand(std::format("coc {}", editorId).c_str());
		} else if (const auto world = RE::TESForm::GetFormByID<RE::TESWorldSpace>(state.worldspace)) {
			const std::string editorId = world->GetFormEditorID();
			if (editorId.empty()) {
				Hud::Notify(std::format("Can't teleport into {}'s location", target->name));
				return;
			}
			const auto grid = [](float a_coord) { return static_cast<int>(std::floor(a_coord / 4096.0f)); };
			RE::Console::ExecuteCommand(std::format("cow {} {} {}", editorId, grid(state.x), grid(state.y)).c_str());
		} else {
			Hud::Notify(std::format("Can't teleport to {}", target->name));
			return;
		}
		pendingTeleport = PendingTeleport{ state.cell, state.worldspace, position, Clock::now() + TELEPORT_TIMEOUT };
		Hud::Notify(std::format("Teleporting to {}", target->name));
	}

	void SendPing()
	{
		const auto player = Player();
		if (!player) {
			return;
		}
		const auto [cell, worldspace] = SpaceOf(player);
		const auto& pos = player->data.location;
		outgoing.push_back(Protocol::Encode(Protocol::Ping{ 0, cell, worldspace, pos.x, pos.y, pos.z }, Protocol::MessageType::kPing));
		Hud::Notify("You called your party over");
	}

	std::uint32_t RoundTripMs()
	{
		return roundTripMs;
	}

	void OnGameLoaded()
	{
		// The loaded save has its own XP: the difference isn't something we earned.
		lastXp.reset();
		unclaimedXp = 0.0f;
		heldXp = 0.0f;
		killWindowEnd = {};
	}

	void Reset()
	{
		localId = 0;
		known.clear();
		outgoing.clear();
		sentStatus.reset();
		heartbeatsSent.clear();
		lastAck = {};
		roundTripMs = 0;
		unstable = false;
		slowAnswers = 0;
		OnGameLoaded();
		xpCredit = 0.0f;
		teleportTarget = 0;
		pendingTeleport.reset();
		Downed::Reset();
	}

	std::string Describe()
	{
		std::string out = std::format("party: rtt={}ms unstable={} xp={:.0f} xpCredit={:.0f} xpWithheld={}", roundTripMs, unstable, lastXp.value_or(-1.0f), xpCredit, xpWithheld);
		for (const auto& [id, entry] : known) {
			out += std::format(" [{} hp={} lvl={} loc={:08X} cell={:08X}{}]", id, entry.status.health, entry.status.level, entry.status.location, entry.status.cell, entry.status.downed ? " downed" : "");
		}
		return out + " " + Downed::Describe();
	}
}
