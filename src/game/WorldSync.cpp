#include "game/WorldSync.h"

#include "MainThread.h"
#include "game/DeathReports.h"
#include "game/FoundDead.h"
#include "game/Hud.h"
#include "game/LootScope.h"
#include "game/LootSync.h"
#include "game/SessionDeaths.h"
#include "game/NpcSync.h"
#include "game/PendingContainers.h"
#include "game/QuestSync.h"
#include "game/Puppets.h"
#include "game/RemoteEdits.h"
#include "game/RemotePlayers.h"
#include "game/Story.h"
#include "game/WeaponFire.h"
#include "game/WorkshopSync.h"

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
		std::unordered_set<std::uint32_t> killInbox;  // deaths in deathInbox the player caused
		// Actors whose 3D loaded (true) or unloaded (false), in order (the host checks them for deaths no event reported).
		std::vector<std::pair<std::uint32_t, bool>> loadInbox;
		// Every shareable reference whose 3D loaded (any kind: containers, loose items and doors too), for
		// what waits for it to load.
		std::vector<std::uint32_t> refLoadInbox;
		std::atomic<std::uint32_t>        playerKills{ 0 };  // anything the player killed, shared or not
		std::vector<std::uint32_t> hitInbox;
		std::vector<std::pair<std::uint32_t, float>> blowInbox;  // the same hits, with the health each took (WeaponFire::OurBlows)
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
		struct StandInHit
		{
			std::uint32_t actor;
			float         damage;
			bool          byPlayer;  // we hit them, rather than an NPC in our world
			std::uint32_t attacker;  // the NPC that hit them (0 when byPlayer)
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
			Protocol::RefState lastSent;
			Clock::time_point  until;
		};
		std::unordered_map<std::uint32_t, Watched>           watched;
		std::uint32_t                                        linkedWatched = 0;  // linked refs (a terminal's door) put on watch
		std::uint32_t                                        refStatesApplied = 0;  // lock/open changes taken from other players

		// Activators (buttons, switches, levers) pressed in another player's game are pressed here too, by
		// their stand-in (so our own watcher, which takes only the player's presses, never reports it back).
		std::uint32_t                                          activationsSent = 0, activationsApplied = 0, activationsRefused = 0;
		std::uint32_t                                          refStatesRefused = 0, corpsesMoved = 0;
		constexpr float                                        CORPSE_SLACK = 96.0f;  // a corpse this close to where it fell for the reporter stays put
		constexpr float                                        ACTIVATION_REACH = 1500.0f;  // a button is pressed from arm's length; this allows for lag and a stand-in that is a step behind
		std::unordered_map<std::uint32_t, Protocol::RefState> pendingRefStates;

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

		FoundDead::Npc DescribeDead(RE::Actor* a_actor);

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
			if (actor && actor->IsDead(false)) {
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

			// The console kill goes through the engine's normal death path (death animation,
			// ragdoll, quest/script death events), exactly as if it happened here.
			// The corpse lies where it fell in the reporter's game (each game's copy stood somewhere else when
			// the report came; a far-off corpse is confusing and its loot is in the wrong place), unless that is
			// far from ours: a report doesn't pull an NPC across the map.
			const bool  placed = at.x != 0.0f || at.y != 0.0f || at.z != 0.0f;
			const float apart = actor->data.location.GetDistance(at);
			if (placed && apart > CORPSE_SLACK && RemoteEdits::MayMoveCorpse(apart)) {
				RE::Console::ExecuteCommand(std::format("{:08X}.setpos x {:.1f}", a_refId, at.x).c_str());
				RE::Console::ExecuteCommand(std::format("{:08X}.setpos y {:.1f}", a_refId, at.y).c_str());
				RE::Console::ExecuteCommand(std::format("{:08X}.setpos z {:.1f}", a_refId, at.z).c_str());
				NpcSync::CountCommands(3);
				++corpsesMoved;
			}
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
			if (IsShareableRef(a_refId) && dead.Add(a_refId, Clock::now()) && !TryKill(a_refId, a_death, a_inWorld)) {
				reports.Hold(a_refId, a_death);
			}
		}

		class DeathWatcher :
			public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent& a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				const auto ref = a_event.actorDying.get();
				if (!a_event.dead || !ref) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto id = ref->GetFormID();
				const auto actor = ref->As<RE::Actor>();
				const auto killer = a_event.actorKiller.get();
				const bool byPlayer = killer && killer->IsPlayerRef() && !ref->IsPlayerRef();
				if (actor && Puppets::IsPuppet(actor)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (byPlayer) {
					++playerKills;
				}
				if (!IsShareableRef(id)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ inboxLock };
				deathInbox.push_back(id);
				if (byPlayer) {
					killInbox.insert(id);
				}
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
				const bool alive = actor && !actor->IsDeleted() && actor->Get3D() && !actor->IsDead(false);
				if (dead.OnLoaded(id, alive, reports.Holds(id) || pendingHealth.contains(id), now)) {
					REX::INFO("WorldSync: {:08X} is alive again here (its cell reset); no longer dead for us", id);
				} else if (actor && actor->IsDead(false)) {
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
			for (const auto id : toCheck) {
				const auto actor = RE::TESForm::GetFormByID<RE::Actor>(id);
				if (!actor || actor->IsDeleted() || !actor->IsDead(false) || actor->IsPlayerRef() || Puppets::IsPuppet(actor) || dead.Contains(id)) {
					continue;
				}
				if (!FoundDead::Shared(Story::Shared(), DescribeDead(actor))) {
					++ownStoryDead;
					continue;
				}
				dead.Add(id, Clock::now());
				dead.SawDead(id);
				// Said to be found dead, so the server takes it over the NPC's runner (see Server.cpp, handleDeath).
				// No position: a friend's copy dies where it stands. Ours can lie in another cell than theirs
				// (an NPC that walks about, alive in their game), and coordinates in one cell are nowhere in
				// another.
				ReportDead({ .refId = id, .foundDead = true });
				++foundDead;
				reports.Forget(id);
				pendingHealth.erase(id);
			}
			toCheck.clear();
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
			// Our copy may be kept from a friend's explosion: what that took comes back first, and the report is measured
			// without the health lent against it.
			WeaponFire::EndLending(a_refId);
			auto&       values = static_cast<RE::ActorValueOwner&>(*actor);
			const float maximum = values.GetPermanentActorValue(*info);
			const float target = std::clamp(a_fraction, 0.0f, 1.0f) * maximum;
			const float delta = target - values.GetActorValue(*info);
			if (delta < 0.0f) {
				values.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, delta);
			} else if (delta > 0.0f) {
				values.RestoreActorValue(*info, delta);
			}
			return true;
		}

		class HitWatcher :
			public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				const auto target = a_event.target.get();
				const auto cause = a_event.cause.get();
				if (!target || !cause) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// An NPC in our world (or we, with friendly fire on) hit another player's stand-in:
				// that player takes the damage. Hits on mirrored NPCs are dropped in Frame(). NPCs
				// made at runtime (random encounters, spawned enemies) exist only in our game, so
				// their hits would come from nowhere in the victim's: not passed on.
				const bool byPlayer = cause->IsPlayerRef();
				const auto attacker = byPlayer ? 0 : cause->GetFormID();
				const auto victim = target->As<RE::Actor>();
				if (victim && Puppets::IsPuppet(victim) && (byPlayer || IsShareableRef(attacker))) {
					if ((!byPlayer || friendlyFire) && a_event.usesHitData) {
						const float damage = a_event.hitData.healthDamage > 0.0f ? a_event.hitData.healthDamage : a_event.hitData.totalDamage;
						if (damage > 0.0f) {
							std::scoped_lock l{ inboxLock };
							standInHits.push_back({ victim->GetFormID(), damage, byPlayer, attacker });
						}
					}
				}
				if (!byPlayer) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// Only damage we dealt to others (not ourselves); each player reports their own hits.
				const auto actor = target->As<RE::Actor>();
				if (actor && IsShareableRef(target->GetFormID()) && !Puppets::IsPuppet(actor)) {
					std::scoped_lock l{ inboxLock };
					hitInbox.push_back(target->GetFormID());
					const float taken = !a_event.usesHitData ? 0.0f : a_event.hitData.healthDamage > 0.0f ? a_event.hitData.healthDamage : a_event.hitData.totalDamage;
					blowInbox.push_back({ target->GetFormID(), taken });
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
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_holder);
				return ref && !ref->IsDeleted();
			}

			LootScope::HolderFacts Holder(std::uint32_t a_holder) override { return ReadHolderFacts(a_holder); }

			LootScope::ItemFacts Item(std::uint32_t a_item) override { return ReadItemFacts(RE::TESForm::GetFormByID(a_item)); }

			bool QuestAlias(std::uint32_t a_holder, std::uint32_t a_item) override
			{
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_holder);
				const auto item = RE::TESForm::GetFormByID(a_item);
				return ref && item && HoldsAsQuestItem(ref, item);
			}

			void Apply(const Protocol::ContainerChange& a_change) override
			{
				const auto command = a_change.count < 0 ?
					std::format("{:08X}.removeitem {:08X} {}", a_change.container, a_change.item, -static_cast<std::int64_t>(a_change.count)) :
					std::format("{:08X}.additem {:08X} {}", a_change.container, a_change.item, a_change.count);
				RE::Console::ExecuteCommand(command.c_str());
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
			for (auto& packet : loot.Report(a_moves, engine)) {
				Queue(std::move(packet));
			}
		}

		// Deaths in our world: reported, unless someone else reported them (we caused those because they did).
		void ReportDeaths(const std::vector<std::uint32_t>& a_deaths, const std::unordered_set<std::uint32_t>& a_killedByUs)
		{
			for (const auto id : a_deaths) {
				if (dead.Add(id, Clock::now())) {
					const auto corpse = RE::TESForm::GetFormByID<RE::Actor>(id);
					const auto at = corpse && corpse->Get3D() ? corpse->data.location : RE::NiPoint3{};  // a script can kill one with no 3D
					ReportDead({ .refId = id, .killed = a_killedByUs.contains(id), .x = at.x, .y = at.y, .z = at.z });
				}
				dead.SawDead(id);  // ours, or the kill we ran for another game's report: it fell here
				reports.Forget(id);
				pendingHealth.erase(id);
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

		// Current open/lock state of a loaded reference, or nullopt if it isn't loaded or has neither.
		std::optional<Protocol::RefState> ReadRefState(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || a_ref->IsDeleted() || !a_ref->Get3D() || a_ref->As<RE::Actor>()) {
				return std::nullopt;  // talking to someone is an activation too, but an actor has no door or lock state
			}
			using Open = RE::BGSOpenCloseForm::OPEN_STATE;
			Protocol::RefState state{ a_ref->GetFormID() };
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
			if (state.open == Protocol::RefState::kUnknown && state.locked == Protocol::RefState::kUnknown) {
				return std::nullopt;
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
			if (a_state.locked != Protocol::RefState::kUnknown && current->locked != Protocol::RefState::kUnknown && a_state.locked != current->locked) {
				ref->GetLock()->SetLocked(a_state.locked == Protocol::RefState::kYes);
				ref->AddLockChange();
				++refStatesApplied;
				REX::INFO("WorldSync: {:08X} {} (changed in another player's world)", a_state.refId, a_state.locked == Protocol::RefState::kYes ? "locked" : "unlocked");
			}
			if (a_state.open != Protocol::RefState::kUnknown && current->open != Protocol::RefState::kUnknown && a_state.open != current->open) {
				RE::BGSOpenCloseForm::SetOpenState(ref, a_state.open == Protocol::RefState::kYes, false);
				++refStatesApplied;
			}
			return true;
		}

		// The reference a linked-ref entry points at. The engine fills the entry with the target's form ID
		// and resolves it to a pointer later ("transient" refs); until then the pointer field holds the
		// ID, and reading it as a pointer crashed both games at the Red Rocket terminal (2026-10-10).
		RE::TESObjectREFR* LinkedTarget(const RE::ExtraLinkedRef& a_links, const RE::ExtraLinkedRef::LinkedRef& a_link)
		{
			const auto raw = reinterpret_cast<std::uintptr_t>(a_link.REFR);
			if (!raw) {
				return nullptr;
			}
			if (a_links.transientRefs || raw <= 0xFFFFFFFFull) {
				return RE::TESForm::GetFormByID<RE::TESObjectREFR>(static_cast<std::uint32_t>(raw));
			}
			return a_link.REFR;
		}

		// Starts watching references the player activated, and reports changes to watched ones.
		void WatchRefStates(Clock::time_point a_now, const std::vector<Activation>& a_activations)
		{
			for (const auto& activation : a_activations) {
				auto& entry = watched[activation.ref];
				entry.until = a_now + WATCH_TIME;
				// What the thing is linked to is watched as well: a terminal's door opens without anyone
				// activating the door, and the friend would otherwise stay locked out.
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(activation.ref);
				const auto links = ref && ref->extraList ? ref->extraList->GetByType<RE::ExtraLinkedRef>() : nullptr;
				if (links) {
					for (const auto& link : links->linkedRefs) {
						const auto target = LinkedTarget(*links, link);
						if (target && Protocol::IsShareableRef(target->GetFormID())) {
							watched[target->GetFormID()].until = a_now + WATCH_TIME;
							++linkedWatched;
						}
					}
				}
			}
			for (auto it = watched.begin(); it != watched.end();) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(it->first);
				const auto state = ReadRefState(ref);
				if (!state || a_now > it->second.until) {
					it = watched.erase(it);
					continue;
				}
				if (*state != it->second.lastSent) {
					it->second.lastSent = *state;
					Queue(Protocol::Encode(*state, Protocol::MessageType::kReportRefState));
				}
				++it;
			}
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
			auto facts = ReadItemFacts(ref->GetObjectReference());
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
			Hud::Notify(std::format("Multiplayer: the session's shared history is {}% full; loot and kill sharing stop at 100%. "
									"The host can start a fresh session by removing the session file (see the docs)",
				(std::min)(percent, std::size_t{ 100 })));
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
		const auto ui = RE::UI::GetSingleton();
		return !ui || !ui->GetMenuOpen("LoadingMenu"sv);
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
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&menuWatcher);
		}
		FindVendorChests();
	}

	void OnWelcome(bool a_friendlyFire)
	{
		friendlyFire = a_friendlyFire;
		dead.Expect(Clock::now());  // the session's world state follows the welcome
	}

	LootSync::Loot& Loot()
	{
		return loot;
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
					if (!senderAt || senderAt->GetDistance(ref->data.location) > ACTIVATION_REACH) {
						++refStatesRefused;
						REX::INFO("WorldSync: {:08X} state from player {} refused: they are {} away", a_state.refId, a_state.playerId, senderAt ? std::format("{:.0f}", senderAt->GetDistance(ref->data.location)) : "nowhere known");
						return;
					}
				}
			}
			// So our own watcher doesn't report this change back.
			if (const auto it = watched.find(a_state.refId); it != watched.end()) {
				it->second.lastSent = a_state;
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
		const auto lost = loot.CatchUp(a_state.containerFirstIndex, a_state.containerChanges, Clock::now(), engine);
		engine.inWorld.reset();
		if (lost) {
			REX::WARN("WorldSync: the server's log is shorter than our save (lost {} changes); counting again from #{}", lost, a_state.containerFirstIndex);
		}
		for (const auto id : a_state.pickedUp) {
			OfferPickup(id, inWorld);
		}
		for (const auto& state : a_state.refStates) {
			OfferRefState(state, inWorld);
		}
		for (const auto& stage : a_state.questStages) {
			QuestSync::Apply(stage, false);
		}
		REX::INFO("WorldSync: session state: {} dead, {} container changes from #{}, {} pickups, {} doors, {} quest stages",
			a_state.deadActors.size(), a_state.containerChanges.size(), a_state.containerFirstIndex, a_state.pickedUp.size(),
			a_state.refStates.size(), a_state.questStages.size());
		NoticeHistory();
	}

	void ApplyRemoteHealth(std::uint32_t a_refId, float a_health)
	{
		if (!IsShareableRef(a_refId) || dead.Contains(a_refId)) {
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

	bool MayClaim(std::uint32_t a_refId)
	{
		return dead.MayClaim(a_refId, Clock::now());
	}

	void OnActorOwners(const std::vector<Protocol::ActorOwner>& a_owners)
	{
		// Another player's game runs these, so they live there. A persistent NPC (or one of a cell we left
		// lately) is in our memory even where we aren't, and when we host, ours is checked (CheckFoundDead).
		for (const auto& owner : a_owners) {
			if (owner.playerId != 0 && !loot.Ours(owner.playerId) && IsShareableRef(owner.refId)) {
				toCheck.insert(owner.refId);
			}
		}
	}

	void ApplyContainerChange(const Protocol::IndexedContainerChange& a_change)
	{
		MainThread::Assert("WorldSync::ApplyContainerChange");
		loot.Live(a_change, Clock::now(), engine);
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
		std::vector<std::uint32_t> newDeaths;
		std::vector<std::pair<std::uint32_t, bool>> newLoads;
		std::vector<std::uint32_t>                  newRefLoads;
		std::unordered_set<std::uint32_t> killedByUs;
		std::vector<std::uint32_t> newHits;
		std::vector<std::pair<std::uint32_t, float>> newBlows;
		std::vector<std::uint32_t> newPickups;
		std::vector<StandInHit> newStandInHits;
		std::vector<Activation>    newActivations;
		std::vector<Move>          newMoves;
		{
			std::scoped_lock l{ inboxLock };
			newDeaths.swap(deathInbox);
			newLoads.swap(loadInbox);
			newRefLoads.swap(refLoadInbox);
			killedByUs.swap(killInbox);
			killInbox.clear();
			newHits.swap(hitInbox);
			newBlows.swap(blowInbox);
			newPickups.swap(pickupInbox);
			newStandInHits.swap(standInHits);
			newActivations.swap(activateInbox);
			newMoves.swap(lootInbox);
		}

		// A copy kept from a friend's explosion that our player hit is ours to hurt again, before its health goes out
		// next frame (measured without the health lent against the explosion).
		WeaponFire::OurBlows(newBlows);
		ReportDeaths(newDeaths, killedByUs);
		// After the deaths: a corpse looted the frame it died goes out dead first.
		ReportMoves(newMoves);

		TrackLoads(newLoads);
		CheckFoundDead(a_hosting);

		// Report the health of actors we hit last frame (damage has been applied by now).
		if (const auto info = HealthInfo(); info && !hitLastFrame.empty()) {
			for (const auto id : hitLastFrame) {
				const auto actor = LoadedActor(id);
				if (actor && !actor->IsDead(false) && !dead.Contains(id)) {
					const auto& values = static_cast<RE::ActorValueOwner&>(*actor);
					const float maximum = values.GetPermanentActorValue(*info);
					if (maximum > 0.0f) {
						const float fraction = values.GetActorValue(*info) / maximum;
						Queue(Protocol::Encode(Protocol::ActorHealth{ id, fraction }, Protocol::MessageType::kReportHealth));
					}
				}
			}
		}
		hitLastFrame.clear();
		hitLastFrame.insert(newHits.begin(), newHits.end());
		// Fighting or talking to an NPC another player's game runs brings it over to ours.
		for (const auto id : newHits) {
			NpcSync::OnLocalInteraction(id);
		}
		for (const auto& activation : newActivations) {
			NpcSync::OnLocalInteraction(activation.ref);
			NpcSync::OnActivated(activation.ref);
			// A button, switch or lever: the others press it too, so what it drives happens in their games.
			const auto base = RE::TESForm::GetFormByID(activation.base);
			if (base && base->Is(RE::ENUM_FORM_ID::kACTI)) {
				Queue(Protocol::Encode(Protocol::Activation{ 0, activation.ref }, Protocol::MessageType::kReportActivation));
				++activationsSent;
			}
		}
		for (const auto& hit : newStandInHits) {
			if (const auto playerId = RemotePlayers::PlayerIdFor(hit.actor)) {
				Queue(Protocol::Encode(Protocol::PlayerHit{ playerId, hit.damage, hit.byPlayer, hit.attacker }, Protocol::MessageType::kPlayerHit));
			}
		}

		const auto now = Clock::now();
		MatchPickups(now, newActivations, newPickups);
		WatchRefStates(now, newActivations);
		// Container changes that came before one we still lack, with nothing arriving for a while: the
		// server is asked again (the answer to the last request may have been refused, or never come).
		if (const auto request = loot.Due(now)) {
			Queue(Protocol::Encode(*request));
			REX::INFO("WorldSync: still missing container change #{} ({} waiting); asking again", request->containerFrom, loot.Numbers().Numbers().Waiting());
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
		const bool anyWaiting = !reports.Empty() || !pendingHealth.empty() || !loot.Waiting().empty() || !pendingPickups.empty() || !pendingRefStates.empty();
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
		std::vector<std::uint32_t>        deaths;
		std::unordered_set<std::uint32_t> killedByUs;
		std::vector<Move>                 moves;
		{
			std::scoped_lock l{ inboxLock };
			deaths.swap(deathInbox);
			killedByUs.swap(killInbox);
			moves.swap(lootInbox);
		}
		// The deaths first, as in a frame: a corpse looted in the last moments goes out dead, so the others
		// put its loot in (after a load Reset would forget the death, and they would hold the loot as a
		// living NPC's until they gave up on it).
		ReportDeaths(deaths, killedByUs);
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
		switchOnTried.clear();
		pendingHealth.clear();
		pendingPickups.clear();
		pickedUpKnown.clear();
		hitLastFrame.clear();
		recentActivations.clear();
		watched.clear();
		pendingRefStates.clear();
		unmatchedPickups.clear();
		// What is loaded is kept (a save's actors can load before the game says the save has loaded), and
		// all of it is checked again when we host: what we knew to be dead is forgotten.
		checkAllLoaded = true;
		std::scoped_lock l{ inboxLock };
		pickupInbox.clear();
		standInHits.clear();
		activateInbox.clear();
		deathInbox.clear();
		killInbox.clear();
		hitInbox.clear();
		blowInbox.clear();
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
		return std::format("dead={} pending={} reported={} applied={} containerNext={} gap={} gaps={} rerequests={} rewound={} dupChanges={} pendingLoot={} pendingDropped={} pendingFull={} lootSent={} weaponArmor={} vendor={} barter={} perPlayer={} notYetDead={} unsupported={} holderDropped={} pendingPickups={} watched={} linked={} acts={}/{}/{} states={}/{} corpses={} pendingDoors={} personal={} personalSent={} ownSkipped={} dupRisk={} offThread={} foundDead={} ownStoryDead={} loadedActors={}",
			dead.Size(), reports.Size(), reported, applied, numbers.Next(), numbers.Waiting(), counts.gaps, counts.asked, counts.rewinds, counts.duplicates, loot.Waiting().size(), stats.pendingDropped, stats.pendingFull, stats.sent, kept(Scope::kWeaponArmor), kept(Scope::kVendor), barterMoves.load(),
			kept(Scope::kPerPlayer), kept(Scope::kNotYetDead), kept(Scope::kUnsupported), holderDropped, pendingPickups.size(), watched.size(), linkedWatched, activationsSent, activationsApplied, activationsRefused, refStatesApplied, refStatesRefused, corpsesMoved, pendingRefStates.size(),
			personalSkipped + loot.KeptApplied(Scope::kPersonal), loot.KeptSent(Scope::kPersonal), receiver.OwnSkipped(), receiver.DupRisk(), MainThread::OffThread(), foundDead, ownStoryDead, loadedActors.size()) +
		       std::format(" storyKillKept={} switchedOnToKill={} loadApplied={} retryUs={}/{} revived={} settled={}", storyKillsKept, switchedOnToKill, loadApplied, retryUs, retryUsMax, dead.Revived(), dead.Settled(Clock::now()));
	}
}
