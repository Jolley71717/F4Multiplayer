#include "game/Party.h"
#include "game/ArmorFrames.h"

#include "game/WeaponFire.h"

#include "Config.h"
#include "game/Compass.h"
#include "game/DownRules.h"
#include "game/Downed.h"
#include "game/Equipment.h"
#include "game/FadeWatch.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
#include "game/MapShare.h"
#include "game/Menu.h"
#include "game/MenuKinds.h"
#include "game/NpcSync.h"
#include "game/OpenMenus.h"
#include "game/PingRules.h"
#include "game/QuestSync.h"
#include "game/RemotePlayers.h"
#include "game/StandInSeat.h"
#include "game/Story.h"
#include "game/WorldSync.h"
#include "Text.h"

namespace Party
{
	namespace
	{
		void Emote(std::uint8_t a_action);

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

		// The frame our player wears, as friends are told it (see ArmorFrames::OwnWear). Forgotten on a load.
		ArmorFrames::OwnWear ownWear;

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

		// Pings shown (ours and our friends'), ours held to one a second, and for Describe what went out and came in.
		PingRules::Board   pings;
		PingRules::Limiter pingLimit;
		std::uint32_t      pingsSent = 0;
		std::uint32_t      pingsReceived = 0;

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
			static constexpr std::array names{ "compass.n", "compass.ne", "compass.e", "compass.se", "compass.s", "compass.sw", "compass.w", "compass.nw" };
			return Text::Get(names[PingRules::BearingIndex(a_dx, a_dy) % names.size()]);
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
				return Text::Get("player.right_here");
			}
			return Text::Format("player.distance", { { "m", std::format("{:.0f}", meters) }, { "dir", Bearing(dx, dy) } });
		}

		// --- Pings ---------------------------------------------------------------------------------------------

		std::string BaseName(std::uint32_t a_base)
		{
			return Protocol::IsShareableRef(a_base) ? NameOfForm(a_base) : std::string{};
		}

		// Our game's copy of what a ping names: the NPC under the session's id for it (NpcSync, the same NPC in every
		// game, a friend's spawn as our copy of it), or the item's own reference. None for a spot, a friend, or a target
		// our game has no copy of (or not loaded).
		RE::TESObjectREFR* LocalTarget(const Protocol::Ping& a_ping)
		{
			std::uint32_t id = 0;
			if (a_ping.kind == Protocol::PingKind::kActor && a_ping.target) {
				id = NpcSync::LocalRef(a_ping.target);
			} else if (a_ping.kind == Protocol::PingKind::kItem && Protocol::IsShareableRef(a_ping.target)) {
				id = a_ping.target;
			}
			const auto ref = id ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(id) : nullptr;
			return ref && ref->GetParentCell() && !ref->IsDeleted() && !ref->IsDisabled() ? ref : nullptr;
		}

		// What a ping names, as our player reads it: our own game's name for the NPC or item ("Raider", "Stimpak"),
		// else the one the pinger's game sent; the friend's name ("you" for us); "over here" for a spot.
		std::string WhatOf(const Protocol::Ping& a_ping)
		{
			switch (a_ping.kind) {
			case Protocol::PingKind::kSpot:
				return Text::Get("party.ping_here");
			case Protocol::PingKind::kPlayer:
				{
					if (a_ping.target == localId) {
						return Text::Get("party.ping_you");
					}
					const auto name = RemotePlayers::NameOf(a_ping.target);
					return name.empty() ? Text::Get("party.a_friend") : name;
				}
			case Protocol::PingKind::kActor:
			case Protocol::PingKind::kItem:
				{
					std::string name;
					if (const auto ref = LocalTarget(a_ping); ref && ref->GetObjectReference()) {
						name = RE::TESFullName::GetFullName(*ref->GetObjectReference());
					}
					if (name.empty()) {
						name = BaseName(a_ping.base);
					}
					if (name.empty()) {
						name = a_ping.name;
					}
					return name.empty() ? Text::Get("party.ping_something") : name;
				}
			}
			return Text::Get("party.ping_something");
		}

		// How far and which way a ping is from our player, or where it is when that's elsewhere.
		std::string WhereOf(const Protocol::Ping& a_ping)
		{
			auto where = DistanceTo(a_ping.cell, a_ping.worldspace, a_ping.x, a_ping.y);
			if (where.empty()) {
				const auto it = known.find(a_ping.playerId);
				const auto place = it != known.end() && it->second.hasStatus ? PlaceName(it->second.status.location, a_ping.cell) : NameOfForm(a_ping.cell);
				where = place.empty() ? Text::Get("party.somewhere_else") : Text::Format("party.at_place", { { "place", place } });
			}
			return where;
		}

		// Moves the pings that follow something to where it is now: an NPC or an item our game has (it walks, or is
		// thrown about), a friend (their stand-in's last place), ourselves. The rest stay where they were pinged.
		void FollowPings()
		{
			for (auto& [pinger, mark] : pings.Marks()) {
				auto& ping = mark.ping;
				if (ping.kind == Protocol::PingKind::kPlayer) {
					if (ping.target == localId) {
						if (const auto player = Player()) {
							std::tie(ping.cell, ping.worldspace) = SpaceOf(player);
							ping.x = player->data.location.x;
							ping.y = player->data.location.y;
							ping.z = player->data.location.z;
						}
					} else if (const auto state = RemotePlayers::LastStateOf(ping.target)) {
						ping.cell = state->cell;
						ping.worldspace = state->worldspace;
						ping.x = state->x;
						ping.y = state->y;
						ping.z = state->z;
					}
					continue;
				}
				const auto ref = LocalTarget(ping);
				const auto cell = ref ? ref->GetParentCell() : nullptr;
				if (!cell) {
					continue;
				}
				ping.cell = cell->IsInterior() ? cell->GetFormID() : 0;
				ping.worldspace = cell->IsInterior() ? 0 : (cell->worldSpace ? cell->worldSpace->GetFormID() : 0);
				ping.x = ref->data.location.x;
				ping.y = ref->data.location.y;
				ping.z = ref->data.location.z;
			}
		}

		// Whether a reference is something lying in the world that can be picked up.
		bool IsItem(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref->GetObjectReference();
			if (!base) {
				return false;
			}
			using enum RE::ENUM_FORM_ID;
			switch (base->GetFormType()) {
			case kWEAP:
			case kARMO:
			case kAMMO:
			case kALCH:
			case kMISC:
			case kBOOK:
			case kKEYM:
			case kNOTE:
			case kINGR:
				return true;
			default:
				return false;
			}
		}

		// How far along a ray the world's surface is, by the engine's own pick through the cell's physics world (the
		// call DevCommands' spawnenemies grounds NPCs with). A hit on our own player's body (the camera sits in it in
		// first person and behind it in third) is passed through. Nothing: the sky, or nothing within reach.
		std::optional<float> SurfaceAlong(RE::TESObjectCELL* a_cell, const PingRules::Ray& a_ray, const RE::NiPoint3& a_feet)
		{
			float start = PingRules::MIN_ALONG;
			for (int tries = 0; tries < 3 && start < PingRules::REACH; ++tries) {
				const auto from = PingRules::Along(a_ray, start);
				const auto to = PingRules::Along(a_ray, PingRules::REACH);
				RE::bhkPickData pick;
				pick.SetStartEnd({ from.x, from.y, from.z }, { to.x, to.y, to.z });
				static_cast<void>(a_cell->Pick(pick));  // what it hit is not needed, only where
				if (!pick.HasHit()) {
					return std::nullopt;
				}
				const float fraction = pick.GetHitFraction();
				if (!std::isfinite(fraction)) {
					return std::nullopt;
				}
				const float along = start + std::clamp(fraction, 0.0f, 1.0f) * (PingRules::REACH - start);
				const auto  hit = PingRules::Along(a_ray, along);
				const float dx = hit.x - a_feet.x;
				const float dy = hit.y - a_feet.y;
				if (dx * dx + dy * dy > 60.0f * 60.0f || hit.z < a_feet.z - 10.0f || hit.z > a_feet.z + 150.0f) {
					return along;
				}
				start = along + 20.0f;  // our own body: look past it
			}
			return std::nullopt;
		}

		// The ping our player means: what the crosshair is on (an NPC, a friend, an item lying there), else the surface
		// under it, else where they stand. The ray goes from the camera along the player's look; the targets near it
		// are the loaded references within reach (PingRules::ChooseTarget picks one).
		Protocol::Ping AimedPing(RE::PlayerCharacter* a_player)
		{
			const auto [cell, worldspace] = SpaceOf(a_player);
			const auto& feet = a_player->data.location;
			Protocol::Ping ping{ 0, cell, worldspace, feet.x, feet.y, feet.z };

			PingRules::Vec eye{ feet.x, feet.y, feet.z + 120.0f };
			if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
				const auto& at = camera->cameraRoot->world.translate;
				if (std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z) && (at.x != 0.0f || at.y != 0.0f || at.z != 0.0f)) {
					eye = { at.x, at.y, at.z };
				}
			}
			const auto ray = PingRules::LookRay(eye, a_player->data.angle.z, a_player->data.angle.x);
			const auto surface = SurfaceAlong(a_player->GetParentCell(), ray, feet);

			std::vector<PingRules::Candidate>  candidates;
			std::vector<RE::TESObjectREFR*>    refs;
			if (const auto tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(RE::NiPoint3{ eye.x, eye.y, eye.z }, PingRules::REACH, [&](RE::TESObjectREFR* a_ref) {
					if (!a_ref || a_ref == a_player || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Get3D() || candidates.size() >= 2048) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					const auto& at = a_ref->data.location;
					if (const auto actor = a_ref->As<RE::Actor>()) {
						const auto friendId = RemotePlayers::PlayerIdFor(actor->GetFormID());
						candidates.push_back({ friendId ? Protocol::PingKind::kPlayer : Protocol::PingKind::kActor, friendId, { at.x, at.y, at.z + PingRules::ACTOR_MIDDLE }, PingRules::ACTOR_RADIUS });
						refs.push_back(a_ref);
					} else if (IsItem(a_ref)) {
						candidates.push_back({ Protocol::PingKind::kItem, 0, { at.x, at.y, at.z + 5.0f }, PingRules::ITEM_RADIUS });
						refs.push_back(a_ref);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}

			const auto chosen = PingRules::ChooseTarget(ray, surface, candidates);
			if (!chosen) {
				const auto spot = PingRules::SpotFor(ray, surface, { feet.x, feet.y, feet.z });
				ping.x = spot.x;
				ping.y = spot.y;
				ping.z = spot.z;
				return ping;
			}
			const auto& candidate = candidates[*chosen];
			const auto  ref = refs[*chosen];
			const auto  base = ref->GetObjectReference();
			ping.kind = candidate.kind;
			ping.x = ref->data.location.x;
			ping.y = ref->data.location.y;
			ping.z = ref->data.location.z;
			if (candidate.kind == Protocol::PingKind::kPlayer) {
				ping.target = candidate.id;
				return ping;
			}
			ping.base = base && Protocol::IsShareableRef(base->GetFormID()) ? base->GetFormID() : 0;
			ping.name = base ? std::string{ RE::TESFullName::GetFullName(*base) } : std::string{};
			if (ping.name.size() > Protocol::MAX_PING_NAME_LENGTH) {
				auto cut = Protocol::MAX_PING_NAME_LENGTH;
				while (cut > 0 && (static_cast<unsigned char>(ping.name[cut]) & 0xC0) == 0x80) {
					--cut;  // not in the middle of a character
				}
				ping.name.resize(cut);
			}
			ping.target = candidate.kind == Protocol::PingKind::kActor ? NpcSync::SessionId(ref->GetFormID()) :
			              Protocol::IsShareableRef(ref->GetFormID())   ? ref->GetFormID() :
			                                                             0;
			return ping;
		}

		const char* KindName(Protocol::PingKind a_kind)
		{
			switch (a_kind) {
			case Protocol::PingKind::kSpot:
				return "spot";
			case Protocol::PingKind::kActor:
				return "actor";
			case Protocol::PingKind::kItem:
				return "item";
			case Protocol::PingKind::kPlayer:
				return "player";
			}
			return "?";
		}

		std::string Describe(const RemotePlayers::Info& a_player)
		{
			std::string out = std::format("{} [{}]", a_player.name, Compass::MarkerName(a_player.id));
			const auto  it = known.find(a_player.id);
			if (it != known.end() && it->second.hasStatus) {
				const auto& status = it->second.status;
				out += " (" + (status.downed ? Text::Get("player.down") : status.health == 0 ? Text::Get("player.dead") :
				                                                                                  Text::Format("player.status", { { "hp", status.health }, { "level", status.level } })) + ")";
				if (auto place = PlaceName(status.location, status.cell); !place.empty()) {
					out += " - " + place;
				}
			}
			if (a_player.state) {
				if (auto distance = DistanceTo(a_player.state->cell, a_player.state->worldspace, a_player.state->x, a_player.state->y); !distance.empty()) {
					out += ", " + distance;
				}
			} else {
				out += ", " + Text::Get("player.loading");
			}
			return out;
		}

		// How far and which way a friend is ("30 m NE"), or where they are when that is elsewhere, or that they are loading.
		std::string WhereIs(const RemotePlayers::Info& a_player)
		{
			if (!a_player.state) {
				return Text::Get("player.loading");
			}
			auto where = DistanceTo(a_player.state->cell, a_player.state->worldspace, a_player.state->x, a_player.state->y);
			if (where.empty()) {
				const auto it = known.find(a_player.id);
				const auto place = it != known.end() && it->second.hasStatus ? PlaceName(it->second.status.location, it->second.status.cell) : std::string{};
				where = place.empty() ? Text::Get("party.somewhere_else") : Text::Format("party.at_place", { { "place", place } });
			}
			return where;
		}

		// The frame our player got out of last, as the engine keeps it (PlayerCharacter::lastUsedPowerArmor).
		ArmorFrames::LastFrame LastFrameOf(RE::PlayerCharacter* a_player)
		{
			ArmorFrames::LastFrame last;
			const auto frame = a_player->lastUsedPowerArmor.get();
			if (!frame || frame->IsDeleted()) {
				return last;
			}
			const auto base = frame->GetObjectReference();
			const auto cell = frame->GetParentCell();
			last.found = true;
			last.disabled = frame->IsDisabled();
			last.frame = Equipment::IsPowerArmorFrame(base);
			last.ref = frame->GetFormID();
			last.base = base ? base->GetFormID() : 0;
			if (cell) {
				last.cell = cell->IsInterior() ? cell->GetFormID() : 0;
				last.worldspace = !cell->IsInterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
			}
			last.x = frame->data.location.x;
			last.y = frame->data.location.y;
			last.z = frame->data.location.z;
			last.heading = frame->data.angle.z;
			return last;
		}

		// The furniture our player uses, as StandInSeat::Tell takes it.
		StandInSeat::Own OwnSeatOf(RE::PlayerCharacter* a_player, bool a_powerArmor)
		{
			StandInSeat::Own own{ .sitState = static_cast<std::int32_t>(a_player->DoGetSitSleepState()), .powerArmor = a_powerArmor };
			const auto       middle = a_player->currentProcess ? a_player->currentProcess->middleHigh : nullptr;
			const auto       seat = middle ? middle->occupiedFurniture.get() : nullptr;
			if (!seat || seat->IsDeleted()) {
				return own;
			}
			const auto base = seat->GetObjectReference();
			const auto type = base ? base->GetFormType() : RE::ENUM_FORM_ID::kNONE;
			// A terminal (BGSTerminal) is furniture too.
			const auto furniture = type == RE::ENUM_FORM_ID::kFURN || type == RE::ENUM_FORM_ID::kTERM ? static_cast<const RE::TESFurniture*>(base) : nullptr;
			own.found = true;
			own.actor = seat->As<RE::Actor>() != nullptr;
			own.furniture = furniture != nullptr;
			own.frame = Equipment::IsPowerArmorFrame(base);
			own.station = furniture && furniture->wbData.type == RE::WorkbenchData::Type::kPowerArmor;
			own.workplace = type == RE::ENUM_FORM_ID::kTERM || (furniture && furniture->wbData.type != RE::WorkbenchData::Type::kNone);
			own.ref = seat->GetFormID();
			own.base = base ? base->GetFormID() : 0;
			own.x = seat->data.location.x;
			own.y = seat->data.location.y;
			own.z = seat->data.location.z;
			return own;
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
			status.health = DownRules::StatusHealth(player->IsDead(false), Downed::IsDown(), current, maximum);
			status.level = static_cast<std::uint16_t>((std::max)(player->GetLevel(), std::int16_t{ 0 }));
			status.downed = Downed::WaitsForHelp();
			status.opening = Story::InOpening();
			status.light = player->IsPipboyLightOn();
			// The kind of menu we are in (the Pip-Boy, paused, a container...), never what it shows: friends' stand-ins of us
			// raise their Pip-Boy, and show an icon over the head (MenuIcons).
			status.menu = MenuKinds::Of([](OpenMenus::Menu a_menu) { return OpenMenus::Open(a_menu); });
			// The engine's PlayerInPowerArmor said false on a player visibly in a frame (1.11.240); the
			// ExtraPowerArmor on the actor and the furniture it occupies are what the game itself keeps.
			status.powerArmor = Equipment::InPowerArmor(player);
			// The frame is the furniture occupied when the armor went on, remembered for as long as it stays on (see
			// ArmorFrames::OwnWear).
			const auto wear = ownWear.Update(
				status.powerArmor,
				[&] {
					const auto middle = player->currentProcess ? player->currentProcess->middleHigh : nullptr;
					const auto seat = middle ? middle->occupiedFurniture.get() : nullptr;
					const auto base = seat ? seat->GetObjectReference() : nullptr;
					return ArmorFrames::Seat{ .ref = seat ? seat->GetFormID() : 0, .base = base ? base->GetFormID() : 0, .frame = Equipment::IsPowerArmorFrame(base) };
				},
				Clock::now());
			status.powerArmorFrame = wear.frame;
			status.powerArmorFrameRef = wear.frameRef;
			// Out of it: the frame we got out of last, where it stands (see ArmorFrames::TellParked). Friends show a copy
			// there, in the station we parked it in or wherever we left it.
			status.parked = ArmorFrames::TellParked(status.powerArmor, LastFrameOf(player));
			// The chair, bed, workbench or terminal we use: friends put our stand-in in the same one (not a frame: above).
			status.seat = StandInSeat::Tell(OwnSeatOf(player, status.powerArmor));
			if (wear.unknown) {
				REX::WARN("Party: in power armor for a second and what we sit in ({:08X}) is no frame; friends are told we are in power armor with no frame, and see our pieces with no frame copy", wear.seat);
			}
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
				REX::INFO("Party: connection to the host is unstable");
				Hud::Notify(Text::Get("party.unstable"));
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
				Hud::Notify(Text::Format("party.xp", { { "xp", std::format("{:.0f}", xpNotice) } }));
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

		// Why the player can't teleport right now (the key of the text saying so; "" = they can). Teleporting out of a
		// conversation, a scene or furniture (a workbench, a vertibird seat) can leave a quest
		// stuck, and Survival has no fast travel.
		std::string TeleportBlocked(RE::PlayerCharacter* a_player)
		{
			if (Story::InOpening()) {
				return "party.cant_teleport_opening";
			}
			if (OpenMenus::Open(OpenMenus::Menu::kDialogue)) {
				return "party.cant_teleport_talking";
			}
			if (a_player->GetCurrentScene()) {
				return "party.cant_teleport_scene";
			}
			if (static_cast<int>(a_player->DoGetSitSleepState()) != 0) {
				return "party.cant_teleport_busy";
			}
			if (a_player->GetDifficultyLevel() >= RE::DifficultyLevel::kSurvival) {
				return "party.cant_teleport_survival";
			}
			return {};
		}

		void HandleHotkeys()
		{
			for (const auto action : Hotkeys::Poll()) {
				// The menu is how you get into a session, so it doesn't need one.
				if (action == Hotkeys::Action::kMenu) {
					Menu::Open();
					continue;
				}
				if (localId == 0) {
					Hud::Notify(Text::Get("party.not_in_session"));
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
				case Hotkeys::Action::kWhere:
					ShowWhere();
					break;
				case Hotkeys::Action::kEmotePoint:
				case Hotkeys::Action::kEmoteCheer:
				case Hotkeys::Action::kEmoteClap:
					Emote(action == Hotkeys::Action::kEmotePoint ? Protocol::ShotAction::kPoint : action == Hotkeys::Action::kEmoteCheer ? Protocol::ShotAction::kCheer : Protocol::ShotAction::kClap);
					break;
				case Hotkeys::Action::kMenu:
					break;  // handled above
				}
			}
		}

		// An emote: our stand-in plays it for the others, and so do we (third person shows it).
		// (A frame has its own skeleton and animation graph: the human arm animation of a point, cheer or clap there slid the
		// player's view sideways. Refused with a line saying why.)
		void Emote(std::uint8_t a_action)
		{
			if (const auto player = Player(); player && Equipment::InPowerArmor(player)) {
				Hud::Notify(Text::Get("party.no_emote_armor"));
				return;
			}
			WeaponFire::QueueAction(a_action);
			if (const auto idle = WeaponFire::EmoteIdleName(a_action)) {
				RE::Console::ExecuteCommand(std::format("player.playidle {}", idle).c_str());
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
		pings.Expire(now);
		FollowPings();
		// Not in the opening: no friend could get there to help.
		Downed::Frame(localId != 0 && !RemotePlayers::List().empty() && !Story::InOpening());
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
			const auto  news = DownRules::ReadFriendStatus(before.downed, before.health, a_status.downed, a_status.health);
			if (news == DownRules::FriendNews::kDown) {
				// Holding the teleport key goes to them.
				teleportTarget = a_status.playerId;
				const auto target = FindPlayer(a_status.playerId);
				const auto where = target && target->state ? DistanceTo(target->state->cell, target->state->worldspace, target->state->x, target->state->y) : std::string{};
				REX::INFO("Party: player {} is down", a_status.playerId);
				Hud::Notify(where.empty() ? Text::Format("party.friend_down_far", { { "player", name }, { "key", KeyName(Config::Get().keyTeleport) } }) :
				                            Text::Format("party.friend_down_near", { { "player", name }, { "where", where } }));
			} else if (news == DownRules::FriendNews::kDied) {
				REX::INFO("Party: player {} died", a_status.playerId);
				Hud::Notify(Text::Format("party.friend_died", { { "player", name } }));
			} else if (news == DownRules::FriendNews::kBackUp) {
				REX::INFO("Party: player {} is back up", a_status.playerId);
				Hud::Notify(Text::Format("party.friend_back_up", { { "player", name } }));
			}
		}
		entry.status = a_status;
		entry.hasStatus = true;
		RemotePlayers::SetLight(a_status.playerId, a_status.light);
		RemotePlayers::SetMenu(a_status.playerId, a_status.menu);
		RemotePlayers::SetPowerArmor(a_status.playerId, a_status.powerArmor, a_status.powerArmorFrame, a_status.powerArmorFrameRef, a_status.parked);
		RemotePlayers::SetSeat(a_status.playerId, a_status.seat);
		RemotePlayers::SetHealth(a_status.playerId, a_status.health, a_status.downed);
		Downed::SetFriendDown(a_status.playerId, a_status.downed);
	}

	// A friend's ping: on our compass in their color for PingRules::LIFETIME (in place of their last one), and a HUD
	// line naming what it is and how far.
	void ApplyPing(const Protocol::Ping& a_ping)
	{
		++pingsReceived;
		pings.Add(a_ping, Clock::now());
		FollowPings();  // our own copy of the NPC or item, where it is here
		const auto& shown = pings.Marks().at(a_ping.playerId).ping;
		REX::INFO("Party: player {} pinged a {} (target {:08X}, ours {:08X})", a_ping.playerId, KindName(a_ping.kind), a_ping.target,
			LocalTarget(a_ping) ? LocalTarget(a_ping)->GetFormID() : 0);
		const auto name = RemotePlayers::NameOf(a_ping.playerId);
		Hud::Notify(Text::Format("party.ping", { { "player", name.empty() ? Text::Get("party.a_friend") : name }, { "what", WhatOf(shown) }, { "where", WhereOf(shown) } }));
	}

	void ApplyXp(const Protocol::XpGain& a_gain)
	{
		const auto player = Player();
		const float share = a_gain.xp * Config::Get().xpShare;
		if (!player || player->IsDead(false) || share < 1.0f || !WorldSync::InWorld() || Story::InOpening()) {
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
			REX::INFO("Party: connection to the host restored");
			Hud::Notify(Text::Get("party.restored"));
		}
		slowAnswers = roundTripMs > HIGH_PING_MS ? slowAnswers + 1 : 0;
		if (slowAnswers >= 5 && now >= nextPingWarning) {
			nextPingWarning = now + 60s;
			REX::INFO("Party: high ping to the host ({} ms)", roundTripMs);
			Hud::Notify(Text::Format("party.high_ping", { { "ms", roundTripMs } }));
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
			REX::INFO("Party: player {} killed {:08X}", a_death.playerId, a_death.refId);
			Hud::Notify(Text::Format("party.killed", { { "killer", killer }, { "victim", victim } }));
		}
	}

	bool FriendInOpening()
	{
		return std::ranges::any_of(known, [](const auto& a_entry) { return a_entry.first != localId && a_entry.second.hasStatus && a_entry.second.status.opening; });
	}

	void OnPlayerLeft(std::uint32_t a_id)
	{
		known.erase(a_id);
		Downed::OnPlayerLeft(a_id);
		pings.Forget(a_id);
		if (teleportTarget == a_id) {
			teleportTarget = 0;
		}
	}

	std::vector<std::string> PlayerLines()
	{
		std::vector<std::string> lines;
		for (const auto& player : RemotePlayers::List()) {
			lines.push_back(Describe(player));
		}
		return lines;
	}

	void ShowPlayerList()
	{
		const auto players = RemotePlayers::List();
		const auto ping = roundTripMs ? " (" + Text::Format("party.ping_info", { { "ms", roundTripMs } }) + ")" : std::string{};
		Hud::Notify(players.empty()        ? Text::Get("party.alone") :
		            players.size() == 1    ? Text::Format("party.count_one", { { "ping", ping } }) :
		                                     Text::Format("party.count_many", { { "n", players.size() }, { "ping", ping } }));
		for (const auto& player : players) {
			Hud::Notify(Describe(player));
		}
	}

	void ShowWhere()
	{
		const auto players = RemotePlayers::List();
		if (players.empty()) {
			Hud::Notify(Text::Get("party.alone"));
			return;
		}
		for (const auto& player : players) {
			Hud::Notify(Text::Format("party.where", { { "player", player.name }, { "where", WhereIs(player) } }));
		}
	}

	void PickTeleportTarget()
	{
		const auto players = RemotePlayers::List();
		if (players.empty()) {
			Hud::Notify(Text::Get("party.no_teleport_target"));
			return;
		}
		// The next player after the current pick (players are sorted by ID).
		auto next = std::ranges::find_if(players, [](const auto& a_player) { return a_player.id > teleportTarget; });
		if (next == players.end()) {
			next = players.begin();
		}
		teleportTarget = next->id;
		Hud::Notify(Text::Format("party.teleport_prompt", { { "player", Describe(*next) }, { "key", KeyName(Config::Get().keyTeleport) } }));
	}

	void TeleportToTarget()
	{
		auto target = teleportTarget ? FindPlayer(teleportTarget) : std::nullopt;
		if (!target) {
			const auto players = RemotePlayers::List();
			if (players.size() != 1) {
				Hud::Notify(players.empty() ? Text::Get("party.no_teleport_target") :
				                              Text::Format("party.teleport_choose", { { "key", KeyName(Config::Get().keyTeleport) } }));
				return;
			}
			target = players.front();
		}
		const auto player = Player();
		if (!player || player->IsDead(false)) {
			return;
		}
		if (const auto reason = TeleportBlocked(player); !reason.empty()) {
			REX::INFO("Party: can't teleport to player {}: {}", target->id, Text::GetEnglish(reason));
			Hud::Notify(Text::Get(reason));
			return;
		}
		if (!target->state) {
			REX::INFO("Party: can't teleport to player {}: no position from them yet", target->id);
			Hud::Notify(Text::Format("party.cant_find", { { "player", target->name } }));
			return;
		}
		// Their opening is a place (and a time) that doesn't exist in our game.
		if (const auto it = known.find(target->id); it != known.end() && it->second.hasStatus && it->second.status.opening) {
			REX::INFO("Party: can't teleport to player {}: they are still in the opening", target->id);
			Hud::Notify(Text::Format("party.still_in_vault", { { "player", target->name } }));
			return;
		}
		const auto& state = *target->state;
		const RE::NiPoint3 position{ state.x, state.y, state.z + 10.0f };

		// Same place: go straight there. Elsewhere: load their cell or worldspace first. The stand-ins go first, in this
		// frame (RemotePlayers::DespawnAll, as before a save): the engine clears the cells for the coc or cow later in this
		// same frame, after our work, and a stand-in it unloads with them can lose its cell while its record stays, which
		// crashes the engine unloading it or the next save. They are placed again where we arrive.
		if (SpaceOf(player) == std::pair{ state.cell, state.worldspace }) {
			FadeWatch::Arm("moved the player to a friend");
			player->SetPosition(position, true);
			REX::INFO("Party: teleported to player {}", target->id);
			Hud::Notify(Text::Format("party.teleported", { { "player", target->name } }));
			return;
		}
		if (const auto cell = state.cell ? RE::TESForm::GetFormByID<RE::TESObjectCELL>(state.cell) : nullptr) {
			const std::string editorId = cell->GetFormEditorID();
			if (editorId.empty()) {
				REX::INFO("Party: can't teleport to player {}: cell {:08X} has no editor ID", target->id, state.cell);
				Hud::Notify(Text::Format("party.cant_teleport_into", { { "player", target->name } }));
				return;
			}
			FadeWatch::Arm("teleported the player to a friend");
			RemotePlayers::DespawnAll(true);  // the cells go later in this frame: no time to wait
			RE::Console::ExecuteCommand(std::format("coc {}", editorId).c_str());
		} else if (const auto world = RE::TESForm::GetFormByID<RE::TESWorldSpace>(state.worldspace)) {
			const std::string editorId = world->GetFormEditorID();
			if (editorId.empty()) {
				REX::INFO("Party: can't teleport to player {}: worldspace {:08X} has no editor ID", target->id, state.worldspace);
				Hud::Notify(Text::Format("party.cant_teleport_into", { { "player", target->name } }));
				return;
			}
			const auto grid = [](float a_coord) { return static_cast<int>(std::floor(a_coord / 4096.0f)); };
			FadeWatch::Arm("teleported the player to a friend");
			RemotePlayers::DespawnAll(true);  // the cells go later in this frame: no time to wait
			RE::Console::ExecuteCommand(std::format("cow {} {} {}", editorId, grid(state.x), grid(state.y)).c_str());
		} else {
			REX::INFO("Party: can't teleport to player {}: no cell {:08X} or worldspace {:08X} here", target->id, state.cell, state.worldspace);
			Hud::Notify(Text::Format("party.cant_teleport_to", { { "player", target->name } }));
			return;
		}
		pendingTeleport = PendingTeleport{ state.cell, state.worldspace, position, Clock::now() + TELEPORT_TIMEOUT };
		REX::INFO("Party: teleporting to player {}", target->id);
		Hud::Notify(Text::Format("party.teleporting", { { "player", target->name } }));
	}

	void SendPing()
	{
		const auto player = Player();
		if (!player) {
			return;
		}
		if (localId == 0) {
			Hud::Notify(Text::Get("party.not_in_session"));
			return;
		}
		const auto [cell, worldspace] = SpaceOf(player);
		if ((cell == 0) == (worldspace == 0)) {
			return;  // nowhere the others could place it
		}
		const auto now = Clock::now();
		if (!pingLimit.Try(now)) {
			return;  // a second press within PingRules::MIN_GAP: the last ping stands
		}
		auto ping = AimedPing(player);
		outgoing.push_back(Protocol::Encode(ping, Protocol::MessageType::kPing));
		++pingsSent;
		ping.playerId = localId;
		pings.Add(ping, now);
		REX::INFO("Party: pinged a {} (target {:08X}, base {:08X})", KindName(ping.kind), ping.target, ping.base);
		Hud::Notify(Text::Format("party.called", { { "what", WhatOf(ping) }, { "where", WhereOf(ping) } }));
	}

	std::vector<PingMarker> PingMarkers()
	{
		std::vector<PingMarker> markers;
		for (const auto& [pinger, mark] : pings.Marks()) {
			markers.push_back({ pinger, mark.ping.cell, mark.ping.worldspace, mark.ping.x, mark.ping.y });
		}
		return markers;
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
		ownWear.Forget();
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
		pings.Clear();
		pingLimit.Reset();
	}

	std::string Describe()
	{
		std::string out = std::format("party: rtt={}ms unstable={} xp={:.0f} xpCredit={:.0f} xpWithheld={}", roundTripMs, unstable, lastXp.value_or(-1.0f), xpCredit, xpWithheld);
		for (const auto& [id, entry] : known) {
			out += std::format(" [{} hp={} lvl={} loc={:08X} cell={:08X}{}]", id, entry.status.health, entry.status.level, entry.status.location, entry.status.cell, entry.status.downed ? " downed" : "");
		}
		out += std::format(" pings: sent={} received={} tooSoon={}", pingsSent, pingsReceived, pingLimit.Refused());
		for (const auto& [pinger, mark] : pings.Marks()) {
			const auto ref = LocalTarget(mark.ping);
			out += std::format(" [ping {} {} target={:08X} ours={:08X} base={:08X} at={:.0f},{:.0f},{:.0f} left={}s]", pinger, KindName(mark.ping.kind), mark.ping.target, ref ? ref->GetFormID() : 0,
				mark.ping.base, mark.ping.x, mark.ping.y, mark.ping.z, std::chrono::duration_cast<std::chrono::seconds>(mark.until - Clock::now()).count());
		}
		return out + " " + Downed::Describe();
	}
}
