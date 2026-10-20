#pragma once

// A session played end to end without the game: a Session runs a real Server (ServerFixture's, on a
// FakeTransport) and plays several players. Each Client sends what its game would (a real Hello and
// real reports, encoded by Protocol), decodes everything the server sends back, and keeps what its
// game would now show: who is there and where, who runs which NPC, what is dead, what containers hold,
// what was built at settlements, which quest stages are reached. Where the plugin's own rule for that
// is free of the game, the model uses it rather than a copy, so a scenario tests the server and the
// client side together: LootSync::Loot for loot, WorkshopCells for what waits at a settlement, SendPacer
// for how fast a player's settlement reports go, FoundDead::Shared for the host's save,
// ArmorFrames::ChooseOriginal for a friend's power armor,
// MoveSync::Drive every frame for a friend's stand-in, and StandInGear for what it wears (dressed every
// frame as RemotePlayers::Update does, out of power armor). For the quests a test gives a game
// (GiveQuest), QuestSync's StageLog and StageRules set friends' stages and decide what is reported
// (tests/sim/QuestGame.h). The rest is the model's own, written after the plugin and only as good as
// that copy: applying doors, the stages of quests no test gave (only recorded) and pickups, dropping older
// positions (RemotePlayers::PushState), keying copies of builds (WorkshopSync::Apply), the facts
// ArmorFrames is given (found, ours, in use, distance; judged once a wear), and the stand-in's animation
// graph that MoveSync drives (StandIn).
//
// Players leave and come back with the same identity, crash, reload a save, and can lose, repeat or
// reorder packets; the server can stop and start again from its session file. Everything random comes
// from the session's seed, and the session is settled (the server has acted on everything sent, and
// every player has read what came back) whenever the test says so.

#include "../ChestWorld.h"
#include "../ServerFixture.h"
#include "QuestGame.h"

#include "game/ArmorFrames.h"
#include "game/FoundDead.h"
#include "game/MoveSync.h"
#include "game/SendPacer.h"
#include "game/StandInGear.h"
#include "game/WorkshopCells.h"

#include <cmath>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Sim
{
	using MT = Protocol::MessageType;
	using Packet = std::vector<std::uint8_t>;
	using WorkshopKey = std::pair<std::uint32_t, std::uint32_t>;  // (who built it, their reference ID)
	using Clock = std::chrono::steady_clock;

	// The host's own game: the identity the server is started for (Server::Options::hostIdentity).
	inline constexpr std::uint64_t HOST_IDENTITY = 0x4057'0000'0000'0001;
	inline constexpr std::uint32_t COMMONWEALTH = 0x3C;

	class Session;

	// What goes wrong with one player's packets, both ways. ENet and Steam deliver what is sent reliably
	// exactly once and in order, so by default only what goes unreliably (positions, NPC movement,
	// shots, voice) is touched; reliableToo models a transport (or a game) that breaks that promise.
	struct Faults
	{
		double drop = 0.0;       // lost
		double duplicate = 0.0;  // arrives twice
		double reorder = 0.0;    // held back, arrives after the next one
		bool   reliableToo = false;
	};

	// A power armor frame a plugin placed in the world: every game has it, at the same spot.
	struct PlacedFrame
	{
		std::uint32_t base = 0;
		std::uint32_t worldspace = COMMONWEALTH;
		float         x = 0.0f, y = 0.0f, z = 0.0f;
	};

	// A friend's stand-in's animation graph, as the game has it: walking or standing, and taking only the
	// event that changes that. While busy (mid-jump, mid-landing, mid-stagger) it refuses every event, and
	// once done it comes out walking or standing as the game decides (iSyncIdleLocomotion), not as it was
	// last told.
	struct StandIn
	{
		bool walking = false;
		int  busy = 0;  // frames left
		bool landsWalking = false;
	};

	// Another player, as this player's game shows them.
	struct Friend
	{
		std::string                           name;
		std::uint32_t                         appearance = 0;
		std::optional<Protocol::PlayerState>  state;  // the newest that came
		std::optional<Protocol::PlayerStatus> status;
		std::vector<std::uint32_t>            equipment;
		MoveSync::Step                        graph;    // what MoveSync last told their stand-in's animation graph
		StandIn                               standIn;  // what that graph does
		// Their power armor wear, as ArmorFrames judged our copy of a frame every game has: decided once a
		// wear, as soon as their status and a state are both in.
		std::optional<ArmorFrames::Original> wear;
		// Their gear as StandInGear keeps it, and what their stand-in wears: none while it is still in the
		// settler's own outfit (nothing of theirs was put on it yet).
		StandInGear::Gear                         gear;
		std::optional<std::vector<std::uint32_t>> wearing;
	};

	// The world as this player's game has it, all of which a save keeps. Container contents count from
	// what the game itself put there (the same in every game), so 0 is untouched.
	struct World
	{
		std::set<std::uint32_t>                                        dead;
		std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>> containers;
		std::set<std::uint32_t>                                        pickedUp;
		std::map<std::uint32_t, std::pair<std::uint8_t, std::uint8_t>> doors;  // open, locked (RefState values)
		std::set<std::pair<std::uint32_t, std::uint16_t>>              questStages;
		std::map<std::uint32_t, Protocol::WorkshopItem>                built;     // by this player, by their reference ID
		std::set<std::uint32_t>                                        scrapped;  // pieces of the world itself, gone

		// Container contents with the untouched entries left out, for comparing.
		[[nodiscard]] std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>> Contents() const
		{
			std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>> out;
			for (const auto& [container, items] : containers) {
				for (const auto& [item, count] : items) {
					if (count != 0) {
						out[container][item] = count;
					}
				}
			}
			return out;
		}
	};

	// A save: the world, where it stood in the session's container changes (WorldSync's save record), and
	// the copies of the others' builds it holds (references in the save like any other).
	struct Save
	{
		World                                                       world;
		Protocol::WorldRequest                                      resync;
		WorkshopCells::Waiting<WorkshopKey, Protocol::WorkshopItem> copies;
		std::set<WorkshopKey>                                       copyKeys;
		QuestSave                                                   quests;  // the quests the test gave the game (QuestGame)
	};

	// Draws from the session's seed.
	class Dice
	{
	public:
		explicit Dice(std::uint32_t a_seed) :
			engine(a_seed)
		{}

		bool Chance(double a_p) { return a_p > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(engine) < a_p; }

		template <class T>
		T Between(T a_low, T a_high)
		{
			if constexpr (std::is_floating_point_v<T>) {
				return std::uniform_real_distribution<T>(a_low, a_high)(engine);
			} else {
				return std::uniform_int_distribution<T>(a_low, a_high)(engine);
			}
		}

		template <class C>
		const auto& Pick(const C& a_choices)
		{
			auto it = a_choices.begin();
			std::advance(it, Between<std::size_t>(0, a_choices.size() - 1));
			return *it;
		}

	private:
		std::mt19937 engine;
	};

	// Packets, in the order sent, through their player's faults: lost, repeated, or held back until that
	// player's next packet has gone. a_faultsOf(item) names the player's Faults; a_touchable(item) says
	// whether faults apply to it at all.
	template <class Item, class FaultsOf, class Touchable>
	std::vector<Item> ThroughFaults(std::vector<Item> a_items, Dice& a_dice, FaultsOf&& a_faultsOf, Touchable&& a_touchable)
	{
		std::vector<Item>             out;
		std::map<const Faults*, Item> held;  // one per player at most
		for (auto& item : a_items) {
			const Faults& faults = a_faultsOf(item);
			const auto    release = [&] {
				if (const auto it = held.find(&faults); it != held.end()) {
					out.push_back(std::move(it->second));
					held.erase(it);
				}
			};
			if (!a_touchable(item)) {
				out.push_back(std::move(item));
				release();
				continue;
			}
			if (a_dice.Chance(faults.drop)) {
				continue;
			}
			if (a_dice.Chance(faults.duplicate)) {
				out.push_back(item);
			}
			if (!held.contains(&faults) && a_dice.Chance(faults.reorder)) {
				held.emplace(&faults, std::move(item));
				continue;
			}
			out.push_back(std::move(item));
			release();
		}
		for (auto& [faults, item] : held) {
			out.push_back(std::move(item));
		}
		return out;
	}

	class Client
	{
	public:
		Client(Session& a_session, std::string a_name, std::uint64_t a_identity, bool a_host) :
			session(a_session), name(std::move(a_name)), identity(a_identity), host(a_host)
		{}

		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		// ---- coming and going ----

		// Connects and says hello, with where its world stands in the session (as the plugin does), and
		// settles. True once welcomed.
		bool Join();
		// Leaves: the connection closes and the server hears of it.
		void Leave();
		// The game crashes: our side of the connection is gone, but the server holds it until it times
		// out (or we come back with the same identity first).
		void Vanish();
		// The connection drops (a Steam relay hiccup, a timeout in a long loading screen) while the game
		// plays on and keeps trying the same address (Join comes back). The server holds it as for Vanish.
		void LoseConnection() { Vanish(); }
		// Accepts a friend's invite to the session the server runs now (Session::SwitchServer): a new
		// address, so nothing kept for the last one goes there. True once welcomed.
		bool SwitchServer();
		// The hash of this game's load order, sent in its Hello (the same for everyone by default).
		void LoadOrder(std::uint32_t a_hash) { contentHash = a_hash; }
		// Says hello again on the same connection (a game restarted before Steam dropped the connection).
		void HelloAgain();

		// ---- what the player does (each reported as the game reports it) ----

		void Move(float a_x, float a_y, float a_z = 0.0f, float a_speed = 0.0f, std::uint32_t a_worldspace = COMMONWEALTH);
		// Keeps sending where it stands, as the game does 30 times a second (a few of these, after the
		// last move, outlast a lost packet).
		void StandStill(int a_states = 3);
		void Kill(std::uint32_t a_ref, float a_x = 0.0f, float a_y = 0.0f, float a_z = 0.0f);
		// An NPC died in this game, reported as WorldSync does: a_killed when it was our player's kill (theirs, or our
		// companion's: CopyHealth::Credited), not when something else of ours or nothing killed it.
		void Died(std::uint32_t a_ref, bool a_killed);
		void Hurt(std::uint32_t a_ref, float a_health);
		// What our player took off our copy of an NPC another game runs (a fraction of its maximum), as WorldSync sends it.
		void Damage(std::uint32_t a_ref, float a_damage) { Send(Protocol::Encode(Protocol::ActorHealth{ a_ref, a_damage }, MT::kReportDamage), true); }
		void Loot(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count);
		void PickUp(std::uint32_t a_ref);
		void Door(std::uint32_t a_ref, std::uint8_t a_open, std::uint8_t a_locked);
		// Reaches a quest stage. A quest given to this game (GiveQuest) is set in its QuestGame, which reports
		// it at its next poll as QuestSync does; any other is reported at once, and only recorded.
		void Stage(std::uint32_t a_quest, std::uint16_t a_stage);
		// This game has the quest, in this state: its stages from friends go through QuestSync's rules.
		void GiveQuest(std::uint32_t a_id, Quest a_quest) { questGame.Give(a_id, std::move(a_quest)); }
		QuestGame&       Quests() { return questGame; }
		[[nodiscard]] const QuestGame& Quests() const { return questGame; }
		// Builds a_base at the settlement of workbench a_bench; returns the new object's reference ID in
		// this player's game (a runtime ID, as the game gives).
		std::uint32_t Build(std::uint32_t a_bench, std::uint32_t a_base, float a_x, float a_y, float a_z = 0.0f);
		void MoveBuilt(std::uint32_t a_ref, float a_x, float a_y, float a_z = 0.0f);
		// Scraps something this player built, or a piece of the world itself (a shareable reference).
		void Scrap(std::uint32_t a_bench, std::uint32_t a_ref);
		void Claim(std::uint32_t a_ref, Protocol::ClaimReason a_reason = Protocol::ClaimReason::kUnowned);
		void Release(std::uint32_t a_ref);
		// Moves an NPC this player's game runs (the others copy it).
		void RunNpc(std::uint32_t a_ref, float a_x, float a_y, float a_z = 0.0f);
		void NpcGear(std::uint32_t a_ref, std::vector<std::uint32_t> a_items);
		void Equip(std::vector<std::uint32_t> a_items);
		void Status(Protocol::PlayerStatus a_status);
		// Gets into (or, with 0, out of) a frame every game has placed.
		void WearFrame(std::uint32_t a_frameRef);
		void Revive(std::uint32_t a_playerId);
		void HitPlayer(std::uint32_t a_playerId, float a_damage, std::uint32_t a_attacker);
		// Whether what it builds, moves and scraps waits in line and goes a few hundred a second, as WorkshopSync's
		// reports do (SendPacer, the plugin's own); off, all of it goes at once, as it did before.
		void PaceWorkshop(bool a_paced) { pacedWorkshop = a_paced; }
		// Its settlement reports still waiting in that line.
		[[nodiscard]] std::size_t WorkshopUnsent() const { return workshopOut.Size(); }
		// Anything at all, as a broken or hostile game might send it.
		void Raw(Packet a_packet, bool a_reliable = true) { Send(std::move(a_packet), a_reliable); }

		// Frames go by in this player's game (30 a second): each friend's stand-in is driven by MoveSync from
		// the newest state of theirs, as Puppets does every frame. Settle runs a few.
		void Frames(int a_count);
		// A friend's stand-in in this game is caught mid-jump (or mid-stagger) for a_frames frames, and comes
		// out of it walking or standing.
		void Stumble(std::uint32_t a_friend, int a_frames, bool a_landsWalking)
		{
			if (const auto it = friends.find(a_friend); it != friends.end()) {
				it->second.standIn.busy = a_frames;
				it->second.standIn.landsWalking = a_landsWalking;
			}
		}

		// The host's game: the NPCs others run that are dead in its own world (its save had them dead) are
		// reported found dead, as WorldSync's CheckFoundDead does, story characters as FoundDead::Shared says.
		void CheckFoundDead();
		// Facts about an NPC of this player's game, for FoundDead.
		void Character(std::uint32_t a_ref, FoundDead::Npc a_npc) { characters[a_ref] = a_npc; }
		// The save this game was loaded from had these dead before the session.
		void DeadInSave(std::uint32_t a_ref) { world.dead.insert(a_ref); }

		Save SaveGame() const { return { world, loot.Point(), copies, copyKeys, questGame.Save() }; }
		// Loads a save: the world goes back to it, and a game in a session asks for what changed since
		// (Session::OnGameLoaded).
		void LoadGame(const Save& a_save);

		// ---- what the player's game shows ----

		[[nodiscard]] const std::string& Name() const { return name; }
		[[nodiscard]] std::uint64_t      Identity() const { return identity; }
		[[nodiscard]] bool               Welcomed() const { return welcomed; }
		[[nodiscard]] std::uint32_t      Id() const { return playerId; }
		[[nodiscard]] std::uint64_t      SessionId() const { return sessionId; }
		[[nodiscard]] const World&       WorldNow() const { return world; }
		[[nodiscard]] const std::map<std::uint32_t, Friend>& Friends() const { return friends; }
		[[nodiscard]] const Friend*      FriendById(std::uint32_t a_id) const
		{
			const auto it = friends.find(a_id);
			return it != friends.end() ? &it->second : nullptr;
		}
		[[nodiscard]] const std::map<std::uint32_t, std::uint32_t>& Owners() const { return owners; }
		[[nodiscard]] std::uint32_t      OwnerOf(std::uint32_t a_ref) const
		{
			const auto it = owners.find(a_ref);
			return it != owners.end() ? it->second : 0;
		}
		// The health another game last reported for an NPC (a fraction), if any came.
		// The damage friends took off NPCs, as the session passed it on since the last call.
		std::vector<Protocol::ActorHealth> TakeDamageHeard() { return std::exchange(damageHeard, {}); }
		[[nodiscard]] std::optional<float> HealthHeard(std::uint32_t a_ref) const
		{
			const auto it = npcHealth.find(a_ref);
			return it != npcHealth.end() ? std::optional{ it->second } : std::nullopt;
		}
		// The player whose kill an NPC's death was, as the session told us (0: nobody's kill); nothing if no death came.
		[[nodiscard]] std::optional<std::uint32_t> KilledBy(std::uint32_t a_ref) const
		{
			const auto it = killedBy.find(a_ref);
			return it != killedBy.end() ? std::optional{ it->second } : std::nullopt;
		}
		[[nodiscard]] const std::map<std::uint32_t, Protocol::ActorState>&        NpcStates() const { return npcStates; }
		[[nodiscard]] const std::map<std::uint32_t, std::vector<std::uint32_t>>& NpcGearSeen() const { return npcGear; }
		[[nodiscard]] std::int64_t       Count(std::uint32_t a_container, std::uint32_t a_item) const
		{
			const auto c = world.containers.find(a_container);
			if (c == world.containers.end()) {
				return 0;
			}
			const auto i = c->second.find(a_item);
			return i != c->second.end() ? i->second : 0;
		}
		// Everything built at settlements as this game shows it: its own, and copies of the others'.
		[[nodiscard]] std::map<WorkshopKey, Protocol::WorkshopItem> Settlements() const;
		// Only the copies of builds the server sent (WorkshopSync's copies and pending), whoever it named.
		[[nodiscard]] const std::set<WorkshopKey>& Copies() const { return copyKeys; }
		// What stands at one settlement in this game: what waits for its cell (WorkshopCells), and its own.
		[[nodiscard]] std::set<WorkshopKey> At(std::uint32_t a_bench) const;
		[[nodiscard]] bool               Hides(std::uint32_t a_frameRef) const { return hiddenFrames.contains(a_frameRef); }
		[[nodiscard]] const std::vector<std::string>& Rejections() const { return rejections; }
		[[nodiscard]] const std::vector<Protocol::PlayerHit>& Damage() const { return damage; }
		[[nodiscard]] const std::vector<std::uint32_t>& RevivedBy() const { return revivedBy; }
		// Friends' explosions the server passed on, in order (a test sets them off in its own model of this game).
		[[nodiscard]] const std::vector<Protocol::Explosion>& Explosions() const { return explosions; }
		[[nodiscard]] const std::optional<Protocol::WorldTime>& Time() const { return time; }
		// How many packets of a kind arrived since joining (any connection), and how many didn't decode.
		[[nodiscard]] std::size_t        Heard(MT a_type) const
		{
			const auto it = heard.find(a_type);
			return it != heard.end() ? it->second : 0;
		}
		[[nodiscard]] std::size_t        Undecodable() const { return undecodable; }
		[[nodiscard]] bool               Connected() const { return connected; }

		// Faults on this player's packets, both ways.
		Faults faults;

	private:
		friend class Session;

		void Send(Packet a_packet, bool a_reliable);
		void SendWorkshop(Packet a_packet);
		void FlushWorkshop(Clock::time_point a_now);
		void Pump();  // reads what the server sent
		void Handle(std::span<const std::uint8_t> a_data);
		void OnDisconnected();
		void ApplyWorldState(const Protocol::WorldState& a_state);
		void ApplyContainer(std::uint32_t a_index, std::uint32_t a_playerId, const Protocol::ContainerChange& a_change);
		// Puts another player's container change into our world (every container is loaded here).
		bool Put(const Protocol::ContainerChange& a_change)
		{
			world.containers[a_change.container][a_change.item] += a_change.count;
			return true;
		}
		void ApplyWorkshop(const Protocol::WorkshopItem& a_item);
		void ApplyDoor(const Protocol::RefState& a_state);
		void JudgeWear(std::uint32_t a_friend);
		// One QuestSync poll (half a second of play), and what it reports.
		void PollQuests();
		Packet Hello();

		Session&      session;
		std::string   name;
		std::uint64_t identity;
		bool          host;
		std::uint32_t contentHash = 0x1234;

		PeerId        peer = 0;
		bool          connected = false;
		bool          welcomed = false;
		bool          wantSession = false;  // joined, and not left: a lost connection is tried again
		std::uint32_t playerId = 0;
		std::uint64_t sessionId = 0;
		bool          sharedStory = false;

		World                                         world;
		QuestGame                                     questGame;
		LootSync::Loot                                loot;
		TestServer::ChestWorld                        chests{ [](std::uint32_t) { return true; }, [this](const Protocol::ContainerChange& a_change) { Put(a_change); } };  // every container is loaded here
		std::set<std::uint32_t>                       heardDead;  // dead as the session told us, or as we found and said (WorldSync's dead)
		std::set<std::uint32_t>                       toCheck;    // NPCs others run: the host checks its own world for them
		std::map<std::uint32_t, FoundDead::Npc>       characters;
		std::map<std::uint32_t, Friend>               friends;
		std::map<std::uint32_t, std::uint32_t>        owners;
		std::map<std::uint32_t, Protocol::ActorState> npcStates;
		std::map<std::uint32_t, std::vector<std::uint32_t>> npcGear;
		std::map<std::uint32_t, float>                npcHealth;
		std::vector<Protocol::ActorHealth>            damageHeard;
		std::map<std::uint32_t, std::uint32_t>        killedBy;  // deaths the session told us of: whose kill (0: nobody's)
		WorkshopCells::Waiting<WorkshopKey, Protocol::WorkshopItem> copies;  // the others' builds, waiting for their settlement's cell
		std::set<WorkshopKey>                         copyKeys;
		std::map<std::uint32_t, std::uint32_t>        hiddenFrames;  // our copy of a placed frame -> the friend wearing theirs
		std::map<std::uint32_t, std::uint8_t>         markers;
		std::optional<Protocol::WorldTime>            time;
		std::vector<std::string>                      rejections;
		std::vector<Protocol::PlayerHit>              damage;
		std::vector<std::uint32_t>                    revivedBy;
		std::vector<Protocol::Explosion>              explosions;
		std::map<MT, std::size_t>                     heard;
		std::size_t                                   undecodable = 0;

		std::uint32_t               sequence = 0;
		Protocol::PlayerState       me;
		Protocol::PlayerStatus      myStatus;
		std::optional<std::vector<std::uint32_t>> myGear;  // what this player wears, once they put anything on or off
		std::uint32_t               nextRef = 0;
		SendPacer::Queue<Packet>    workshopOut{ SendPacer::WORKSHOP_PER_SECOND, SendPacer::WORKSHOP_QUEUED };  // WorkshopSync's outgoing line
		bool                        pacedWorkshop = true;
		Clock::time_point           frameClock{};  // this game's frames, 30 a second
	};

	class Session
	{
	public:
		// a_options: the server's (a session file is added; the host's identity too, unless given).
		explicit Session(Server::Options a_options = {}, std::uint32_t a_seed = 1) :
			options(std::move(a_options)), dice(a_seed), seed(a_seed)
		{
			if (options.sessionFile.empty()) {
				options.sessionFile = (std::filesystem::temp_directory_path() / std::format("f4mp-sim-{}-{}.session", a_seed, std::random_device{}())).string();
				ownFile = true;
			}
			if (options.hostIdentity == 0) {
				options.hostIdentity = HOST_IDENTITY;
			}
			// A sim plays an evening in moments, so a player comes back far more often a second than a real
			// one can. Unless a test sets it, the world-state budget refills at once: a rejoin isn't held back
			// for the ten seconds a real game would wait (the server's budget tests try that).
			if (options.worldStateRefill == Server::Options{}.worldStateRefill) {
				options.worldStateRefill = std::chrono::milliseconds{ 0 };
			}
			std::filesystem::remove(options.sessionFile);
			StartServer();
		}

		~Session()
		{
			fixture.reset();
			if (ownFile) {
				std::error_code ignored;
				for (const auto* suffix : { "", ".tmp", ".bak", ".corrupt" }) {
					std::filesystem::remove(options.sessionFile + suffix, ignored);
				}
			}
		}

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		// The host's own game (on the server's machine, with the identity the server was started for).
		Client& Host(std::string a_name = "Host") { return Add(std::move(a_name), options.hostIdentity, true); }
		// A friend's game, with an identity of its own.
		Client& Player(std::string a_name)
		{
			const auto id = 0x5100'0000'0000'0000ull | (static_cast<std::uint64_t>(seed) << 16) | (clients.size() + 1);
			return Add(std::move(a_name), id, false);
		}

		// Delivers what the players sent (through their faults), waits until the server has acted on it
		// all (and, if anyone moved, relayed the positions), and has every player read what came back. A
		// player's settlement reports still in line (SendPacer) are waited for: they go as time lets them.
		void Settle()
		{
			DeliverAll();
			Settled();
		}

		// a_wait goes by, in which each player's settlement reports go as far as their line lets them (the
		// rest still waits), and the session is settled with what was sent.
		void Pass(std::chrono::milliseconds a_wait)
		{
			Deliver();
			std::this_thread::sleep_for(a_wait);
			Settled();
		}

		// The server stops (saving its session file) and everyone connected loses the connection.
		void StopServer()
		{
			DeliverAll();
			if (fixture) {
				fixture->fake->Settle();
				fixture->server.Stop();  // first, so its last line ("server: stopped") is kept
				for (auto& line : fixture->LogLines()) {
					pastLog.push_back(std::move(line));
				}
				fixture.reset();
			}
			for (auto& client : clients) {
				if (client->connected) {
					client->OnDisconnected();
				}
			}
		}

	private:
		// The rest of Settle, once what is to be sent has been.
		void Settled()
		{
			Deliver();
			if (fixture) {
				REQUIRE(fixture->fake->Settle());
				if (std::exchange(moved, false)) {
					// Positions go out on the server's next tick (about 60 a second).
					std::this_thread::sleep_for(std::chrono::milliseconds(40));
					REQUIRE(fixture->fake->Settle());
				}
			}
			for (auto& client : clients) {
				client->Pump();
				client->Frames(FRAMES_PER_SETTLE);
				client->PollQuests();
			}
		}

		// Delivers what was sent, and what waits in the players' settlement lines as it comes due.
		void DeliverAll()
		{
			Deliver();
			while (std::ranges::any_of(clients, [](const auto& a_client) { return a_client->connected && a_client->WorkshopUnsent() > 0; })) {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				Deliver();
			}
		}

	public:
		// The server starts, from the session file if there is one.
		void StartServer()
		{
			if (!fixture) {
				fixture = std::make_unique<TestServer::Fixture>(options);
			}
		}

		void RestartServer()
		{
			StopServer();
			StartServer();
		}

		// The server is another host's now, with a session of its own: the one it ran stops (everyone in it
		// loses the connection) and is gone, and a new one starts, for a host of its own (Host then
		// gives that host's game). Players get there by accepting an invite (Client::SwitchServer).
		void ServeAnotherSession()
		{
			StopServer();
			options.hostIdentity = HOST_IDENTITY + ++otherHosts;
			std::error_code ignored;
			for (const auto* suffix : { "", ".tmp", ".bak", ".corrupt" }) {
				std::filesystem::remove(options.sessionFile + suffix, ignored);
			}
			StartServer();
		}

		[[nodiscard]] bool ServerRunning() const { return fixture != nullptr; }

		// Whether the server logged a line containing a_text (this run or an earlier one).
		[[nodiscard]] bool Logged(std::string_view a_text) const
		{
			const auto has = [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; };
			return std::ranges::any_of(pastLog, has) || (fixture && fixture->Logged(a_text));
		}

		// Every line the server logged (this run and the earlier ones), in order.
		[[nodiscard]] std::vector<std::string> LogLines() const
		{
			auto out = pastLog;
			if (fixture) {
				std::ranges::move(fixture->LogLines(), std::back_inserter(out));
			}
			return out;
		}

		// Play goes on for a while: each game polls its quests (QuestSync, twice a second) and the session
		// is settled after each poll, so what one game reports reaches the others' queues on the way.
		void PlayFor(std::chrono::milliseconds a_time)
		{
			for (auto played = std::chrono::milliseconds{ 0 }; played < a_time; played += QuestGame::POLL_INTERVAL) {
				Settle();
			}
		}

		// As PlayFor, with the server's clock going by too: each poll's half second is waited for, so what
		// the games send meets the server's per-second budgets as a real evening would.
		void PlayInRealTime(std::chrono::milliseconds a_time)
		{
			for (auto played = std::chrono::milliseconds{ 0 }; played < a_time; played += QuestGame::POLL_INTERVAL) {
				SettleAfter(QuestGame::POLL_INTERVAL);
			}
		}

		// Time goes by on the server (a budget refills), and then the session is settled.
		void SettleAfter(std::chrono::milliseconds a_wait)
		{
			Deliver();
			std::this_thread::sleep_for(a_wait);
			Settle();
		}

		[[nodiscard]] const Server::Options& ServerOptions() const { return options; }
		[[nodiscard]] const std::map<std::uint32_t, PlacedFrame>& Frames() const { return frames; }
		void PlaceFrame(std::uint32_t a_ref, PlacedFrame a_frame) { frames[a_ref] = a_frame; }

		Dice& Random() { return dice; }

		// The players who are in the session now.
		[[nodiscard]] std::vector<Client*> Present() const
		{
			std::vector<Client*> out;
			for (const auto& client : clients) {
				if (client->Welcomed()) {
					out.push_back(client.get());
				}
			}
			return out;
		}

		// Where these players' worlds differ in what the session shares (dead NPCs, containers, pickups,
		// doors, quest stages, what is built and scrapped at settlements); empty when they agree.
		[[nodiscard]] static std::string Disagreement(const std::vector<Client*>& a_clients);

	private:
		friend class Client;

		enum class Kind
		{
			kConnect,
			kPacket,
			kDrop,
		};

		struct Outgoing
		{
			Client* from;
			Kind    kind;
			PeerId  peer;
			Packet  data;
			bool    reliable = true;
		};

		Client& Add(std::string a_name, std::uint64_t a_identity, bool a_host)
		{
			clients.push_back(std::make_unique<Client>(*this, std::move(a_name), a_identity, a_host));
			return *clients.back();
		}

		// What each player sent since the last time, in the order sent, through that player's faults.
		void Deliver()
		{
			// What has come due in each player's settlement line goes too (WorkshopSync::TakeOutgoing, each frame).
			const auto now = Clock::now();
			for (auto& client : clients) {
				client->FlushWorkshop(now);
			}
			auto pending = std::exchange(outbox, {});
			if (!fixture) {
				return;  // nobody is listening
			}
			const auto faultsOf = [](const Outgoing& a_out) -> const Faults& { return a_out.from->faults; };
			const auto touchable = [](const Outgoing& a_out) { return a_out.kind == Kind::kPacket && (!a_out.reliable || a_out.from->faults.reliableToo); };
			for (auto& out : ThroughFaults(std::move(pending), dice, faultsOf, touchable)) {
				switch (out.kind) {
				case Kind::kConnect:
					fixture->fake->Connect(out.peer);
					break;
				case Kind::kPacket:
					fixture->fake->Deliver(out.peer, std::move(out.data));
					break;
				case Kind::kDrop:
					fixture->fake->Drop(out.peer);
					break;
				}
			}
		}

		// Frames each game runs while a Settle goes by (as the server's tick does, about 0.1 s).
		static constexpr int FRAMES_PER_SETTLE = 3;

		void Queue(Outgoing a_out) { outbox.push_back(std::move(a_out)); }

		PeerId NewPeer() { return ++lastPeer; }

		Server::Options                              options;
		bool                                         ownFile = false;
		Dice                                         dice;
		std::uint32_t                                seed;
		std::unique_ptr<TestServer::Fixture>         fixture;
		std::vector<std::unique_ptr<Client>>         clients;
		std::vector<Outgoing>                        outbox;
		std::vector<std::string>                     pastLog;
		std::map<std::uint32_t, PlacedFrame>         frames;
		PeerId                                       lastPeer = 100;
		bool                                         moved = false;
		std::uint64_t                                otherHosts = 0;  // sessions served for other hosts (ServeAnotherSession)
	};

	// ---- Client ----

	inline Packet Client::Hello()
	{
		Protocol::Hello hello;
		hello.contentHash = contentHash;
		hello.name = name;
		hello.identity = identity;
		hello.world = loot.Hello(LootSync::Clock::now());
		return Protocol::Encode(hello);
	}

	inline bool Client::Join()
	{
		if (connected) {
			return welcomed;
		}
		if (!session.fixture) {
			return false;  // nobody answers
		}
		peer = session.NewPeer();
		connected = true;
		wantSession = true;
		if (host) {
			session.fixture->fake->SetLocal(peer);  // the server's own machine
		}
		session.Queue({ this, Session::Kind::kConnect, peer, {}, true });
		Send(Hello(), true);
		session.moved = true;  // where the others stand comes on the server's next tick
		session.Settle();
		return welcomed;
	}

	inline void Client::Leave()
	{
		if (!connected) {
			return;
		}
		session.Queue({ this, Session::Kind::kDrop, peer, {}, true });
		wantSession = false;  // playing alone now
		OnDisconnected();
		session.Settle();
	}

	inline void Client::Vanish()
	{
		if (connected) {
			OnDisconnected();
		}
	}

	inline bool Client::SwitchServer()
	{
		if (connected) {
			session.Queue({ this, Session::Kind::kDrop, peer, {}, true });
			OnDisconnected();
		}
		questGame.OnServerChanged();  // QuestSync::OnServerChanged: the address changes
		return Join();
	}

	inline void Client::HelloAgain()
	{
		if (connected) {
			welcomed = false;  // a new welcome follows
			Send(Hello(), true);
		}
	}

	inline void Client::OnDisconnected()
	{
		// Session::OnDisconnected: the others' stand-ins, the NPCs they ran and the copies of what they built
		// go; the world itself (corpses, loot, scraps) stays as it is, and so does where it stands in the
		// session's container changes.
		connected = false;
		welcomed = false;
		peer = 0;
		loot.OnDisconnected();
		friends.clear();
		owners.clear();
		npcStates.clear();
		npcGear.clear();
		npcHealth.clear();
		damageHeard.clear();
		killedBy.clear();
		copies.Clear();
		copyKeys.clear();
		hiddenFrames.clear();
		heardDead.clear();
		toCheck.clear();
		time.reset();
		(void)workshopOut.TakeAll();  // WorkshopSync::TakeOutgoing(false): what waited to be sent is not
		// QuestSync::OnSessionEnded, and the story is our own again (Story::SetShared(false)).
		questGame.OnSessionEnded();
	}

	inline void Client::Send(Packet a_packet, bool a_reliable)
	{
		if (!connected) {
			return;  // playing alone: nobody hears of it
		}
		session.Queue({ this, Session::Kind::kPacket, peer, std::move(a_packet), a_reliable });
	}

	inline void Client::SendWorkshop(Packet a_packet)
	{
		if (!connected) {
			return;
		}
		if (!pacedWorkshop) {
			Send(std::move(a_packet), true);
			return;
		}
		(void)workshopOut.Push(std::move(a_packet));
		FlushWorkshop(Clock::now());  // the game takes its reports every frame: what may go, goes now
	}

	inline void Client::FlushWorkshop(Clock::time_point a_now)
	{
		for (auto& packet : workshopOut.TakeDue(a_now)) {
			Send(std::move(packet), true);
		}
	}

	inline void Client::Move(float a_x, float a_y, float a_z, float a_speed, std::uint32_t a_worldspace)
	{
		me.sequence = ++sequence;
		me.cell = 0;
		me.worldspace = a_worldspace;
		me.x = a_x;
		me.y = a_y;
		me.z = a_z;
		me.speed = a_speed;
		if (connected) {
			session.moved = true;
		}
		Send(Protocol::Encode(me), false);
	}

	inline void Client::StandStill(int a_states)
	{
		for (int i = 0; i < a_states; ++i) {
			Move(me.x, me.y, me.z, 0.0f, me.worldspace);
		}
	}

	inline void Client::Kill(std::uint32_t a_ref, float a_x, float a_y, float a_z)
	{
		world.dead.insert(a_ref);
		heardDead.insert(a_ref);
		Send(Protocol::Encode(Protocol::ActorDeath{ a_ref, 0, true, a_x, a_y, a_z }, MT::kReportDeath), true);
	}

	inline void Client::Died(std::uint32_t a_ref, bool a_killed)
	{
		world.dead.insert(a_ref);
		heardDead.insert(a_ref);
		Send(Protocol::Encode(Protocol::ActorDeath{ .refId = a_ref, .killed = a_killed }, MT::kReportDeath), true);
	}

	inline void Client::Hurt(std::uint32_t a_ref, float a_health)
	{
		Send(Protocol::Encode(Protocol::ActorHealth{ a_ref, a_health }, MT::kReportHealth), true);
	}

	inline void Client::Loot(std::uint32_t a_container, std::uint32_t a_item, std::int32_t a_count)
	{
		world.containers[a_container][a_item] += a_count;
		Send(Protocol::Encode(Protocol::ContainerChange{ a_container, a_item, a_count }, MT::kReportContainer), true);
	}

	inline void Client::PickUp(std::uint32_t a_ref)
	{
		world.pickedUp.insert(a_ref);
		Send(Protocol::Encode(Protocol::RefPickedUp{ a_ref }, MT::kReportPickup), true);
	}

	inline void Client::Door(std::uint32_t a_ref, std::uint8_t a_open, std::uint8_t a_locked)
	{
		Protocol::RefState state;
		state.refId = a_ref;
		state.open = a_open;
		state.locked = a_locked;
		ApplyDoor(state);
		Send(Protocol::Encode(state, MT::kReportRefState), true);
	}

	inline void Client::Stage(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		if (questGame.Modelled(a_quest)) {
			questGame.Do(a_quest, a_stage);  // reported at the next poll, if it is to be
			return;
		}
		world.questStages.insert({ a_quest, a_stage });
		Send(Protocol::Encode(Protocol::QuestStage{ a_quest, a_stage }, MT::kReportQuestStage), true);
	}

	inline std::uint32_t Client::Build(std::uint32_t a_bench, std::uint32_t a_base, float a_x, float a_y, float a_z)
	{
		const std::uint32_t ref = 0xFF000000 | ++nextRef;
		Protocol::WorkshopItem item;
		item.playerId = playerId;
		item.refId = ref;
		item.op = Protocol::WorkshopOp::kPlaced;
		item.base = a_base;
		item.workshop = a_bench;
		item.position[0] = a_x;
		item.position[1] = a_y;
		item.position[2] = a_z;
		world.built[ref] = item;
		SendWorkshop(Protocol::Encode(item, MT::kReportWorkshopItem));
		return ref;
	}

	inline void Client::MoveBuilt(std::uint32_t a_ref, float a_x, float a_y, float a_z)
	{
		const auto it = world.built.find(a_ref);
		if (it == world.built.end()) {
			return;
		}
		it->second.position[0] = a_x;
		it->second.position[1] = a_y;
		it->second.position[2] = a_z;
		SendWorkshop(Protocol::Encode(it->second, MT::kReportWorkshopItem));
	}

	inline void Client::Scrap(std::uint32_t a_bench, std::uint32_t a_ref)
	{
		Protocol::WorkshopItem item;
		item.refId = a_ref;
		item.op = Protocol::WorkshopOp::kScrapped;
		item.workshop = a_bench;
		if (const auto it = world.built.find(a_ref); it != world.built.end()) {
			item.base = it->second.base;
			world.built.erase(it);
		}
		if (Protocol::IsShareableRef(a_ref)) {
			world.scrapped.insert(a_ref);
		}
		SendWorkshop(Protocol::Encode(item, MT::kReportWorkshopItem));
	}

	inline void Client::Claim(std::uint32_t a_ref, Protocol::ClaimReason a_reason)
	{
		Send(Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } }), true);
	}

	inline void Client::Release(std::uint32_t a_ref)
	{
		Send(Protocol::EncodeRelease({ a_ref }), true);
	}

	inline void Client::RunNpc(std::uint32_t a_ref, float a_x, float a_y, float a_z)
	{
		Protocol::ActorState state;
		state.refId = a_ref;
		state.worldspace = COMMONWEALTH;
		state.x = a_x;
		state.y = a_y;
		state.z = a_z;
		Send(Protocol::Encode(std::vector<Protocol::ActorState>{ state }, MT::kActorStates), false);
	}

	inline void Client::NpcGear(std::uint32_t a_ref, std::vector<std::uint32_t> a_items)
	{
		Send(Protocol::Encode(Protocol::NpcEquipment{ 0, a_ref, std::move(a_items) }, MT::kReportNpcEquipment), true);
	}

	inline void Client::Equip(std::vector<std::uint32_t> a_items)
	{
		myGear = a_items;
		Send(Protocol::Encode(Protocol::Equipment{ 0, std::move(a_items) }, MT::kEquipment), true);
	}

	inline void Client::Status(Protocol::PlayerStatus a_status)
	{
		myStatus = a_status;
		Send(Protocol::Encode(a_status, MT::kReportStatus), true);
	}

	inline void Client::WearFrame(std::uint32_t a_frameRef)
	{
		auto status = myStatus;
		const auto frame = session.frames.find(a_frameRef);
		status.powerArmor = a_frameRef != 0;
		status.powerArmorFrameRef = a_frameRef;
		status.powerArmorFrame = frame != session.frames.end() ? frame->second.base : 0;
		Status(status);
	}

	inline void Client::Revive(std::uint32_t a_playerId)
	{
		Send(Protocol::Encode(Protocol::Revive{ a_playerId }, MT::kRevive), true);
	}

	inline void Client::HitPlayer(std::uint32_t a_playerId, float a_damage, std::uint32_t a_attacker)
	{
		Send(Protocol::Encode(Protocol::PlayerHit{ a_playerId, a_damage, false, a_attacker }, MT::kPlayerHit), true);
	}

	inline void Client::CheckFoundDead()
	{
		for (const auto ref : std::exchange(toCheck, {})) {
			if (!world.dead.contains(ref) || heardDead.contains(ref)) {
				continue;
			}
			const auto facts = characters.find(ref);
			if (!FoundDead::Shared(sharedStory, facts != characters.end() ? facts->second : FoundDead::Npc{})) {
				continue;  // a character of our own story: our save decides nothing about theirs
			}
			heardDead.insert(ref);
			Send(Protocol::Encode(Protocol::ActorDeath{ .refId = ref, .foundDead = true }, MT::kReportDeath), true);
		}
	}

	inline void Client::LoadGame(const Save& a_save)
	{
		world = a_save.world;
		questGame.Load(a_save.quests);  // and QuestSync::Rebaseline
		// The save's copies are back where it had them; the session's building, sent again after the load,
		// takes them over at their spots (WorkshopSync::OnGameLoaded, FindExisting).
		copies = a_save.copies;
		copyKeys = a_save.copyKeys;
		heardDead.clear();  // WorldSync::Reset: what was known described the game before the load
		// WorldSync's record of the save, then (Session) the world is bound to the session we're in and asks
		// for what came since.
		loot.LoadRecord(a_save.resync.sessionId, a_save.resync.containerFrom, {});  // session 0: the save has no record
		if (const auto request = welcomed ? loot.AfterLoad(LootSync::Clock::now()) : std::nullopt) {
			Send(Protocol::Encode(*request), true);
		}
	}

	inline std::map<WorkshopKey, Protocol::WorkshopItem> Client::Settlements() const
	{
		std::map<WorkshopKey, Protocol::WorkshopItem> out;
		for (const auto& [ref, item] : world.built) {
			auto own = item;
			own.playerId = playerId;
			out[{ playerId, ref }] = own;
		}
		for (const auto& key : copyKeys) {
			if (const auto item = copies.Find(key)) {
				out[key] = *item;
			}
		}
		return out;
	}

	inline std::set<WorkshopKey> Client::At(std::uint32_t a_bench) const
	{
		const auto near = copies.Near(WorkshopCells::Interior(a_bench));
		std::set<WorkshopKey> out(near.begin(), near.end());
		for (const auto& [ref, item] : world.built) {
			if (item.workshop == a_bench) {
				out.insert({ playerId, ref });
			}
		}
		return out;
	}

	inline void Client::Frames(int a_count)
	{
		for (int i = 0; i < a_count; ++i) {
			frameClock += std::chrono::milliseconds(33);
			for (auto& [id, f] : friends) {
				if (!f.state) {
					continue;  // no stand-in yet
				}
				// Dressed out of power armor, its 3D in; no weapon of the tests' is held in the hand.
				const auto dressing = StandInGear::Dress(f.gear, ArmorFrames::Dress::kClothes, true, frameClock, [](std::uint32_t) { return false; },
					[&](const std::vector<std::uint32_t>& a_worn) { return f.wearing == a_worn; });
				if (dressing.put != StandInGear::Put::kNone) {
					f.wearing = f.gear.items;  // emptied and dressed, or their change put on over what it wore
				}
				auto& graph = f.standIn;
				if (graph.busy > 0 && --graph.busy == 0) {
					graph.walking = graph.landsWalking;
				}
				f.graph = MoveSync::Drive(f.graph.moving, f.graph.nextResync, f.state->speed, frameClock, [&](MoveSync::Event a_event) {
					const bool start = a_event == MoveSync::Event::kStart;
					if (graph.busy > 0 || graph.walking == start) {
						return false;  // refused
					}
					graph.walking = start;
					return true;
				});
			}
		}
	}

	inline void Client::PollQuests()
	{
		const auto friendAt = [this](std::uint32_t a_id) -> std::optional<Position> {
			const auto it = friends.find(a_id);
			if (it == friends.end() || !it->second.state) {
				return std::nullopt;  // RemotePlayers::PositionOf: no stand-in yet
			}
			return Position{ it->second.state->x, it->second.state->y, it->second.state->z };
		};
		// Session: QuestSync::Frame(welcomed, hosting), then TakeOutgoing(welcomed, in a session or joining one).
		for (auto& packet : questGame.Poll(welcomed, welcomed || wantSession, { me.x, me.y, me.z }, friendAt, host)) {
			Send(std::move(packet), true);
		}
	}

	inline void Client::Pump()
	{
		if (!connected || !session.fixture) {
			return;
		}
		const auto faultsOf = [&](const FakeTransport::Sent&) -> const Faults& { return faults; };
		const auto touchable = [&](const FakeTransport::Sent& a_sent) { return !a_sent.reliable || faults.reliableToo; };
		const auto arrived = ThroughFaults(session.fixture->fake->TakeSent(peer), session.dice, faultsOf, touchable);
		for (const auto& sent : arrived) {
			Handle(sent.data);
		}
		if (session.fixture->fake->Disconnected(peer)) {
			OnDisconnected();  // the server closed the connection (a rejection, or a newer one of ours)
		}
	}

	inline void Client::ApplyContainer(std::uint32_t a_index, std::uint32_t a_playerId, const Protocol::ContainerChange& a_change)
	{
		loot.Live({ .index = a_index, .playerId = a_playerId, .change = a_change }, LootSync::Clock::now(), chests);
	}

	inline void Client::ApplyWorldState(const Protocol::WorldState& a_state)
	{
		for (const auto ref : a_state.deadActors) {
			heardDead.insert(ref);
			world.dead.insert(ref);
		}
		loot.CatchUp(a_state.containerFirstIndex, a_state.containerChanges, LootSync::Clock::now(), chests);
		world.pickedUp.insert(a_state.pickedUp.begin(), a_state.pickedUp.end());
		for (const auto& state : a_state.refStates) {
			ApplyDoor(state);
		}
		for (const auto& stage : a_state.questStages) {
			if (questGame.Modelled(stage.quest)) {
				questGame.Apply(stage, false);  // caught up from the session
			} else {
				world.questStages.insert({ stage.quest, stage.stage });
			}
		}
	}

	inline void Client::ApplyDoor(const Protocol::RefState& a_state)
	{
		auto& door = world.doors.try_emplace(a_state.refId, Protocol::RefState::kUnknown, Protocol::RefState::kUnknown).first->second;
		if (a_state.open != Protocol::RefState::kUnknown) {
			door.first = a_state.open;
		}
		if (a_state.locked != Protocol::RefState::kUnknown) {
			door.second = a_state.locked;
		}
	}

	// As WorkshopSync::Apply: a copy of what another player built waits for its settlement under (their
	// player ID, their reference ID); a scrap takes that copy away, and a piece of the world itself goes
	// from this world too.
	inline void Client::ApplyWorkshop(const Protocol::WorkshopItem& a_item)
	{
		const WorkshopKey key{ a_item.playerId, a_item.refId };
		if (a_item.op == Protocol::WorkshopOp::kScrapped) {
			copies.Erase(key);
			copyKeys.erase(key);
			if (Protocol::IsShareableRef(a_item.refId)) {
				world.scrapped.insert(a_item.refId);
			}
			return;
		}
		copies.Put(WorkshopCells::Interior(a_item.workshop), key, a_item);
		copyKeys.insert(key);
	}

	// ArmorFrames: a friend in a frame every game has placed; ours is hidden while they wear theirs, if
	// it stands where they got in. Judged once a wear, when their status and a state are both in (the
	// status of someone already there reaches a newcomer before their first state does).
	inline void Client::JudgeWear(std::uint32_t a_friend)
	{
		auto& f = friends[a_friend];
		if (f.wear || !f.status || !f.status->powerArmor || !f.state) {
			return;
		}
		const auto                  ref = f.status->powerArmorFrameRef;
		const auto                  frame = session.frames.find(ref);
		ArmorFrames::OriginalFacts  facts;
		facts.found = frame != session.frames.end();
		if (facts.found) {
			facts.base = frame->second.base;
			facts.frame = true;
			facts.loaded = true;
			facts.ours = myStatus.powerArmor && myStatus.powerArmorFrameRef == ref;
			facts.inUse = hiddenFrames.contains(ref) && hiddenFrames.at(ref) != a_friend;
			if (f.state->cell == 0 && f.state->worldspace == frame->second.worldspace) {
				const float dx = f.state->x - frame->second.x;
				const float dy = f.state->y - frame->second.y;
				const float dz = f.state->z - frame->second.z;
				facts.fromSpot = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
		}
		f.wear = ArmorFrames::ChooseOriginal(ref, f.status->powerArmorFrame, facts);
		if (*f.wear == ArmorFrames::Original::kHide) {
			hiddenFrames[ref] = a_friend;
		}
	}

	inline void Client::Handle(std::span<const std::uint8_t> a_data)
	{
		const auto type = Protocol::PeekType(a_data).value_or(MT{});
		++heard[type];
		bool decoded = true;
		const auto unless = [&](bool a_ok) { decoded = decoded && a_ok; };
		switch (type) {
		case MT::kWelcome:
			if (const auto msg = Protocol::DecodeWelcome(a_data); msg && !welcomed) {
				welcomed = true;
				playerId = msg->playerId;
				sessionId = msg->sessionId;
				sharedStory = msg->sharedStory;
				questGame.story = msg->sharedStory;
				loot.OnWelcome(msg->sessionId, msg->playerId);
				for (auto& [ref, item] : world.built) {
					item.playerId = playerId;
				}
				// Session forgets what gear it sent on a welcome, so its next check sends it again.
				if (myGear) {
					Send(Protocol::Encode(Protocol::Equipment{ 0, *myGear }, MT::kEquipment), true);
				}
			} else {
				unless(msg.has_value());
			}
			break;
		case MT::kReject:
			if (const auto msg = Protocol::DecodeReject(a_data)) {
				rejections.push_back(msg->reason);
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerJoined:
			if (const auto msg = Protocol::DecodePlayerJoined(a_data)) {
				auto& f = friends[msg->playerId];
				f = Friend{};
				f.name = msg->name;
				f.appearance = msg->appearance;
			} else {
				unless(false);
			}
			break;
		case MT::kRenamed:
			if (const auto msg = Protocol::DecodeRename(a_data)) {
				if (const auto it = friends.find(msg->playerId); it != friends.end()) {
					it->second.name = msg->name;
				}
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerLeft:
			if (const auto msg = Protocol::DecodePlayerLeft(a_data)) {
				friends.erase(msg->playerId);
				std::erase_if(hiddenFrames, [&](const auto& a_entry) { return a_entry.second == msg->playerId; });
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerStates:
			if (const auto msg = Protocol::DecodePlayerStates(a_data)) {
				for (const auto& entry : msg->players) {
					const auto it = friends.find(entry.playerId);
					if (entry.playerId == playerId || it == friends.end()) {
						continue;
					}
					auto& f = it->second;
					if (f.state && entry.state.sequence <= f.state->sequence) {
						continue;  // older than what we have (RemotePlayers::PushState)
					}
					f.state = entry.state;
					JudgeWear(entry.playerId);
				}
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerStatus:
			if (const auto msg = Protocol::DecodePlayerStatus(a_data)) {
				const auto it = friends.find(msg->playerId);
				if (it == friends.end()) {
					break;  // nobody we know of
				}
				auto& f = it->second;
				const bool sameWear = f.status && f.status->powerArmor == msg->powerArmor && f.status->powerArmorFrameRef == msg->powerArmorFrameRef;
				f.status = *msg;
				if (!sameWear) {
					f.wear.reset();
					std::erase_if(hiddenFrames, [&](const auto& a_entry) { return a_entry.second == msg->playerId; });
					JudgeWear(msg->playerId);
				}
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerEquipment:
			if (auto msg = Protocol::DecodeEquipment(a_data)) {
				if (const auto it = friends.find(msg->playerId); it != friends.end()) {
					StandInGear::Report(it->second.gear, msg->items);
					it->second.equipment = std::move(msg->items);
				}
			} else {
				unless(false);
			}
			break;
		case MT::kActorDied:
			if (const auto msg = Protocol::DecodeActorDeath(a_data)) {
				heardDead.insert(msg->refId);
				world.dead.insert(msg->refId);
				killedBy[msg->refId] = msg->killed ? msg->playerId : 0;
			} else {
				unless(false);
			}
			break;
		case MT::kActorHealth:
			if (const auto msg = Protocol::DecodeActorHealth(a_data)) {
				if (!world.dead.contains(msg->refId)) {
					npcHealth[msg->refId] = msg->health;
				}
			} else {
				unless(false);
			}
			break;
		case MT::kActorDamaged:
			if (const auto msg = Protocol::DecodeActorHealth(a_data)) {
				if (damageHeard.size() < 4096) {
					damageHeard.push_back(*msg);
				}
			} else {
				unless(false);
			}
			break;
		case MT::kContainerChanged:
			if (const auto msg = Protocol::DecodeIndexedContainerChange(a_data)) {
				ApplyContainer(msg->index, msg->playerId, msg->change);
			} else {
				unless(false);
			}
			break;
		case MT::kWorldState:
			if (const auto msg = Protocol::DecodeWorldState(a_data)) {
				ApplyWorldState(*msg);
			} else {
				unless(false);
			}
			break;
		case MT::kRefPickedUp:
			if (const auto msg = Protocol::DecodeRefPickedUp(a_data)) {
				world.pickedUp.insert(msg->refId);
			} else {
				unless(false);
			}
			break;
		case MT::kRefStateChanged:
			if (const auto msg = Protocol::DecodeRefState(a_data)) {
				ApplyDoor(*msg);
			} else {
				unless(false);
			}
			break;
		case MT::kQuestStage:
			if (const auto msg = Protocol::DecodeQuestStage(a_data)) {
				if (questGame.Modelled(msg->quest)) {
					questGame.Apply(*msg, true);  // live: a friend just did it
				} else {
					world.questStages.insert({ msg->quest, msg->stage });
				}
			} else {
				unless(false);
			}
			break;
		case MT::kActorOwners:
			if (const auto msg = Protocol::DecodeOwners(a_data)) {
				for (const auto& owner : *msg) {
					if (owner.playerId == 0) {
						owners.erase(owner.refId);
						npcStates.erase(owner.refId);
						continue;
					}
					owners[owner.refId] = owner.playerId;
					if (!loot.Ours(owner.playerId) && Protocol::IsShareableRef(owner.refId)) {
						toCheck.insert(owner.refId);  // WorldSync::OnActorOwners
					}
				}
			} else {
				unless(false);
			}
			break;
		case MT::kActorStatesRelay:
			if (const auto msg = Protocol::DecodeActorStates(a_data)) {
				for (const auto& state : *msg) {
					npcStates[state.refId] = state;
				}
			} else {
				unless(false);
			}
			break;
		case MT::kNpcEquipment:
			if (auto msg = Protocol::DecodeNpcEquipment(a_data)) {
				npcGear[msg->refId] = std::move(msg->items);
			} else {
				unless(false);
			}
			break;
		case MT::kWorkshopItem:
			if (const auto msg = Protocol::DecodeWorkshopItem(a_data)) {
				ApplyWorkshop(*msg);
			} else {
				unless(false);
			}
			break;
		case MT::kWorkshopItems:
			if (const auto msg = Protocol::DecodeWorkshopItems(a_data)) {
				for (const auto& item : *msg) {
					ApplyWorkshop(item);
				}
			} else {
				unless(false);
			}
			break;
		case MT::kWorldTime:
			if (const auto msg = Protocol::DecodeWorldTime(a_data)) {
				time = *msg;
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerDamaged:
			if (const auto msg = Protocol::DecodePlayerHit(a_data)) {
				damage.push_back(*msg);
			} else {
				unless(false);
			}
			break;
		case MT::kRevived:
			if (const auto msg = Protocol::DecodeRevive(a_data)) {
				revivedBy.push_back(msg->playerId);
			} else {
				unless(false);
			}
			break;
		case MT::kMarkersFound:
			if (const auto msg = Protocol::DecodeMarkers(a_data)) {
				for (const auto& marker : msg->markers) {
					markers[marker.refId] |= marker.flags;
				}
			} else {
				unless(false);
			}
			break;
		case MT::kShotFired:
			unless(Protocol::DecodeShot(a_data).has_value());
			break;
		case MT::kPinged:
			unless(Protocol::DecodePing(a_data).has_value());
			break;
		case MT::kPartyXp:
			unless(Protocol::DecodeXpGain(a_data).has_value());
			break;
		case MT::kLineSpoken:
			unless(Protocol::DecodeLine(a_data).has_value());
			break;
		case MT::kQuestDone:
			unless(Protocol::DecodeQuestDone(a_data).has_value());
			break;
		case MT::kVoiceRelay:
			unless(Protocol::DecodeVoice(a_data).has_value());
			break;
		case MT::kActivation:
			unless(Protocol::DecodeActivation(a_data).has_value());
			break;
		case MT::kExplosion:
			if (const auto msg = Protocol::DecodeExplosion(a_data)) {
				explosions.push_back(*msg);
			} else {
				unless(false);
			}
			break;
		case MT::kPlayerFace:
			unless(Protocol::DecodeFace(a_data).has_value());
			break;
		case MT::kHeartbeatAck:
			unless(Protocol::DecodeHeartbeat(a_data).has_value());
			break;
		case MT::kHostInfo:  // the host controls' state, to the host's game
			unless(Protocol::DecodeHostInfo(a_data).has_value());
			break;
		case MT::kRemoved:
			unless(Protocol::DecodeRemoved(a_data).has_value());
			break;
		default:
			unless(false);  // nothing a player's game knows
			break;
		}
		undecodable += !decoded;
	}

	// ---- comparing worlds ----

	namespace Detail
	{
		inline std::string Hex(std::uint32_t a_v) { return std::format("{:08X}", a_v); }
		inline std::string Show(std::uint32_t a_v) { return Hex(a_v); }
		inline std::string Show(const std::pair<std::uint32_t, std::uint16_t>& a_v) { return std::format("{}:{}", Hex(a_v.first), a_v.second); }
		inline std::string Show(const std::pair<std::uint8_t, std::uint8_t>& a_v) { return std::format("open {} locked {}", a_v.first, a_v.second); }
		inline std::string Show(const WorkshopKey& a_v) { return std::format("player {} {}", a_v.first, Hex(a_v.second)); }
		inline std::string Show(const std::map<std::uint32_t, std::int64_t>& a_v)
		{
			std::string out = "{";
			for (const auto& [item, count] : a_v) {
				out += std::format(" {} x{}", Hex(item), count);
			}
			return out + " }";
		}
		inline std::string Show(const Protocol::WorkshopItem& a_v)
		{
			return std::format("op {} base {} at bench {} ({}, {}, {})", a_v.op, Hex(a_v.base), Hex(a_v.workshop), a_v.position[0], a_v.position[1], a_v.position[2]);
		}

		template <class T>
		void DiffSets(std::string& a_out, const char* a_what, const Client& a_a, const std::set<T>& a_sa, const Client& a_b, const std::set<T>& a_sb)
		{
			for (const auto& v : a_sa) {
				if (!a_sb.contains(v)) {
					a_out += std::format("  {}: {} has {}, {} doesn't\n", a_what, a_a.Name(), Show(v), a_b.Name());
				}
			}
			for (const auto& v : a_sb) {
				if (!a_sa.contains(v)) {
					a_out += std::format("  {}: {} has {}, {} doesn't\n", a_what, a_b.Name(), Show(v), a_a.Name());
				}
			}
		}

		template <class K, class V, class Same>
		void DiffMaps(std::string& a_out, const char* a_what, const Client& a_a, const std::map<K, V>& a_ma, const Client& a_b, const std::map<K, V>& a_mb, Same&& a_same)
		{
			for (const auto& [k, v] : a_ma) {
				const auto it = a_mb.find(k);
				if (it == a_mb.end()) {
					a_out += std::format("  {}: {} has {} = {}, {} doesn't\n", a_what, a_a.Name(), Show(k), Show(v), a_b.Name());
				} else if (!a_same(v, it->second)) {
					a_out += std::format("  {}: {} differs: {} has {}, {} has {}\n", a_what, Show(k), a_a.Name(), Show(v), a_b.Name(), Show(it->second));
				}
			}
			for (const auto& [k, v] : a_mb) {
				if (!a_ma.contains(k)) {
					a_out += std::format("  {}: {} has {} = {}, {} doesn't\n", a_what, a_b.Name(), Show(k), Show(v), a_a.Name());
				}
			}
		}
	}

	inline std::string Session::Disagreement(const std::vector<Client*>& a_clients)
	{
		using namespace Detail;
		std::string out;
		for (std::size_t i = 1; i < a_clients.size(); ++i) {
			const Client& a = *a_clients[0];
			const Client& b = *a_clients[i];
			const auto&   wa = a.WorldNow();
			const auto&   wb = b.WorldNow();
			DiffSets(out, "dead", a, wa.dead, b, wb.dead);
			DiffMaps(out, "container", a, wa.Contents(), b, wb.Contents(), std::equal_to<>{});
			DiffSets(out, "picked up", a, wa.pickedUp, b, wb.pickedUp);
			DiffMaps(out, "door", a, wa.doors, b, wb.doors, std::equal_to<>{});
			DiffSets(out, "quest stage", a, wa.questStages, b, wb.questStages);
			DiffSets(out, "scrapped", a, wa.scrapped, b, wb.scrapped);
			const auto same = [](const Protocol::WorkshopItem& x, const Protocol::WorkshopItem& y) {
				return x.op == y.op && x.base == y.base && x.workshop == y.workshop && std::ranges::equal(x.position, y.position);
			};
			DiffMaps(out, "built", a, a.Settlements(), b, b.Settlements(), same);
		}
		return out;
	}

	// Records a failure (with what differs) unless these players' worlds agree.
	inline void CheckAgree(const std::vector<Client*>& a_clients, const char* a_file, int a_line)
	{
		if (const auto differences = Session::Disagreement(a_clients); !differences.empty()) {
			Test::Fail(a_file, a_line, "the players' worlds differ:\n" + differences);
		}
	}
}

#define CHECK_AGREE(...) Sim::CheckAgree(__VA_ARGS__, __FILE__, __LINE__)
