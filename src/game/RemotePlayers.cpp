#include "game/RemotePlayers.h"
#include "game/DeferredDelete.h"
#include "game/Prediction.h"

#include "game/Face.h"
#include "Config.h"
#include "game/Equipment.h"
#include "game/Puppets.h"
#include "game/WeaponFire.h"

namespace RemotePlayers
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Render remote players slightly in the past so there are always two states to blend.
		// States arrive about every 33 ms; two of them, plus slack for the relay and network jitter.
		constexpr auto INTERPOLATION_DELAY = 70ms;
		constexpr auto STALE_AFTER = 5s;
		constexpr std::size_t MAX_SNAPSHOTS = 32;

		// Exterior puppets further than this are outside the loaded area and are despawned.
		constexpr float MAX_EXTERIOR_DISTANCE = 9000.0f;

		// The game sometimes puts the NPC's own outfit back on a stand-in (when its inventory is
		// set up late, or its 3D reloads). What it wears is checked this often and put right.
		constexpr auto EQUIPMENT_CHECK_INTERVAL = 3s;
		constexpr int  MAX_EQUIPMENT_FIXES = 3;  // in a row without it sticking, then give up

		struct Snapshot
		{
			Clock::time_point     received;
			Protocol::PlayerState state;
		};

		struct RemotePlayer
		{
			std::string          name;
			std::uint32_t        appearance = 0;
			std::vector<std::uint32_t> equipment;
			std::optional<Protocol::Face> face;
			RE::TESNPC*          faceBase = nullptr;  // our copy of the base NPC wearing their face
			bool                 faceTried = false;
			bool                 hasEquipment = false;
			bool                 equipmentApplied = false;
			Clock::time_point    equipmentCheckAt{};
			int                  equipmentFixes = 0;
			std::deque<Snapshot> snapshots;
			RE::ObjectRefHandle  actor;
			// The pointer registered with Puppets. Kept separately so it can be unregistered even
			// if the game deletes the actor behind our back (its handle then no longer resolves).
			// Never dereferenced; registeredId is its form ID.
			RE::Actor*           registered = nullptr;
			std::uint32_t        registeredId = 0;
			std::uint8_t         health = 100;
			bool                 downed = false;
			std::string          shownName;  // the name tag currently on the stand-in
			Clock::time_point    knownAt{};    // when we first heard of them
			bool                 lightOn = false;  // their Pip-Boy light
			RE::ObjectRefHandle  light;            // the light that stands in for it, following the stand-in
			Clock::time_point    lightPlacedAt{};
			Clock::time_point    spawnedAt{};  // when the current stand-in was placed
			bool                 powerArmorOn = false;   // they are in power armor
			std::uint32_t        powerArmorFrame = 0;    // the base form of their frame
			bool                 inPowerArmor = false;   // the stand-in has climbed into our frame
			RE::ObjectRefHandle  frame;                  // the frame we placed for it
			Clock::time_point    framePlacedAt{};
			Clock::time_point    frameActionAt{};        // when the stand-in last climbed in or out (the animation takes a moment)
		};

		std::map<std::uint32_t, RemotePlayer> players;

		// Stand-ins to delete once their 3D is in (see DeferredDelete).
		struct Grave
		{
			RE::ObjectRefHandle actor;
			Clock::time_point   spawnedAt;
		};
		std::vector<Grave> graveyard;

		// The first stand-in waits this long for the player's face, instead of being placed and then
		// replaced a moment later when the face arrives.
		constexpr auto FACE_GRACE = 1s;

		// The light form placed at a friend's stand-in while their Pip-Boy light is on (dev: friendlight).
		std::uint32_t friendLightForm = 0x0001ED2D;  // a plain white light, radius 800, in Fallout4.esm
		constexpr float LIGHT_HEIGHT = 110.0f;  // above the feet: about the Pip-Boy
		std::uint32_t   lightsPlaced = 0;
		std::uint32_t   framesPlaced = 0;
		std::uint32_t                         equipmentFixesTotal = 0;
		// Frames drawn while a friend was moving, and how many of those ran out of states to blend
		// (the next one was late) and held them still: the interpolation delay is too short if
		// this is more than a few percent.
		std::uint32_t                         movingFrames = 0;
		std::uint32_t                         heldFrames = 0;

		// The items we can put on a stand-in here (forms another game has but ours doesn't are skipped).
		std::vector<std::uint32_t> Wearable(const std::vector<std::uint32_t>& a_items)
		{
			std::vector<std::uint32_t> out;
			for (const auto item : a_items) {
				const auto form = RE::TESForm::GetFormByID(item);
				if (form && form->Is(RE::ENUM_FORM_ID::kARMO, RE::ENUM_FORM_ID::kWEAP)) {
					out.push_back(item);
				}
			}
			return out;
		}

		constexpr std::uint32_t PLAYER_FACTION = 0x0001C21C;

		float LerpAngle(float a_from, float a_to, float a_t)
		{
			constexpr float twoPi = std::numbers::pi_v<float> * 2.0f;
			float delta = std::fmod(a_to - a_from, twoPi);
			if (delta > std::numbers::pi_v<float>) {
				delta -= twoPi;
			} else if (delta < -std::numbers::pi_v<float>) {
				delta += twoPi;
			}
			return a_from + delta * a_t;
		}

		struct Sampled
		{
			Protocol::PlayerState state;
			float                 motionAngle = 0.0f;  // world heading of travel, radians
		};

		// Movement direction relative to facing, as a fraction of a turn in [0, 1).
		float RelativeDirection(float a_motionAngle, float a_heading)
		{
			constexpr float twoPi = std::numbers::pi_v<float> * 2.0f;
			float rel = std::fmod(a_motionAngle - a_heading, twoPi);
			if (rel < 0.0f) {
				rel += twoPi;
			}
			return rel / twoPi;
		}

		// Interpolated state at (now - delay), or nullopt if we have nothing recent.
		std::optional<Sampled> Sample(const RemotePlayer& a_player, Clock::time_point a_now)
		{
			const auto& snaps = a_player.snapshots;
			if (snaps.empty() || a_now - snaps.back().received > STALE_AFTER) {
				return std::nullopt;
			}

			const auto renderTime = a_now - INTERPOLATION_DELAY;
			if (renderTime <= snaps.front().received) {
				return Sampled{ snaps.front().state, snaps.front().state.heading };
			}

			for (std::size_t i = 1; i < snaps.size(); ++i) {
				const auto& a = snaps[i - 1];
				const auto& b = snaps[i];
				if (renderTime > b.received) {
					continue;
				}

				// Don't blend across a cell or worldspace change (doors, fast travel).
				if (a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace) {
					return Sampled{ b.state, b.state.heading };
				}

				const float span = std::chrono::duration<float>(b.received - a.received).count();
				const float t = span > 0.0f ? std::chrono::duration<float>(renderTime - a.received).count() / span : 1.0f;

				auto out = b.state;
				out.x = std::lerp(a.state.x, b.state.x, t);
				out.y = std::lerp(a.state.y, b.state.y, t);
				out.z = std::lerp(a.state.z, b.state.z, t);
				out.heading = LerpAngle(a.state.heading, b.state.heading, t);

				const float dx = b.state.x - a.state.x;
				const float dy = b.state.y - a.state.y;
				const float motionAngle = dx * dx + dy * dy > 1.0f ? std::atan2(dx, dy) : out.heading;
				return Sampled{ out, motionAngle };
			}

			// Past the newest snapshot: hold position rather than guessing ahead, and stop
			// animating if updates have dried up.
			auto held = snaps.back().state;
			heldFrames += held.speed > 0.0f && a_now - snaps.back().received < 1s;
			if (a_now - snaps.back().received > INTERPOLATION_DELAY + 250ms) {
				held.speed = 0.0f;
			}
			return Sampled{ held, held.heading };
		}

		std::string NameTag(const RemotePlayer& a_player)
		{
			if (a_player.downed) {
				return a_player.name + " (down - help them up)";
			}
			if (a_player.health == 0) {
				return a_player.name + " (dead)";
			}
			// Rounded to tens so the tag doesn't change with every scratch.
			const int shown = (a_player.health + 9) / 10 * 10;
			return shown >= 100 ? a_player.name : std::format("{} ({}%)", a_player.name, shown);
		}

		RE::Actor* GetActor(const RemotePlayer& a_player)
		{
			const auto ref = a_player.actor.get();
			if (!ref || ref->IsDeleted()) {
				return nullptr;
			}
			return ref->As<RE::Actor>();
		}

		// A dead puppet (killed despite protection, e.g. by a script) is replaced by a fresh one.
		bool IsBroken(RE::Actor* a_actor)
		{
			return a_actor->IsDead(false) || a_actor->IsDisabled();
		}

		// Removes the stand-in's light, once its 3D is in (the same rule as for the stand-in itself).
		void DeleteLight(RemotePlayer& a_player)
		{
			if (const auto light = a_player.light.get()) {
				if (DeferredDelete::Safe(light->Get3D() != nullptr, Clock::now() - a_player.lightPlacedAt)) {
					light->Disable();
					light->SetDelete(true);
				} else {
					graveyard.push_back({ a_player.light, a_player.lightPlacedAt });
				}
			}
			a_player.light = {};
		}

		// Keeps a light at the stand-in while their Pip-Boy light is on, and removes it when it goes off.
		void UpdateLight(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Clock::time_point a_now)
		{
			auto light = a_player.light.get();
			if (!a_player.lightOn || !a_actor || !a_actor->Get3D()) {
				if (light) {
					DeleteLight(a_player);
				}
				return;
			}
			// The stand-in walked into another cell: the light is remade there (a reference stays in the
			// cell it was created in).
			if (light && light->GetParentCell() != a_actor->GetParentCell()) {
				DeleteLight(a_player);
				light = nullptr;
			}
			RE::NiPoint3 at = a_actor->data.location;
			at.z += LIGHT_HEIGHT;
			if (!light) {
				const auto form = RE::TESForm::GetFormByID<RE::TESObjectLIGH>(friendLightForm);
				if (!form) {
					return;
				}
				RE::NEW_REFR_DATA data;
				data.location = at;
				data.object = form;
				data.interior = a_cell->IsInterior() ? a_cell : nullptr;
				data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
				data.clearStillLoadingFlag = true;
				const auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(data);
				light = handle.get().get();
				if (!light) {
					return;
				}
				a_player.light = handle;
				a_player.lightPlacedAt = a_now;
				++lightsPlaced;
			}
			light->SetLocationOnReference(at);
		}

		// The frame placed for the stand-in is removed once the stand-in is out of it (or gone).
		void DeleteFrame(RemotePlayer& a_player)
		{
			if (const auto frame = a_player.frame.get()) {
				// Our own player may have climbed into the friend's frame copy: then it stays (deleting it would
				// leave them in nothing) and is simply no longer ours to manage.
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (player && player->extraList && player->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor) && player->data.location.GetDistance(frame->data.location) < 200.0f) {
					REX::INFO("RemotePlayers: the player is in '{}'s frame copy; leaving it", a_player.name);
					a_player.frame = {};
					a_player.inPowerArmor = false;
					return;
				}
				if (DeferredDelete::Safe(frame->Get3D() != nullptr, Clock::now() - a_player.framePlacedAt)) {
					frame->Disable();
					frame->SetDelete(true);
				} else {
					graveyard.push_back({ a_player.frame, a_player.framePlacedAt });
				}
			}
			a_player.frame = {};
			a_player.inPowerArmor = false;
		}

		// Power armor: a friend in a frame is shown in one. A frame of their frame's base is placed at
		// the stand-in and the stand-in activates it, the way an NPC takes power armor; the armor pieces
		// come with their equipment. When they climb out, the stand-in activates the frame again and the
		// frame is removed.
		constexpr auto FRAME_ACTION_GAP = 4s;
		void UpdatePowerArmor(RemotePlayer& a_player, RE::Actor* a_actor, RE::TESObjectCELL* a_cell, Clock::time_point a_now)
		{
			if (!a_actor || !a_actor->Get3D() || a_now - a_player.frameActionAt < FRAME_ACTION_GAP) {
				return;
			}
			auto frame = a_player.frame.get();
			if (a_player.powerArmorOn && !a_player.inPowerArmor) {
				const auto base = RE::TESForm::GetFormByID<RE::TESFurniture>(a_player.powerArmorFrame);
				if (!base) {
					return;
				}
				if (frame && frame->GetParentCell() != a_actor->GetParentCell()) {
					DeleteFrame(a_player);
					frame = nullptr;
				}
				if (!frame) {
					RE::NEW_REFR_DATA data;
					data.location = a_actor->data.location;
					data.direction = a_actor->data.angle;
					data.object = base;
					data.interior = a_cell->IsInterior() ? a_cell : nullptr;
					data.world = a_cell->IsInterior() ? nullptr : a_cell->worldSpace;
					data.clearStillLoadingFlag = true;
					const auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(data);
					frame = handle.get().get();
					if (!frame) {
						return;
					}
					a_player.frame = handle;
					a_player.framePlacedAt = a_now;
					a_player.frameActionAt = a_now;  // let it load before the stand-in uses it
					++framesPlaced;
					return;
				}
				if (!frame->Get3D()) {
					return;
				}
				Puppets::AllowAI(a_actor, std::chrono::milliseconds(6000));
				frame->ActivateRef(a_actor, nullptr, 0, false, false, false);
				a_player.inPowerArmor = true;
				a_player.frameActionAt = a_now;
				a_player.equipmentApplied = false;  // the pieces go on once it is in the frame
				REX::INFO("RemotePlayers: '{}' stand-in gets into power armor (frame {:08X})", a_player.name, a_player.powerArmorFrame);
			} else if (!a_player.powerArmorOn && a_player.inPowerArmor) {
				if (frame && frame->Get3D()) {
					Puppets::AllowAI(a_actor, std::chrono::milliseconds(6000));
					frame->ActivateRef(a_actor, nullptr, 0, false, false, false);
				}
				a_player.inPowerArmor = false;
				a_player.frameActionAt = a_now;
				a_player.equipmentApplied = false;
				REX::INFO("RemotePlayers: '{}' stand-in gets out of power armor", a_player.name);
			} else if (!a_player.powerArmorOn && frame && a_now - a_player.frameActionAt >= FRAME_ACTION_GAP) {
				DeleteFrame(a_player);  // out of it and the animation is done
			}
		}

		void DeleteActor(RemotePlayer& a_player)
		{
			DeleteLight(a_player);
			DeleteFrame(a_player);
			if (a_player.registered) {
				Puppets::Unregister(a_player.registered);
				a_player.registered = nullptr;
				a_player.registeredId = 0;
			}
			if (const auto actor = GetActor(a_player)) {
				if (DeferredDelete::Safe(actor->Get3D() != nullptr, Clock::now() - a_player.spawnedAt)) {
					actor->Disable();
					actor->SetDelete(true);
				} else {
					graveyard.push_back({ a_player.actor, a_player.spawnedAt });
				}
			}
			a_player.actor = {};
		}

		bool SameSpace(const Protocol::PlayerState& a_state, const RE::TESObjectCELL* a_localCell)
		{
			if (a_localCell->IsInterior()) {
				return a_state.cell == a_localCell->GetFormID();
			}
			return a_state.cell == 0 && a_localCell->worldSpace && a_state.worldspace == a_localCell->worldSpace->GetFormID();
		}

		// The default stand-in bases: settlers with a fixed look and no factions, one per sex.
		constexpr std::uint32_t FEMALE_SETTLER = 0x0020A578;
		constexpr std::uint32_t MALE_SETTLER = 0x0020A57B;

		RE::Actor* Spawn(RemotePlayer& a_player, const Protocol::PlayerState& a_state, RE::TESObjectCELL* a_localCell)
		{
			// Prefer the look the player picked; fall back to our default if it isn't a valid NPC here.
			// Unique NPCs (companions, quest characters) would bring their scripts along, so not those.
			auto npc = a_player.appearance ? RE::TESForm::GetFormByID<RE::TESNPC>(a_player.appearance) : nullptr;
			if (npc && npc->IsUnique()) {
				REX::WARN("RemotePlayers: '{}' asked to look like unique NPC {:08X}; using the default", a_player.name, a_player.appearance);
				npc = nullptr;
			}
			if (!npc) {
				auto baseID = Config::Get().puppetBaseForm;
				// With their face known, the base is a settler of their sex (unless the ini picked another base).
				if (a_player.face && baseID == FEMALE_SETTLER && !a_player.face->female) {
					baseID = MALE_SETTLER;
				}
				npc = RE::TESForm::GetFormByID<RE::TESNPC>(baseID);
				if (!npc) {
					REX::ERROR("RemotePlayers: puppet base form {:08X} is not an NPC", baseID);
					return nullptr;
				}
			}
			// Their own face, hair and body shape on a copy of that base (made once per look).
			if (a_player.face && !a_player.faceTried) {
				a_player.faceTried = true;
				a_player.faceBase = Face::MakeBase(npc, *a_player.face);
			}
			if (a_player.faceBase) {
				npc = a_player.faceBase;
			}

			RE::NEW_REFR_DATA data;
			data.location = { a_state.x, a_state.y, a_state.z };
			data.direction = { 0.0f, 0.0f, a_state.heading };
			data.object = npc;
			data.interior = a_localCell->IsInterior() ? a_localCell : nullptr;
			data.world = a_localCell->IsInterior() ? nullptr : a_localCell->worldSpace;
			data.clearStillLoadingFlag = true;

			const auto handle = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(data);
			const auto ref = handle.get();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor) {
				REX::ERROR("RemotePlayers: failed to spawn puppet for '{}'", a_player.name);
				return nullptr;
			}

			Puppets::Register(actor);
			// Enemies treat the stand-in like the player (it's what NPCs in this world can attack),
			// and hitting it isn't a crime.
			RE::Console::ExecuteCommand(std::format("{:08X}.addtofaction {:08X} 1", actor->GetFormID(), PLAYER_FACTION).c_str());

			REX::INFO("RemotePlayers: spawned {:08X} for '{}'", actor->GetFormID(), a_player.name);
			return actor;
		}
	}

	void Add(std::uint32_t a_id, std::string a_name, std::uint32_t a_appearance)
	{
		// A reused ID is a new player: start from scratch (old snapshots would hide their states).
		Remove(a_id);
		auto& player = players[a_id];
		if (player.knownAt == Clock::time_point{}) {
			player.knownAt = Clock::now();
		}
		player.name = std::move(a_name);
		player.appearance = a_appearance;
	}

	void Remove(std::uint32_t a_id)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			DeleteActor(it->second);
			players.erase(it);
		}
	}

	void RemoveAll()
	{
		for (auto& [id, player] : players) {
			DeleteActor(player);
		}
		players.clear();
	}

	void PushState(std::uint32_t a_id, const Protocol::PlayerState& a_state)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;  // state for a player we haven't been told about yet
		}
		auto& snaps = it->second.snapshots;
		if (!snaps.empty() && a_state.sequence <= snaps.back().state.sequence) {
			return;
		}
		snaps.push_back({ Clock::now(), a_state });
		while (snaps.size() > MAX_SNAPSHOTS) {
			snaps.pop_front();
		}
	}

	void SetFace(std::uint32_t a_id, Protocol::Face a_face)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		auto& player = it->second;
		if (player.face && *player.face == a_face) {
			return;
		}
		player.face = std::move(a_face);
		player.faceBase = nullptr;  // the old copy stays in memory (forms can't be freed); a new one is made
		player.faceTried = false;
		// The stand-in is rebuilt with the new look the next time it's drawn.
		DeleteActor(player);
	}

	void SetEquipment(std::uint32_t a_id, std::vector<std::uint32_t> a_items)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		it->second.equipment = std::move(a_items);
		it->second.hasEquipment = true;
		it->second.equipmentApplied = false;
		it->second.equipmentFixes = 0;
	}

	std::vector<Info> List()
	{
		std::vector<Info> out;
		for (const auto& [id, remote] : players) {
			Info info{ id, remote.name, std::nullopt, GetActor(remote) };
			if (!remote.snapshots.empty()) {
				info.state = remote.snapshots.back().state;
			}
			out.push_back(std::move(info));
		}
		return out;
	}

	std::string NameOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		return it != players.end() ? it->second.name : std::string{};
	}

	void SetHealth(std::uint32_t a_id, std::uint8_t a_percent, bool a_downed)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.health = a_percent;
			it->second.downed = a_downed;
		}
	}

	void PlayShot(std::uint32_t a_id)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			WeaponFire::PlayShot(GetActor(it->second));
		}
	}

	void PlayAction(std::uint32_t a_id, std::uint8_t a_action)
	{
		const auto it = players.find(a_id);
		if (it == players.end()) {
			return;
		}
		auto& player = it->second;
		if (a_action == Protocol::ShotAction::kLightOn || a_action == Protocol::ShotAction::kLightOff) {
			player.lightOn = a_action == Protocol::ShotAction::kLightOn;
			return;  // Update() keeps the light with the stand-in
		}
		WeaponFire::PlayAction(GetActor(player), a_action);
	}

	std::uint32_t PlayerIdFor(std::uint32_t a_actorFormId)
	{
		for (const auto& [id, remote] : players) {
			if (remote.registered && remote.registeredId == a_actorFormId) {
				return id;
			}
		}
		return 0;
	}

	void DespawnAll()
	{
		for (auto& [id, player] : players) {
			DeleteActor(player);
		}
	}

	void SetLight(std::uint32_t a_id, bool a_on)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.lightOn = a_on;  // Update() keeps the light with the stand-in
		}
	}

	std::uint32_t ActorIdOf(std::uint32_t a_id)
	{
		const auto it = players.find(a_id);
		const auto actor = it != players.end() ? it->second.actor.get() : nullptr;
		return actor ? actor->GetFormID() : 0;
	}

	void SetPowerArmor(std::uint32_t a_id, bool a_on, std::uint32_t a_frameBase)
	{
		if (const auto it = players.find(a_id); it != players.end()) {
			it->second.powerArmorOn = a_on;
			if (a_frameBase) {
				it->second.powerArmorFrame = a_frameBase;
			}
		}
	}

	void SetLightForm(std::uint32_t a_formId)
	{
		friendLightForm = a_formId;
		for (auto& [id, player] : players) {
			DeleteLight(player);  // placed again with the new form
		}
	}

	void RebuildFaces()
	{
		for (auto& [id, player] : players) {
			player.faceBase = nullptr;
			player.faceTried = false;
			DeleteActor(player);
		}
	}

	void Update()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto localCell = player ? player->GetParentCell() : nullptr;
		const auto ui = RE::UI::GetSingleton();
		const bool loading = ui && ui->GetMenuOpen("LoadingMenu"sv);

		const auto now = Clock::now();
		std::erase_if(graveyard, [&](const Grave& a_grave) {
			const auto actor = a_grave.actor.get();
			if (!actor) {
				return true;
			}
			if (!DeferredDelete::Safe(actor->Get3D() != nullptr, now - a_grave.spawnedAt)) {
				return false;
			}
			actor->Disable();
			actor->SetDelete(true);
			return true;
		});
		for (auto& [id, remote] : players) {
			// Drop snapshots that are too old to ever be rendered again.
			while (remote.snapshots.size() > 2 && now - remote.snapshots[1].received > INTERPOLATION_DELAY + 1s) {
				remote.snapshots.pop_front();
			}

			movingFrames += !remote.snapshots.empty() && remote.snapshots.back().state.speed > 0.0f && now - remote.snapshots.back().received < 1s;
			const auto sampled = Sample(remote, now);
			const auto state = sampled ? std::optional{ sampled->state } : std::nullopt;
			bool       visible = state && localCell && !loading && SameSpace(*state, localCell);
			if (visible && localCell->IsExterior()) {
				const auto& p = player->data.location;
				const float dx = state->x - p.x;
				const float dy = state->y - p.y;
				visible = dx * dx + dy * dy < MAX_EXTERIOR_DISTANCE * MAX_EXTERIOR_DISTANCE;
			}

			auto actor = GetActor(remote);
			if (!actor && remote.registered) {
				// The game removed our puppet (e.g. cell unload); forget the stale registration.
				DeleteActor(remote);
			}
			if (actor && IsBroken(actor)) {
				DeleteActor(remote);
				actor = nullptr;
			}
			if (!visible) {
				if (actor) {
					DeleteActor(remote);
				}
				continue;
			}

			// A puppet left behind in another cell (e.g. we went through a door) is replaced.
			if (actor && actor->GetParentCell() != localCell && localCell->IsInterior()) {
				DeleteActor(remote);
				actor = nullptr;
			}

			if (!actor) {
				if (!remote.face && now - remote.knownAt < FACE_GRACE) {
					continue;  // their face is on its way
				}
				actor = Spawn(remote, *state, localCell);
				if (!actor) {
					continue;
				}
				remote.spawnedAt = now;
				remote.actor = actor->GetHandle();
				remote.registered = actor;
				remote.registeredId = actor->GetFormID();
				remote.equipmentApplied = false;
				remote.equipmentFixes = 0;
				remote.shownName.clear();
			}

			UpdateLight(remote, actor, localCell, now);
			UpdatePowerArmor(remote, actor, localCell, now);

			// Shown when looking at them, instead of the NPC's name, with their health when hurt.
			if (auto label = NameTag(remote); label != remote.shownName && actor->extraList) {
				actor->extraList->SetOverrideName(label.c_str());
				remote.shownName = std::move(label);
			}

			if (remote.hasEquipment && !remote.equipmentApplied && actor->Get3D()) {
				Equipment::Apply(actor, remote.equipment);
				Puppets::RedrawWeapon(actor);
				remote.equipmentApplied = true;
				remote.equipmentCheckAt = now + EQUIPMENT_CHECK_INTERVAL;
			} else if (remote.equipmentApplied && now >= remote.equipmentCheckAt && actor->Get3D()) {
				remote.equipmentCheckAt = now + EQUIPMENT_CHECK_INTERVAL;
				if (Equipment::Read(actor) == Wearable(remote.equipment)) {
					remote.equipmentFixes = 0;
				} else if (remote.equipmentFixes < MAX_EQUIPMENT_FIXES) {
					++remote.equipmentFixes;
					++equipmentFixesTotal;
					remote.equipmentApplied = false;  // applied again next frame
					REX::INFO("RemotePlayers: '{}' stand-in isn't wearing their gear; dressing it again", remote.name);
				}
			}

			Puppets::SetTarget(actor, Puppets::Motion{
				.position = { state->x, state->y, state->z },
				.heading = state->heading,
				.speed = state->speed,
				.direction = RelativeDirection(sampled->motionAngle, state->heading),
				.moveMode = state->moveMode,
				.flags = state->flags,
			});
		}
	}


	std::string Describe()
	{
		std::string out;
		for (const auto& [id, remote] : players) {
			const auto actor = GetActor(remote);
			const auto& snaps = remote.snapshots;
			const auto& last = snaps.empty() ? Protocol::PlayerState{} : snaps.back().state;
			// Average time between the states we hold (about the last second).
			const auto gap = snaps.size() > 1 ? std::chrono::duration_cast<std::chrono::milliseconds>(snaps.back().received - snaps.front().received).count() /
			                                        static_cast<long long>(snaps.size() - 1) :
			                                    0;
			out += std::format("{}[{} '{}' puppet={:08X} x={:.0f} y={:.0f} cell={:08X} ws={:08X} gapMs={} pa={}/{} frame={:08X}]",
				out.empty() ? "" : " ", id, remote.name, actor ? actor->GetFormID() : 0,
				last.x, last.y, last.cell, last.worldspace, gap, remote.powerArmorOn, remote.inPowerArmor, remote.powerArmorFrame);
		}
		if (movingFrames) {
			out += std::format(" held={}/{}", heldFrames, movingFrames);
			out += std::format(" lights={} frames={}", lightsPlaced, framesPlaced);
		}
		if (equipmentFixesTotal) {
			out += std::format(" gearFixes={}", equipmentFixesTotal);
		}
		return out.empty() ? "none" : out;
	}
}
