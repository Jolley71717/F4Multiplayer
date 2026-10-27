#include "net/Session.h"

#include "Config.h"
#include "FormIds.h"
#include "FormTranslate.h"
#include "MainThread.h"
#include "Name.h"
#include "Protocol.h"
#include "Redact.h"
#include "Server.h"
#include "Text.h"
#include "game/Achievements.h"
#include "game/AimPoint.h"
#include "game/AutoSave.h"
#include "game/AutoSaveRules.h"
#include "game/CharacterBinding.h"
#include "game/Compass.h"
#include "game/FadeWatch.h"
#include "game/MenuIcons.h"
#include "game/Conversations.h"
#include "game/CopyWatch.h"
#include "game/Downed.h"
#include "game/Drops.h"
#include "game/Equipment.h"
#include "game/Face.h"
#include "game/FrameClock.h"
#include "game/Gait.h"
#include "game/GameStop.h"
#include "game/HostRules.h"
#include "game/Hud.h"
#include "game/LooksMenu.h"
#include "game/MapShare.h"
#include "game/Destructibles.h"
#include "game/MovedObjects.h"
#include "game/WorkshopSync.h"
#include "game/Voice.h"
#include "game/Party.h"
#include "game/WorldClock.h"
#include "game/NpcSync.h"
#include "game/Perf.h"
#include "game/OpenMenus.h"
#include "game/QuestSync.h"
#include "game/RadiantSync.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "game/SaveGuard.h"
#include "game/Spawns.h"
#include "game/Storage.h"
#include "game/Story.h"
#include "game/Traps.h"
#include "game/WeaponFire.h"
#include "game/WorldSync.h"
#include "net/Identity.h"
#include "net/ModDiff.h"
#include "net/NameChoice.h"
#include "net/NetClient.h"
#include "net/Refusals.h"
#include "net/SessionSteps.h"
#include "steam/Steam.h"
#include "steam/SteamTransport.h"

#include <unordered_set>

namespace Session
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto SEND_INTERVAL = 33ms;  // 30 Hz
		constexpr auto RECONNECT_DELAY = 5s;

		std::uint32_t           contentHash = 0;
		std::vector<std::string> plugins;  // the load order, sent with the hello
		std::unique_ptr<Server> hostedServer;
		bool                    steamMode = false;  // friends connect through Steam

		// The connection in use: UDP for "ip[:port]" addresses (and the host's own game), Steam for
		// "steam:<id>".
		NetClient         udpClient;
		SteamClient       steamClient;
		ClientConnection* client = &udpClient;
		std::string       serverAddress;

		bool              welcomed = false;
		std::uint32_t     localId = 0;
		std::uint32_t     sequence = 0;
		Clock::time_point nextSend{};
		// The name our hello went out with, or our last rename: the name is looked at once a second while in the
		// session (with the character check) and sent again (kRename) when it differs, which it does when a joiner
		// names a new character in the opening after connecting (the hello went with the Steam name).
		std::string       sentName;

		// Equipment is checked once a second and sent when it changes (and after joining).
		constexpr auto                            EQUIPMENT_INTERVAL = 1s;
		Clock::time_point                         nextEquipmentCheck{};
		std::optional<Protocol::Equipment>        sentEquipment;  // with its mods
		std::optional<Protocol::Face>             sentFace;
		Clock::time_point nextConnectAttempt{};
		Refusals          refusals;  // why the server last turned us away, and when to try again
		ModDiff::Tracker  hostMods;  // our mods compared with the host's, once its list came (kLoadOrder)
		bool              shuttingDown = false;  // the game is quitting (Shutdown): nothing more is done
		// Host controls: the server's last word on its lock, password and bans (kHostInfo, to our own game only while we
		// host), counted so the menu sees a new one; and the host removed us, or refused us for a ban (Leave after the poll).
		std::optional<Protocol::HostInfo> hostInfo;
		std::uint32_t                     hostInfoSerial = 0;
		bool                              removedAfterPoll = false;
		Clock::time_point nextHostAttempt{};  // bHost: when Frame may try to start the server again
		std::string       lastHostError;      // what went wrong the last time (said once, not at every try)

		// The character whose session we host, and the one our game joined with (see CharacterBinding).
		constexpr auto                    CHARACTER_CHECK_INTERVAL = 1s;
		Clock::time_point                 nextCharacterCheck{};
		CharacterBinding::Tracker         binding;
		bool                              leaveAfterPoll = false;  // let back into another session than the one we joined
		std::unordered_set<std::uint64_t> legacyTold;              // characters told about the old shared session file
		std::uint64_t                     rulesToldFor = 0;        // the session whose rules for being down the HUD has told
		// A new character with no name yet outside Vault 111 (an alternate start): told once per new game that the session
		// waits for the name, if it takes this long.
		constexpr auto                    UNNAMED_NOTICE_AFTER = 2min;
		std::optional<Clock::time_point>  unnamedSince;
		bool                              unnamedTold = false;

		RE::NiPoint3      lastPosition;
		Clock::time_point lastPositionTime{};

		// The session's own messages (connected, refused and why, disconnected) name nobody, so they go to
		// the log at INFO too; players joining and leaving go to the HUD by name and to the log by ID. A
		// refusal's reason is the host's text: Redact::OneLine keeps it one line (the wire drops control
		// characters too, but not every message here came over it). The player reads a_shown in their language; the
		// log gets a_logged, in English.
		void Say(const std::string& a_shown, const std::string& a_logged)
		{
			REX::INFO("Session: {}", Redact::OneLine(a_logged));
			Hud::Notify(a_shown);
		}

		void Notify(std::string_view a_key, std::initializer_list<Text::Arg> a_args = {})
		{
			Say(Text::Format(a_key, a_args), Text::FormatEnglish(a_key, a_args));
		}

		// FNV-1a over the load order, so players with different mods are told up front. The free high-resolution
		// texture pack is textures only (its plugin is empty), so a player without it can play with one who has it;
		// unless other plugins load after it, whose form IDs it moves up a place (Protocol::ComparedFullPlugins).
		std::uint32_t ComputeContentHash()
		{
			const auto               data = RE::TESDataHandler::GetSingleton();
			std::vector<std::string> full;
			std::vector<std::string> light;
			for (const auto file : data->compiledFileCollection.files) {
				full.emplace_back(file->GetFilename());
			}
			for (const auto file : data->compiledFileCollection.smallFiles) {
				light.emplace_back(file->GetFilename());
			}
			plugins = Protocol::ComparedLoadOrder(std::move(full), light);
			if (std::ranges::any_of(plugins, [](const std::string& a_name) { return Protocol::SamePluginName(a_name, Protocol::TEXTURE_PACK); })) {
				REX::INFO("Session: the HD texture pack counts in the load order: the plugins after it have other form IDs without it");
			}
			return Protocol::LoadOrderHash(plugins);
		}

		// The name we say hello with (NameChoice::Choose), and the one Frame compares once a second while we are in the
		// session and in the world (kRename when it changed: a new character named in the opening after connecting).
		// Cleaned as the server cleans it (common/Name.h), so what the wire's bytes hold is the shown name, not
		// characters the server would drop anyway.
		std::string PlayerName()
		{
			const auto& ini = Config::Get().playerName;
			std::string character;
			if (NameChoice::WantsCharacter(ini)) {
				// The player's name in the game: it is empty (or absent) until a new character has confirmed one in the opening.
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					if (const char* name = player->GetDisplayFullName()) {
						character = name;
					}
				}
			}
			auto chosen = NameChoice::Choose(ini, character, Steam::PersonaName());
			auto shown = Name::Sanitize(chosen);
			return shown.empty() ? chosen : shown;  // an ini name nothing is left of: the server makes it "Player N"
		}

		bool InGame()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell();
		}

		// The loaded character's ID: the game makes it when the player confirms the name in the opening and
		// keeps it in every save of that character. 0 is an ordinary ID (a character from coc at the main menu
		// has it, and many saves do). Its low half only (CharacterBinding::Character): a quickload of the same
		// character can bring another high half.
		std::uint64_t CurrentCharacter()
		{
			const auto saves = RE::BGSSaveLoadManager::GetSingleton();
			return saves ? CharacterBinding::Character(saves->currentPlayerID) : 0;
		}

		// What CharacterBinding needs to know of us. Joining: set to reach a friend's server, whether in it, connecting
		// or waiting to try again.
		CharacterBinding::Link LinkNow()
		{
			return { Config::Get().host, hostedServer != nullptr, !hostedServer && !serverAddress.empty() };
		}

		std::uint32_t hitsTaken = 0;
		std::uint32_t hitsIgnored = 0;
		float         lastHitMult = 1.0f;  // how much the last hit was scaled (difficulty, VATS)

		// Whether the NPC that hit our stand-in is there to see in our world: loaded, alive and
		// within weapon range. Otherwise we'd take damage from nowhere (it died in our game, or
		// hasn't spawned yet).
		bool AttackerHere(const RE::PlayerCharacter* a_player, std::uint32_t a_attacker)
		{
			constexpr float RANGE = 10000.0f;  // about 140 m: a sniper rifle's reach
			// A friend's spawn (made in their game at runtime) is here as our copy of it, if we placed one.
			const auto      local = Protocol::IsSpawnRef(a_attacker) ? Spawns::LocalOf(a_attacker) : a_attacker;
			const auto      actor = local ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
			if (!actor || !actor->Get3D() || actor->IsDead(false)) {
				return false;
			}
			const auto here = a_player->GetParentCell();
			const auto there = actor->GetParentCell();
			if (!here || !there || (here->IsInterior() || there->IsInterior() ? here != there : here->worldSpace != there->worldSpace)) {
				return false;
			}
			return actor->data.location.GetDistance(a_player->data.location) < RANGE;
		}

		// Menus that stop the game (pause menu, Pip-Boy, ...): no damage lands while it's stopped.
		bool GamePaused()
		{
			return GameStop::Now();
		}

		// Something hit our stand-in in another player's world.
		float GameSettingFloat(const char* a_name, float a_default)
		{
			const auto settings = RE::GameSettingCollection::GetSingleton();
			const auto setting = settings ? settings->GetSetting(a_name) : nullptr;
			return setting && setting->GetType() == RE::Setting::SETTING_TYPE::kFloat ? setting->GetFloat() : a_default;
		}

		// The hit landed on our stand-in in a friend's game, where nothing knew our difficulty or
		// that we're in VATS. Scale it the way our own game scales damage to the player.
		float DamageTakenMult(RE::PlayerCharacter* a_player, bool a_byPlayer)
		{
			static constexpr std::array<const char*, 7> DIFFICULTY{ "fDiffMultHPToPCVE", "fDiffMultHPToPCE", "fDiffMultHPToPCN", "fDiffMultHPToPCH",
				"fDiffMultHPToPCVH", "fDiffMultHPToPCSV", "fDiffMultHPToPCTSV" };
			float mult = 1.0f;
			// Friendly fire was already scaled by the shooter's difficulty.
			if (!a_byPlayer) {
				const auto level = std::clamp(static_cast<int>(a_player->GetDifficultyLevel()), 0, static_cast<int>(DIFFICULTY.size()) - 1);
				mult *= GameSettingFloat(DIFFICULTY[level], 1.0f);
			}
			const auto vats = RE::VATS::GetSingleton();
			if (OpenMenus::Open(OpenMenus::Menu::kVats)) {
				mult *= GameSettingFloat("fVATSPlayerMenuDamageMult", 1.0f);
			} else if (vats && vats->mode.any(RE::VATS::VATS_MODE_ENUM::kPlayback)) {
				mult *= GameSettingFloat("fVATSPlayerDamageMult", 0.1f);
			}
			return mult;
		}

		void ApplyDamageToPlayer(const Protocol::PlayerHit& a_hit)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto values = RE::ActorValue::GetSingleton();
			if (!WorldSync::InWorld() || !player || !values || !values->health || player->IsDead(false) || a_hit.damage <= 0.0f) {
				return;
			}
			if (GamePaused() || Story::InOpening() || (!a_hit.byPlayer && !AttackerHere(player, a_hit.attacker))) {
				++hitsIgnored;
				return;
			}
			++hitsTaken;
			lastHitMult = DamageTakenMult(player, a_hit.byPlayer);
			const float damage = (std::min)(a_hit.damage * lastHitMult, 1000.0f);
			static_cast<RE::ActorValueOwner&>(*player).ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *values->health, -damage);
		}

		// What the HUD says of our mods compared with the host's (ModDiff::Tracker decides when): the plugins only one of us
		// has, or, refused for our load order, the moved ones too.
		void NotifyMods(bool a_refused = false)
		{
			if (const auto count = hostMods.Differing(a_refused); count == 0) {
				Notify("mods.match");
			} else {
				Notify("mods.differ", { { "n", count }, { "key", ModDiff::KeyName(Config::Get().keyMenu) } });
			}
		}

		// The session's load order came (kLoadOrder: our hello's differed, or we asked): ours compared with it, the
		// plugins the host lets differ left out, goes to the log name by name, and to the HUD when it is news
		// (ModDiff::Tracker). Whatever it says we are let in (form IDs are translated by plugin name), unless our hello or
		// the session's first sent no list.
		void OnHostLoadOrder(const Protocol::LoadOrder& a_hosts)
		{
			auto       diff = ModDiff::Compare(plugins, a_hosts.plugins, a_hosts.ignored);
			const bool same = diff == hostMods.Known();
			if (same) {
				REX::INFO("Session: mods: the host's load order again, {} differ as before", diff.Count());
			} else {
				for (const auto& line : ModDiff::LogLines(diff)) {
					REX::INFO("Session: mods: {}", Redact::OneLine(line));
				}
				if (!a_hosts.ignored.empty()) {
					std::string names;
					for (const auto& name : a_hosts.ignored) {
						names += (names.empty() ? "" : ", ") + name;
					}
					REX::INFO("Session: mods: the host lets these differ where they move no other plugin: {}", Redact::OneLine(names));
				}
			}
			if (hostMods.OnList(std::move(diff), Clock::now())) {
				NotifyMods();
			}
		}

		// Our form IDs and the session's (common/FormIds.h): the identity until the server's plugin table comes (just
		// before the welcome), and again once we leave. Every packet we send and get goes through FormTranslate on the
		// way (SendOut, Receive), so the rest of the game only sees our own IDs. What had no ID on the other side is
		// counted, and the plugin it was of named once in the log.
		FormIds::Mapper forms;
		bool            tableThisConnection = false;
		struct FormSkips
		{
			std::uint32_t packets = 0;  // messages dropped
			std::uint32_t entries = 0;  // list entries left out (and container changes emptied)
			std::uint32_t fields = 0;   // details set to none
		};
		FormSkips                       skippedOut;
		FormSkips                       skippedIn;
		std::unordered_set<std::string> formPluginsLogged;  // "<" or ">" and the plugin's name
		constexpr std::size_t           MAX_FORM_PLUGINS_LOGGED = 256;

		void NoteSkips(const FormTranslate::Result& a_result, FormTranslate::Way a_way, std::span<const std::uint8_t> a_packet)
		{
			const bool out = a_way == FormTranslate::Way::kToSession;
			auto&      skips = out ? skippedOut : skippedIn;
			skips.packets += a_result.kind == FormTranslate::Result::Kind::kDropped ? 1 : 0;
			skips.entries += a_result.entries;
			skips.fields += a_result.fields;
			const auto type = a_packet.empty() ? 0 : a_packet[0];
			for (const auto id : a_result.unmapped) {
				auto plugin = out ? forms.LocalPlugin(id) : forms.SessionPlugin(id);
				if (plugin.empty()) {
					plugin = "a plugin past the table's end";
				}
				if (formPluginsLogged.size() >= MAX_FORM_PLUGINS_LOGGED || !formPluginsLogged.insert((out ? ">" : "<") + plugin).second) {
					continue;
				}
				if (out) {
					REX::INFO("Session: forms: {} is not in the session's plugin table; what of ours comes from it is not shared (first: {:08X}, message {})", Redact::OneLine(plugin), id, type);
				} else {
					REX::INFO("Session: forms: {} is not in our game; what comes from it is skipped here (first: {:08X}, message {})", Redact::OneLine(plugin), id, type);
				}
			}
		}

		// Every packet we send: in the session's numbering, or not at all when what it is about has none there.
		void SendOut(std::vector<std::uint8_t> a_packet, bool a_reliable)
		{
			if (!forms.Identity()) {
				auto result = FormTranslate::Translate(a_packet, forms, FormTranslate::Way::kToSession);
				NoteSkips(result, FormTranslate::Way::kToSession, a_packet);
				if (result.kind == FormTranslate::Result::Kind::kDropped) {
					Storage::Unsent(a_packet);  // a move of ours at a friend's storage that can't go is undone
					return;
				}
				if (result.kind == FormTranslate::Result::Kind::kChanged) {
					a_packet = std::move(result.packet);
				}
			}
			client->Send(std::move(a_packet), a_reliable);
		}

		// The session's plugin table came (before the welcome, or grown by a joiner's plugins).
		void OnPluginTable(const Protocol::PluginTable& a_table)
		{
			const bool grew = std::exchange(tableThisConnection, true);
			forms = FormIds::Mapper(plugins, a_table.plugins);
			if (forms.Identity()) {
				REX::INFO("Session: forms: the session's plugin table is our load order ({} plugins): nothing to translate", forms.Session().Size());
			} else {
				REX::INFO("Session: forms: the session's plugin table has {} plugins, our load order {}: form IDs are translated by plugin name "
						  "({} of ours are not in it, {} of its are not in our game){}",
					forms.Session().Size(), forms.Local().Size(), forms.OursOnly(), forms.SessionsOnly(), grew ? " (grown by a joiner's plugins)" : "");
			}
		}

		void HandlePacket(std::span<const std::uint8_t> a_data);

		// Every packet we get: in our numbering, or not at all when what it is about has none here.
		void Receive(std::span<const std::uint8_t> a_data)
		{
			if (!forms.Identity()) {
				auto result = FormTranslate::Translate(a_data, forms, FormTranslate::Way::kToLocal);
				NoteSkips(result, FormTranslate::Way::kToLocal, a_data);
				if (result.kind == FormTranslate::Result::Kind::kDropped) {
					return;
				}
				// A friend whose state named a place our game lacks (a DLC or mod): somewhere we cannot reach, not where
				// their last readable state was. A state of theirs we can read after this undoes it (RemotePlayers).
				for (const auto id : result.placeless) {
					if (id != localId) {
						RemotePlayers::NoteUnplaced(id);
					}
				}
				if (result.kind == FormTranslate::Result::Kind::kChanged) {
					HandlePacket(result.packet);
					return;
				}
			}
			HandlePacket(a_data);
		}

		void HandlePacket(std::span<const std::uint8_t> a_data)
		{
			using Protocol::MessageType;
			switch (Protocol::PeekType(a_data).value_or(MessageType{})) {
			case MessageType::kPluginTable:
				if (const auto msg = Protocol::DecodePluginTable(a_data)) {
					OnPluginTable(*msg);
				}
				break;
			case MessageType::kWelcome:
				if (welcomed) {
					break;  // one welcome per connection: a second would reset the world counters mid-session
				}
				if (const auto msg = Protocol::DecodeWelcome(a_data)) {
					if (SessionSteps::OnWelcome(binding, WorldSync::Loot(), hostedServer != nullptr, *msg) == CharacterBinding::Step::kLeave) {
						leaveAfterPoll = true;  // nothing more of this connection is taken (see Frame)
						break;
					}
					welcomed = true;
					sentEquipment.reset();
					sentFace.reset();
					localId = msg->playerId;
					NpcSync::SetLocalPlayer(localId);
					Party::SetLocalPlayer(localId);
					RadiantSync::SetLocalPlayer(localId);
					WorldSync::OnWelcome(msg->friendlyFire);
					Drops::OnWelcome(msg->sessionId, msg->playerId);
					Equipment::OnSessionStart();
					Story::SetShared(msg->sharedStory);
					RadiantSync::OnWelcome(msg->sessionId);  // after the story's sharing: the runs this world shared in the session
					HostRules::Set(msg->rules, msg->sessionId);
					Storage::OnWelcome(msg->rules.sharedStorage, hostedServer != nullptr, msg->sessionId);
					refusals.Clear();
					const bool tellMods = hostMods.OnWelcome();
					Notify(msg->sharedStory ? (msg->friendlyFire ? "session.connected_shared_fire" : "session.connected_shared") :
					                          (msg->friendlyFire ? "session.connected_own_fire" : "session.connected_own"));
					if (tellMods) {
						NotifyMods();  // the list that came before the welcome, after the line saying we are in
					}
					// A joiner is told the host's rules for being down, once a session (not again at each rejoin), and only
					// when they are not the defaults. The host set them.
					if (!hostedServer && msg->sessionId != rulesToldFor) {
						rulesToldFor = msg->sessionId;
						if (const auto summary = HostRules::Summary(msg->rules); !summary.empty()) {
							Hud::Notify(summary);
						}
						// With no revives a lethal hit kills, and a death loads the newest save, which can be minutes old.
						if (const auto warning = AutoSaveRules::DeathWarning({ .reviveOff = !msg->rules.revive }); !warning.empty()) {
							Hud::Notify(Text::Get(warning));
						}
					}
				}
				break;
			case MessageType::kLoadOrder:
				if (const auto msg = Protocol::DecodeLoadOrder(a_data)) {
					OnHostLoadOrder(*msg);
				}
				break;
			case MessageType::kReject:
				if (const auto msg = Protocol::DecodeReject(a_data)) {
					const auto answer = refusals.OnReject(msg->reason);
					// Refused for our load order: the list just before it names every plugin that differs, so the HUD says
					// that (one message) in place of the refusal's text.
					const bool modsDiffer = hostMods.OnRefused() && msg->reason.starts_with(Protocol::REJECT_LOAD_ORDER);
					if (answer.notify && modsDiffer) {
						REX::INFO("Session: refused: {}", Redact::OneLine(msg->reason));
						NotifyMods(true);
					} else if (answer.notify) {
						Say(Text::Format("session.notice", { { "text", Text::Reject(msg->reason) } }),
							Text::FormatEnglish("session.notice", { { "text", msg->reason } }));
					}
					nextConnectAttempt = Clock::now() + answer.retryIn;
					// Banned: trying again would be refused every time (and fill the host's log); the player joins again
					// from the menu once the host has unbanned them.
					if (msg->reason == Protocol::REJECT_BANNED) {
						removedAfterPoll = true;
					}
				}
				break;
			case MessageType::kRemoved:
				// The host kicked or banned us: said in our language, and we stop reconnecting (the connection closes next).
				if (const auto msg = Protocol::DecodeRemoved(a_data)) {
					Notify(msg->reason == Protocol::RemovedReason::kBanned ? "session.banned" : "session.kicked");
					removedAfterPoll = true;
				}
				break;
			case MessageType::kHostInfo:
				// Only our own server sends it, to the host's game.
				if (const auto msg = Protocol::DecodeHostInfo(a_data); msg && hostedServer) {
					hostInfo = *msg;
					++hostInfoSerial;
				}
				break;
			case MessageType::kPlayerJoined:
				if (const auto msg = Protocol::DecodePlayerJoined(a_data)) {
					// The server cleaned the name; cleaned again here, so a changed server's name can't reach the HUD's text
					// fields with markup or bytes that aren't UTF-8 in it (common/Name.h).
					const auto name = Name::Shown(msg->name, msg->playerId);
					RemotePlayers::Add(msg->playerId, name, msg->appearance);
					WorkshopSync::OnPlayerJoined();  // our pieces' power, for their copies of them
					REX::INFO("Session: player {} joined", msg->playerId);
					RadiantSync::OnPlayerJoined();  // our radiants' runs, for them
					Hud::Notify(Text::Format("session.player_joined", { { "player", name } }));
				}
				break;
			case MessageType::kRenamed:
				if (const auto msg = Protocol::DecodeRename(a_data)) {
					const auto before = RemotePlayers::NameOf(msg->playerId);
					const auto name = Name::Shown(msg->name, msg->playerId);
					if (!before.empty() && name != before) {
						RemotePlayers::Rename(msg->playerId, name);  // their name tag and the player list follow
						REX::INFO("Session: player {} renamed", msg->playerId);
						Hud::Notify(Text::Format("session.player_renamed", { { "player", before }, { "name", name } }));
					}
				}
				break;
			case MessageType::kPlayerLeft:
				if (const auto msg = Protocol::DecodePlayerLeft(a_data)) {
					const auto name = RemotePlayers::NameOf(msg->playerId);
					RemotePlayers::Remove(msg->playerId);
					Party::OnPlayerLeft(msg->playerId);
					Voice::OnPlayerLeft(msg->playerId);
					WorkshopSync::OnPlayerLeft(msg->playerId);  // their copies' power, shown no more
					MovedObjects::OnPlayerLeft(msg->playerId);
					REX::INFO("Session: player {} left", msg->playerId);
					Hud::Notify(name.empty() ? Text::Get("session.someone_left") : Text::Format("session.player_left", { { "player", name } }));
				}
				break;
			case MessageType::kPlayerReloaded:
				if (const auto msg = Protocol::DecodePlayerReloaded(a_data)) {
					RadiantSync::OnPlayerReloaded(msg->playerId);  // our radiants' runs, which their game knows no more
				}
				break;
			case MessageType::kExplosion:
				if (const auto msg = Protocol::DecodeExplosion(a_data)) {
					WeaponFire::ApplyExplosion(*msg);
				}
				break;
			case MessageType::kStorageOwners:
				if (const auto msg = Protocol::DecodeStorageOwners(a_data)) {
					Storage::OnOwners(*msg);
				}
				break;
			case MessageType::kStorageContents:
				if (const auto msg = Protocol::DecodeStorageListing(a_data)) {
					Storage::OnContents(*msg);
				}
				break;
			case MessageType::kStorageRefused:
				if (const auto msg = Protocol::DecodeStorageRefusal(a_data)) {
					Storage::OnRefused(*msg);
				}
				break;
			case MessageType::kStorageCaughtUp:
				if (const auto msg = Protocol::DecodeStorageCaughtUp(a_data)) {
					Storage::OnCaughtUp(*msg);  // after the world state it follows: our moves at a friend's storage settle
				}
				break;
			case MessageType::kDestructions:
				if (const auto msg = Protocol::DecodeDestructions(a_data)) {
					Destructibles::Apply(*msg);
				}
				break;
			case MessageType::kObjectHeld:
				if (const auto msg = Protocol::DecodeObjectHold(a_data)) {
					MovedObjects::OnHeld(*msg);
				}
				break;
			case MessageType::kObjectRested:
				if (const auto msg = Protocol::DecodeObjectRest(a_data)) {
					MovedObjects::OnRested(*msg);
				}
				break;
			case MessageType::kActivation:
				if (const auto msg = Protocol::DecodeActivation(a_data)) {
					WorldSync::ApplyRemoteActivation(msg->refId, msg->playerId);
				}
				break;
			case MessageType::kActorDied:
				if (const auto msg = Protocol::DecodeActorDeath(a_data)) {
					// Before the kill: the victim's name is still readable either way. A spawn's is our NPC under it.
					if (!Protocol::IsSpawnRef(msg->refId)) {
						Party::OnActorDied(*msg);
					} else if (const auto local = Spawns::LocalOf(msg->refId)) {
						auto named = *msg;
						named.refId = local;
						Party::OnActorDied(named);
					}
					WorldSync::ApplyRemoteDeath(*msg);
				}
				break;
			case MessageType::kActorHealth:
				if (const auto msg = Protocol::DecodeActorHealth(a_data)) {
					WorldSync::ApplyRemoteHealth(msg->refId, msg->health);
				}
				break;
			case MessageType::kActorDamaged:
				if (const auto msg = Protocol::DecodeActorHealth(a_data)) {
					WorldSync::ApplyRemoteDamage(msg->refId, msg->health);
				}
				break;
			case MessageType::kContainerChanged:
				if (const auto msg = Protocol::DecodeIndexedContainerChange(a_data)) {
					WorldSync::ApplyContainerChange(*msg);
				}
				break;
			case MessageType::kRefPickedUp:
				if (const auto msg = Protocol::DecodeRefPickedUp(a_data)) {
					WorldSync::ApplyRemotePickup(msg->refId);
				}
				break;
			case MessageType::kQuestStage:
				if (const auto msg = Protocol::DecodeQuestStage(a_data)) {
					if (!WorldSync::ApplyScriptDoor(*msg)) {  // a stage that opens a door (ScriptDoors), not one QuestSync shares
						QuestSync::Apply(*msg, true);
					}
				}
				break;
			case MessageType::kRadiantStarted:
				if (const auto msg = Protocol::DecodeRadiantStart(a_data)) {
					RadiantSync::OnStart(*msg);
				}
				break;
			case MessageType::kRadiantStage:
				if (const auto msg = Protocol::DecodeRadiantStage(a_data)) {
					RadiantSync::OnStage(*msg);
				}
				break;
			case MessageType::kActorOwners:
				if (const auto msg = Protocol::DecodeOwners(a_data)) {
					Spawns::OnOwners(*msg);  // first: NpcSync finds our spawns under the ids granted here
					NpcSync::ApplyOwners(*msg);
					WorldSync::OnActorOwners(*msg);
				}
				break;
			case MessageType::kSpawned:
				if (const auto msg = Protocol::DecodeSpawn(a_data)) {
					Spawns::Add(*msg);
				}
				break;
			case MessageType::kActorStatesRelay:
				if (const auto msg = Protocol::DecodeActorStates(a_data)) {
					NpcSync::ApplyStates(*msg);
				}
				break;
			case MessageType::kPlayerDamaged:
				if (const auto msg = Protocol::DecodePlayerHit(a_data)) {
					ApplyDamageToPlayer(*msg);
				}
				break;
			case MessageType::kPlayerStatus:
				if (const auto msg = Protocol::DecodePlayerStatus(a_data)) {
					Party::ApplyStatus(*msg);
				}
				break;
			case MessageType::kWorldTime:
				if (const auto msg = Protocol::DecodeWorldTime(a_data)) {
					WorldClock::Apply(*msg);
				}
				break;
			case MessageType::kPinged:
				if (const auto msg = Protocol::DecodePing(a_data)) {
					Party::ApplyPing(*msg);
				}
				break;
			case MessageType::kPartyXp:
				if (const auto msg = Protocol::DecodeXpGain(a_data)) {
					Party::ApplyXp(*msg);
				}
				break;
			case MessageType::kDropPlaced:
				if (const auto msg = Protocol::DecodeDroppedItem(a_data)) {
					Drops::OnPlaced(*msg);
				}
				break;
			case MessageType::kDropGone:
				if (const auto msg = Protocol::DecodeDropGone(a_data)) {
					Drops::OnGone(*msg);
				}
				break;
			case MessageType::kDropList:
				if (const auto msg = Protocol::DecodeDropList(a_data)) {
					Drops::OnList(*msg);
				}
				break;
			case MessageType::kMarkersFound:
				if (const auto msg = Protocol::DecodeMarkers(a_data)) {
					MapShare::Apply(*msg);
				}
				break;
			case MessageType::kLineSpoken:
				if (const auto msg = Protocol::DecodeLine(a_data)) {
					Conversations::Apply(*msg);
				}
				break;
			case MessageType::kNpcEquipment:
				if (const auto msg = Protocol::DecodeNpcEquipment(a_data)) {
					NpcSync::ApplyEquipment(*msg);
				}
				break;
			case MessageType::kNpcIdentity:
				if (const auto msg = Protocol::DecodeNpcIdentity(a_data)) {
					NpcSync::ApplyIdentity(*msg);
				}
				break;
			case MessageType::kPlayerFace:
				if (auto msg = Protocol::DecodeFace(a_data)) {
					RemotePlayers::SetFace(msg->playerId, std::move(*msg));
				}
				break;
			case MessageType::kWorkshopItem:
				if (const auto msg = Protocol::DecodeWorkshopItem(a_data)) {
					WorkshopSync::Apply(*msg);
				}
				break;
			case MessageType::kWorkshopItems:
				if (const auto msg = Protocol::DecodeWorkshopItems(a_data)) {
					for (const auto& item : *msg) {
						WorkshopSync::Apply(item);
					}
				}
				break;
			case MessageType::kWorkshopPower:
				if (const auto msg = Protocol::DecodePowerStates(a_data)) {
					WorkshopSync::ApplyPower(*msg);
				}
				break;
			case MessageType::kVoiceRelay:
				if (const auto msg = Protocol::DecodeVoice(a_data)) {
					Voice::Apply(*msg);
				}
				break;
			case MessageType::kRevived:
				if (const auto msg = Protocol::DecodeRevive(a_data)) {
					Downed::ApplyRevive(*msg);
				}
				break;
			case MessageType::kQuestDone:
				if (const auto msg = Protocol::DecodeQuestDone(a_data)) {
					QuestSync::ApplyDone(*msg);
				}
				break;
			case MessageType::kHeartbeatAck:
				if (const auto msg = Protocol::DecodeHeartbeat(a_data)) {
					Party::ApplyHeartbeatAck(*msg);
				}
				break;
			case MessageType::kShotFired:
				if (const auto msg = Protocol::DecodeShot(a_data)) {
					if (msg->refId == 0) {
						msg->action == Protocol::ShotAction::kShot ? RemotePlayers::PlayShot(msg->playerId, msg->sound) : RemotePlayers::PlayAction(msg->playerId, msg->action);
					} else {
						msg->action == Protocol::ShotAction::kShot ? NpcSync::ApplyShot(msg->refId, msg->sound) : NpcSync::ApplyAction(msg->refId, msg->action);
					}
				}
				break;
			case MessageType::kPlayerEquipment:
				if (auto msg = Protocol::DecodeEquipment(a_data)) {
					RemotePlayers::SetEquipment(msg->playerId, std::move(msg->items), std::move(msg->mods));
				}
				break;
			case MessageType::kRefStateChanged:
				if (const auto msg = Protocol::DecodeRefState(a_data)) {
					WorldSync::ApplyRemoteRefState(*msg);
				}
				break;
			case MessageType::kTraps:
				if (const auto msg = Protocol::DecodeTrapStates(a_data)) {
					Traps::Apply(*msg);
				}
				break;
			case MessageType::kWorldState:
				if (const auto msg = Protocol::DecodeWorldState(a_data)) {
					WorldSync::ApplyWorldState(*msg);
				}
				break;
			case MessageType::kPlayerStates:
				if (const auto msg = Protocol::DecodePlayerStates(a_data)) {
					for (const auto& entry : msg->players) {
						if (entry.playerId != localId) {
							RemotePlayers::PushState(entry.playerId, entry.state);
						}
					}
				}
				break;
			default:
				break;
			}
		}

		std::optional<Protocol::PlayerState> SampleLocalState(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return std::nullopt;
			}

			// Stamped with the time of the engine's last frame (FrameClock.h, EngineClock.h), which moved the player here,
			// rather than this moment of our work, which comes up to a frame off that pace in a game that skips frames
			// unevenly: so stamped, an even run went out as a stand and a leap (F4Multiplayer-ktmu).
			const auto  at = FrameClock::StampAt(a_now);
			const auto& pos = player->data.location;
			float       speed = 0.0f;
			if (lastPositionTime != Clock::time_point{}) {
				const float dt = std::chrono::duration<float>(at - lastPositionTime).count();
				if (dt > 0.0f) {
					const float dx = pos.x - lastPosition.x;
					const float dy = pos.y - lastPosition.y;
					speed = std::sqrt(dx * dx + dy * dy) / dt;
				}
			}
			lastPosition = pos;
			lastPositionTime = at;

			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.cell = cell->IsInterior() ? cell->GetFormID() : 0;
			state.worldspace = !cell->IsInterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
			state.x = pos.x;
			state.y = pos.y;
			state.z = pos.z;
			state.heading = player->data.angle.z;
			const auto process = player->currentProcess;
			state.speed = Gait::Reported(process && process->middleHigh ? process->middleHigh->animationSpeed : 0.0f, speed);
			state.moveMode = static_cast<std::uint16_t>(static_cast<const RE::ActorState&>(*player).moveMode);
			state.sentMs = Protocol::SentMs(at);
			// Where the player's own graph has them go, rather than guessed from their path by friends, and where they
			// look: their angle x, which their own game aims their arms by (AimPoint.h). The graph's Pitch only read
			// +-25.5 looking all the way up and down.
			{
				static const RE::BSFixedString directionVar{ "Direction" };
				const auto graph = static_cast<const RE::IAnimationGraphManagerHolder*>(player);
				float      direction = 0.0f;
				state.direction = graph->GetGraphVariableImplFloat(directionVar, direction) ? Protocol::PackDirection(direction) : std::uint8_t{ 0 };
				state.pitch = Protocol::PackPitch(AimPoint::PitchFromLooking(player->data.angle.x));
			}
			// A downed player's stand-in crouches.
			if (player->IsSneaking() || Downed::IsDown()) {
				state.flags |= Protocol::kSneaking;
			}
			if (player->GetWeaponMagicDrawn()) {
				state.flags |= Protocol::kWeaponDrawn;
			}
			using CharacterState = RE::IMovementState::CHARACTER_STATE;
			switch (static_cast<const RE::IMovementState&>(*player).DoGetCharacterState()) {
			case CharacterState::kJumping:
				state.flags |= Protocol::kInAir | Protocol::kJumping;
				break;
			case CharacterState::kInAir:
				state.flags |= Protocol::kInAir;
				break;
			case CharacterState::kSwimming:
				state.flags |= Protocol::kSwimming;
				break;
			default:
				break;
			}
			return state;
		}

		// A settler of the same sex as our character, unless the ini picks someone.
		std::uint32_t DefaultAppearance()
		{
			constexpr std::uint32_t FEMALE_SETTLER = 0x0020A578;
			constexpr std::uint32_t MALE_SETTLER = 0x0020A57B;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto npc = player ? player->GetNPC() : nullptr;
			return npc && npc->IsFemale() ? FEMALE_SETTLER : MALE_SETTLER;
		}

		// Echo test mode (see SetEcho).
		constexpr std::uint32_t ECHO_ID = 0xFFFFFFFF;
		bool                    echo = false;
		float                   echoOffsetX = 0.0f;
		float                   echoOffsetY = 0.0f;

		void OnDisconnected()
		{
			if (welcomed) {
				Notify("session.disconnected");
			}
			welcomed = false;
			hostMods.OnDisconnected();  // an ask out on this connection gets no answer
			hostInfo.reset();
			forms = {};                 // our own IDs again, until the next session's table
			tableThisConnection = false;
			formPluginsLogged.clear();
			localId = 0;
			Drops::Reset();  // our copies of what lies about go (the record stays: it is our world's)
			Traps::Reset();
			RadiantSync::SetLocalPlayer(0);
			Storage::OnDisconnected();  // before WorldSync forgets: the session's things leave a friend's copy we view
			RemotePlayers::RemoveAll();
			WorldSync::Reset();
			SessionSteps::OnDisconnected(WorldSync::Loot());
			Spawns::Reset();  // our copies of friends' spawns go (before NpcSync forgets their mirrors)
			NpcSync::Reset();
			WeaponFire::EndAllShields();
			WeaponFire::DropHandThrowables();
			Party::Reset();
			WorldClock::Reset();
			Voice::Reset();
			MapShare::Reset();
			WorkshopSync::Reset();
			Conversations::Reset();
			MovedObjects::Reset();
			Destructibles::Reset();
			// Nothing of the session reaches a solo game: no stage still waiting is set, and the story is our own.
			QuestSync::OnSessionEnded();
			Story::SetShared(false);
			HostRules::Clear();  // out of a session, nothing of the host's rules applies
			if (echo) {
				RemotePlayers::Add(ECHO_ID, "Echo", DefaultAppearance());
				sentEquipment.reset();
				sentFace.reset();
			}
		}

		ClientConnection& ConnectionFor(const std::string& a_address)
		{
			if (SteamTransport::ParseAddress(a_address)) {
				return steamClient;
			}
			return udpClient;
		}

		// Leaves the current session (if any) and joins another.
		void SwitchServer(std::string a_address)
		{
			client->Disconnect();
			client->Poll();
			OnDisconnected();
			QuestSync::OnServerChanged();  // what was kept for the last session isn't the next one's
			serverAddress = std::move(a_address);
			client = &ConnectionFor(serverAddress);
			nextConnectAttempt = {};
			refusals.Clear();
			SessionSteps::OnServerChanged(binding, WorldSync::Loot());
			hostMods.Reset();  // what we knew of the last host's mods
		}

		// A Steam invite or "Join Game" was accepted.
		void HandleSteamJoin()
		{
			// A friend's invite accepted (or Join Game) while we host: Steam turned it down, and only the log said so.
			if (Steam::TakeRefusedJoin()) {
				Notify("session.hosting_cant_join");
			}
			const auto target = Steam::TakeJoinTarget();
			if (!target) {
				return;
			}
			if (hostedServer) {
				Notify("session.hosting_cant_join");
				return;
			}
			// bHost waiting for our world: the hosting would take us back from the friend's game.
			if (Config::Get().host) {
				Notify("session.host_waits_for_world");
				return;
			}
			const auto address = SteamTransport::FormatAddress(*target);
			if (address != serverAddress) {
				Notify("session.joining_friend");
				SwitchServer(address);
			}
		}

		// Events from our world go to the server only while we're in a session, and not from the
		// opening (see Story::InOpening).
		void SendWorldEvents()
		{
			for (auto& packet : WorldSync::TakeOutgoing()) {
				if (welcomed && !Story::InOpening()) {
					SendOut(std::move(packet), true);
				}
			}
		}
	}

	void Initialize()
	{
		contentHash = ComputeContentHash();
		WorldSync::Install();
		WorkshopSync::Install();
		WeaponFire::Install();
		REX::INFO("Session: load order hash {:08X}", contentHash);

		const auto& settings = Config::Get();
		const bool  wantSteam = settings.transport == "steam" || settings.transport == "auto";
		steamMode = wantSteam && Steam::Available();
		if (settings.transport == "steam" && !steamMode) {
			Notify("session.no_steam");
		}
		REX::INFO("Session: transport '{}' -> {}", settings.transport, steamMode ? "steam" : "udp");
		if (wantSteam && !steamMode) {
			// Decided once, here: the line a "my invites and Join Game do nothing" report needs first.
			REX::WARN("Session: Steam is not available to the mod: {}. Steam invites, Join Game and Steam's relays are off this run{}", Steam::Unavailable(),
				settings.transport == "auto" ? " (only direct UDP, sTransport = enet, can work)" : "");
		}

		// With bHost, hosting starts once our own world is there (see Frame), and StartHosting points us at it.
		serverAddress = settings.host ? std::string{} : settings.serverAddress;
		client = &ConnectionFor(serverAddress);
	}

	std::string StartHosting()
	{
		if (hostedServer) {
			return {};
		}
		// The opening's world isn't the session's: nothing is shared from it (see Story::InOpening).
		if (Story::InOpening()) {
			// A new character with no name yet still has the last character's ID: hosting would open that one's session.
			const auto key = Story::Why() == Story::Opening::kUnnamed ? "host.err_opening_unnamed" : "host.err_opening";
			REX::INFO("Session: can't host: {}", Text::GetEnglish(key));
			return Text::Get(key);
		}
		const auto& settings = Config::Get();
		hostedServer = std::make_unique<Server>([](std::string_view a_line) {
			REX::INFO("{}", a_line);
		});
		Server::Options options;
		options.port = settings.port;
		options.maxPlayers = settings.maxPlayers;
		options.password = settings.password;
		// With Steam, friends never connect over UDP, so only our own game may (no firewall prompt).
		options.udpLoopbackOnly = steamMode;
		options.hostIdentity = Identity::Get();  // our Hello says this: the server knows our connection as the host's
		options.friendlyFire = settings.friendlyFire;
		options.sharedStory = settings.sharedStory;
		options.rules = HostRules::FromSettings(settings);
		options.ignoredPlugins = settings.ignoredPlugins;
		const auto character = CurrentCharacter();
		binding.OnHostingStarted(character);
		if (settings.saveSession) {
			if (const auto folder = Identity::DataFolder()) {
				// This character's own session (the old shared file is taken over if the loaded save played in it). Told
				// once per character, not at every start (a port that can't be had is tried every few seconds).
				const auto path = CharacterBinding::PrepareFile(*folder, character, settings.devChannel, WorldSync::Loot().Point().sessionId,
					[character](const std::string& a_line) {
						REX::INFO("Session: {}", a_line);
						if (legacyTold.insert(character).second) {
							Notify("session.notice", { { "text", a_line } });
						}
					});
				options.sessionFile = path.string();
				REX::INFO("Session: hosting character {:016X}; its session is kept in {}", character, path.filename().string());
			}
		}
		// The host's bans, beside the sessions: one list for every character this PC hosts (the rig's own, apart).
		if (const auto folder = Identity::DataFolder()) {
			options.banFile = (*folder / (settings.devChannel ? "F4Multiplayer.dev.bans" : "F4Multiplayer.bans")).string();
		}
		std::vector<std::unique_ptr<ServerTransport>> extra;
		if (steamMode) {
			extra.push_back(std::make_unique<SteamServerTransport>(settings.maxPlayers + 2));
		}
		if (!hostedServer->Start(options, std::move(extra))) {
			hostedServer.reset();
			REX::INFO("Session: can't host: {}", Text::FormatEnglish("host.err_port", { { "port", settings.port } }));
			return Text::Format("host.err_port", { { "port", settings.port } });
		}
		if (steamMode) {
			Steam::StartHosting(settings.maxPlayers);
		}
		// Connect to our own server, leaving whatever we were joining (started from the menu mid-game).
		// Steam's StartHosting has left any lobby we were in.
		const auto own = std::format("127.0.0.1:{}", settings.port);
		if (serverAddress != own) {
			SwitchServer(own);
		}
		return {};
	}

	void StopHosting()
	{
		if (!hostedServer) {
			return;
		}
		SwitchServer({});  // our own connection first, so the server's stop doesn't count as a drop
		hostedServer->Stop();
		hostedServer.reset();
		if (steamMode) {
			Steam::StopHosting();
		}
	}

	void Leave()
	{
		if (hostedServer) {
			return;  // the host leaves by stopping
		}
		Steam::Leave();
		SwitchServer({});
	}

	bool DropConnection()
	{
		if (hostedServer || client->GetStatus() != ClientConnection::Status::kConnected) {
			return false;
		}
		client->Disconnect();
		client->Poll();
		OnDisconnected();
		nextConnectAttempt = {};
		return true;
	}

	std::optional<Protocol::HostInfo> HostControls()
	{
		return hostedServer ? hostInfo : std::nullopt;
	}

	std::uint32_t HostControlsSerial()
	{
		return hostInfoSerial;
	}

	bool SendHostCommand(const Protocol::HostCommand& a_command)
	{
		if (!hostedServer || !welcomed) {
			return false;
		}
		SendOut(Protocol::Encode(a_command), true);
		return true;
	}

	void AskHostControls()
	{
		if (hostedServer && welcomed) {
			SendOut(Protocol::Encode(Protocol::HostInfoRequest{}), true);
		}
	}

	std::string DescribeHostControls()
	{
		if (!hostedServer) {
			return "hostctl: not hosting";
		}
		if (!hostInfo) {
			return std::format("hostctl: hosting, no state from the server yet (welcomed={})", welcomed);
		}
		std::string bans;
		for (const auto& ban : hostInfo->list) {
			bans += (bans.empty() ? "" : ",") + ban.alias;  // aliases only: the dev channel's reply may be pasted anywhere
		}
		return std::format("hostctl: locked={} password={} bans={} [{}] result={} action={} serial={}", hostInfo->locked, hostInfo->password, hostInfo->bans, bans,
			static_cast<int>(hostInfo->result), static_cast<int>(hostInfo->action), hostInfoSerial);
	}

	Status GetStatus()
	{
		Status status;
		switch (client->GetStatus()) {
		case ClientConnection::Status::kConnecting:
			status.link = Status::Link::kConnecting;
			break;
		case ClientConnection::Status::kConnected:
			status.link = welcomed ? Status::Link::kJoined : Status::Link::kConnected;
			break;
		default:
			// Disconnected with an address set: Frame retries it (every 5 s, or a minute after a reject).
			status.link = serverAddress.empty() ? Status::Link::kNone : Status::Link::kConnecting;
			break;
		}
		status.hosting = hostedServer && hostedServer->Running();
		status.steam = steamMode;
		status.address = serverAddress;
		status.rejectReason = refusals.Last();
		status.retrySoon = refusals.RetrySoon();
		status.hostWaiting = !hostedServer && Config::Get().host;
		return status;
	}

	void Frame()
	{
		if (shuttingDown) {
			return;
		}
		const auto now = Clock::now();
		// Each stretch below is a part of the dev channel's `perf`, the packets it sends included.
		Perf::Stretches perf;
		PERF_NEXT(perf, "Story");
		WeaponFire::Install();  // no-op once hooked; the player may not exist at startup
		// First: the rest of the session checks it. A new character with no name yet is held wherever it is, but on the dev
		// channel: the rig's coc character never gets a name, and its files are its own.
		Story::Frame(!Config::Get().devChannel && InGame() && binding.Unnamed(CurrentCharacter()));
		if (Story::InVaultStart()) {
			binding.OnVaultOpening();  // a Vault 111 start is held as it always was, and no longer (not a walk into the vault)
		}
		if (Story::Why() != Story::Opening::kUnnamed || Story::InVaultPlaces()) {
			unnamedSince.reset();  // nor in the vault's places: a vanilla start is unnamed there until the Vault-Tec form
		} else if (!unnamedSince) {
			unnamedSince = now;
		} else if (now - *unnamedSince >= UNNAMED_NOTICE_AFTER && (Config::Get().host || !serverAddress.empty()) && !std::exchange(unnamedTold, true)) {
			Notify("session.unnamed_waits");  // a start that never asks for a name: the session would wait for good
		}

		PERF_NEXT(perf, "Steam");
		if (steamMode) {
			Steam::Frame();
			HandleSteamJoin();
		}

		PERF_NEXT(perf, "Packets");
		// Events first: reconnecting must not happen before a disconnect has been handled.
		for (auto& event : client->Poll()) {
			if (leaveAfterPoll) {
				break;
			}
			switch (event.type) {
			case ClientConnection::Event::Type::kConnected:
				{
					const auto& settings = Config::Get();
					Protocol::Hello hello;
					hello.contentHash = contentHash;
					hello.name = PlayerName();
					sentName = hello.name;
					hello.password = settings.password;
					hello.appearance = settings.myAppearance ? settings.myAppearance : DefaultAppearance();
					hello.world = SessionSteps::OnHello(binding, WorldSync::Loot(), CurrentCharacter());
					hello.identity = Identity::Get();
					hello.plugins = plugins;
					SendOut(Protocol::Encode(hello), true);
					hostMods.OnConnected();
				}
				break;
			case ClientConnection::Event::Type::kDisconnected:
				OnDisconnected();
				break;
			case ClientConnection::Event::Type::kPacket:
				Receive(event.data);
				break;
			}
		}
		if (std::exchange(leaveAfterPoll, false)) {
			Leave();
			Notify("session.host_other_session");
		}
		if (std::exchange(removedAfterPoll, false)) {
			REX::INFO("Session: the host removed us or refused us for a ban; not reconnecting");
			Leave();
		}

		PERF_NEXT(perf, "Hosting");
		// Once a second, hosting follows the character that is loaded (CharacterBinding::Tracker::Check). Steam's lobby
		// and rich presence come with the server, so friends can't even try before our own game is in.
		if (now >= nextCharacterCheck) {
			nextCharacterCheck = now + CHARACTER_CHECK_INTERVAL;
			// Our name, once a second while in the session: a joiner who connected before naming a new character
			// in the opening said hello under the Steam name, and sends the character's once it is there. Only in
			// the world: at the main menu or between saves the character's name may be missing, and the others
			// would see us renamed to the Steam name and back.
			if (welcomed && InGame()) {
				if (auto name = PlayerName(); name != sentName) {
					sentName = std::move(name);
					REX::INFO("Session: our name changed; telling the others");
					SendOut(Protocol::Encode(Protocol::Rename{ 0, sentName }, Protocol::MessageType::kRename), true);
				}
			}
			const auto unnamed = binding.UnnamedId();
			const auto step = binding.Check(LinkNow(), { CurrentCharacter(), InGame(), Story::InOpening() });
			if (unnamed && !binding.UnnamedId()) {
				REX::INFO("Session: the new character is named: {:016X} -> {:016X} (in the opening: {})", *unnamed, CurrentCharacter(), Story::Why() == Story::Opening::kVault);
			}
			switch (step) {
			case CharacterBinding::Step::kStopHosting:
				StopHosting();
				Notify("session.character_changed_host");
				break;
			case CharacterBinding::Step::kStartHosting:
				if (now >= nextHostAttempt) {
					nextHostAttempt = now + RECONNECT_DELAY;
					auto error = StartHosting();
					if (!error.empty() && error != lastHostError) {
						Hud::Notify(Text::Format("session.notice", { { "text", error } }));  // StartHosting logged the English
					}
					lastHostError = std::move(error);
				}
				break;
			default:
				break;
			}
		}

		if (!serverAddress.empty() && client->GetStatus() == ClientConnection::Status::kDisconnected && now >= nextConnectAttempt && InGame()) {
			nextConnectAttempt = now + RECONNECT_DELAY;
			client->Connect(serverAddress, Config::Get().port);
		}

		PERF_NEXT(perf, "OurState");
		if ((welcomed || echo) && now >= nextSend) {
			// On schedule rather than "a full interval after this frame": at 60 fps that would
			// often wait an extra frame and send at 20 Hz.
			nextSend += SEND_INTERVAL;
			if (nextSend <= now) {
				nextSend = now + SEND_INTERVAL;  // fell behind (a hitch or the first send)
			}
			if (auto state = SampleLocalState(now)) {
				if (welcomed) {
					SendOut(Protocol::Encode(*state), false);
				}
				if (echo) {
					state->x += echoOffsetX;
					state->y += echoOffsetY;
					RemotePlayers::PushState(ECHO_ID, *state);
				}
			}
		}

		PERF_NEXT(perf, "Weapons");
		// Shots: ours and those of NPCs we run. Unreliable: a late shot is worse than a lost one. Each
		// carries the shooter's gun sound, which others' copies of the gun can't pick (no mods).
		const auto shots = WeaponFire::TakeShots();
		const auto npcShots = WeaponFire::TakeNpcShots();
		const auto sound = shots ? WeaponFire::ShotSound(RE::PlayerCharacter::GetSingleton()) : 0;
		if (welcomed) {
			for (std::uint32_t i = 0; i < shots; ++i) {
				SendOut(Protocol::Encode(Protocol::Shot{ 0, 0, Protocol::ShotAction::kShot, sound }, Protocol::MessageType::kReportShot), false);
			}
			// One our game made at runtime fires under the spawn id we share it as.
			for (const auto id : npcShots) {
				const auto wire = Protocol::IsSpawnRef(id) ? Spawns::OwnIdOf(id) : id;
				if (wire && NpcSync::RunsLocally(wire)) {
					const auto npcSound = WeaponFire::ShotSound(RE::TESForm::GetFormByID<RE::Actor>(id));
					SendOut(Protocol::Encode(Protocol::Shot{ 0, wire, Protocol::ShotAction::kShot, npcSound }, Protocol::MessageType::kReportShot), false);
				}
			}
		}
		if (echo) {
			for (std::uint32_t i = 0; i < shots; ++i) {
				RemotePlayers::PlayShot(ECHO_ID, sound);
			}
		}

		// The explosions our player and our NPCs set off: the others set them off at the same spot. Reliable, unlike shots:
		// the game that runs an NPC one reaches deals its blow there, and a lost report was a blow dealt nowhere.
		WeaponFire::Frame();
		for (const auto& explosion : WeaponFire::TakeExplosions()) {
			if (welcomed) {
				SendOut(Protocol::Encode(explosion, Protocol::MessageType::kReportExplosion), true);
			}
		}

		// Reloads and aiming (ours and our NPCs'). The Pip-Boy light travels in the status message.
		auto actions = WeaponFire::TakeActions();
		const auto npcActions = WeaponFire::TakeNpcActions();
		if (welcomed) {
			for (const auto action : actions) {
				SendOut(Protocol::Encode(Protocol::Shot{ 0, 0, action }, Protocol::MessageType::kReportShot), false);
			}
			for (const auto& [id, action] : npcActions) {
				const auto wire = Protocol::IsSpawnRef(id) ? Spawns::OwnIdOf(id) : id;
				if (wire && NpcSync::RunsLocally(wire)) {
					SendOut(Protocol::Encode(Protocol::Shot{ 0, wire, action }, Protocol::MessageType::kReportShot), false);
				}
			}
		}
		if (echo) {
			for (const auto action : actions) {
				RemotePlayers::PlayAction(ECHO_ID, action);
			}
		}

		PERF_NEXT(perf, "OurLook");
		if ((welcomed || echo) && now >= nextEquipmentCheck && InGame()) {
			nextEquipmentCheck = now + EQUIPMENT_INTERVAL;
			// Our look, once per session (and again after a plastic surgeon: it's compared each time).
			auto face = Face::Read(RE::PlayerCharacter::GetSingleton()->GetNPC());
			// With LooksMenu, its body sliders and overlays too: the face waits for the first read of them (a moment), so
			// that friends do not make our stand-in once without them and again with them.
			face.bodyMorphs = LooksMenu::PlayerMorphs();
			face.overlays = LooksMenu::PlayerOverlays();
			if (face.race != 0 && LooksMenu::Settled() && (!sentFace || *sentFace != face)) {
				if (welcomed) {
					SendOut(Protocol::Encode(face, Protocol::MessageType::kReportFace), true);
				}
				if (echo) {
					RemotePlayers::SetFace(ECHO_ID, face);
				}
				sentFace = std::move(face);
			}
			// The mods too: a barrel swapped at a workbench is a change, and their stand-in's copy is built with them.
			const auto player = RE::PlayerCharacter::GetSingleton();
			Protocol::Equipment equipment{ 0, Equipment::Read(player), Equipment::ReadMods(player) };
			if (!sentEquipment || *sentEquipment != equipment) {
				if (welcomed) {
					SendOut(Protocol::Encode(equipment, Protocol::MessageType::kEquipment), true);
				}
				if (echo) {
					RemotePlayers::SetEquipment(ECHO_ID, equipment.items, equipment.mods);
				}
				sentEquipment = std::move(equipment);
			}
		}

		PERF_NEXT(perf, "WorldSync");
		// Events from our world go to the server only while we're in a session, and not from the
		// opening (see Story::InOpening). Sent before WorldSync::Frame runs: a death it finds this frame
		// leaves next frame, after the takeover claim the same hit makes (NpcSync::OnLocalInteraction, sent
		// below this frame). The server believes our kill of an NPC another game runs only once it has that
		// claim (Server.cpp, handleDeath), so this order is what keeps a one-shot kill.
		SendWorldEvents();
		WorldSync::Frame(welcomed && hostedServer && hostedServer->Running());
		if (welcomed && !Story::InOpening()) {
			Storage::Frame();
		}

		PERF_NEXT(perf, "NpcSync");
		if (welcomed) {
			NpcSync::Frame(hostedServer && hostedServer->Running());
			Spawns::Frame();
		}
		PERF_NEXT(perf, "QuestSync");
		QuestSync::Frame(welcomed, hostedServer && hostedServer->Running());
		PERF_NEXT(perf, "Achievements");
		Achievements::Frame();
		PERF_NEXT(perf, "QuestSync");
		// Our quest reports wait while we join or rejoin a session (an address to connect to; a host's own
		// server is one), and playing alone they are dropped: a session joined later doesn't get them.
		for (auto& packet : QuestSync::TakeOutgoing(welcomed, welcomed || !serverAddress.empty())) {
			SendOut(std::move(packet), true);
		}
		PERF_NEXT(perf, "NpcSync");
		// After NpcSync::Frame and WorldSync::Frame: this frame's takeover claims leave ahead of the deaths
		// WorldSync found this frame (see above). Our runtime NPCs' spawns (and their release after a load) go ahead of
		// NpcSync's states and releases of them.
		for (auto& packet : Spawns::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		for (auto& packet : NpcSync::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet.data), packet.reliable);
			}
		}

		PERF_NEXT(perf, "PartyClock");
		Party::Frame();
		if (welcomed) {
			WorldClock::Frame();
		}
		for (auto& packet : Party::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		for (auto& packet : WorldClock::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), false);
			}
		}
		PERF_NEXT(perf, "MapWorkshop");
		if (welcomed) {
			MapShare::Frame();
			WorkshopSync::Frame(hostedServer && hostedServer->Running());
		}
		for (auto& packet : WorkshopSync::TakeOutgoing(welcomed)) {
			SendOut(std::move(packet), true);
		}
		for (auto& packet : MapShare::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		PERF_NEXT(perf, "Drops");
		if (welcomed && InGame()) {
			Drops::Frame();
		}
		for (auto& packet : Drops::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		// The world's traps (out of the opening: its world isn't the session's).
		if (welcomed && InGame() && !Story::InOpening()) {
			Traps::Frame();
		}
		for (auto& packet : Traps::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		PERF_NEXT(perf, "Conversations");
		Conversations::Frame(welcomed);
		for (auto& packet : Conversations::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		PERF_NEXT(perf, "MovedObjects");
		MovedObjects::Frame(welcomed && !Story::InOpening());
		for (auto& packet : MovedObjects::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet.data), packet.reliable);
			}
		}
		Destructibles::Frame(welcomed && !Story::InOpening());
		for (auto& packet : Destructibles::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), true);
			}
		}
		PERF_NEXT(perf, "Voice");
		Voice::Frame(welcomed);
		for (auto& packet : Voice::TakeOutgoing()) {
			if (welcomed) {
				SendOut(std::move(packet), false);
			}
		}

		PERF_NEXT(perf, "LooksMenu");
		if (InGame()) {
			LooksMenu::Frame();
		}
		PERF_NEXT(perf, "RemotePlayers");
		RemotePlayers::Update();
		PERF_NEXT(perf, "Compass");
		Compass::Frame();
		MenuIcons::Frame();
		if (InGame()) {
			FadeWatch::Frame();
		}
		PERF_NEXT(perf, "Puppets");
		Puppets::Tick();
	}

	void OnBeforeLoad()
	{
		binding.OnLoadStarting();
	}

	void OnBeforeSaveOrLoad()
	{
		// The player's last moves, and what the last frame left to send, go out now, while their world
		// stands: after a load they would be forgotten (WorldSync::Reset), and sent ahead of the load's
		// request they are in its answer, numbered before it.
		// Each step on its own: one that throws must not keep the rest from running, or a friend's stand-in or a copy
		// of an item lying in the session would be written into the save (the save goes on: nothing can stop it here).
		const auto step = [](std::string_view a_what, auto&& a_work) {
			try {
				a_work();
			} catch (const std::exception& e) {
				REX::ERROR("Session: before the save or load, {}: {}", a_what, e.what());
			} catch (...) {
				REX::ERROR("Session: before the save or load, {}: an unknown exception", a_what);
			}
		};
		step("the loot", [] { WorldSync::FlushLoot(); });
		step("the storage", [] { Storage::BeforeSaveOrLoad(); });  // no save holds the session's things in a friend's copy
		step("the world events", [] { SendWorldEvents(); });
		step("the drops", [] {
			Drops::DiscardCopies();  // no copy of an item lying in the session is saved (the graveyard DespawnAll empties)
			for (auto& packet : Drops::TakeOutgoing()) {
				if (welcomed) {
					SendOut(std::move(packet), true);  // a take given back: before the save, as the moves
				}
			}
		});
		step("the spawns", [] { Spawns::DiscardCopies(); });  // into the stand-ins' graveyard, which DespawnAll empties: no copy is saved
		step("the stand-ins", [] { RemotePlayers::DespawnAll(); });
		step("the mirrors", [] { NpcSync::ReleaseMirrors(); });
		step("the shields", [] { WeaponFire::EndAllShields(); });  // a ghost made against a friend's explosion is never saved
		step("the throwables", [] { WeaponFire::DropHandThrowables(); });
		step("going down", [] { Downed::OnBeforeSave(); });
		step("the moved objects", [] { MovedObjects::BeforeSaveOrLoad(); });
		step("the destructibles", [] { Destructibles::BeforeSaveOrLoad(); });
		// The session file is never older than our save. (A load asks too; it costs a write.)
		step("the session file", [] {
			if (hostedServer) {
				hostedServer->RequestSave();
			}
		});
	}

	void Shutdown()
	{
		if (std::exchange(shuttingDown, true)) {
			return;
		}
		// Only what must happen before the game is gone: the world is being torn down, so no stand-ins or HUD.
		client->Disconnect();
		const bool hosting = hostedServer != nullptr;
		if (hostedServer) {
			hostedServer->Stop();  // its thread writes the session file before it ends
			hostedServer.reset();
		}
		if (steamMode) {
			Steam::StopHosting();
		}
		REX::INFO("Session: game shutting down{}", hosting ? "; session saved" : "");
	}

	void OnGameLoaded(bool a_newGame)
	{
		// What we knew about the world described the game before the load.
		WorldSync::Reset();
		QuestSync::Rebaseline();
		Storage::OnGameLoaded();
		Achievements::OnGameLoaded();
		MapShare::Rebaseline();
		Downed::OnGameLoaded();
		Party::OnGameLoaded();
		RemotePlayers::OnGameLoaded();
		Spawns::OnGameLoaded();
		LooksMenu::OnGameLoaded();
		WorldClock::Rebaseline();
		MovedObjects::Reset();
		Destructibles::Reset();
		WorkshopSync::OnGameLoaded();
		Drops::OnGameLoaded();
		Traps::OnGameLoaded();
		WeaponFire::OnGameLoaded();  // a ghost of ours a save brought comes off, once in a session
		if (a_newGame) {
			unnamedTold = false;  // its own notice if it isn't named (Frame)
		}
		// Then whether the loaded world is the session's at all: another character's (or a new game's opening) isn't. The
		// host stops hosting (Frame starts it again with that character's session, out of the opening), and a friend
		// leaves. (After the above: leaving removes stand-ins and copies, and their handles from before the load are let
		// go by then.) Only then is the world bound to the session it is still in, and its changes asked for.
		SessionSteps::OnGameLoaded(
			binding, WorldSync::Loot(), LinkNow(), a_newGame, CurrentCharacter(),
			[a_newGame](CharacterBinding::Step a_step) {
				WorkshopSync::OnWorldGone();
				if (a_step == CharacterBinding::Step::kStopHosting) {
					StopHosting();
					Notify(a_newGame ? "session.hosting_resumes" : "session.character_changed_host");
				} else {
					Leave();
					Notify("session.character_changed_left");
				}
			},
			[](const Protocol::WorldRequest& a_request) {
				SendOut(Protocol::Encode(a_request), true);
				Drops::SendRecord();  // what the loaded world holds of the items given: the server makes the session agree
				REX::INFO("Session: save loaded; asking for the session's changes since #{}", a_request.containerFrom);
			});
	}

	std::string Describe()
	{
		const char* status = "disconnected";
		switch (client->GetStatus()) {
		case ClientConnection::Status::kConnecting:
			status = "connecting";
			break;
		case ClientConnection::Status::kConnected:
			status = welcomed ? "joined" : "connected";
			break;
		default:
			break;
		}
		return std::format("status={} id={} hosting={} server='{}' hash={:08X} reject='{}' world: {} players: {}",
			       status, localId, hostedServer && hostedServer->Running(), serverAddress, contentHash,
			       refusals.Last(), WorldSync::Describe() + " " + NpcSync::Describe() + " " + Spawns::Describe() + " " + QuestSync::Describe() + " " + Achievements::Describe() + " " + Story::Describe() + " " + WeaponFire::Describe() + " " + CopyWatch::Describe() + " " + Party::Describe() + " " + WorldClock::Describe() + " " + Voice::Describe() + " " + Drops::Describe() + " " + Traps::Describe() + " " + MapShare::Describe() + " " + WorkshopSync::Describe() + " " + MovedObjects::Status() + " " + Destructibles::Describe() + " " + Storage::Describe() + " " + Conversations::Describe() + " " + Compass::Describe() + " " + MenuIcons::Describe() + " " + FadeWatch::Describe() + " " + Hud::Describe() + " " + LooksMenu::Describe() + " " + Face::Describe() + " " + Equipment::DescribeOwnGear(), RemotePlayers::Describe()) +
		       std::format(" hits: taken={} ignored={} mult={} forms={} skippedOut={} skippedIn={}", hitsTaken, hitsIgnored, lastHitMult, forms.Identity() ? "same" : "translated", skippedOut.packets + skippedOut.entries + skippedOut.fields, skippedIn.packets + skippedIn.entries + skippedIn.fields) + " " + SaveGuard::Describe() + " " + AutoSave::Describe() + " frameHook=" + MainThread::HookState() + " " + (steamMode ? Steam::Describe() : "steam=off");
	}

	std::optional<ModDiff::Difference> HostMods()
	{
		return hostMods.Known();
	}

	AskResult AskHostMods()
	{
		if (!welcomed || hostedServer || client->GetStatus() != ClientConnection::Status::kConnected) {
			return AskResult::kNotJoined;
		}
		if (!hostMods.OnAsk(Clock::now())) {
			Notify("mods.ask_wait");  // the server would not answer yet (ModDiff::Tracker::ASK_INTERVAL)
			return AskResult::kTooSoon;
		}
		SendOut(Protocol::Encode(Protocol::LoadOrderRequest{}), true);
		return AskResult::kAsked;
	}

	std::string DescribeMods()
	{
		const auto& known = hostMods.Known();
		const bool  asking = hostMods.Asking(Clock::now());
		if (!known) {
			return std::format("mods: plugins={} nothing known of the host's (asking={})", plugins.size(), asking);
		}
		std::string out = std::format("mods: asking={}", asking);
		for (const auto& line : ModDiff::LogLines(*known)) {
			out += " | " + line;
		}
		return out;
	}

	std::string DescribeForms(std::optional<std::uint32_t> a_id)
	{
		const auto skips = [](const FormSkips& a_skips) { return std::format("{}/{}/{}", a_skips.packets, a_skips.entries, a_skips.fields); };
		std::string out = std::format("forms: {} table={} ours={} oursOnly={} tableOnly={} skipped(messages/entries/details) out={} in={}",
			forms.Identity() ? (tableThisConnection ? "same" : "none") : "translated", forms.Session().Size(), FormIds::Table::Of(plugins).Size(), forms.OursOnly(), forms.SessionsOnly(),
			skips(skippedOut), skips(skippedIn));
		if (a_id) {
			const auto name = [](const std::string& a_plugin) { return a_plugin.empty() ? std::string{ "?" } : a_plugin; };
			const auto hex = [](std::optional<std::uint32_t> a_v) { return a_v ? std::format("{:08X}", *a_v) : std::string{ "none" }; };
			const auto toSession = forms.ToSession(*a_id);
			const auto toLocal = forms.ToLocal(*a_id);
			out += std::format(" | ours {:08X} ({}) is the session's {} | the session's {:08X} ({}) is our {}", *a_id, name(forms.LocalPlugin(*a_id)), hex(toSession), *a_id,
				name(forms.SessionPlugin(*a_id)), hex(toLocal));
		}
		return out;
	}

	void ConnectTo(std::string a_address)
	{
		// (As for a Steam join: with bHost, our hosting starts once we're in the world, and would take us back.)
		if (!hostedServer && Config::Get().host) {
			Notify("session.host_waits_for_world");
			return;
		}
		SwitchServer(std::move(a_address));
	}

	void SetEcho(bool a_enabled, float a_offsetX, float a_offsetY)
	{
		echo = a_enabled;
		echoOffsetX = a_offsetX;
		echoOffsetY = a_offsetY;
		if (a_enabled) {
			RemotePlayers::Add(ECHO_ID, "Echo", DefaultAppearance());
			sentEquipment.reset();
			sentFace.reset();
		} else {
			RemotePlayers::Remove(ECHO_ID);
		}
	}

	void InjectPacket(std::span<const std::uint8_t> a_data)
	{
		HandlePacket(a_data);
	}
}
