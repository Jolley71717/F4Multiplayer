#include "game/WorldSync.h"

#include "MainThread.h"
#include "game/CopyWatch.h"
#include "game/DamageHold.h"
#include "game/DeathReports.h"
#include "game/DoorShare.h"
#include "game/Drops.h"
#include "game/Gifts.h"
#include "game/Equipment.h"
#include "game/FloraShare.h"
#include "game/FoundDead.h"
#include "game/GameStop.h"
#include "game/HealthShare.h"
#include "game/Hud.h"
#include "game/LinkedRefs.h"
#include "game/LogPace.h"
#include "game/LootScope.h"
#include "game/LootSync.h"
#include "game/Destructibles.h"
#include "game/MovedObjects.h"
#include "game/SessionDeaths.h"
#include "game/NpcHold.h"
#include "game/NpcSync.h"
#include "game/PendingContainers.h"
#include "game/QuestStageLayout.h"
#include "game/QuestSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/Papyrus.h"
#include "game/ScriptDoors.h"
#include "game/SharedGates.h"
#include "game/Spawns.h"
#include "game/Storage.h"
#include "game/Story.h"
#include "game/Traps.h"
#include "game/WeaponFire.h"
#include "game/WorkshopSync.h"
#include "Text.h"

namespace WorldSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Protocol::IsShareableRef;

		// What waits for its reference to load is applied when the game says it loaded (refLoadInbox), with
		// a full pass when the player comes out of a loading screen or changes cell. A slow sweep of a few
		// entries at a time catches anything else (a reference that loaded before its change came).
		constexpr auto        SWEEP_INTERVAL = 2s;
		constexpr std::size_t SWEEP_ENTRIES = 256;

		// Every actor known to be dead this session (and whether our game may offer to run an NPC: SessionDeaths),
		// and the deaths we still have to kill locally (they weren't loaded when we heard about them, or the
		// reporter's position is still on its way), judged by where each reporter is as their states reach us.
		SessionDeaths::Ledger dead;
		DeathReports::Book    reports{ [](std::uint32_t a_player) -> std::optional<RemoteEdits::Spot> {
			const auto state = RemotePlayers::LastStateOf(a_player);
			return state ? std::optional{ RemoteEdits::SpotOf(*state) } : std::nullopt;
		} };

		// Game events can arrive on any thread, so sinks only queue IDs; Frame() does the work.
		std::mutex                 inboxLock;
		std::vector<std::uint32_t> deathInbox;
		// Deaths in deathInbox an actor dealt, and that actor (our player's own ID for theirs). Whether the kill is our
		// player's is decided in Frame(), where who runs what can be read (CopyHealth::Credited).
		using Killers = std::unordered_map<std::uint32_t, std::uint32_t>;
		Killers killInbox;
		// NPCs that began to fall (the engine's dying event, which comes as the fall starts and the death event at its end),
		// and who killed them, the same way: ReportFalling.
		std::vector<std::uint32_t> dyingInbox;
		Killers                    dyingKillers;
		constexpr std::size_t      MAX_DYING = 256;
		// NPCs we run whose last health a friend's blow took (ApplyRemoteDamage), and when: their fall is left to the death
		// event. A plugin's only (a spawn's fall isn't reported early); an entry no fall took (an essential NPC kneels,
		// the NPC unloads) goes after FRIENDS_BLOW_KEEP, so a long session doesn't fill it.
		std::unordered_map<std::uint32_t, Clock::time_point> friendsBlows;
		constexpr auto                                       FRIENDS_BLOW_KEEP = std::chrono::seconds(30);
		// Deaths of NPCs our game made at runtime (and whether the player caused them): Frame asks Spawns which are shared.
		std::vector<std::pair<std::uint32_t, bool>> runtimeDeathInbox;
		constexpr std::size_t                       MAX_RUNTIME_DEATHS = 256;
		// Actors whose 3D loaded (true) or unloaded (false), in order (the host checks them for deaths no event reported).
		std::vector<std::pair<std::uint32_t, bool>> loadInbox;
		// Every shareable reference whose 3D loaded (any kind: containers, loose items and doors too), for
		// what waits for it to load.
		std::vector<std::uint32_t> refLoadInbox;
		std::atomic<std::uint32_t>        playerKills{ 0 };  // anything the player killed, shared or not
		// Hits by an actor (our player's, or any other's) on NPCs every game has. Which are ours to report is decided
		// in Frame(), where who runs what can be read (CopyHealth::Judge).
		struct Hit
		{
			std::uint32_t target = 0;
			std::uint32_t cause = 0;
			CopyHealth::Blow blow;                // what it was (CopyHealth::OwnersToDeal, its explosion judged in Frame)
			std::uint32_t    sourceRef = 0;       // the reference the hit names as its source (CopyWatch::SourceOf)
		};
		constexpr std::size_t MAX_HIT_INBOX = 512;  // other actors' hits a frame (a big fight); our player's are always kept
		std::vector<Hit>      hitInbox;
		// Items the player moved between their inventory and a container or an NPC. Whether one is the
		// session's is decided in Frame(), on the main thread, where the references can be read.
		using Move = LootSync::Move;
		std::vector<Move> lootInbox;
		// Trading moves everything through ordinary transfers against the vendor's chest or the vendor
		// (BarterMenu::CompleteTrade), so moves while the barter menu is open (and just after) are trades:
		// vendors and caps are each player's. Set from the menu event's thread.
		constexpr auto          BARTER_TAIL = 1s;
		std::atomic<bool>       bartering{ false };
		std::atomic<Clock::rep> barterClosedAt{ 0 };
		std::atomic<std::uint32_t> barterMoves{ 0 };
		std::vector<std::uint32_t>             pickupInbox;  // base forms picked up from the world
		// Plants our player picked (FloraShare), by reference: reported as pickups in Frame().
		constexpr std::size_t      MAX_HARVEST_INBOX = 64;
		std::vector<std::uint32_t> harvestInbox;
		std::uint32_t              harvestsSent = 0;
		std::uint32_t              harvestsApplied = 0;
		struct StandInHit
		{
			std::uint32_t actor;
			float         damage;
			bool          byPlayer;  // we hit them, rather than an NPC in our world
			std::uint32_t attacker;  // the NPC that hit them (0 when byPlayer)
			bool          explosion = false;  // an explosion's blow (judged in Frame: RemoteEdits::PassStandInHit)
			std::uint32_t sourceRef = 0;      // the reference the hit names as its source (CopyWatch::SourceOf)
		};
		std::vector<StandInHit> standInHits;
		std::atomic<bool>       friendlyFire{ false };

		struct Activation
		{
			std::uint32_t     ref;
			std::uint32_t     base;
			Clock::time_point time{};
		};
		std::vector<Activation> activateInbox;
		// Doors an NPC opened (TESActivateEvent: the door, and the NPC as Activation::base), sorted out in Frame()
		// (DoorShare::WatchesNpcOpen).
		std::vector<Activation> npcDoorInbox;
		std::uint32_t           npcDoorsWatched = 0;

		// Recent local activations and world pickups, matched up in Frame() (either can arrive first).
		constexpr auto          MATCH_WINDOW = 2s;
		std::vector<Activation> recentActivations;
		std::vector<Activation> unmatchedPickups;  // ref unused

		// Doors and lockable things the player used recently. Their open/lock state is reported
		// whenever it changes while watched (lockpicking can take a while, hence the long window).
		constexpr auto WATCH_TIME = 5min;
		constexpr std::size_t MAX_PENDING_HEALTH = 512;  // health reports waiting for their actor to load  // hacking a terminal and walking its menus, or a long lockpick, can take minutes
		struct Watched
		{
			Protocol::RefState lastSent;        // what the session last heard of it from us, or took as where it stands
			Clock::time_point  until;
			bool               quiet = false;  // on watch because it is loaded (DoorShare's poll), not used: its first read isn't reported, and it is read by the poll's sweep only
			bool               used = false;   // the player used it since its state was last sent (or taken as the poll's baseline): it is sent once, changed or not
			bool               held = false;   // a change not reported yet (read too far from it for a friend to take, or paced), kept for when it may go
			DoorShare::Pace    pace;           // how often it was reported lately
		};
		std::unordered_map<std::uint32_t, Watched>           watched;
		std::uint32_t                                        linkedWatched = 0;  // linked refs (a terminal's door) put on watch
		// Every loaded door, and when it was found loaded: by the game's load event (TESObjectLoadedEvent), which names
		// few of them, and by the walk of the attached cells (FindDoors), which names the rest. Read by a sweep every
		// DoorShare::POLL_INTERVAL while it stays loaded, DoorShare::POLL_PER_FRAME a frame: a plain one is put on watch,
		// so what a script does to it is shared as a player's use is. Kept across a load or a disconnect (a door the
		// world keeps loaded fires no new event); one gone from the world is dropped when read.
		std::unordered_map<std::uint32_t, Clock::time_point> loadedDoors;
		std::vector<std::uint32_t>                           doorSweep;  // the loaded doors as the sweep began, read in turn
		std::size_t                                          doorSweepAt = 0;
		Clock::time_point                                    nextDoorPoll{};
		DoorShare::Walk                                      doorWalk;        // the walk of the attached cells for the doors the load event didn't name (FindDoors)
		std::uint32_t                                        doorsFound = 0;  // doors the walk put on the list that no load event had
		std::uint32_t                                        doorsPolled = 0;  // doors put on watch by the poll
		std::uint32_t                                        reportsHeld = 0;  // changes read too far from the thing for a friend to take them, kept for when we are near
		std::uint32_t                                        reportsPaced = 0;  // changes held as the thing was reported DoorShare::REPORT_BURST times lately
		long long                                            doorPollUs = 0, doorPollUsMax = 0;  // the sweep's cost in a frame: the last, the most
		std::uint32_t                                        refStatesApplied = 0;  // lock/open changes taken from other players

		// Activators (buttons, switches, levers) pressed in another player's game are pressed here too, by
		// their stand-in (so our own watcher, which takes only the player's presses, never reports it back).
		std::uint32_t                                          activationsSent = 0, activationsApplied = 0, activationsRefused = 0;
		std::uint32_t                                          refStatesRefused = 0, corpsesMoved = 0;
		std::uint32_t                                          restsReported = 0, restsApplied = 0;  // where corpses came to rest after an early report: sent, taken
		constexpr float                                        CORPSE_SLACK = 96.0f;  // a corpse this close to where it fell for the reporter stays put
		constexpr float                                        ACTIVATION_REACH = 1500.0f;  // a button is pressed from arm's length; this allows for lag and a stand-in that is a step behind
		std::unordered_map<std::uint32_t, Protocol::RefState> pendingRefStates;
		DoorShare::Book                                        refBook;      // what the session last said of each door: a world state's repeats aren't applied again
		std::unordered_map<std::uint32_t, bool>                opensShared;  // per reference with an open state: whether a player's opening it is shared
		std::unordered_set<std::uint32_t>                      opensUnlogged;  // ... of them, ones worked out by the poll and not logged (logged once the player uses one)
		std::unordered_set<std::uint32_t>                      doorRefsOut;  // DoorShare::NEVER, by this game's IDs
		bool                                                   doorListResolved = false;

		// Security gates and the like, run by the game's two-state script (SharedGates): their open state is shared as a
		// door's is, and applied here through the script.
		std::unordered_map<std::uint32_t, bool>              gates;         // per loaded activator: whether it is a shared gate
		std::unordered_map<std::uint32_t, std::string_view>  gateScripts;   // per shared gate: the script it runs by (SharedGates::RunningScript)
		std::unordered_map<std::uint32_t, Clock::time_point> gateLoads;     // activators the game loaded in the last moments (SharedGates::LOAD_SETTLE)
		SharedGates::Applied                                 gatesApplied;  // gates we set from another player's state, until they get there
		std::unordered_set<std::uint32_t>                    gateBasesOut, gateRefsOut;  // SharedGates::NOT_A_WAY and NEVER, by this game's IDs
		Clock::time_point                                    applyingSince{};            // since when remote changes could be applied, without a break
		bool                                                 wasApplying = false;
		std::uint32_t                                        gatesSet = 0, gateReportsHeld = 0, gatePressesLeft = 0;

		// Doors only a quest stage opens (ScriptDoors: Vault 114's gear door), and the Prydwen moored at Boston Airport,
		// which a stage enables: the stage is shared, and set here once its door is loaded (the Prydwen's at once).
		ScriptDoors::Book          scriptDoors;
		std::vector<std::uint32_t> scriptDoorQuests;  // ScriptDoors::DOORS[i]'s quest by this game's ID (0: not in this game)
		std::vector<std::uint32_t> scriptDoorRefs;    // ... and its door
		std::vector<std::uint32_t> scriptDoorEffects;  // ... and the effect whose own flight sets the stage here (0: none)
		bool                       scriptDoorsReported = false;  // the host's open ones, since its welcome or the last load
		Clock::time_point          nextDoorLook{};
		std::uint32_t              scriptDoorsSent = 0, scriptDoorsSet = 0, scriptDoorsGaveUp = 0;
		constexpr auto             DOOR_LOOK_INTERVAL = 500ms;

		// Container changes are numbered by the server. Everything before the ledger's next number is in
		// our world already or waits in the loot's queue (its container isn't loaded, or an NPC lives here);
		// changes that came early wait in the ledger for those before them. The session and that number
		// belong to the world rather than the connection, so they are kept in the save (see
		// SaveResyncPoint). The ledger also knows our player IDs in the session (our own changes are in our
		// world already): a reconnect normally gets the old one back (the server knows us by
		// Protocol::Hello::identity), but not while the server still holds the old connection. Session moves
		// it through loads, hellos and welcomes with SessionSteps.
		LootSync::Loot    loot;
		bool              lootFullWarned = false;
		Clock::time_point lootAgedAt{};  // when the wait for living NPCs was last counted (see AgeLoot)

		// Vendors' chests (each faction's merchant container): trading stays in each player's game.
		std::unordered_set<std::uint32_t> vendorChests;

		std::unordered_set<std::uint32_t> pendingPickups;
		// Pending pickups whose item was found in no cell, and since when (see TryApplyPickup).
		std::unordered_map<std::uint32_t, Clock::time_point> pickupUnplacedSince;
		// Every pickup we were told of this session (for how full its history is; see NoticeHistory).
		std::unordered_set<std::uint32_t> pickedUpKnown;
		std::uint64_t                     historyNoticedIn = 0;  // the session the player was told of (once a session)
		std::uint32_t                     personalSkipped = 0;  // pickups of personal or quest items left alone (container changes: loot's)

		// Actors we hit last frame; their resulting health is reported this frame, once the
		// damage has been applied.
		std::unordered_set<std::uint32_t> hitLastFrame;
		// Health values from other players for actors that weren't loaded yet.
		std::unordered_map<std::uint32_t, float> pendingHealth;

		// Events to send. A save or a load can flush loot into it from its own thread when the main thread
		// doesn't come (see main.cpp RunBeforeSaveOrLoad), hence the lock.
		std::mutex                             outgoingLock;
		std::vector<std::vector<std::uint8_t>> outgoing;
		// When what waits (WaitingIds) is tried again.
		RetrySchedule                          retrySchedule{ SWEEP_INTERVAL, SWEEP_ENTRIES };
		std::uint32_t                          loadApplied = 0;  // waiting changes applied as their reference loaded
		std::int64_t                           retryUs = 0, retryUsMax = 0;  // the last and longest retry pass, in microseconds
		std::uint32_t                          applied = 0;
		std::uint32_t                          reported = 0;
		// The host's world is the session's: an actor already dead in it (killed before the session, or
		// while nobody was joined, and saved that way) is dead for everyone, though no death event says so.
		// A joiner's fresh world had bloatflies alive and fighting that the host's save had dead.
		// What our game has loaded is known from its load and unload events. When we host, an actor is
		// checked as it loads, and everything loaded is checked again whenever a session starts or a save
		// is loaded: the bloatflies' corpses had loaded during an earlier session, and nothing loaded them
		// again. An NPC another player's game takes over is checked too (OnActorOwners).
		// Limit: only what is in our game's memory can be checked. Persistent actors always are, and so are
		// those of cells loaded lately; an ordinary NPC of a cell our game hasn't loaded isn't, and lives on
		// in a friend's game until ours loads it (then it dies there too: the server takes our found-dead
		// report over its runner's word, see Server.cpp handleDeath). It can only fight our
		// stand-in where we are, and there our game has it loaded.
		// With everyone on their own story, story characters are left out (FoundDead::Shared).
		std::unordered_set<std::uint32_t>      loadedActors;  // their 3D is loaded, by the events (FoundDead::TrackLoad)
		std::unordered_set<std::uint32_t>      toCheck;       // for a death no event reported, once we can
		bool                                   checkAllLoaded = true;
		std::uint32_t                          foundDead = 0;
		std::uint32_t                          ownStoryDead = 0;  // found dead but left alone: a story character, with everyone on their own story
		NpcHold::Book                          falling;           // dead here as we checked, but falling or just hit by us: left to the death event
		std::uint32_t                          leftToEvent = 0;   // such checks put off

		void Queue(std::vector<std::uint8_t> a_packet)
		{
			std::scoped_lock l{ outgoingLock };
			outgoing.push_back(std::move(a_packet));
		}

		void ReportDead(const Protocol::ActorDeath& a_death)
		{
			Queue(Protocol::Encode(a_death, Protocol::MessageType::kReportDeath));
			++reported;
		}

		// A loaded, real (not a stand-in) actor, or nullptr.
		RE::Actor* LoadedActor(std::uint32_t a_refId)
		{
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_refId);
			if (!actor || actor->IsDeleted() || actor->IsPlayerRef() || Puppets::IsPuppet(actor) || !actor->Get3D() || !actor->GetParentCell()) {
				return nullptr;
			}
			return actor;
		}

		// The id the wire names one of our actors by: a plugin's reference's own, or the spawn id a runtime NPC is shared
		// under (Spawns: ours, or our copy of a friend's). 0: none.
		std::uint32_t WireId(std::uint32_t a_local)
		{
			return IsShareableRef(a_local) ? a_local : Spawns::IdOf(a_local);
		}

		FoundDead::Npc DescribeDead(RE::Actor* a_actor);

		std::uint32_t                     downNotDead = 0;    // a death event or a found-dead check on one that only went down (not reported)
		std::uint32_t                     essentialHolds = 0;  // deaths heard of for one essential here: held, not applied
		std::uint32_t                     damageReported = 0;  // what our hits took off copies, sent (kReportDamage)
		std::uint32_t                     damageApplied = 0;   // friends' damage taken off NPCs we run (kActorDamaged)
		std::uint32_t                     damageIgnored = 0;   // ... that came for one we don't run (its runner takes it off)
		std::uint32_t                     damageDropped = 0;   // ... dropped for good for any other reason (DamageHold::Why)
		std::uint32_t                     damageLate = 0;      // ... taken off after a hold (DamageHold)
		// Friends' damage on NPCs we run that can't be taken off yet (DamageHold): a loading screen, the server's word
		// that we run it on its way, the NPC not loaded here yet. Tried again every frame in the world, before the
		// NPC's health is shared.
		DamageHold::Book heldDamage;
		bool             heldInWorld = false;  // whether the last frame could apply (the hold restarts as the world comes back)
		LogPace::Pace    heldDamageLog, droppedDamageLog;
		// Health reports (a friend's, or our own hit's) that go nowhere because the session has the NPC dead, and the
		// deaths told again that killed a copy alive here after all (TakeDeath).
		std::uint32_t healthDropped = 0;
		LogPace::Pace droppedHealthLog;
		LogPace::Pace refStateLog;  // the lock lines of a friend's door state applied here
		std::uint32_t killsRetried = 0;
		std::unordered_set<std::uint32_t> essentialHeld;       // those, logged once each

		// Down, and gets up again (SessionDeaths::GetsUp: Dogmeat at the end of his health): alive for the session.
		bool GetsUp(RE::Actor* a_actor)
		{
			return SessionDeaths::GetsUp(static_cast<std::int32_t>(static_cast<RE::ActorState&>(*a_actor).lifeState), IsEssential(a_actor));
		}

		// Dead in our game, not just down (the game's IsDead answers true for one that is down).
		bool DeadHere(RE::Actor* a_actor)
		{
			return a_actor->IsDead(false) && !GetsUp(a_actor);
		}

		// Where an actor of our world is, as players' states name places.
		RemoteEdits::Spot SpotOf(const RE::Actor* a_actor)
		{
			const auto cell = a_actor->GetParentCell();
			const auto& at = a_actor->data.location;
			if (cell && cell->IsInterior()) {
				return { cell->GetFormID(), 0, at.x, at.y, at.z };
			}
			return { 0, cell && cell->worldSpace ? cell->worldSpace->GetFormID() : 0, at.x, at.y, at.z };
		}

		std::uint32_t                     storyKillsKept = 0;  // story characters a friend killed away from us: alive here (own story)
		std::unordered_set<std::uint32_t> storyKillsLogged;
		std::uint32_t                     switchedOnToKill = 0;  // off here, on and killed in the reporter's game: switched on to die
		std::unordered_set<std::uint32_t> switchOnTried;

		// Moves our copy to where the corpse lies in the reporter's game (a_at: all zero, unknown), unless it is close
		// enough already or far from ours: a report doesn't pull an NPC across the map.
		void PlaceCorpse(RE::Actor* a_actor, const RE::NiPoint3& a_at)
		{
			const bool  placed = a_at.x != 0.0f || a_at.y != 0.0f || a_at.z != 0.0f;
			const float apart = a_actor->data.location.GetDistance(a_at);
			if (placed && apart > CORPSE_SLACK && RemoteEdits::MayMoveCorpse(apart)) {
				const auto id = a_actor->GetFormID();
				RE::Console::ExecuteCommand(std::format("{:08X}.setpos x {:.1f}", id, a_at.x).c_str());
				RE::Console::ExecuteCommand(std::format("{:08X}.setpos y {:.1f}", id, a_at.y).c_str());
				RE::Console::ExecuteCommand(std::format("{:08X}.setpos z {:.1f}", id, a_at.z).c_str());
				NpcSync::CountCommands(3);
				++corpsesMoved;
			}
		}

		// Kills an actor that died in another player's world. Returns true once it is dead here
		// (or can never be, or is not to be), false to try again later.
		// a_inWorld (each Try* takes it): CanApply(), worked out once by the caller for all it tries.
		bool TryKill(std::uint32_t a_refId, const DeathReports::Report& a_death, bool a_inWorld)
		{
			const RE::NiPoint3 at{ a_death.x, a_death.y, a_death.z };
			if (!a_inWorld) {
				return false;
			}
			const auto form = RE::TESForm::GetFormByID(a_refId);
			if (form && !form->As<RE::Actor>()) {
				return true;  // not an actor: nothing to kill
			}
			const auto actor = form ? form->As<RE::Actor>() : nullptr;
			if (actor && (actor->IsPlayerRef() || Puppets::IsPuppet(actor))) {
				return true;
			}
			if (actor && DeadHere(actor)) {
				dead.SawDead(a_refId);
				return true;
			}
			// Off here, while the reporter's game switched it on and it died there before any state of it reached us (a
			// script enabled it: the mole rats that burrow up at Red Rocket): switched on once its cell is drawn, as NpcSync
			// does for one whose owner's copy runs, so the kill finds it and leaves a corpse here too. Off, it has no 3D and
			// the kill waited for good. Never a character of our own story (RemoteEdits::MaySwitchOn); tried once.
			if (actor && actor->IsDisabled() && !actor->IsDeleted()) {
				const auto cell = actor->GetParentCell();
				if (RemoteEdits::SwitchOnToKill(true, false, cell && cell->IsAttached(), Story::Shared(), DescribeStory(actor)) && switchOnTried.insert(a_refId).second) {
					RE::Console::ExecuteCommand(std::format("{:08X}.enable", a_refId).c_str());
					NpcSync::CountCommands(1);
					++switchedOnToKill;
					REX::INFO("WorldSync: switching {:08X} on to kill it: it is off here, and died in player {}'s game", a_refId, a_death.reporter);
				}
				return false;  // killed once it has loaded
			}
			if (!LoadedActor(a_refId)) {
				return false;  // not loaded yet; try again when its cell loads
			}
			// With everyone on their own story, a unique or essential character that died in a friend's game nowhere
			// near the friend (a quest of theirs killed it, or the report is made up) lives on here: ours is then
			// ours alone (the session has it dead, so nobody runs it for us and no health comes). It leaves the dead
			// we know of, so if we kill it ourselves later our report goes out like any other.
			// A report whose reporter seems nowhere near waits a moment first: their position may be behind it.
			switch (reports.Judge(Story::Shared(), a_death, { DescribeDead(actor), SpotOf(actor) }, Clock::now())) {
			case DeathReports::Verdict::kWait:
				return false;
			case DeathReports::Verdict::kKeepAlive:
				dead.Forget(a_refId);
				++storyKillsKept;
				if (storyKillsLogged.insert(a_refId).second) {
					REX::INFO("WorldSync: {:08X} died in player {}'s game with them nowhere near it; a character of our own story, it lives on here", a_refId, a_death.reporter);
				}
				return true;
			case DeathReports::Verdict::kKill:
				break;
			}
			// Essential here: the kill would only knock it down, and the death would pass for applied. It is held, and
			// tried again: a quest that killed it in the reporter's game can keep it essential in ours a while, and once
			// the flag goes it dies here too. Meanwhile our game may offer to run it, marked essential (MayClaim): from
			// the host's game that undoes a death an older build saved by mistake (Dogmeat, taken for dead as he went
			// down), and the owner the server then gives it tells us it lives (OnActorOwners).
			if (IsEssential(actor)) {
				if (essentialHeld.insert(a_refId).second) {
					++essentialHolds;
					REX::INFO("WorldSync: {:08X} is essential here; its death waits until it can die here", a_refId);
				}
				return false;
			}

			// The console kill goes through the engine's normal death path (death animation,
			// ragdoll, quest/script death events), exactly as if it happened here.
			// The corpse lies where it fell in the reporter's game (each game's copy stood somewhere else when
			// the report came; a far-off corpse is confusing and its loot is in the wrong place), unless that is
			// far from ours: a report doesn't pull an NPC across the map.
			PlaceCorpse(actor, at);
			RE::Console::ExecuteCommand(std::format("{:08X}.kill", a_refId).c_str());
			NpcSync::CountCommands(1);
			++applied;
			// Killed by a friend: their stand-in gives the corpse the push it got in the friend's game, so it
			// ragdolls away instead of dropping flat.
			if (const auto standIn = a_death.killer ? RemotePlayers::ActorIdOf(a_death.killer) : 0) {
				// The calling reference is the source of the push and the argument is what flies: the stand-in
				// pushes the corpse (the other way round ragdolled the stand-in).
				RE::Console::ExecuteCommand(std::format("{:08X}.pushactoraway {:08X} 3", standIn, a_refId).c_str());
				NpcSync::CountCommands(1);
			}
			REX::INFO("WorldSync: killed {:08X} (died in another player's world)", a_refId);
			return true;
		}

		void TakeDeath(std::uint32_t a_refId, const DeathReports::Report& a_death, bool a_inWorld)
		{
			if (!IsShareableRef(a_refId)) {
				return;
			}
			const auto now = Clock::now();
			if (dead.Add(a_refId, now, a_death.reporter)) {
				if (!TryKill(a_refId, a_death, a_inWorld)) {
					reports.Hold(a_refId, a_death);
				}
				return;
			}
			// Known dead already, and told again: the session's reminder, sent when our game offered to run it or hit it
			// alive (its cell reset here while the session, which the host's world alone brings back, has it dead), or a
			// replay. A copy alive and loaded here is killed after all, unless its death is held already (killed as it
			// loads) or was learned of a moment ago (that kill is landing).
			if (!a_inWorld || reports.Holds(a_refId) || !dead.KnownFor(a_refId, SessionDeaths::KILL_RETRY_AFTER, now)) {
				return;
			}
			const auto actor = LoadedActor(a_refId);
			if (!actor || DeadHere(actor)) {
				return;
			}
			++killsRetried;
			REX::INFO("WorldSync: {:08X} is alive here though the session has it dead (told again); killing it after all", a_refId);
			if (!TryKill(a_refId, a_death, a_inWorld)) {
				reports.Hold(a_refId, a_death);
			}
		}

		// Who dealt a death, as Frame takes it (Killers): our player's own ID for theirs, another actor's for its.
		void NoteKiller(Killers& a_killers, std::uint32_t a_id, RE::TESObjectREFR* a_dying, RE::TESObjectREFR* a_killer, bool a_byPlayer)
		{
			if (a_byPlayer) {
				a_killers[a_id] = Protocol::PLAYER_REF_ID;
			} else if (a_killer && a_killer->As<RE::Actor>() && a_killer != a_dying) {
				a_killers[a_id] = a_killer->GetFormID();
			}
		}

		class DeathWatcher :
			public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent& a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				const auto ref = a_event.actorDying.get();
				if (!ref) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto id = ref->GetFormID();
				const auto actor = ref->As<RE::Actor>();
				const auto killer = a_event.actorKiller.get();
				const bool byPlayer = killer && killer->IsPlayerRef() && !ref->IsPlayerRef();
				if (actor && Puppets::IsPuppet(actor)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// The dying event, as the fall begins: one our game runs may be reported dead now (ReportFalling), with the
				// killer this event names. Its death event follows at the end of the fall.
				if (!a_event.dead) {
					if (actor && IsShareableRef(id)) {
						std::scoped_lock l{ inboxLock };
						if (dyingInbox.size() < MAX_DYING) {
							dyingInbox.push_back(id);
							NoteKiller(dyingKillers, id, ref, killer, byPlayer);
						}
					}
					return RE::BSEventNotifyControl::kContinue;
				}
				if (byPlayer) {
					++playerKills;
				}
				if (!IsShareableRef(id)) {
					if (actor && Protocol::IsSpawnRef(id)) {
						// Our copy of a friend's spawn, maybe: watched like any other copy.
						WeaponFire::NoteDeath(id);
						CopyWatch::NoteDeath(id, killer ? killer->GetFormID() : 0);
						std::scoped_lock l{ inboxLock };
						if (runtimeDeathInbox.size() < MAX_RUNTIME_DEATHS) {
							runtimeDeathInbox.emplace_back(id, byPlayer);
						}
					}
					return RE::BSEventNotifyControl::kContinue;
				}
				WeaponFire::NoteDeath(id);
				CopyWatch::NoteDeath(id, killer ? killer->GetFormID() : 0);
				std::scoped_lock l{ inboxLock };
				deathInbox.push_back(id);
				NoteKiller(killInbox, id, ref, killer, byPlayer);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		DeathWatcher deathWatcher;

		// Only the ID is taken here: the event comes from whichever thread loaded the actor.
		class LoadWatcher :
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent& a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (IsShareableRef(a_event.formID)) {
					if (a_event.loaded) {
						std::scoped_lock l{ inboxLock };
						refLoadInbox.push_back(a_event.formID);
					}
					const auto form = RE::TESForm::GetFormByID(a_event.formID);
					if (form && form->GetFormType() == RE::ENUM_FORM_ID::kACHR) {
						std::scoped_lock l{ inboxLock };
						loadInbox.emplace_back(a_event.formID, a_event.loaded);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		LoadWatcher loadWatcher;

		void TrackLoads(const std::vector<std::pair<std::uint32_t, bool>>& a_events)
		{
			const auto now = Clock::now();
			for (const auto& [id, isLoaded] : a_events) {
				if (!isLoaded) {
					loadedActors.erase(id);
					continue;
				}
				toCheck.insert(id);
				// Killed here a while ago and loaded alive: its cell reset (days away, or a long sleep). It is no longer
				// dead for us, so our game offers to run it again; the host's game doing so brings it back for everyone.
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
				const bool alive = actor && !actor->IsDeleted() && actor->Get3D() && !DeadHere(actor);
				if (dead.OnLoaded(id, alive, reports.Holds(id) || pendingHealth.contains(id), now)) {
					REX::INFO("WorldSync: {:08X} is alive again here (its cell reset); no longer dead for us", id);
				} else if (actor && DeadHere(actor)) {
					dead.SawDead(id);  // its corpse
				}
				FoundDead::TrackLoad(loadedActors, id, [](std::uint32_t a_id) {
					const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
					return actor && actor->Get3D();
				});
			}
		}

		// A quest holds the reference in an alias: a quest item, or a character a quest is about.
		bool InQuestAlias(RE::TESObjectREFR* a_ref)
		{
			return a_ref->extraList && a_ref->extraList->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
		}

		FoundDead::Npc DescribeDead(RE::Actor* a_actor)
		{
			const auto base = a_actor->GetObjectReference();
			const auto npc = base ? base->As<RE::TESNPC>() : nullptr;
			return { npc && npc->IsUnique(), npc && (npc->IsEssential() || npc->IsProtected()), InQuestAlias(a_actor) };
		}

		// When we host, reports the actors to check that are dead in our world and not known to be yet.
		void CheckFoundDead(bool a_hosting)
		{
			if (!a_hosting) {
				// Not ours to say. Once we host (again), everything loaded is checked: a new session knows nothing.
				toCheck.clear();
				falling.Clear();
				checkAllLoaded = true;
				return;
			}
			if (!InWorld() || Story::InOpening()) {
				return;
			}
			if (checkAllLoaded) {
				checkAllLoaded = false;
				toCheck.insert(loadedActors.begin(), loadedActors.end());
			}
			const auto                 now = Clock::now();
			std::vector<std::uint32_t> later;
			for (const auto id : toCheck) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
				if (!actor || actor->IsDeleted() || !actor->IsDead(false) || actor->IsPlayerRef() || Puppets::IsPuppet(actor) || dead.Contains(id)) {
					continue;
				}
				if (!DeadHere(actor)) {
					++downNotDead;  // down, and gets up again
					continue;
				}
				// Falling as we look, or our player hit it lately: not found dead but dying here, and its death event
				// reports it as a death seen happen, our kill. Our hit on a copy a friend's game runs asks to take it over,
				// and the server's answer (it stays theirs) brings it here the frame the killing blow lands: reported
				// found dead, the kill was not believed (only the host's game, and only of what it never had alive), and
				// the death event that followed was taken for the same death. Every host's kill of a friend's NPC was
				// lost so. Checked again each frame, for at most NpcHold::DYING, then found dead after all.
				const bool fallingNow = static_cast<RE::ActorState&>(*actor).lifeState == static_cast<std::uint32_t>(RE::ACTOR_LIFE_STATE::kDying);
				if (NpcHold::LeaveToEvent(falling, id, fallingNow, NpcSync::FoughtLately(id), now)) {
					later.push_back(id);
					continue;
				}
				if (!FoundDead::Shared(Story::Shared(), DescribeDead(actor))) {
					++ownStoryDead;
					continue;
				}
				dead.Add(id, Clock::now());
				dead.SawDead(id);
				dead.Reported(id);
				// Said to be found dead, so the server takes it over the NPC's runner (see Server.cpp, handleDeath).
				// No position: a friend's copy dies where it stands. Ours can lie in another cell than theirs
				// (an NPC that walks about, alive in their game), and coordinates in one cell are nowhere in
				// another.
				ReportDead({ .refId = id, .foundDead = true });
				++foundDead;
				reports.Forget(id);
				pendingHealth.erase(id);
			}
			for (const auto id : toCheck) {
				if (!std::ranges::contains(later, id)) {
					falling.Forget(id);
				}
			}
			leftToEvent += static_cast<std::uint32_t>(later.size());
			toCheck.clear();
			toCheck.insert(later.begin(), later.end());
		}

		const RE::ActorValueInfo* HealthInfo()
		{
			const auto values = RE::ActorValue::GetSingleton();
			return values ? values->health : nullptr;
		}

		// Sets a loaded actor's current health to a fraction of its maximum. Returns false to try again later.
		bool TrySetHealth(std::uint32_t a_refId, float a_fraction, bool a_inWorld)
		{
			if (!a_inWorld) {
				return false;
			}
			const auto info = HealthInfo();
			const auto actor = LoadedActor(a_refId);
			if (!info || !actor) {
				return false;
			}
			if (actor->IsDead(false)) {
				return true;
			}
			auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
			const float maximum = values.GetPermanentActorValue(*info);
			const float before = values.GetActorValue(*info);
			// On a copy, a raise that may be from before our latest hits is held back (CopyHealth::Book::Heard).
			const float target = CopyWatch::HealthToSet(a_refId, before, std::clamp(a_fraction, 0.0f, 1.0f) * maximum);
			const float delta = target - before;
			if (delta < 0.0f) {
				values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, delta);
			} else if (delta > 0.0f) {
				values.RestoreActorValue(*info, delta);
			}
			CopyWatch::HealthSet(a_refId, values.GetActorValue(*info));
			return true;
		}

		// The health of the NPCs our game runs, as the friends last heard it (HealthShare): what changed is reported.
		HealthShare::Book healthShare;

		// Reports the health of every loaded NPC our game runs that changed since the session last heard it, by
		// anything (a fire, a burn, poison: no hit of ours reports those), a few times a second at most for each.
		void ShareHealth(const RE::ActorValueInfo& a_info, Clock::time_point a_now)
		{
			const auto local = NpcSync::LocalPlayer();
			const auto lists = RE::ProcessLists::GetSingleton();
			if (local == 0 || !lists) {
				healthShare.Clear();
				return;
			}
			std::unordered_set<std::uint32_t> seen;
			for (const auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor->IsPlayerRef() || actor->IsDeleted() || Puppets::IsPuppet(actor) || actor->IsDead(false) || !actor->Get3D() || !actor->GetParentCell()) {
					continue;
				}
				const auto id = actor->GetFormID();
				const auto wire = WireId(id);
				if (wire == 0 || dead.Contains(id) || NpcSync::Owner(wire) != local) {
					continue;
				}
				const auto& values = static_cast<RE::ActorValueOwner&>(*actor);
				const float maximum = values.GetPermanentActorValue(a_info);
				if (maximum <= 0.0f) {
					continue;
				}
				seen.insert(wire);
				const float fraction = std::clamp(values.GetActorValue(a_info) / maximum, 0.0f, 1.0f);
				if (healthShare.Due(wire, fraction, a_now)) {
					Queue(Protocol::Encode(Protocol::ActorHealth{ wire, fraction }, Protocol::MessageType::kReportHealth));
				}
			}
			healthShare.Prune([&](std::uint32_t a_wire) { return seen.contains(a_wire); }, a_now);
		}

		std::atomic<std::uint32_t> hitsOnUsByPuppets{ 0 };  // hits on our player by a friend's stand-in (HitWatcher)
		std::uint32_t              standInBlowsThere = 0;  // explosions' hits on stand-ins not passed on: they go off in that game too

		class HitWatcher :
			public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				// Any hit, by anything (a hazard's has no cause): our copies of NPCs other games run are watched.
				CopyWatch::NoteHit(a_event);
				const auto target = a_event.target.get();
				const auto cause = a_event.cause.get();
				if (!target || !cause) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// An NPC in our world (or we, with friendly fire on) hit another player's stand-in:
				// that player takes the damage. Hits on mirrored NPCs are dropped in Frame(). One made
				// at runtime (random encounters, spawned enemies) is passed on only if our game shares
				// it (Spawns: the victim's game has a copy of it), as Frame() finds.
				const bool byPlayer = cause->IsPlayerRef();
				const auto attacker = byPlayer ? 0 : cause->GetFormID();
				// A friend's stand-in replays their attacks and must hit nobody; one
				// that hits our player is logged, with what it hit with, for the rig.
				if (target->IsPlayerRef() && !byPlayer) {
					const auto by = cause->As<RE::Actor>();
					if (by && Puppets::IsPuppet(by)) {
						const auto n = ++hitsOnUsByPuppets;
						if (n <= 20) {
							REX::INFO("WorldSync: our player was hit by puppet {:08X} (weapon {:08X}, projectile {:08X}, damage {:.1f}, flags {:08X}); {} so far", attacker,
								a_event.hitData.weapon.object ? a_event.hitData.weapon.object->GetFormID() : 0, a_event.projectileFormID,
								a_event.usesHitData ? a_event.hitData.totalDamage : -1.0f, a_event.usesHitData ? a_event.hitData.flags.underlying() : 0u, n);
						}
					}
				}
				// Our player shot a loose object: where it comes to rest is ours to report (MovedObjects). Or a destructible one
				// (a car, a barrel): the stages it passes are (Destructibles).
				if (byPlayer && !target->As<RE::Actor>()) {
					MovedObjects::OnHit(target->GetFormID());
					Destructibles::OnHit(target->GetFormID());
				}
				const auto victim = target->As<RE::Actor>();
				if (victim && Puppets::IsPuppet(victim) && (byPlayer || IsShareableRef(attacker) || Protocol::IsSpawnRef(attacker))) {
					if ((!byPlayer || friendlyFire) && a_event.usesHitData) {
						const float damage = a_event.hitData.healthDamage > 0.0f ? a_event.hitData.healthDamage : a_event.hitData.totalDamage;
						if (damage > 0.0f) {
							std::scoped_lock l{ inboxLock };
							standInHits.push_back({ victim->GetFormID(), damage, byPlayer, attacker, a_event.hitData.flags.any(RE::HitData::Flag::kExplosion), CopyWatch::SourceOf(a_event) });
						}
					}
				}
				// Only damage dealt to others (not the attacker itself); each game reports its own hits: our player's, and
				// those of the actors our game runs (decided in Frame()). One made at runtime
				// is reported if it is shared (Frame() asks Spawns).
				const auto actor = target->As<RE::Actor>();
				if (actor && target != cause && (byPlayer || cause->As<RE::Actor>()) && (IsShareableRef(target->GetFormID()) || Protocol::IsSpawnRef(target->GetFormID())) && !Puppets::IsPuppet(actor)) {
					std::scoped_lock l{ inboxLock };
					if (byPlayer || hitInbox.size() < MAX_HIT_INBOX) {
						hitInbox.push_back({ target->GetFormID(), byPlayer ? Protocol::PLAYER_REF_ID : cause->GetFormID(), CopyWatch::BlowOf(a_event), CopyWatch::SourceOf(a_event) });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitWatcher hitWatcher;

		class ContainerWatcher :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Shared storage's own changes (a friend's copy shown, a refused move undone) and Gifts' (a gift's copy, a take held
				// back or granted): never a move of the player's.
				if (Storage::Applying() || Gifts::Applying()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// Only moves the local player makes. Changes we apply for other players go between a
				// container and nothing, so they never match and never echo back.
				using Protocol::PLAYER_REF_ID;
				std::optional<Move> move;
				const auto          count = a_event.itemCount;
				if (a_event.newContainerFormID == PLAYER_REF_ID && a_event.oldContainerFormID == 0) {
					// Picked up from the world. The event doesn't say which reference (referenceFormID is 0),
					// so Frame() pairs it with the item the player just activated.
					std::scoped_lock l{ inboxLock };
					pickupInbox.push_back(a_event.baseObjectFormID);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event.oldContainerFormID == 0 && a_event.newContainerFormID != 0) {
					// From nothing into a container: in workshop mode, maybe a scrap's components in its workbench (Drops).
					Drops::OnAddedFromNowhere(a_event.newContainerFormID, a_event.baseObjectFormID, count);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event.oldContainerFormID == PLAYER_REF_ID && a_event.newContainerFormID == 0) {
					// Dropped (or used up: Drops tells them apart): an item that may be given to a friend.
					Drops::OnLeftInventory(a_event.baseObjectFormID, count, a_event.referenceFormID);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (count <= 0 || count > Protocol::MAX_ITEM_COUNT) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event.newContainerFormID == PLAYER_REF_ID && IsShareableRef(a_event.oldContainerFormID)) {
					move = Move{ a_event.oldContainerFormID, a_event.baseObjectFormID, -count };
				} else if (a_event.oldContainerFormID == PLAYER_REF_ID && IsShareableRef(a_event.newContainerFormID)) {
					move = Move{ a_event.newContainerFormID, a_event.baseObjectFormID, count };
				}
				if (!move) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// A weapon or armor put in a container may be a gift, and a take of one may be of a gift's copy: Gifts keeps
				// every move at the container its view watches (never a vendor's), so a take just after a trade isn't lost.
				Gifts::OnMoved(move->holder, move->item, move->count);
				const auto closed = Clock::time_point{ Clock::duration{ barterClosedAt.load() } };
				if (bartering || Clock::now() - closed < BARTER_TAIL) {
					++barterMoves;  // a trade
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ inboxLock };
				lootInbox.push_back(*move);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ContainerWatcher containerWatcher;

		// Only a flag is set here (the event's thread); see bartering.
		class MenuWatcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event.opening && a_event.menuName == "ContainerMenu"sv) {
					Storage::OnMenuClosed();  // a friend's storage viewed: the session's things leave our copy
					Gifts::OnMenuClosed();  // our copies of the gifts in it leave it
				}
				if (a_event.menuName == "BarterMenu"sv) {
					if (!a_event.opening) {
						barterClosedAt = Clock::now().time_since_epoch().count();
					}
					bartering = a_event.opening;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuWatcher menuWatcher;

		class ActivateWatcher :
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				const auto& target = a_event.objectActivated;
				const auto& by = a_event.actionRef;
				if (target && by && by->IsPlayerRef() && target->IsCreated()) {
					Drops::OnActivated(target->GetFormID());  // perhaps a copy of an item a friend dropped: Take asks the server
				}
				// An NPC opening a door: only taken here (the event's thread), and looked at in Frame().
				if (target && by && !by->IsPlayerRef() && by->Is(RE::ENUM_FORM_ID::kACHR) && IsShareableRef(target->GetFormID())) {
					const auto doorBase = target->GetObjectReference();
					if (doorBase && doorBase->Is(RE::ENUM_FORM_ID::kDOOR)) {
						std::scoped_lock l{ inboxLock };
						npcDoorInbox.push_back({ target->GetFormID(), by->GetFormID() });
					}
					return RE::BSEventNotifyControl::kContinue;
				}
				if (!target || !by || !by->IsPlayerRef() || !IsShareableRef(target->GetFormID())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto base = target->GetObjectReference();
				std::scoped_lock l{ inboxLock };
				activateInbox.push_back({ target->GetFormID(), base ? base->GetFormID() : 0 });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ActivateWatcher activateWatcher;

		// A plant picked: only the reference is taken here (the event's thread); see FloraShare.
		class HarvestWatcher :
			public RE::BSTEventSink<RE::TESHarvestEvent::ItemHarvested>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHarvestEvent::ItemHarvested& a_event, RE::BSTEventSource<RE::TESHarvestEvent::ItemHarvested>*) override
			{
				const auto ref = a_event.referenceHarvested;
				const auto by = a_event.harvestedBy;
				const auto id = ref ? ref->GetFormID() : 0;
				if (FloraShare::Reports(by && by->IsPlayerRef(), IsShareableRef(id))) {
					std::scoped_lock l{ inboxLock };
					if (harvestInbox.size() < MAX_HARVEST_INBOX) {
						harvestInbox.push_back(id);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HarvestWatcher harvestWatcher;

		// Our player equipping something: using up food or a stim is one too, which Drops tells from a drop by it.
		class EquipWatcher :
			public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent& a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (a_event.equipped && a_event.actor && a_event.actor->IsPlayerRef()) {
					Drops::OnEquipped(a_event.baseObject);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		EquipWatcher equipWatcher;

		constexpr std::uint32_t FEATURED_ITEM_KEYWORD = 0x001B3FAC;  // bobbleheads and magazines

		// What kind of item a base form is, for LootScope (the quest-alias fact is the holder's, see
		// HoldsAsQuestItem).
		LootScope::ItemFacts ReadItemFacts(RE::TESForm* a_item)
		{
			using Item = LootScope::Item;
			using Type = RE::ENUM_FORM_ID;
			LootScope::ItemFacts facts;
			if (!a_item) {
				return facts;
			}
			switch (a_item->GetFormType()) {
			case Type::kKEYM:
				facts.kind = Item::kKey;
				break;
			case Type::kNOTE:
				facts.kind = Item::kNote;
				break;
			case Type::kBOOK:
				facts.kind = Item::kBook;
				break;
			case Type::kWEAP:
				facts.kind = Item::kWeapon;
				break;
			case Type::kARMO:
				facts.kind = Item::kArmor;
				break;
			case Type::kMISC:
				facts.kind = Item::kMisc;
				break;
			case Type::kAMMO:
				facts.kind = Item::kAmmo;
				break;
			case Type::kALCH:
				facts.kind = Item::kAid;
				break;
			case Type::kINGR:
				facts.kind = Item::kIngredient;
				break;
			case Type::kCMPO:
				facts.kind = Item::kComponent;
				break;
			default:
				return facts;
			}
			const auto keywords = a_item->As<RE::BGSKeywordForm>();
			facts.featured = keywords && keywords->HasKeywordID(FEATURED_ITEM_KEYWORD);
			return facts;
		}

		constexpr std::uint32_t PLAYER_BASE = 0x00000007;     // the player's NPC
		constexpr std::uint32_t PLAYER_FACTION = 0x0001C21C;  // PlayerFaction

		bool OwnedByPlayer(const RE::TESForm* a_owner)
		{
			return a_owner && (a_owner->GetFormID() == PLAYER_BASE || a_owner->GetFormID() == PLAYER_FACTION);
		}

		// A container that keeps what the player puts in it (LootScope::IsStorage), so a friend's copy of it may hold their
		// stash: one the player owns, or whose cell they own (a home bought: FFDiamondCity02 gives Home Plate's cell to
		// PlayerFaction); a settlement's workbench or a piece built there; or one in the build area of a settlement the
		// player owns (WorkshopScript.SetOwnedByPlayer makes the player's NPC the workbench's owner). A Sanctuary house's
		// dresser is a roll of loot until Sanctuary is the player's.
		bool IsStorage(RE::TESObjectREFR* a_ref)
		{
			LootScope::StorageFacts facts;
			const auto              cell = a_ref->GetParentCell();
			facts.playerOwned = OwnedByPlayer(a_ref->GetOwner()) || (cell && OwnedByPlayer(cell->GetOwner()));
			facts.workshopItem = WorkshopSync::IsWorkbenchAnchor(a_ref) || WorkshopSync::WorkbenchOf(*a_ref) != nullptr;
			if (!facts.playerOwned && !facts.workshopItem) {
				const auto bench = RE::Workshop::FindNearestValidWorkshop(*a_ref);
				facts.inOwnedSettlement = bench && OwnedByPlayer(bench->GetOwner()) && RE::Workshop::IsLocationWithinBuildableArea(*bench, a_ref->data.location);
			}
			return LootScope::IsStorage(facts);
		}

		// IsStorage, looked up again after STORAGE_FOR (a settlement taken, a home bought): each move and each friend's change
		// asks it, and the nearest workbench is a search.
		constexpr auto STORAGE_FOR = std::chrono::seconds{ 5 };
		std::unordered_map<std::uint32_t, std::pair<bool, Clock::time_point>> storageSeen;  // main thread

		bool StorageOf(RE::TESObjectREFR* a_ref)
		{
			const auto now = Clock::now();
			if (const auto it = storageSeen.find(a_ref->GetFormID()); it != storageSeen.end() && now - it->second.second < STORAGE_FOR) {
				return it->second.first;
			}
			if (storageSeen.size() >= 256) {
				std::erase_if(storageSeen, [&](const auto& a_entry) { return now - a_entry.second.second >= STORAGE_FOR; });
			}
			const bool storage = IsStorage(a_ref);
			storageSeen[a_ref->GetFormID()] = { storage, now };
			return storage;
		}

		// What holds the items, for LootScope: a reference ID from either end of a move.
		LootScope::HolderFacts ReadHolderFacts(std::uint32_t a_id)
		{
			using Holder = LootScope::Holder;
			LootScope::HolderFacts facts;
			if (!IsShareableRef(a_id)) {
				facts.kind = a_id == Protocol::PLAYER_REF_ID ? Holder::kPlayer : Holder::kRuntime;
				return facts;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			if (!ref || ref->IsDeleted()) {
				return facts;
			}
			if (const auto actor = ref->As<RE::Actor>()) {
				if (actor->IsPlayerRef() || Puppets::IsPuppet(actor)) {
					facts.kind = Holder::kPlayer;
					return facts;
				}
				facts.kind = Holder::kActor;
				facts.alive = !actor->IsDead(false);
				facts.knownDead = dead.Contains(a_id);
				facts.companion = NpcSync::IsOurCompanion(actor);  // holds what we gave them: a friend's loot says nothing of ours
				return facts;
			}
			const auto base = ref->GetObjectReference();
			if (base && base->Is(RE::ENUM_FORM_ID::kCONT)) {
				facts.kind = Holder::kContainer;
				facts.storage = StorageOf(ref);
			} else if (base && base->Is(RE::ENUM_FORM_ID::kFURN)) {
				facts.kind = Holder::kFurniture;  // a power armor frame
			}
			// A settlement's workbench holds what we gave it (as WorkshopSync finds them: the anchor, not every
			// reference with workshop data).
			facts.workshop = WorkshopSync::IsWorkbenchAnchor(ref);
			facts.vendorChest = vendorChests.contains(a_id);
			return facts;
		}

		// Each faction's merchant container, once game data is loaded.
		void FindVendorChests()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return;
			}
			std::size_t unknown = 0;
			for (const auto faction : data->GetFormArray<RE::TESFaction>()) {
				const auto chest = faction ? faction->vendorData.merchantContainer : nullptr;
				// Like a linked reference (see LinkedTarget), the field may still hold the form ID the engine
				// read from the file rather than the reference.
				const auto raw = reinterpret_cast<std::uintptr_t>(chest);
				const auto id = raw <= 0xFFFFFFFFull ? static_cast<std::uint32_t>(raw) : chest->GetFormID();
				if (!IsShareableRef(id)) {
					continue;
				}
				// No reference by that ID: a reference the engine put below 4 GB would look like this, and
				// its chest would be missing from the set (its trades are still kept by the barter menu).
				if (raw <= 0xFFFFFFFFull && !RE::TESForm::GetFormByID<RE::TESObjectREFR>(id)) {
					++unknown;
				}
				vendorChests.insert(id);
			}
			REX::INFO("WorldSync: {} vendor chests ({} not found as references)", vendorChests.size(), unknown);
		}

		// Whether a container holds the item as part of a quest (it's in a quest alias).
		bool HoldsAsQuestItem(RE::TESObjectREFR* a_container, const RE::TESForm* a_item)
		{
			const auto list = a_container->inventoryList;
			if (!list) {
				return false;
			}
			RE::BSAutoReadLock l{ list->rwLock };
			for (auto& entry : list->data) {
				if (entry.object != a_item) {
					continue;
				}
				for (auto stack = entry.stackData.get(); stack; stack = stack->nextStack.get()) {
					if (stack->extra && stack->extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray)) {
						return true;
					}
				}
			}
			return false;
		}

		// The engine's side of the loot (LootSync). Main thread only.
		class EngineWorld final :
			public LootSync::World
		{
		public:
			// CanApply(), when the caller worked it out once for everything it offers (a whole world state).
			std::optional<bool> inWorld;

			bool Loaded(std::uint32_t a_holder) override
			{
				if (!(inWorld ? *inWorld : InWorld() && !Story::InOpening())) {  // in the opening: once they're out
					return false;
				}
				// In a cell, loaded or not (a reference of a cell the game let go of keeps its cell): one in none is
				// between places, or on its way out, and waits.
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_holder);
				return ref && !ref->IsDeleted() && ref->GetParentCell();
			}

			LootScope::HolderFacts Holder(std::uint32_t a_holder) override { return ReadHolderFacts(a_holder); }

			std::vector<LootSync::Stack> Stacks(std::uint32_t a_holder) override
			{
				std::vector<LootSync::Stack> stacks;
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_holder);
				const auto list = ref && !ref->IsDeleted() ? ref->inventoryList : nullptr;
				if (!list) {
					return stacks;
				}
				RE::BSAutoReadLock l{ list->rwLock };
				for (auto& entry : list->data) {
					if (!entry.object) {
						continue;
					}
					bool alias = false;
					for (auto stack = entry.stackData.get(); stack && !alias; stack = stack->nextStack.get()) {
						alias = stack->extra && stack->extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
					}
					const auto count = entry.GetCount();
					stacks.push_back({ entry.object->GetFormID(), static_cast<std::int32_t>((std::min)(count, static_cast<std::uint32_t>(Protocol::MAX_ITEM_COUNT))), alias });
				}
				return stacks;
			}

			bool Stocked(std::uint32_t a_holder) override
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_holder);
				return ref && !ref->IsDeleted() && ref->inventoryList != nullptr;
			}

			LootScope::ItemFacts Item(std::uint32_t a_item) override { return ReadItemFacts(RE::TESForm::GetFormByID(a_item)); }

			bool QuestAlias(std::uint32_t a_holder, std::uint32_t a_item) override
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_holder);
				const auto item = RE::TESForm::GetFormByID(a_item);
				return ref && item && HoldsAsQuestItem(ref, item);
			}

			void Apply(const Protocol::ContainerChange& a_change) override
			{
				// LootScope keeps the player and stand-ins out already (Holder::kPlayer); judged once more where it is done.
				if (!Equipment::MayChange(RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_change.container), OwnGear::Change::kContainer)) {
					return;
				}
				const auto command = a_change.count < 0 ?
					std::format("{:08X}.removeitem {:08X} {}", a_change.container, a_change.item, -static_cast<std::int64_t>(a_change.count)) :
					std::format("{:08X}.additem {:08X} {}", a_change.container, a_change.item, a_change.count);
				RE::Console::ExecuteCommand(command.c_str());
			}

			// Shared storage: who keeps a container, and our view of a friend's (Storage).
			StorageView::Keeper Keeper(std::uint32_t a_holder) override { return Storage::KeeperOf(a_holder); }

			std::optional<std::uint32_t> ViewFrom(std::uint32_t a_holder) override { return Storage::ViewFrom(a_holder); }

			std::uint32_t KeptSince(std::uint32_t a_holder) override { return Storage::KeptSince(a_holder); }

			bool Aliased(std::uint32_t a_holder, std::uint32_t a_item) override { return Storage::Aliased(a_holder, a_item); }

			bool Reporting(const Protocol::ContainerChange& a_change) override { return Storage::Reporting(a_change); }

			void OwnStorage(const Protocol::IndexedContainerChange& a_change, bool a_lacking) override { Storage::OwnStorage(a_change, a_lacking); }

			void Short(std::uint32_t a_index, const Protocol::ContainerChange& a_change, std::int32_t a_missing) override
			{
				Storage::Short(a_index, a_change, a_missing);
			}
		};

		EngineWorld engine;

		// What waits for a_container tried again, as it loads or a sweep comes to it (see ApplyWaiting).
		std::size_t RetryContainer(std::uint32_t a_container, bool a_inWorld)
		{
			if (!a_inWorld) {
				return 0;
			}
			engine.inWorld = true;
			const auto done = loot.Retry(a_container, engine);
			engine.inWorld.reset();
			return done;
		}

		// The player's own moves: those that are the session's are reported.
		void ReportMoves(const std::vector<Move>& a_moves)
		{
			for (const auto& move : a_moves) {
				// A weapon or armor put in a container: a friend's game would get a plain copy, so it isn't shared there.
				// Our player is told once a session how to give it instead. (With the host's bContainerGifts, one put in a gift's place
				// is given: Gifts.)
				if (move.count > 0 && !Gifts::GivesIn(move.holder) && LootScope::Of(ReadHolderFacts(move.holder), ReadItemFacts(RE::TESForm::GetFormByID(move.item)), LootScope::Direction::kPut) ==
										  LootScope::Scope::kWeaponArmor) {
					Drops::OnKeptInContainer();
				}
			}
			for (auto& packet : loot.Report(a_moves, engine)) {
				Queue(std::move(packet));
			}
		}

		// Deaths in our world: reported, unless someone else reported them (we caused those because they did).
		// Our player's kill: theirs, or our companion's (CopyHealth::Credited).
		bool KilledByUs(const Killers& a_killers, std::uint32_t a_id)
		{
			const auto it = a_killers.find(a_id);
			return it != a_killers.end() && CopyHealth::Credited(CopyWatch::CauseOf(it->second), NpcSync::LocalPlayer());
		}

		// a_atFall: reported as they began to fall (ReportFalling); their death events, at the end of the fall, say where
		// the corpse came to rest if it was thrown or fell elsewhere meanwhile (Protocol::CORPSE_REST_WINDOW).
		void ReportDeaths(const std::vector<std::uint32_t>& a_deaths, const Killers& a_killers, bool a_atFall = false)
		{
			for (const auto id : a_deaths) {
				// Gone down, not dead: it gets up again (Dogmeat at the end of his health). Never reported, nor known dead
				// here: the session would have it dead for good.
				if (const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id); actor && GetsUp(actor)) {
					if (++downNotDead <= 20) {
						REX::INFO("WorldSync: {:08X} went down but gets up again; not reported dead", id);
					}
					continue;
				}
				if (dead.Add(id, Clock::now())) {
					dead.Reported(id);  // ours: an owner named for it in the next moments crossed this report (SessionDeaths::OnOwned)
					// Our kill, of a friend's NPC too: a grenade's, a mine's or a fire's blows name no attacker, so no hit
					// of ours told the server we fight it. The takeover claim leaves this frame, the death next frame
					// (Session::Frame), so the server believes it (Server.cpp, handleDeath).
					if (KilledByUs(a_killers, id)) {
						NpcSync::OnLocalInteraction(id);
					}
					const auto corpse = RE::TESForm::GetFormByID<RE::Actor>(id);
					const auto at = corpse && corpse->Get3D() ? corpse->data.location : RE::NiPoint3{};  // a script can kill one with no 3D
					ReportDead({ .refId = id, .killed = KilledByUs(a_killers, id), .x = at.x, .y = at.y, .z = at.z });
					if (a_atFall && corpse && corpse->Get3D()) {
						dead.FellReported(id, { at.x, at.y, at.z });
					}
				} else if (const auto fell = dead.TakeFell(id)) {
					// Its death event, the fall over: the same death once more, not a kill, if the corpse lies elsewhere now.
					const auto corpse = RE::TESForm::GetFormByID<RE::Actor>(id);
					if (corpse && corpse->Get3D() && corpse->data.location.GetDistance(RE::NiPoint3{ (*fell)[0], (*fell)[1], (*fell)[2] }) > CORPSE_SLACK) {
						const auto& at = corpse->data.location;
						ReportDead({ .refId = id, .x = at.x, .y = at.y, .z = at.z });
						++restsReported;
					}
				}
				dead.SawDead(id);  // ours, or the kill we ran for another game's report: it fell here
				reports.Forget(id);
				pendingHealth.erase(id);
				friendsBlows.erase(id);
			}
		}

		// The NPCs our game runs that began to fall since the last frame (their dying events) are reported dead now
		// (NpcHold::ReportAtFall), not when their death event comes at the end of the fall: the friends' copies would
		// stand and fight for those seconds. The kill credit is the dying event's killer, as the death event's would be.
		void ReportFalling(const std::vector<std::uint32_t>& a_dying, const Killers& a_killers)
		{
			const auto local = NpcSync::LocalPlayer();
			std::vector<std::uint32_t> fell;  // not `falling`, the book above
			for (const auto id : a_dying) {
				const bool friendsBlow = friendsBlows.erase(id) != 0;
				const auto actor = local != 0 && !dead.Contains(id) ? LoadedActor(id) : nullptr;
				if (actor && actor->IsDead(false) && NpcHold::ReportAtFall(NpcSync::Owner(id) == local, a_killers.contains(id), friendsBlow, GetsUp(actor))) {
					fell.push_back(id);
				}
			}
			ReportDeaths(fell, a_killers, true);
		}

		// Deaths of NPCs our game made at runtime: a shared one's (ours, or our copy of a friend's we killed first) is
		// reported under its spawn id. Not kept with the session's dead: the id may name another spawn later.
		void ReportRuntimeDeaths(const std::vector<std::pair<std::uint32_t, bool>>& a_deaths)
		{
			for (const auto& [local, byUs] : a_deaths) {
				const auto id = Spawns::OnLocalDeath(local);
				if (id == 0) {
					continue;
				}
				const auto corpse = RE::TESForm::GetFormByID<RE::Actor>(local);
				const auto at = corpse && corpse->Get3D() ? corpse->data.location : RE::NiPoint3{};
				ReportDead({ .refId = id, .killed = byUs, .x = at.x, .y = at.y, .z = at.z });
			}
		}

		void WarnIfLootFull()
		{
			if (loot.Stats().pendingFull && !std::exchange(lootFullWarned, true)) {
				REX::WARN("WorldSync: {} container changes wait already; dropping more", LootSync::Loot::MAX_PENDING);
			}
		}

		// Loot waiting for a living NPC waits only so long in the world (LootSync::Loot::NOT_YET_DEAD_LIMIT): its
		// time is counted every sweep interval, not every frame (the wait is minutes long), and not in loading
		// screens. Everything waiting is tried again then too.
		void AgeLoot(Clock::time_point a_now, bool a_inWorld)
		{
			if (!a_inWorld || !loot.WaitsForTheLiving()) {
				lootAgedAt = a_now;
				return;
			}
			if (a_now - lootAgedAt < SWEEP_INTERVAL) {
				return;
			}
			engine.inWorld = true;
			loot.Retry(engine, a_now - lootAgedAt);
			engine.inWorld.reset();
			lootAgedAt = a_now;
		}

		// Pairs world pickups with the reference the player activated, and reports them.
		void MatchPickups(Clock::time_point a_now, std::vector<Activation>& a_activations, const std::vector<std::uint32_t>& a_pickups)
		{
			for (auto& activation : a_activations) {
				activation.time = a_now;
				recentActivations.push_back(activation);
			}
			for (const auto base : a_pickups) {
				unmatchedPickups.push_back({ 0, base, a_now });
			}
			std::erase_if(unmatchedPickups, [&](const Activation& a_pickup) {
				const auto match = std::ranges::find(recentActivations, a_pickup.base, &Activation::base);
				if (match == recentActivations.end()) {
					return false;
				}
				Queue(Protocol::Encode(Protocol::RefPickedUp{ match->ref }, Protocol::MessageType::kReportPickup));
				recentActivations.erase(match);
				return true;
			});
			const auto expired = [&](const Activation& a_entry) { return a_now - a_entry.time > MATCH_WINDOW; };
			std::erase_if(recentActivations, expired);
			std::erase_if(unmatchedPickups, expired);
		}

		// Calls a_visit with the name of each script bound to the reference (from its base, or of its own). A plain
		// ObjectReference object is only Papyrus holding the reference somewhere, not a script on it.
		template <class F>
		void ForEachScript(RE::TESObjectREFR* a_ref, F&& a_visit)
		{
			const auto game = RE::GameVM::GetSingleton();
			const auto vm = game ? game->GetVM() : nullptr;
			if (!vm) {
				return;
			}
			const auto& handles = vm->GetObjectHandlePolicy();
			const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESObjectREFR>(), a_ref);
			if (handle == handles.EmptyHandle()) {
				return;
			}
			const RE::BSTThreadScrapFunction<RE::BSContainer::ForEachResult(RE::BSScript::Object*)> visit{ [&](RE::BSScript::Object* a_object) {
				const auto  type = a_object ? a_object->type.get() : nullptr;
				const char* name = type ? type->GetName() : nullptr;
				if (name && *name && _stricmp(name, "ObjectReference") != 0) {
					a_visit(std::string_view{ name });
				}
				return Papyrus::BOUND_NEXT;  // on to the next script, not only the first
			} };
			vm->ForEachBoundObject(handle, visit);
		}

		// DoorShare::NEVER by the IDs this game has them under, once its data is loaded.
		void ResolveDoorList()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler || doorListResolved) {
				return;
			}
			for (const auto& ref : DoorShare::NEVER) {
				if (const auto id = handler->LookupFormID(ref.id, ref.file)) {
					doorRefsOut.insert(id);
				}
			}
			doorListResolved = !doorRefsOut.empty();  // else asked again: the game's data may not be loaded yet
		}

		// Whether a loaded reference's open state is a player's to share (DoorShare::SharesOpen), worked out
		// once per reference: an elevator's, or a gear door's, is its script's. What it is is logged when the player
		// used it (a_log), not for every door the poll comes across.
		bool OpensShared(RE::TESObjectREFR* a_ref, bool a_log = true)
		{
			if (const auto it = opensShared.find(a_ref->GetFormID()); it != opensShared.end()) {
				if (!a_log || !opensUnlogged.erase(a_ref->GetFormID())) {
					return it->second;
				}
				opensShared.erase(it);  // worked out by the poll, used now: worked out again, and logged
			}
			const auto       base = a_ref->GetObjectReference();
			const auto       door = base ? base->As<RE::TESObjectDOOR>() : nullptr;
			DoorShare::Facts facts;
			facts.door = door != nullptr;
			facts.doorFlags = door ? static_cast<std::uint8_t>(door->flags) : 0;
			std::string others;
			if (door) {
				ResolveDoorList();
				facts.listedOut = doorRefsOut.contains(a_ref->GetFormID());
				ForEachScript(a_ref, [&](std::string_view a_name) {
					++facts.scripts;
					if (DoorShare::IsQuiet(a_name)) {
						++facts.quietScripts;
					} else {
						others += others.empty() ? "" : ", ";
						others += a_name;
					}
				});
			}
			const bool shared = DoorShare::SharesOpen(facts);
			if (!shared && a_log) {
				// Not a door: an activator's script, or a container's lid, a chair, a marker (by the base's type). A door
				// only a quest stage opens is shared by that stage (ScriptDoors), not by its open state.
				REX::INFO("WorldSync: {:08X} (base {:08X}) is opened by {}; its open state isn't shared", a_ref->GetFormID(), base ? base->GetFormID() : 0,
					!door                              ? std::format("a script or itself (a {}, not a door)", base ? base->GetFormTypeString() : "?") :
					facts.listedOut                    ? std::string("the story (a door listed out)") :
					facts.scripts > facts.quietScripts ? std::format("a script on the door ({})", others) :
														 std::string("itself (an automatic door)"));
			} else if (shared && facts.scripts > 0 && a_log) {
				REX::INFO("WorldSync: {:08X} (base {:08X}) has {} quiet script(s); its open state is shared as a plain door's", a_ref->GetFormID(), base ? base->GetFormID() : 0, facts.scripts);
			}
			opensShared.emplace(a_ref->GetFormID(), shared);
			if (!a_log) {
				opensUnlogged.insert(a_ref->GetFormID());
			}
			return shared;
		}

		// SharedGates::NOT_A_WAY and NEVER by the IDs this game has them under, once its data is loaded.
		void ResolveGateLists()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler || !gateBasesOut.empty() || !gateRefsOut.empty()) {
				return;
			}
			for (const auto& base : SharedGates::NOT_A_WAY) {
				if (const auto id = handler->LookupFormID(base.id, base.file)) {
					gateBasesOut.insert(id);
				}
			}
			for (const auto& ref : SharedGates::NEVER) {
				if (const auto id = handler->LookupFormID(ref.id, ref.file)) {
					gateRefsOut.insert(id);
				}
			}
		}

		// Whether a loaded reference is a gate whose open state is shared (SharedGates::Shares): an activator that runs
		// the game's two-state script, or one of the few that extend it, with nothing beside it but the quiet ones.
		// Worked out once per reference, while it is loaded (its scripts are bound to it then); false, and asked again
		// later, while it isn't.
		bool IsSharedGate(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return false;
			}
			if (const auto it = gates.find(a_ref->GetFormID()); it != gates.end()) {
				return it->second;
			}
			const auto base = a_ref->GetObjectReference();
			if (!base || !base->Is(RE::ENUM_FORM_ID::kACTI)) {
				gates.emplace(a_ref->GetFormID(), false);
				return false;
			}
			if (!a_ref->Get3D() || a_ref->IsDeleted()) {
				return false;
			}
			const auto game = RE::GameVM::GetSingleton();
			const auto vm = game ? game->GetVM() : nullptr;
			if (!vm) {
				return false;
			}
			SharedGates::Facts facts;
			facts.activator = true;
			facts.listedOut = gateBasesOut.contains(base->GetFormID()) || gateRefsOut.contains(a_ref->GetFormID());
			std::string      names;
			std::string_view running = SharedGates::SCRIPT;
			ForEachScript(a_ref, [&](std::string_view a_name) {
				names += names.empty() ? "" : ", ";
				names += a_name;
				++facts.scripts;
				if (SharedGates::IsGateScript(a_name)) {
					++facts.gateScripts;
				} else if (const auto child = SharedGates::RunningScript(a_name)) {
					++facts.childScripts;
					running = *child;
				} else if (SharedGates::IsSibling(a_name)) {
					++facts.siblings;
				}
			});
			const bool shared = SharedGates::Shares(facts);
			if (shared) {
				REX::INFO("WorldSync: {:08X} (base {:08X}) is a two-state gate ({}{}); its open state is shared", a_ref->GetFormID(), base->GetFormID(), running,
					facts.siblings > 0 ? " and a quiet script beside it" : "");
				gateScripts.emplace(a_ref->GetFormID(), running);
			} else if (facts.gateScripts + facts.childScripts > 0) {
				REX::INFO("WorldSync: {:08X} (base {:08X}) runs the two-state script {}; its open state isn't shared", a_ref->GetFormID(), base->GetFormID(),
					facts.listedOut ? "but is listed out (not a way through, or the story's)" :
									  std::format("with {} other script(s)", facts.scripts - facts.gateScripts - facts.childScripts - facts.siblings));
			} else if (facts.scripts > 0 && facts.siblings > 0) {
				// A gate the survey shares with a script beside it, seen without its two-state script: not bound yet, or not visited.
				REX::INFO("WorldSync: {:08X} (base {:08X}) has a script the two-state gates have beside them but not the two-state script ({}); its open state isn't shared", a_ref->GetFormID(), base->GetFormID(), names);
			}
			gates.emplace(a_ref->GetFormID(), shared);
			return shared;
		}

		// The script a shared gate runs by (SharedGates::RunningScript): the two-state script itself, or the one that
		// extends it.
		std::string_view GateScript(std::uint32_t a_ref)
		{
			const auto it = gateScripts.find(a_ref);
			return it != gateScripts.end() ? it->second : SharedGates::SCRIPT;
		}

		// What a shared gate's script says of it, or nullopt if there is no script object to read (yet). A script that
		// extends the two-state one has its isOpen and its states (GetProperty looks in the script it extends too).
		std::optional<SharedGates::ScriptState> ReadGate(RE::TESObjectREFR* a_ref)
		{
			const auto object = Papyrus::BoundObject(a_ref, GateScript(a_ref->GetFormID()), false);
			if (!object) {
				return std::nullopt;
			}
			const auto isOpen = object->GetProperty(RE::BSFixedString{ "isOpen" });
			if (!isOpen || !isOpen->is<bool>()) {
				return std::nullopt;
			}
			SharedGates::ScriptState state;
			state.open = RE::BSScript::get<bool>(*isOpen);
			const auto current = object->currentState.c_str();
			state.busy = current && _stricmp(current, "busy") == 0;
			return state;
		}

		// When the game last loaded this activator, if it was a moment ago (SharedGates::Settled).
		std::optional<Clock::time_point> GateLoadedAt(std::uint32_t a_ref)
		{
			const auto it = gateLoads.find(a_ref);
			return it != gateLoads.end() ? std::optional{ it->second } : std::nullopt;
		}

		// The shared open/lock state of a loaded reference (see DoorShare::Shared; either part may be
		// unknown), or nullopt if it isn't loaded. A shared gate's open state is where its script is taking it. a_log: as OpensShared's.
		std::optional<Protocol::RefState> ReadRefState(RE::TESObjectREFR* a_ref, bool a_log = true)
		{
			if (!a_ref || a_ref->IsDeleted() || !a_ref->Get3D() || a_ref->As<RE::Actor>()) {
				return std::nullopt;  // talking to someone is an activation too, but an actor has no door or lock state
			}
			using Open = RE::BGSOpenCloseForm::OPEN_STATE;
			Protocol::RefState state{ a_ref->GetFormID() };
			if (IsSharedGate(a_ref)) {
				if (const auto gate = ReadGate(a_ref)) {
					state.open = SharedGates::Target(*gate) ? Protocol::RefState::kYes : Protocol::RefState::kNo;
				}
				if (const auto lock = a_ref->GetLock()) {
					state.locked = (lock->flags & std::to_underlying(RE::REFR_LOCK::Flags::kLocked)) ? Protocol::RefState::kYes : Protocol::RefState::kNo;
				}
				return state;
			}
			switch (RE::BGSOpenCloseForm::GetOpenState(a_ref)) {
			case Open::kOpen:
			case Open::kOpening:
				state.open = Protocol::RefState::kYes;
				break;
			case Open::kClosed:
			case Open::kClosing:
				state.open = Protocol::RefState::kNo;
				break;
			default:
				break;
			}
			if (const auto lock = a_ref->GetLock()) {
				state.locked = (lock->flags & std::to_underlying(RE::REFR_LOCK::Flags::kLocked)) ? Protocol::RefState::kYes : Protocol::RefState::kNo;
			}
			if (state.open != Protocol::RefState::kUnknown) {
				state = DoorShare::Shared(state, OpensShared(a_ref, a_log));
			}
			return state;
		}

		// Applies another player's door/lock state. Returns false to try again later.
		bool TryApplyRefState(const Protocol::RefState& a_state, bool a_inWorld)
		{
			if (!a_inWorld) {
				return false;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_state.refId);
			if (ref && ref->As<RE::Actor>()) {
				return true;
			}
			const auto current = ReadRefState(ref);
			if (!current) {
				return ref && ref->IsDeleted();  // gone for good: drop it
			}
			// A shared gate is opened or closed through its script, once it has settled after loading (SharedGates).
			const bool gate = a_state.open != Protocol::RefState::kUnknown && IsSharedGate(ref);
			std::optional<SharedGates::Apply> gateApply;
			if (gate) {
				const auto now = Clock::now();
				const bool open = a_state.open == Protocol::RefState::kYes;
				gateApply = SharedGates::Decide(open, ReadGate(ref), SharedGates::Settled(GateLoadedAt(a_state.refId), applyingSince, now));
				if (*gateApply == SharedGates::Apply::kWait) {
					return false;
				}
			}
			if (!DoorShare::Says(*current)) {
				return true;  // here, and nothing of it is shared (an elevator's open state is its script's)
			}
			if (a_state.locked != Protocol::RefState::kUnknown && current->locked != Protocol::RefState::kUnknown && a_state.locked != current->locked) {
				ref->GetLock()->SetLocked(a_state.locked == Protocol::RefState::kYes);
				ref->AddLockChange();
				++refStatesApplied;
				// A door whose script flips its lock is applied every few seconds for as long as both players are near it: one line per interval.
				if (refStateLog.Due(Clock::now())) {
					REX::INFO("WorldSync: {:08X} {} (changed in another player's world) ({} such since the last line)", a_state.refId, a_state.locked == Protocol::RefState::kYes ? "locked" : "unlocked", refStateLog.Since());
				}
			}
			if (gate) {
				if (*gateApply == SharedGates::Apply::kSet) {
					const bool open = a_state.open == Protocol::RefState::kYes;
					// As a terminal does: the script plays the animation and turns the gate's collision off or on.
					if (!Papyrus::CallMethod(ref, GateScript(a_state.refId), "SetOpenNoWait", false, open)) {
						return false;
					}
					gatesApplied.Set(a_state.refId, open, Clock::now());
					++gatesSet;
					++refStatesApplied;
					REX::INFO("WorldSync: {} gate {:08X} (changed in another player's world)", open ? "opening" : "closing", a_state.refId);
				}
				return true;
			}
			if (a_state.open != Protocol::RefState::kUnknown && current->open != Protocol::RefState::kUnknown && a_state.open != current->open) {
				RE::BGSOpenCloseForm::SetOpenState(ref, a_state.open == Protocol::RefState::kYes, false);
				++refStatesApplied;
			}
			return true;
		}

		// The form ID a form's memory says it has, every read guarded: a stale pointer must cost a lookup, not the game.
		bool ReadFormId(const RE::TESForm* a_form, std::uint32_t& a_out)
		{
			__try {
				a_out = a_form->formID;
				return true;
			} __except (1) {
				return false;
			}
		}

		// The reference a linked-ref entry points at (LinkedRefs::Resolve): by the form ID the engine may still hold
		// there, or by a pointer that names itself. Before 0.7.3 the field was read as a pointer whatever it held, and
		// both games crashed at the Red Rocket terminal (2026-10-10); a pointer was then still taken on trust, and an
		// entry marked transient was looked up by the pointer's low half.
		RE::TESObjectREFR* LinkedTarget(const RE::ExtraLinkedRef::LinkedRef& a_link)
		{
			return LinkedRefs::Resolve<RE::TESObjectREFR>(
				reinterpret_cast<std::uintptr_t>(a_link.REFR),
				[](const RE::TESObjectREFR* a_ref) -> std::optional<std::uint32_t> {
					std::uint32_t id = 0;
					return ReadFormId(a_ref, id) ? std::optional{ id } : std::nullopt;
				},
				[](std::uint32_t a_id) { return RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id); });
		}

		// The references a reference links to (any keyword), as far as they can be had.
		template <class F>
		void ForEachLinked(RE::TESObjectREFR* a_ref, F&& a_visit)
		{
			const auto links = a_ref && a_ref->extraList ? a_ref->extraList->GetByType<RE::ExtraLinkedRef>() : nullptr;
			if (!links) {
				return;
			}
			for (const auto& link : links->linkedRefs) {
				if (const auto target = LinkedTarget(link)) {
					a_visit(target);
				}
			}
		}

		// Puts on watch the doors NPCs our game runs opened near our player (DoorShare::WatchesNpcOpen): their state is
		// reported as a door our player opened is, so a friend's door opens and closes as their copy of the NPC walks
		// through it.
		void WatchNpcDoors(Clock::time_point a_now, const std::vector<Activation>& a_opens)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (a_opens.empty() || !player) {
				return;
			}
			for (const auto& open : a_opens) {
				const auto door = RE::TESForm::GetFormByID<RE::TESObjectREFR>(open.ref);
				const auto npc = RE::TESForm::GetFormByID<RE::Actor>(open.base);
				if (!door || !npc || npc->IsPlayerRef() || door->IsDeleted() || !door->Get3D() || !door->GetParentCell()) {
					continue;
				}
				DoorShare::NpcOpen facts;
				const auto session = RemotePlayers::PlayerIdFor(npc->GetFormID()) == 0 && !Puppets::IsPuppet(npc) ? NpcSync::SessionId(npc->GetFormID()) : 0;
				facts.runsHere = session != 0 && NpcSync::RunsLocally(session);
				facts.sharesOpen = OpensShared(door);
				facts.loadDoor = door->extraList && door->extraList->HasType(RE::EXTRA_DATA_TYPE::kTeleport);
				facts.playerDistance = player->GetParentCell() ? player->data.location.GetDistance(door->data.location) : DoorShare::NPC_REACH * 2;
				if (!DoorShare::WatchesNpcOpen(facts)) {
					continue;
				}
				auto& entry = watched[open.ref];
				entry.until = std::max(entry.until, a_now + DoorShare::NPC_WATCH);
				++npcDoorsWatched;
			}
		}

		// One look at a watched reference (DoorShare::OnLook): a change is reported, or held, or a polled door's first
		// read taken as where it stands. False when it isn't to be watched any more (gone, or nothing of it is shared).
		bool LookAt(std::uint32_t a_id, Watched& a_entry, Clock::time_point a_now, RE::PlayerCharacter* a_player)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			const auto state = ReadRefState(ref, !a_entry.quiet);
			if (!state || !DoorShare::Says(*state)) {
				return false;
			}
			// A gate we set from a friend's state reads as it was until its script gets round to it: not sent back.
			if (state->open != Protocol::RefState::kUnknown && gatesApplied.Holds(a_id, state->open == Protocol::RefState::kYes, a_now)) {
				if (!state->SameState(a_entry.lastSent)) {
					++gateReportsHeld;
				}
				return true;
			}
			// The same state is no news whoever said it: one we took from a friend names them, ours doesn't. A change
			// is reported from near enough for a friend to take it (they refuse a report from a player far from the
			// thing), else kept for when we are; and no more often than DoorShare::Pace lets it.
			DoorShare::Watch look;
			look.quiet = a_entry.quiet;
			look.seen = a_entry.lastSent.refId != 0;
			look.changed = !state->SameState(a_entry.lastSent);
			look.used = a_entry.used;
			look.near = a_player && a_player->GetParentCell() &&
			            a_player->data.location.GetDistance(ref->data.location) <= (IsSharedGate(ref) ? SharedGates::REPORT_REACH : DoorShare::REPORT_REACH);
			look.pending = pendingRefStates.contains(a_id);
			look.paced = !a_entry.pace.May(a_now);
			switch (DoorShare::OnLook(look)) {
			case DoorShare::Look::kBaseline:
				a_entry.lastSent = *state;  // where it stands: a save's door, or the session's
				break;
			case DoorShare::Look::kHold:
				if (!std::exchange(a_entry.held, true)) {
					++(look.near ? reportsPaced : reportsHeld);
				}
				break;
			case DoorShare::Look::kSend:
				a_entry.lastSent = *state;
				a_entry.held = false;
				a_entry.used = false;
				a_entry.pace.Spend(a_now);
				refBook.Sent(*state);
				Queue(Protocol::Encode(*state, Protocol::MessageType::kReportRefState));
				break;
			case DoorShare::Look::kNothing:
				if (!look.changed) {
					a_entry.held = false;  // what was held went back: nothing to say
				}
				break;
			}
			return true;
		}

		// One loaded door in the poll's sweep: a plain one is put on watch quietly (its first read is where it stands)
		// and looked at. A door gone from the world, one whose open state isn't a player's (an automatic door, a
		// scripted one, the story's) or one with nothing to share is forgotten until it loads again; a scripted door's
		// lock is still shared when the player uses it.
		void PollDoor(std::uint32_t a_id, Clock::time_point a_now, RE::PlayerCharacter* a_player)
		{
			const auto loaded = loadedDoors.find(a_id);
			if (loaded == loadedDoors.end() || !DoorShare::Settled(loaded->second, applyingSince, a_now)) {
				return;  // dropped since the sweep began, or only just loaded, or our player only just out of the loading screen
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			const auto forget = [&] {
				loadedDoors.erase(a_id);
				if (const auto it = watched.find(a_id); it != watched.end() && it->second.quiet && !DoorShare::KeptWhenGone(it->second.held)) {
					watched.erase(it);
				}
			};
			if (!ref || ref->IsDeleted() || !ref->Get3D() || !ref->GetParentCell() || !OpensShared(ref, false)) {
				forget();
				return;
			}
			auto [it, added] = watched.try_emplace(a_id);
			auto& entry = it->second;
			if (added) {
				entry.quiet = true;
				++doorsPolled;
			}
			if (!entry.quiet) {
				return;  // the player used it lately: read every frame (WatchRefStates)
			}
			entry.until = a_now + WATCH_TIME;
			if (!LookAt(a_id, entry, a_now, a_player)) {
				forget();
			}
		}

		// The loaded doors the load event didn't name (DoorShare::FIND_PER_FRAME): a walk of the loaded cells'
		// references, a few a frame, puts every door with its 3D up on the poll's list from when it is found, as the
		// event does from when it loads (a door on the list already keeps its time). The cells: the interior we are in,
		// or the attached cells of the grid and the worldspace's persistent cell (a load door, and most doors a quest
		// holds, is a persistent reference, in no grid cell's list; most of the persistent cell's references have no
		// 3D up, and are passed over). The cells are the pass's as it began; one detached since is passed over. A door
		// the poll found not to be shared (an elevator's, a scripted one: opensShared) isn't put back each pass.
		void FindDoors(Clock::time_point a_now)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			const auto world = tes->interiorCell ? nullptr : tes->worldSpace;
			const auto persistent = world ? world->persistentCell : nullptr;
			if (doorWalk.Due(a_now)) {
				std::vector<std::uint32_t> cells;
				if (const auto interior = tes->interiorCell) {
					cells.push_back(interior->GetFormID());
				} else if (const auto grid = tes->gridCells) {
					for (std::uint32_t x = 0; x < grid->dimension; ++x) {
						for (std::uint32_t y = 0; y < grid->dimension; ++y) {
							const auto gridCell = grid->Get(x, y);
							const auto cell = gridCell ? gridCell->cell : nullptr;
							if (cell && cell->IsAttached()) {
								cells.push_back(cell->GetFormID());
							}
						}
					}
					if (persistent) {
						cells.push_back(persistent->GetFormID());
					}
				}
				doorWalk.Begin(std::move(cells), a_now);
			}
			const auto cellId = doorWalk.Cell();
			if (cellId == 0) {
				return;
			}
			RE::TESObjectCELL* cell = nullptr;
			if (tes->interiorCell && tes->interiorCell->GetFormID() == cellId) {
				cell = tes->interiorCell;
			} else if (persistent && persistent->GetFormID() == cellId) {
				cell = persistent;
			} else if (const auto found = RE::TESForm::GetFormByID<RE::TESObjectCELL>(cellId); found && found->IsAttached()) {
				cell = found;
			}
			if (!cell) {
				doorWalk.Skip(a_now);
				return;
			}
			std::vector<std::uint32_t> doors;
			std::size_t                total = 0, read = 0;
			{
				RE::BSAutoLock l{ cell->spinLock };
				total = cell->references.size();
				read = doorWalk.Chunk(total);
				for (std::size_t i = doorWalk.From(), end = i + read; i < end; ++i) {
					const auto ref = cell->references[static_cast<std::uint32_t>(i)].get();  // the array's index type (i < its size)
					const auto base = ref ? ref->GetObjectReference() : nullptr;
					if (base && base->Is(RE::ENUM_FORM_ID::kDOOR) && !ref->IsDeleted() && ref->Get3D() && IsShareableRef(ref->GetFormID())) {
						doors.push_back(ref->GetFormID());
					}
				}
			}
			doorWalk.Read(read, total, a_now);
			for (const auto id : doors) {
				if (const auto it = opensShared.find(id); it != opensShared.end() && !it->second) {
					continue;
				}
				if (loadedDoors.try_emplace(id, a_now).second) {
					++doorsFound;
				}
			}
		}

		// Starts watching references the player activated, and reports changes to watched ones.
		void WatchRefStates(Clock::time_point a_now, const std::vector<Activation>& a_activations)
		{
			for (const auto& activation : a_activations) {
				auto& entry = watched[activation.ref];
				entry.until = a_now + WATCH_TIME;
				entry.quiet = false;  // used: read every frame again, as a door the poll found is only on its ticks
				entry.used = true;    // and its state goes once all the same: a load door's use changes nothing readable, and the poll's baseline filled lastSent
				// What the thing is linked to is watched as well: a terminal's door opens without anyone
				// activating the door, and the friend would otherwise stay locked out. A terminal opens a chain of
				// gates, each linking the next: the gates linked from a gate are followed too (SharedGates::CHAIN_DEPTH).
				std::vector<RE::TESObjectREFR*> chain;
				ForEachLinked(RE::TESForm::GetFormByID<RE::TESObjectREFR>(activation.ref), [&](RE::TESObjectREFR* a_target) {
					if (Protocol::IsShareableRef(a_target->GetFormID())) {
						auto& linked = watched[a_target->GetFormID()];
						linked.until = a_now + WATCH_TIME;
						linked.quiet = false;
						linked.used = true;
						++linkedWatched;
						if (IsSharedGate(a_target)) {
							chain.push_back(a_target);
						}
					}
				});
				std::unordered_set<std::uint32_t> seen{ activation.ref };
				for (std::size_t depth = 0; depth < SharedGates::CHAIN_DEPTH && !chain.empty(); ++depth) {
					std::vector<RE::TESObjectREFR*> next;
					for (const auto gate : chain) {
						seen.insert(gate->GetFormID());
						ForEachLinked(gate, [&](RE::TESObjectREFR* a_target) {
							const auto id = a_target->GetFormID();
							if (Protocol::IsShareableRef(id) && !seen.contains(id) && IsSharedGate(a_target)) {
								seen.insert(id);
								watched[id].until = a_now + WATCH_TIME;
								++linkedWatched;
								next.push_back(a_target);
							}
						});
					}
					chain = std::move(next);
				}
			}
			// What the player used (and what it links to, a door an NPC opened) is read every frame. Once that watch is
			// over, a plain door still loaded goes back to the poll's sweep.
			const auto player = RE::PlayerCharacter::GetSingleton();
			for (auto it = watched.begin(); it != watched.end();) {
				auto& entry = it->second;
				if (entry.quiet) {
					++it;  // the sweep's (below)
					continue;
				}
				if (a_now > entry.until) {
					if (loadedDoors.contains(it->first)) {
						entry.quiet = true;
						++it;
					} else {
						it = watched.erase(it);
					}
					continue;
				}
				if (!LookAt(it->first, entry, a_now, player)) {
					if (DoorShare::KeptWhenGone(entry.held)) {
						entry.quiet = true;  // a change not sent yet outlives the door's 3D: the poll settles it when the door is back
						++it;
					} else {
						it = watched.erase(it);
					}
					continue;
				}
				++it;
			}
			// Every loaded plain door is on watch too (DoorShare's poll): a sweep of them every POLL_INTERVAL, a few
			// a frame (DoorShare::POLL_PER_FRAME), so a cell full of doors costs no frame more than a few of them. The
			// loaded doors are the load event's and the walk's (FindDoors); doorPollUs is the two together, per frame.
			const auto started = Clock::now();
			if (doorSweepAt >= doorSweep.size() && a_now >= nextDoorPoll) {
				nextDoorPoll = a_now + DoorShare::POLL_INTERVAL;
				doorSweep.clear();
				for (const auto& door : loadedDoors) {
					doorSweep.push_back(door.first);
				}
				doorSweepAt = 0;
			}
			for (std::size_t n = 0; n < DoorShare::POLL_PER_FRAME && doorSweepAt < doorSweep.size(); ++n) {
				PollDoor(doorSweep[doorSweepAt++], a_now, player);
			}
			FindDoors(a_now);
			doorPollUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
			doorPollUsMax = (std::max)(doorPollUsMax, doorPollUs);
		}

		// Removes an item another player picked up. Returns false to try again later.
		bool TryApplyPickup(std::uint32_t a_refId, bool a_inWorld)
		{
			MainThread::Assert("WorldSync::TryApplyPickup");
			if (!a_inWorld) {
				return false;
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_refId);
			if (!ref) {
				return false;
			}
			// Only loose items. Quest and personal items (bobbleheads, magazines, keys, holotapes, paper
			// notes) stay so every player can still pick up their own copy.
			if (ref->As<RE::Actor>()) {
				return true;
			}
			// A plant a friend picked is picked here too: marked harvested, nothing given (FloraShare).
			const auto base = ref->GetObjectReference();
			switch (FloraShare::OnPickup(base && base->Is(RE::ENUM_FORM_ID::kFLOR), FloraShare::Harvested(ref->formFlags))) {
			case FloraShare::Pickup::kHarvest:
				RE::Console::ExecuteCommand(std::format("{:08X}.setharvested 1", a_refId).c_str());
				++harvestsApplied;
				return true;
			case FloraShare::Pickup::kHarvested:
				return true;
			case FloraShare::Pickup::kItem:
				break;
			}
			auto facts = ReadItemFacts(base);
			facts.questAlias = InQuestAlias(ref);
			switch (LootScope::OfPickup(facts)) {
			case LootScope::Scope::kShared:
				break;
			case LootScope::Scope::kPersonal:
				++personalSkipped;
				return true;
			default:
				return true;
			}
			// Taken out of this world, never out of an inventory: a persistent item's reference stays itself in the
			// inventory that took it, ours too (OwnGear). One in no cell and no inventory the engine names may be
			// between cells for a moment: it waits and is tried again, for OwnGear::PICKUP_WAIT.
			const auto verdict = Equipment::Judge(ref, OwnGear::Change::kTakeLoose);
			if (verdict == OwnGear::Verdict::kNotPlaced) {
				const auto now = Clock::now();
				if (OwnGear::TryAgain(verdict, now - pickupUnplacedSince.try_emplace(a_refId, now).first->second)) {
					return false;
				}
			}
			pickupUnplacedSince.erase(a_refId);
			if (!Equipment::Count(ref, OwnGear::Change::kTakeLoose, verdict)) {
				return true;
			}
			if (!ref->IsDisabled() && !ref->IsDeleted()) {
				ref->Disable();
			}
			return true;
		}

		// Whether remote changes can be applied now: in the world, and out of the opening (once they're out).
		bool CanApply()
		{
			return InWorld() && !Story::InOpening();
		}

		// A health report that goes nowhere because the session has the NPC dead (a_what: whose): counted, and logged
		// now and then. With the ledger put right whenever a game runs the NPC (OnActorOwners), these are reports for an
		// NPC alive in one game only, which the session would drop too (Server.cpp, handleHealth).
		void DropHealth(std::uint32_t a_refId, const char* a_what, std::optional<float> a_fraction = std::nullopt)
		{
			++healthDropped;
			if (droppedHealthLog.Due(Clock::now())) {
				REX::INFO("WorldSync: {} dropped: the session has {:08X} dead{} ({} such since the last line)", a_what, a_refId,
					a_fraction ? std::format(" (said {:.0f}% of its health)", *a_fraction * 100.0f) : std::string{}, droppedHealthLog.Since());
			}
		}

		// Takes a friend's damage (a fraction of the NPC's maximum) off the NPC we run, if it can be now (DamageHold::Decide):
		// what can't be yet is held (a_retry: it is held already, so it stays), what can never be is dropped.
		DamageHold::Decision TakeDamage(std::uint32_t a_refId, float a_damage, Clock::time_point a_now, bool a_retry)
		{
			// A spawn's: our own NPC (a copy of a friend's is not ours to hurt). A plugin's reference is the same everywhere.
			const auto local = Protocol::IsSpawnRef(a_refId) ? Spawns::LocalOf(a_refId) : IsShareableRef(a_refId) ? a_refId : 0;
			DamageHold::Facts facts;
			facts.known = local != 0;
			facts.inWorld = CanApply();
			facts.local = NpcSync::LocalPlayer();
			facts.owner = NpcSync::Owner(a_refId);
			const auto info = HealthInfo();
			RE::Actor* actor = nullptr;
			if (facts.known && facts.inWorld) {
				facts.ledgerDead = dead.Contains(local);
				actor = LoadedActor(local);
				facts.loaded = actor != nullptr;
				facts.deadHere = actor && actor->IsDead(false);
				facts.hasHealth = actor && info && static_cast<RE::ActorValueOwner&>(*actor).GetPermanentActorValue(*info) > 0.0f;
			}
			const auto decision = DamageHold::Decide(facts);
			switch (decision.verdict) {
			case DamageHold::Verdict::kHold:
				if (a_retry) {
					break;  // held already, and stays
				}
				if (heldDamage.Hold(a_refId, a_damage, decision.why, a_now)) {
					if (heldDamageLog.Due(a_now)) {
						REX::INFO("WorldSync: player's damage on {:08X} ({:.0f}% of its health) held: {} ({} held since the last line, {} waiting)", a_refId,
							a_damage * 100.0f, DamageHold::Name(decision.why), heldDamageLog.Since(), heldDamage.Size());
					}
				} else {
					++damageDropped;
					if (droppedDamageLog.Due(a_now)) {
						REX::WARN("WorldSync: player's damage on {:08X} ({:.0f}% of its health) dropped: {} NPCs held for already ({} dropped since the last line)", a_refId,
							a_damage * 100.0f, DamageHold::MAX_HELD, droppedDamageLog.Since());
					}
				}
				break;
			case DamageHold::Verdict::kDrop:
				if (decision.why == DamageHold::Why::kNotOurs) {
					++damageIgnored;  // its runner takes it off: every game but the sender's hears of it
					break;
				}
				++damageDropped;
				if (droppedDamageLog.Due(a_now)) {
					REX::INFO("WorldSync: player's damage on {:08X} ({:.0f}% of its health) dropped: {} ({} dropped since the last line)", a_refId, a_damage * 100.0f,
						DamageHold::Name(decision.why), droppedDamageLog.Since());
				}
				break;
			case DamageHold::Verdict::kApply: {
				auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
				const float maximum = values.GetPermanentActorValue(*info);
				values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, -a_damage * maximum);
				++damageApplied;
				if (a_retry) {
					++damageLate;
				}
				// The friend's killing blow: their report has their kill (ReportFalling leaves this fall to the death event).
				if (values.GetActorValue(*info) <= 0.0f && !Protocol::IsSpawnRef(a_refId)) {
					std::erase_if(friendsBlows, [&](const auto& a_entry) { return a_now - a_entry.second > FRIENDS_BLOW_KEEP; });
					if (friendsBlows.size() < MAX_DYING) {
						friendsBlows.insert_or_assign(local, a_now);
					}
				}
				break;
			}
			}
			return decision;
		}

		// Tries every NPC damage is held for again (DamageHold), each frame in the world: applied as the NPC loads or the
		// server's word that we run it comes, dropped once it can never be or after DamageHold::HOLD in the world.
		void RetryHeldDamage(Clock::time_point a_now)
		{
			const bool inWorld = CanApply();
			if (inWorld && !heldInWorld) {
				heldDamage.BackInWorld(a_now);  // the loading screen's time didn't count
			}
			heldInWorld = inWorld;
			if (!inWorld || heldDamage.Size() == 0) {
				return;
			}
			heldDamage.Retry([&](DamageHold::Held& a_held) {
				const auto decision = TakeDamage(a_held.id, (std::min)(a_held.fraction, 1.0f), a_now, true);
				a_held.why = decision.why;
				return decision.verdict != DamageHold::Verdict::kHold;
			});
			for (const auto& gone : heldDamage.Expire(a_now)) {
				++damageDropped;
				if (droppedDamageLog.Due(a_now)) {
					REX::INFO("WorldSync: player's damage on {:08X} ({:.0f}% of its health) dropped: {} ({}; {} dropped since the last line)", gone.id, gone.fraction * 100.0f,
						DamageHold::Name(DamageHold::Why::kExpired), DamageHold::Name(gone.why), droppedDamageLog.Since());
				}
			}
		}

		// ScriptDoors::DOORS by the IDs this game has them under, once its data is loaded. A door whose plugin
		// isn't loaded keeps 0 and is never looked at.
		void ResolveScriptDoors()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler || !scriptDoorQuests.empty()) {
				return;
			}
			for (const auto& door : ScriptDoors::DOORS) {
				const auto quest = handler->LookupForm<RE::TESQuest>(door.quest, door.file);
				scriptDoorQuests.push_back(quest ? quest->GetFormID() : 0);
				scriptDoorRefs.push_back(quest ? handler->LookupFormID(door.door, door.file) : 0);
				scriptDoorEffects.push_back(quest && door.effect != 0 ? handler->LookupFormID(door.effect, door.file) : 0);
			}
		}

		// Looks at the doors only a quest stage opens, and the Prydwen (ScriptDoors), twice a second: one our player
		// opened (or our game's flyover moored) is reported (a stage of a quest QuestSync doesn't share), and the
		// session's are set here once their door is loaded (the Prydwen's at once). The host's game reports those open
		// in its world once its server welcomes it.
		void LookAtScriptDoors(Clock::time_point a_now, bool a_hosting)
		{
			if (a_now < nextDoorLook || scriptDoorQuests.empty()) {
				return;
			}
			nextDoorLook = a_now + DOOR_LOOK_INTERVAL;
			// Not at the main menu or behind a loading screen: the first look after a load must see the save's stages.
			if (!InWorld()) {
				return;
			}
			if (a_hosting && !std::exchange(scriptDoorsReported, true)) {
				scriptDoors.ReportOpen();
			}
			const bool canApply = CanApply() && !GameStop::Now();  // a menu pauses scripts
			for (std::size_t i = 0; i < scriptDoorQuests.size(); ++i) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(scriptDoorQuests[i]);
				if (!quest) {
					continue;
				}
				const auto& door = ScriptDoors::DOORS[i];
				const auto  ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(scriptDoorRefs[i]);
				ScriptDoors::Look look;
				look.running = (quest->data.flags & StageLog::QUEST_RUNNING) != 0;
				look.done = QuestStageLayout::IsStageDone(quest, door.stage);
				look.doorLoaded = ref && !ref->IsDeleted() && !ref->IsDisabled() && ref->Get3D() && ref->GetParentCell();
				look.canApply = canApply;
				// Our own game on its way to the stage (the flyover effect enabled: its script plays the flight once its 3D loads, indoors too, and sets the stage itself).
				const auto effect = scriptDoorEffects[i] != 0 ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(scriptDoorEffects[i]) : nullptr;
				look.busy = effect && !effect->IsDeleted() && !effect->IsDisabled();
				switch (scriptDoors.See(i, look, a_now)) {
				case ScriptDoors::Act::kReport:
					Queue(Protocol::Encode(Protocol::QuestStage{ quest->GetFormID(), door.stage }, Protocol::MessageType::kReportQuestStage));
					++scriptDoorsSent;
					REX::INFO("WorldSync: {} is {} here ({:08X} stage {}); shared", door.what, door.state, quest->GetFormID(), door.stage);
					break;
				case ScriptDoors::Act::kSet:
					// Through the console, as QuestSync sets a friend's stage: the stage's script runs as it did for them.
					RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", quest->GetFormID(), door.stage).c_str());
					++scriptDoorsSet;
					REX::INFO("WorldSync: {} {} here now (setting {:08X} stage {}, done in another player's game)", door.what, door.state, quest->GetFormID(), door.stage);
					break;
				case ScriptDoors::Act::kHold:
					REX::INFO("WorldSync: {} is {} in another player's game, and ours is on its way ({:08X} enabled): waiting for it to set {:08X} stage {}",
						door.what, door.state, scriptDoorEffects[i], quest->GetFormID(), door.stage);
					break;
				case ScriptDoors::Act::kGiveUp:
					++scriptDoorsGaveUp;
					REX::WARN("WorldSync: {:08X} stage {} still not done {} s after setting it; {} left as it is", quest->GetFormID(), door.stage,
						std::chrono::duration_cast<std::chrono::seconds>(ScriptDoors::Book::TIMEOUT).count(), door.what);
					break;
				case ScriptDoors::Act::kNone:
					break;
				}
			}
		}

		// Applies whatever waits for reference a_id (an actor's death or health, changes to a container, a
		// pickup, a door's state). A lookup in each queue; the game is asked only about what waits. Returns
		// how many were applied.
		std::size_t ApplyWaiting(std::uint32_t a_id, bool a_inWorld)
		{
			std::size_t done = 0;
			if (reports.RetryOne(a_id, [&](std::uint32_t a_ref, const DeathReports::Report& a_death) { return TryKill(a_ref, a_death, a_inWorld); })) {
				++done;
			}
			if (const auto it = pendingHealth.find(a_id); it != pendingHealth.end() && (dead.Contains(a_id) || TrySetHealth(a_id, it->second, a_inWorld))) {
				pendingHealth.erase(it);
				++done;
			}
			done += RetryContainer(a_id, a_inWorld);
			if (pendingPickups.contains(a_id) && TryApplyPickup(a_id, a_inWorld)) {
				pendingPickups.erase(a_id);
				++done;
			}
			if (const auto it = pendingRefStates.find(a_id); it != pendingRefStates.end() && TryApplyRefState(it->second, a_inWorld)) {
				pendingRefStates.erase(it);
				++done;
			}
			if (a_inWorld && Destructibles::ApplyWaiting(a_id)) {
				++done;
			}
			if (a_inWorld && MovedObjects::ApplyWaiting(a_id)) {
				++done;
			}
			return done;
		}

		// Every reference something waits for.
		std::vector<std::uint32_t> WaitingIds()
		{
			auto ids = loot.Containers();
			const auto held = reports.Refs();
			ids.reserve(ids.size() + held.size() + pendingHealth.size() + pendingPickups.size() + pendingRefStates.size());
			ids.insert(ids.end(), held.begin(), held.end());
			for (const auto& [id, health] : pendingHealth) {
				ids.push_back(id);
			}
			ids.insert(ids.end(), pendingPickups.begin(), pendingPickups.end());
			for (const auto& [id, state] : pendingRefStates) {
				ids.push_back(id);
			}
			const auto wrecks = Destructibles::WaitingRefs();
			ids.insert(ids.end(), wrecks.begin(), wrecks.end());
			const auto places = MovedObjects::WaitingRefs();
			ids.insert(ids.end(), places.begin(), places.end());
			return ids;
		}

		// Once a session, when what our game was told of its shared history nears what a session can hold:
		// past that, kills, pickups and loot stop being shared, and the player should know why.
		void NoticeHistory()
		{
			const auto percent = SharedHistoryPercent(dead.Size(), pickedUpKnown.size(), loot.Point().containerFrom);
			if (percent < 80 || historyNoticedIn == loot.Point().sessionId) {
				return;
			}
			historyNoticedIn = loot.Point().sessionId;
			REX::WARN("WorldSync: the session's shared history is {}% full", percent);
			Hud::Notify(Text::Format("world.history_full", { { "percent", (std::min)(percent, std::size_t{ 100 }) } }));
		}

		constexpr std::uint32_t RESYNC_RECORD = 'CONT';
		constexpr std::uint32_t RESYNC_VERSION = 1;
	}

	bool InWorld()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return false;
		}
		return !Hud::LoadingScreenUp();
	}

	void Install()
	{
		if (const auto source = RE::TESDeathEvent::GetEventSource()) {
			source->RegisterSink(&deathWatcher);
			REX::INFO("WorldSync: watching deaths");
		} else {
			REX::ERROR("WorldSync: death event source unavailable");
		}
		if (const auto source = RE::TESObjectLoadedEvent::GetEventSource()) {
			source->RegisterSink(&loadWatcher);
		}
		if (const auto source = RE::TESHitEvent::GetEventSource()) {
			source->RegisterSink(&hitWatcher);
		}
		if (const auto source = RE::TESContainerChangedEvent::GetEventSource()) {
			source->RegisterSink(&containerWatcher);
		}
		if (const auto source = RE::TESActivateEvent::GetEventSource()) {
			source->RegisterSink(&activateWatcher);
		}
		if (const auto source = RE::TESHarvestEvent::ItemHarvested::GetEventSource()) {
			source->RegisterSink(&harvestWatcher);
		} else {
			REX::ERROR("WorldSync: harvest event source unavailable; picked plants are not shared");
		}
		if (const auto source = RE::TESEquipEvent::GetEventSource()) {
			source->RegisterSink(&equipWatcher);
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&menuWatcher);
		}
		FindVendorChests();
		ResolveScriptDoors();
		ResolveGateLists();
	}

	void OnWelcome(bool a_friendlyFire)
	{
		friendlyFire = a_friendlyFire;
		dead.Expect(Clock::now());  // the session's world state follows the welcome
		heldDamage.Clear();         // what was held was for the session we were in before
	}

	LootSync::Loot& Loot()
	{
		return loot;
	}

	LootScope::HolderFacts HolderFactsOf(std::uint32_t a_ref)
	{
		return ReadHolderFacts(a_ref);
	}

	std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_container)
	{
		return LootSync::SharedIn(engine, a_container);
	}

	std::set<std::uint32_t> AliasedIn(std::uint32_t a_container)
	{
		return LootSync::Aliased(engine, a_container);
	}

	std::optional<Protocol::StorageListing> StorageListing(std::uint32_t a_container)
	{
		return loot.Listing(a_container, engine);
	}

	void QueueEvent(std::vector<std::uint8_t> a_packet)
	{
		Queue(std::move(a_packet));
	}

	namespace
	{
		// The Apply* entry points, with CanApply() worked out by the caller (once for a whole world state).
		void OfferPickup(std::uint32_t a_refId, bool a_inWorld)
		{
			if (!IsShareableRef(a_refId)) {
				return;
			}
			pickedUpKnown.insert(a_refId);
			if (!TryApplyPickup(a_refId, a_inWorld)) {
				pendingPickups.insert(a_refId);
			}
		}

		void OfferRefState(const Protocol::RefState& a_state, bool a_inWorld)
		{
			if (!IsShareableRef(a_state.refId)) {
				return;
			}
			// A live report (one that names its reporter) counts only when they are near the thing, if the
			// thing is here to measure against: a lock changed from across the map is a mistake or mischief.
			// A world state names nobody and is taken as it is.
			if (a_state.playerId != 0) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_state.refId);
				if (ref && ref->Get3D()) {
					const auto senderAt = RemotePlayers::PositionOf(a_state.playerId);
					const auto reach = IsSharedGate(ref) ? SharedGates::REACH : ACTIVATION_REACH;  // a gate's terminal can be down the corridor
					if (!senderAt || senderAt->GetDistance(ref->data.location) > reach) {
						++refStatesRefused;
						REX::INFO("WorldSync: {:08X} state from player {} refused: they are {} away", a_state.refId, a_state.playerId, senderAt ? std::format("{:.0f}", senderAt->GetDistance(ref->data.location)) : "nowhere known");
						return;
					}
				}
			}
			// A world state repeats what we know each time it comes: only news is applied (DoorShare::Book).
			if (!refBook.Take(a_state)) {
				return;
			}
			// So our own watcher doesn't report this change back, nor what the state leaves out (DoorShare::Heard).
			if (const auto it = watched.find(a_state.refId); it != watched.end()) {
				const auto                ours = ReadRefState(RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_state.refId), !it->second.quiet);
				const Protocol::RefState had = it->second.lastSent.refId != 0 ? it->second.lastSent : ours.value_or(Protocol::RefState{});
				it->second.lastSent = DoorShare::Heard(a_state, had, !ours || ours->open != Protocol::RefState::kUnknown);
			}
			if (TryApplyRefState(a_state, a_inWorld)) {
				pendingRefStates.erase(a_state.refId);
			} else {
				pendingRefStates[a_state.refId] = a_state;
			}
		}
	}

	void ApplyRemoteDeath(const Protocol::ActorDeath& a_death)
	{
		if (Protocol::IsSpawnRef(a_death.refId)) {
			Spawns::ApplyDeath(a_death);  // a spawn's: its death is the session's for no longer than it lives
			return;
		}
		// The same death again from the player we heard it from: where the corpse came to rest in their game
		// (Protocol::CORPSE_REST_WINDOW). A report still held kills it there; one done moves our corpse.
		if (dead.RestsFrom(a_death.refId, a_death.playerId, Clock::now())) {
			++restsApplied;
			if (reports.Rests(a_death.refId, a_death.x, a_death.y, a_death.z) || !CanApply()) {
				return;
			}
			if (const auto actor = LoadedActor(a_death.refId); actor && DeadHere(actor)) {
				PlaceCorpse(actor, RE::NiPoint3{ a_death.x, a_death.y, a_death.z });
			}
			return;
		}
		TakeDeath(a_death.refId, reports.Received(a_death, Clock::now()), CanApply());
	}

	FoundDead::Npc DescribeStory(RE::Actor* a_actor)
	{
		return DescribeDead(a_actor);
	}

	void ApplyRemoteActivation(std::uint32_t a_refId, std::uint32_t a_playerId)
	{
		if (!InWorld() || Story::InOpening() || !IsShareableRef(a_refId)) {
			return;
		}
		const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_refId);
		if (!ref || ref->IsDeleted() || !ref->Get3D() || !ref->GetParentCell()) {
			return;  // not where we are: whatever it drives reaches us as a door state, or not at all
		}
		const auto base = ref->GetObjectReference();
		if (!base || !base->Is(RE::ENUM_FORM_ID::kACTI)) {
			return;
		}
		// A shared gate pressed by hand: its state follows (SharedGates), where a replayed press would toggle it, the
		// wrong way if ours and theirs disagree.
		if (IsSharedGate(ref)) {
			++gatePressesLeft;
			return;
		}
		// A trap: its state is shared (Traps). A press replayed by the stand-in would disarm ours, and the disarm's XP
		// and reward go to our player.
		if (Traps::IsTrap(ref)) {
			++activationsRefused;
			return;
		}
		// Only a press by someone standing at the thing: a report naming a far-away activator (or
		// from a player we know nothing about) would run its script here on their say-so.
		const auto senderAt = RemotePlayers::PositionOf(a_playerId);
		if (!senderAt || senderAt->GetDistance(ref->data.location) > ACTIVATION_REACH) {
			++activationsRefused;
			return;
		}
		// The friend's stand-in presses it, not our player: an activator's script acts on whoever pressed it
		// (a sink's drink gave our player rads, a teleport marker moved them). Without a stand-in here (not
		// spawned yet, or gone) it isn't pressed: the door or lock it drives is on the friend's watch and
		// arrives as its state.
		const auto standIn = RE::TESForm::GetFormByID<RE::Actor>(RemotePlayers::ActorIdOf(a_playerId));
		if (!standIn || standIn->IsDeleted() || !standIn->Get3D() || !standIn->GetParentCell()) {
			++activationsRefused;
			return;
		}
		ref->ActivateRef(standIn, nullptr, 0, false, false, false);
		++activationsApplied;
	}

	bool ApplyScriptDoor(const Protocol::QuestStage& a_stage)
	{
		MainThread::Assert("WorldSync::ApplyScriptDoor");
		const auto door = ScriptDoors::Find(scriptDoorQuests, a_stage.quest, a_stage.stage);
		if (!door) {
			return false;
		}
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_stage.quest);
		if (quest && !scriptDoors.Pending(*door) && !QuestStageLayout::IsStageDone(quest, a_stage.stage)) {
			const auto& known = ScriptDoors::DOORS[*door];
			REX::INFO("WorldSync: {} is {} in another player's game ({:08X} stage {}); {}", known.what, known.state, a_stage.quest, a_stage.stage,
				known.loaded ? "set here once it is loaded" : "set here at once");
		}
		scriptDoors.Take(*door);
		return true;
	}

	void ApplyWorldState(const Protocol::WorldState& a_state)
	{
		MainThread::Assert("WorldSync::ApplyWorldState");
		const bool inWorld = CanApply();
		for (const auto id : a_state.deadActors) {
			TakeDeath(id, DeathReports::Book::Replayed(Clock::now()), inWorld);
		}
		dead.Settle();
		// The server sends the changes our world doesn't have: those made since our save. Our own
		// are skipped (see ContainerLedger::Receiver).
		engine.inWorld = inWorld;
		const auto strays = loot.Numbers().Numbers().Stats().strays;
		const auto lost = loot.CatchUp(a_state.containerFirstIndex, a_state.containerChanges, Clock::now(), engine);
		engine.inWorld.reset();
		if (lost) {
			REX::WARN("WorldSync: the server's log is shorter than our save (lost {} changes); counting again from #{}", lost, a_state.containerFirstIndex);
		}
		if (const auto letGo = loot.Numbers().Numbers().Stats().strays - strays) {
			REX::WARN("WorldSync: the server's log ends at #{}; let go of {} waiting container changes numbered past it", a_state.containerFirstIndex, letGo);
		}
		for (const auto id : a_state.pickedUp) {
			OfferPickup(id, inWorld);
		}
		for (const auto& state : a_state.refStates) {
			OfferRefState(state, inWorld);
		}
		for (const auto& stage : a_state.questStages) {
			if (!ApplyScriptDoor(stage)) {
				QuestSync::Apply(stage, false);
			}
		}
		REX::INFO("WorldSync: session state: {} dead, {} container changes from #{}, {} pickups, {} doors, {} quest stages",
			a_state.deadActors.size(), a_state.containerChanges.size(), a_state.containerFirstIndex, a_state.pickedUp.size(),
			a_state.refStates.size(), a_state.questStages.size());
		NoticeHistory();
	}

	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health)
	{
		// A spawn's: our own NPC, or our copy of a friend's (none placed: there is nothing to set).
		if (Protocol::IsSpawnRef(a_refId)) {
			if (const auto local = Spawns::LocalOf(a_refId)) {
				TrySetHealth(local, a_health, CanApply());
			}
			return;
		}
		if (!IsShareableRef(a_refId)) {
			return;
		}
		if (dead.Contains(a_refId)) {
			DropHealth(a_refId, "a friend's report of its health", a_health);
			return;
		}
		if (TrySetHealth(a_refId, a_health, CanApply())) {
			pendingHealth.erase(a_refId);
		} else {
			if (pendingHealth.size() >= MAX_PENDING_HEALTH && !pendingHealth.contains(a_refId)) {
				pendingHealth.clear();  // a flood of reports for actors we never load: start over rather than grow
			}
			pendingHealth[a_refId] = a_health;
		}
	}

	void ApplyRemoteDamage(std::uint32_t a_refId, float a_damage)
	{
		if (!(a_damage > 0.0f && a_damage <= 1.0f)) {
			return;
		}
		TakeDamage(a_refId, a_damage, Clock::now(), false);
	}

	bool MayClaim(std::uint32_t a_refId, bool a_essential)
	{
		return dead.MayClaim(a_refId, Clock::now(), a_essential);
	}

	bool IsEssential(RE::Actor* a_actor)
	{
		const auto npc = a_actor->GetNPC();
		if ((npc && npc->IsEssential()) || a_actor->boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential)) {
			return true;
		}
		// A quest's alias that holds it can make it essential (a companion's).
		const auto aliases = a_actor->extraList ? a_actor->extraList->GetByType<RE::ExtraAliasInstanceArray>() : nullptr;
		if (!aliases) {
			return false;
		}
		const RE::BSAutoReadLock l{ aliases->aliasArrayLock };
		return std::ranges::any_of(aliases->aliasArray, [](const RE::BGSRefAliasInstanceData& a_entry) { return a_entry.alias && a_entry.alias->flags.all(RE::BGSBaseAlias::FLAGS::kEssential); });
	}

	void OnActorOwners(const std::vector<Protocol::ActorOwner>& a_owners)
	{
		// Another player's game runs these, so they live there. A persistent NPC (or one of a cell we left
		// lately) is in our memory even where we aren't, and when we host, ours is checked (CheckFoundDead).
		for (const auto& owner : a_owners) {
			if (owner.playerId != 0 && !loot.Ours(owner.playerId) && IsShareableRef(owner.refId)) {
				toCheck.insert(owner.refId);
			}
			// A game runs one we have dead: it is alive there, so alive for the session (the host's game brought it back
			// with a cell reset, or its death was undone, or what we knew was from another world: the server gives
			// nobody a dead NPC). No longer dead here, whether our copy lives (its death was held: essential here, or
			// not loaded yet) or lies dead (its own cell's reset brings it back; until then its hits on our stand-in in
			// the runner's game count all the same: PlayerHits). Before, only a held death was forgotten: a joiner whose
			// copy had died kept the NPC dead, and could neither hurt it nor be hurt by it once the host's game had it
			// back. Not a death our own game reported a moment ago (SessionDeaths::OWNED_CROSSING).
			if (owner.playerId != 0 && IsShareableRef(owner.refId) && dead.OnOwned(owner.refId, Clock::now())) {
				const bool held = reports.Holds(owner.refId);
				reports.Forget(owner.refId);
				essentialHeld.erase(owner.refId);
				const auto actor = LoadedActor(owner.refId);
				REX::INFO("WorldSync: {:08X} lives after all (player {} runs it); its death is forgotten here{}", owner.refId, owner.playerId,
					held ? " (it was held)" : actor && DeadHere(actor) ? "; our copy lies dead until its cell resets" : "");
			}
		}
	}

	void ApplyContainerChange(const Protocol::IndexedContainerChange& a_change)
	{
		MainThread::Assert("WorldSync::ApplyContainerChange");
		loot.Live(a_change, Clock::now(), engine);
	}

	void OnMenuShown(RE::ObjectRefHandle a_container)
	{
		// Only the handle is taken here (off the main thread). The menu may have drawn its list by the time this runs (a
		// frame or so): a take that goes in now shows at once anyway, as a friend's change to a chest our player has open
		// does (the console's removeitem, which the open menu follows).
		MainThread::Post([a_container] {
			const auto ref = a_container.get();
			if (!ref || !CanApply()) {
				return;
			}
			engine.inWorld = true;
			loot.Opened(ref->GetFormID(), engine);
			engine.inWorld.reset();
		});
	}

	void ApplyRemotePickup(std::uint32_t a_refId)
	{
		OfferPickup(a_refId, CanApply());
	}

	void ApplyRemoteRefState(const Protocol::RefState& a_state)
	{
		OfferRefState(a_state, CanApply());
	}

	void Frame(bool a_hosting)
	{
		MainThread::Assert("WorldSync::Frame");
		CopyWatch::Frame();
		std::vector<std::uint32_t> newDeaths;
		std::vector<std::pair<std::uint32_t, bool>> newLoads;
		std::vector<std::uint32_t>                  newRefLoads;
		Killers                    killers;
		std::vector<std::uint32_t> newDying;
		Killers                    dyingBy;
		std::vector<Hit>           newHits;
		std::vector<std::uint32_t> newPickups;
		std::vector<std::uint32_t> newHarvests;
		std::vector<StandInHit> newStandInHits;
		std::vector<Activation>    newActivations;
		std::vector<Activation>    newNpcDoors;
		std::vector<Move>          newMoves;
		std::vector<std::pair<std::uint32_t, bool>> newRuntimeDeaths;
		{
			std::scoped_lock l{ inboxLock };
			newDeaths.swap(deathInbox);
			newRuntimeDeaths.swap(runtimeDeathInbox);
			newLoads.swap(loadInbox);
			newRefLoads.swap(refLoadInbox);
			killers.swap(killInbox);
			killInbox.clear();
			newDying.swap(dyingInbox);
			dyingBy.swap(dyingKillers);
			newHits.swap(hitInbox);
			newPickups.swap(pickupInbox);
			newHarvests.swap(harvestInbox);
			newStandInHits.swap(standInHits);
			newActivations.swap(activateInbox);
			newNpcDoors.swap(npcDoorInbox);
			newMoves.swap(lootInbox);
		}

		ReportDeaths(newDeaths, killers);
		ReportFalling(newDying, dyingBy);
		ReportRuntimeDeaths(newRuntimeDeaths);
		// After the deaths: a corpse looted the frame it died goes out dead first.
		ReportMoves(newMoves);

		TrackLoads(newLoads);
		CheckFoundDead(a_hosting);

		// Report the health of actors we hit last frame (damage has been applied by now).
		if (const auto info = HealthInfo(); info && !hitLastFrame.empty()) {
			for (const auto id : hitLastFrame) {
				const auto wire = WireId(id);
				const auto actor = wire ? LoadedActor(id) : nullptr;
				if (!actor || actor->IsDead(false)) {
					continue;
				}
				if (dead.Contains(id)) {
					DropHealth(id, "our hit on it");  // the session would drop it too: ours fights a copy alive only here
					continue;
				}
				const auto& values = static_cast<RE::ActorValueOwner&>(*actor);
				const float maximum = values.GetPermanentActorValue(*info);
				if (maximum > 0.0f) {
					const float fraction = values.GetActorValue(*info) / maximum;
					Queue(Protocol::Encode(Protocol::ActorHealth{ wire, fraction }, Protocol::MessageType::kReportHealth));
					healthShare.Sent(wire, fraction, Clock::now());
				}
			}
		}
		hitLastFrame.clear();
		// Friends' damage held for NPCs we run (DamageHold): taken off now if it can be, before their health is shared,
		// so what we share has it.
		RetryHeldDamage(Clock::now());
		// The NPCs we run: what they lost (or got back) since, by anything but our hits.
		if (const auto info = HealthInfo()) {
			ShareHealth(*info, Clock::now());
		}
		// What our hits took off copies of NPCs other games run: theirs to take off (their health comes back shared).
		for (const auto& [wire, damage] : CopyWatch::TakeDamage()) {
			Queue(Protocol::Encode(Protocol::ActorHealth{ wire, damage }, Protocol::MessageType::kReportDamage));
			++damageReported;
		}
		// Our hits (CopyHealth::Judge): our player's, and on a copy of an NPC another game runs, those of the actors our
		// game runs (our companion, a settler, a raider of ours). Fighting or talking to an NPC another player's game runs
		// brings it over to ours; another actor of ours claims it only with the blow that killed it (CopyHealth::After).
		const auto local = NpcSync::LocalPlayer();
		for (const auto& hit : newHits) {
			// The id the session knows it by (a runtime NPC's spawn id); one not shared is nothing to report.
			const auto wire = WireId(hit.target);
			if (wire == 0) {
				continue;
			}
			const auto owner = NpcSync::Owner(wire);
			// Another actor's hit on an NPC our game runs, or nobody does, is nothing to report: not looked into.
			const bool copy = owner != 0 && local != 0 && owner != local;
			const auto hitter = hit.cause == Protocol::PLAYER_REF_ID ? CopyHealth::Hitter::kOurPlayer :
			                    copy                                 ? CopyHealth::Judge(CopyWatch::CauseOf(hit.cause), local) :
			                                                           CopyHealth::Hitter::kNotOurs;
			const auto target = hitter != CopyHealth::Hitter::kNotOurs ? RE::TESForm::GetFormByID<RE::Actor>(hit.target) : nullptr;
			const bool killed = dead.Contains(hit.target) || (target && target->IsDead(false));
			const auto after = CopyHealth::After(hitter, owner, local, killed, copy && hitter != CopyHealth::Hitter::kNotOurs && CopyWatch::OwnersToDeal(hit.blow, hit.target, owner, hit.sourceRef));
			// On a copy what the hit took goes as damage (CopyWatch books it: TakeDamage below), not as the copy's health.
			if (after.report && !copy) {
				hitLastFrame.insert(hit.target);
			}
			if (after.claim) {
				NpcSync::OnLocalInteraction(wire);
			}
		}
		for (const auto& activation : newActivations) {
			NpcSync::OnLocalInteraction(activation.ref);
			NpcSync::OnActivated(activation.ref);
			Traps::OnPlayerActivated(activation.ref);  // a mine gone a moment later was picked up
			// A button, switch or lever: the others press it too, so what it drives happens in their games. Not a trap: its
			// state is shared (Traps), and a press replayed by our stand-in would disarm theirs and give their player the XP.
			const auto base = RE::TESForm::GetFormByID(activation.base);
			if (base && base->Is(RE::ENUM_FORM_ID::kACTI) && !Traps::IsTrap(RE::TESForm::GetFormByID<RE::TESObjectREFR>(activation.ref))) {
				Queue(Protocol::Encode(Protocol::Activation{ 0, activation.ref }, Protocol::MessageType::kReportActivation));
				++activationsSent;
			}
		}
		for (const auto& hit : newStandInHits) {
			// One made at runtime hits under the spawn id our game shares it as: only our own (a copy of a friend's
			// hits in its owner's game; one not shared is in no other game).
			const auto attacker = hit.byPlayer || IsShareableRef(hit.attacker) ? hit.attacker : Spawns::OwnIdOf(hit.attacker);
			if (!hit.byPlayer && attacker == 0) {
				continue;
			}
			// An explosion that goes off in their game too hurts them there (RemoteEdits::PassStandInHit).
			if (hit.explosion) {
				const auto standIn = RE::TESForm::GetFormByID<RE::TESObjectREFR>(hit.actor);
				const auto seen = standIn ? WeaponFire::BlastAt(standIn, hit.sourceRef) : std::nullopt;
				if (!RemoteEdits::PassStandInHit(true, seen ? &*seen : nullptr)) {
					++standInBlowsThere;
					continue;
				}
			}
			if (const auto playerId = RemotePlayers::PlayerIdFor(hit.actor)) {
				Queue(Protocol::Encode(Protocol::PlayerHit{ playerId, hit.damage, hit.byPlayer, attacker }, Protocol::MessageType::kPlayerHit));
			}
		}

		const auto now = Clock::now();
		MatchPickups(now, newActivations, newPickups);
		for (const auto id : newHarvests) {
			Queue(Protocol::Encode(Protocol::RefPickedUp{ id }, Protocol::MessageType::kReportPickup));
			++harvestsSent;
		}
		WatchNpcDoors(now, newNpcDoors);
		WatchRefStates(now, newActivations);
		LookAtScriptDoors(now, a_hosting);
		// Container changes that came before one we still lack, with nothing arriving for a while: the
		// server is asked again (the answer to the last request may have been refused, or never come), and
		// after asking long enough the gap is skipped (ContainerLedger::Ledger::Skip).
		const auto& numbers = loot.Numbers().Numbers();
		const auto  skippedBefore = numbers.Stats().skipped;
		const auto  nextBefore = numbers.Next();
		if (const auto request = loot.Due(now, engine)) {
			if (Story::InOpening()) {
				loot.Unasked();  // never sent from the opening (Session::SendWorldEvents): no world state answers it
			} else {
				Queue(Protocol::Encode(*request));
				// Our world hasn't changed, so the server keeps our storage listings fresh and nothing is settled again (a gap request).
				REX::INFO("WorldSync: still missing container change #{} ({} waiting); asking again", request->containerFrom, numbers.Waiting());
			}
		}
		if (const auto skipped = numbers.Stats().skipped - skippedBefore) {
			REX::WARN("WorldSync: gave up on container changes #{} to #{} after asking {} times; the {} after them went in ({} still waiting)", nextBefore,
				nextBefore + skipped - 1, ContainerLedger::Ledger::SKIP_AFTER, numbers.Next() - nextBefore - skipped, numbers.Waiting());
		}
		WarnIfLootFull();
		// Remote changes waiting for their reference: tried as it loads; all of them when the player comes out
		// of a loading screen (or the opening) or into another cell; otherwise a few at a time, slowly.
		const bool inWorld = CanApply();
		std::uint32_t cell = 0;
		if (inWorld) {
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto parent = player ? player->GetParentCell() : nullptr;
			cell = parent ? parent->GetFormID() : 0;
		}
		// A gate just loaded, or just come back to after a loading screen, runs its OnLoad first (SharedGates::Settled).
		if (inWorld && !wasApplying) {
			applyingSince = now;
		}
		wasApplying = inWorld;
		std::erase_if(gateLoads, [&](const auto& a_entry) { return now - a_entry.second > SharedGates::LOAD_SETTLE; });
		for (const auto id : newRefLoads) {
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
			const auto base = ref ? ref->GetObjectReference() : nullptr;
			if (base && base->Is(RE::ENUM_FORM_ID::kACTI)) {
				gateLoads[id] = now;
			} else if (base && base->Is(RE::ENUM_FORM_ID::kDOOR)) {
				// Read while it stays loaded (PollDoor), from DoorShare::LOAD_SETTLE on. Loaded again (its cell came back, a
				// reset relocked it), it is where it stands once more: the poll's look at it starts afresh.
				loadedDoors.insert_or_assign(id, now);
				if (const auto it = watched.find(id); it != watched.end() && it->second.quiet && !DoorShare::KeptWhenGone(it->second.held)) {
					watched.erase(it);
				}
			}
		}
		const bool anyWaiting = !reports.Empty() || !pendingHealth.empty() || !loot.Waiting().empty() || !pendingPickups.empty() || !pendingRefStates.empty() || Destructibles::AnyWaiting() ||
		                       MovedObjects::AnyWaiting();
		const auto started = Clock::now();
		const auto plan = retrySchedule.Frame(now, inWorld, cell, std::move(newRefLoads), anyWaiting, [] { return WaitingIds(); });
		for (const auto id : plan.loaded) {
			loadApplied += static_cast<std::uint32_t>(ApplyWaiting(id, true));
		}
		for (const auto id : plan.others) {
			ApplyWaiting(id, true);
		}
		if (inWorld && anyWaiting) {
			retryUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
			retryUsMax = (std::max)(retryUsMax, retryUs);
		}
		AgeLoot(now, inWorld);
	}

	void FlushLoot()
	{
		MainThread::Assert("WorldSync::FlushLoot");
		std::vector<std::uint32_t> deaths;
		Killers                    killers;
		std::vector<Move>          moves;
		std::vector<std::pair<std::uint32_t, bool>> runtimeDeaths;
		{
			std::scoped_lock l{ inboxLock };
			deaths.swap(deathInbox);
			killers.swap(killInbox);
			moves.swap(lootInbox);
			runtimeDeaths.swap(runtimeDeathInbox);
		}
		// The deaths first, as in a frame: a corpse looted in the last moments goes out dead, so the others
		// put its loot in (after a load Reset would forget the death, and they would hold the loot as a
		// living NPC's until they gave up on it).
		ReportDeaths(deaths, killers);
		ReportRuntimeDeaths(runtimeDeaths);
		ReportMoves(moves);
	}

	std::uint32_t TakePlayerKills()
	{
		return playerKills.exchange(0);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		std::scoped_lock l{ outgoingLock };
		return std::exchange(outgoing, {});
	}

	void Reset()
	{
		dead.Reset(Clock::now());
		reports.Clear();
		storyKillsLogged.clear();
		essentialHeld.clear();
		switchOnTried.clear();
		pendingHealth.clear();
		friendsBlows.clear();
		pendingPickups.clear();
		pickupUnplacedSince.clear();
		pickedUpKnown.clear();
		hitLastFrame.clear();
		healthShare.Clear();
		recentActivations.clear();
		watched.clear();
		pendingRefStates.clear();
		refBook.Clear();
		opensShared.clear();
		opensUnlogged.clear();
		gates.clear();
		gateScripts.clear();
		gatesApplied.Clear();
		scriptDoors.Clear();
		scriptDoorsReported = false;
		nextDoorLook = {};
		nextDoorPoll = {};  // loadedDoors is kept: the doors still loaded fire no new load event, and the rest are dropped when read
		doorSweep.clear();
		doorSweepAt = 0;
		unmatchedPickups.clear();
		// What is loaded is kept (a save's actors can load before the game says the save has loaded), and
		// all of it is checked again when we host: what we knew to be dead is forgotten.
		falling.Clear();
		checkAllLoaded = true;
		CopyWatch::Reset();
		std::scoped_lock l{ inboxLock };
		pickupInbox.clear();
		harvestInbox.clear();
		standInHits.clear();
		activateInbox.clear();
		npcDoorInbox.clear();
		deathInbox.clear();
		killInbox.clear();
		dyingInbox.clear();
		dyingKillers.clear();
		runtimeDeathInbox.clear();
		hitInbox.clear();
		lootInbox.clear();
		std::scoped_lock o{ outgoingLock };
		outgoing.clear();
	}

	void SaveResyncPoint(const F4SE::SerializationInterface* a_intfc)
	{
		const auto point = loot.Point();
		if (point.sessionId == 0) {
			return;
		}
		const auto& waiting = loot.Waiting();
		const auto  count = static_cast<std::uint32_t>(waiting.size());
		if (!a_intfc->OpenRecord(RESYNC_RECORD, RESYNC_VERSION) ||
			!a_intfc->WriteRecordData(point.sessionId) || !a_intfc->WriteRecordData(point.containerFrom) || !a_intfc->WriteRecordData(count)) {
			REX::ERROR("WorldSync: failed to save the session record");
			return;
		}
		for (const auto& entry : waiting) {
			a_intfc->WriteRecordData(entry.index);
			a_intfc->WriteRecordData(entry.change.container);
			a_intfc->WriteRecordData(entry.change.item);
			a_intfc->WriteRecordData(entry.change.count);
		}
	}

	void LoadResyncPoint(const F4SE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		RevertResyncPoint();
		std::uint64_t savedSession = 0;
		std::uint32_t savedNext = 0;
		std::uint32_t count = 0;
		constexpr std::uint32_t HEADER = sizeof(savedSession) + sizeof(savedNext) + sizeof(count);
		constexpr std::uint32_t ENTRY = 4 * sizeof(std::uint32_t);
		if (a_version != RESYNC_VERSION || a_length < HEADER ||
			a_intfc->ReadRecordData(savedSession) != sizeof(savedSession) ||
			a_intfc->ReadRecordData(savedNext) != sizeof(savedNext) ||
			a_intfc->ReadRecordData(count) != sizeof(count) || count > (a_length - HEADER) / ENTRY) {
			REX::WARN("WorldSync: ignoring an unreadable session record");
			return;
		}
		std::vector<LootSync::Pending> saved;
		for (std::uint32_t i = 0; i < count; ++i) {
			LootSync::Pending entry{};
			a_intfc->ReadRecordData(entry.index);
			a_intfc->ReadRecordData(entry.change.container);
			a_intfc->ReadRecordData(entry.change.item);
			a_intfc->ReadRecordData(entry.change.count);
			saved.push_back(entry);
		}
		const auto waiting = saved.size();
		loot.LoadRecord(savedSession, savedNext, std::move(saved));
		REX::INFO("WorldSync: save has session {:016X} container changes up to #{} ({} waiting)", savedSession, savedNext, waiting);
	}

	void RevertResyncPoint()
	{
		loot.LoadRecord(0, 0, {});
	}

	std::string Describe()
	{
		using LootScope::Scope;
		const auto  kept = [](Scope a_scope) { return loot.KeptSent(a_scope) + loot.KeptApplied(a_scope); };
		const auto& receiver = loot.Numbers();
		const auto& numbers = receiver.Numbers();
		const auto& counts = numbers.Stats();
		const auto& stats = loot.Stats();
		// personal= means what it did in 0.8.5: other players' personal items left alone (pickups and loot).
		// The player's own moves kept here are personalSent= and, by what held the item, holderDropped=.
		const auto  holderDropped = loot.KeptSent(Scope::kPerPlayer) + loot.KeptSent(Scope::kVendor) + loot.KeptSent(Scope::kNotYetDead) + loot.KeptSent(Scope::kUnsupported);
		return std::format("dead={} pending={} reported={} applied={} containerNext={} gap={} gaps={} rerequests={} rewound={} dupChanges={} pendingLoot={} pendingDropped={} pendingFull={} lootSent={} weaponArmor={} vendor={} barter={} perPlayer={} notYetDead={} unsupported={} holderDropped={} pendingPickups={} watched={} linked={} acts={}/{}/{} states={}/{} corpses={} pendingDoors={} personal={} personalSent={} ownSkipped={} dupRisk={} offThread={} foundDead={} ownStoryDead={} leftToEvent={} loadedActors={} flora={}/{} emptied={}/{}/{} emptiedKept={}",
			dead.Size(), reports.Size(), reported, applied, numbers.Next(), numbers.Waiting(), counts.gaps, counts.asked, counts.rewinds, counts.duplicates, loot.Waiting().size(), stats.pendingDropped, stats.pendingFull, stats.sent, kept(Scope::kWeaponArmor), kept(Scope::kVendor), barterMoves.load(),
			kept(Scope::kPerPlayer), kept(Scope::kNotYetDead), kept(Scope::kUnsupported), holderDropped, pendingPickups.size(), watched.size(), linkedWatched, activationsSent, activationsApplied, activationsRefused, refStatesApplied, refStatesRefused, corpsesMoved, pendingRefStates.size(),
			personalSkipped + loot.KeptApplied(Scope::kPersonal), loot.KeptSent(Scope::kPersonal), receiver.OwnSkipped(), receiver.DupRisk(), MainThread::OffThread(), foundDead, ownStoryDead, leftToEvent, loadedActors.size(), harvestsSent, harvestsApplied, stats.emptiedSent, stats.emptiedApplied, stats.emptiedStacks, stats.emptiedKept) +
		       std::format(" storyKillKept={} switchedOnToKill={} loadApplied={} retryUs={}/{} revived={} corrected={} killsRetried={} settled={} strayChanges={} gapsSkipped={}/{} downNotDead={} essentialHolds={} healthShared={}/{} healthDropped={} damage={}/{}/{} damageHeld={}/{}/{}/{} scriptDoors={}/{}/{}/{} gates={}/{}/{}/{} npcDoors={}", storyKillsKept, switchedOnToKill, loadApplied, retryUs, retryUsMax,
			   dead.Revived(), dead.Corrected(), killsRetried, dead.Settled(Clock::now()), counts.strays, counts.skips, counts.skipped, downNotDead, essentialHolds, healthShare.Shared(), healthShare.Firsts(), healthDropped, damageReported, damageApplied, damageIgnored,
			   heldDamage.Size(), damageLate, heldDamage.Expired(), damageDropped, scriptDoorsSent, scriptDoorsSet, scriptDoorsGaveUp,
			   scriptDoors.PendingCount(), gatesSet, gateReportsHeld, gatePressesLeft, gatesApplied.Size(), npcDoorsWatched) +
		       std::format(" polledDoors={}/{} doorsFound={}/{} reportsHeld={} reportsPaced={} doorPollUs={}/{}", doorsPolled, loadedDoors.size(), doorsFound, doorWalk.Passes(), reportsHeld, reportsPaced, doorPollUs, doorPollUsMax) +
		       std::format(" puppetHitsOnUs={} standInBlowsThere={} rests={}/{} openApplied={}", hitsOnUsByPuppets.load(), standInBlowsThere, restsReported, restsApplied, stats.openApplied);
	}
}
