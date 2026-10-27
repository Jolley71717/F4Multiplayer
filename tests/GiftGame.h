#pragma once

// One player's game as far as gifts go (src/game/GiftBook.h), driven against a real Server (ServerFixture). The plugin's
// own code runs in it: GiftBook::Client as Gifts.cpp runs it, and DropBook::Record as Drops keeps it (the co-save record,
// saved and loaded through its bytes). What stands in for the engine is only what GiftBook::Engine reads and changes:
// what each inventory holds (our player's and the containers'), what is loaded, which forms this game has. The calls come
// in the plugin's order (Session's packet loop, Gifts::Frame, Gifts::BeforeSaveOrLoad before a save or a load, the
// co-save record, Session::OnGameLoaded and Drops::SendRecord after a load, Reset as the session ends); the test plays
// the player, and the server's packets go through Protocol as in a game.
//
// Holds counts each item: a world's items of a class are what its player carries and its containers hold. With every
// menu closed (no copy shown) and every packet read, the items of a class across all worlds and lying in the session
// are as many as there ever were: a gift moved never makes a second, and never none.

#include "ServerFixture.h"

#include "game/DropBook.h"
#include "game/GiftBook.h"

#include <array>
#include <deque>
#include <map>
#include <set>
#include <thread>

namespace GiftTest
{
	using namespace TestServer;
	using Clock = GiftBook::Clock;
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using GiftBook::PLAYER;

	constexpr std::uint32_t CHEST = 0x0001D9F6;    // a dresser: a gift's place
	constexpr std::uint32_t BENCH = 0x0002A1B2;    // a settlement's workbench: not one
	constexpr std::uint32_t VENDOR = 0x0002A1B3;   // a merchant's chest: not one
	constexpr std::uint32_t GUN = 0x0004F46A;      // 10mm pistol
	constexpr std::uint32_t ARMOR = 0x00140D0C;    // a power armor piece
	constexpr std::uint32_t RECEIVER = 0x0004F46B;
	constexpr std::uint32_t LEGENDARY = 0x001C3D37;
	constexpr std::uint32_t DLC_MOD = 0x01012345;  // a mod of a plugin not every game has

	inline bool IsType(const std::vector<std::uint8_t>& a_packet, MT a_type)
	{
		return !a_packet.empty() && a_packet[0] == Type(a_type);
	}

	// A gun as our players carry it: modded, renamed.
	inline GiftBook::Stack Gun(std::string a_name = "Old Reliable", std::vector<Protocol::ItemMod> a_mods = { { RECEIVER, 0, 0 }, { LEGENDARY, 1, 0 } })
	{
		return { .base = GUN, .kind = LootScope::Item::kWeapon, .count = 1, .templateIndex = 3, .mods = std::move(a_mods), .name = std::move(a_name) };
	}

	inline GiftBook::Stack Armor(float a_health)
	{
		return { .base = ARMOR, .kind = LootScope::Item::kArmor, .count = 1, .health = a_health };
	}

	// The class of what a gift copies (its stack as a copy of it holds it).
	inline GiftBook::Stack StackOf(const Protocol::DroppedItem& a_item)
	{
		return { .base = a_item.base, .kind = a_item.base == ARMOR ? LootScope::Item::kArmor : LootScope::Item::kWeapon, .count = 1, .templateIndex = a_item.templateIndex,
			.mods = a_item.mods, .health = a_item.health, .name = a_item.name };
	}

	struct Game final :
		GiftBook::Engine
	{
		using Inventories = std::map<std::uint32_t, std::vector<GiftBook::Stack>>;  // PLAYER and each container

		PeerId        peer = 0;
		std::uint64_t identity = 0;
		std::uint32_t localId = 0;
		std::uint64_t sessionId = 0;
		bool          connected = false;
		bool          rule = false;  // the welcome's

		Inventories                                               inventories;
		std::map<std::uint32_t, LootScope::HolderFacts>           holderFacts;  // unknown: a plain container
		std::set<std::uint32_t>                                   away;         // containers not loaded here
		std::set<std::uint32_t>                                   lacking;      // forms this game lacks (a plugin it hasn't)
		DropBook::Record                                          record;       // Drops' co-save record
		Packets                                                   outbox;       // sent with the frame (Session::SendOut)
		Clock::time_point                                         now{};
		std::uint64_t                                             nonces = 0;
		std::array<std::uint32_t, static_cast<std::size_t>(GiftBook::Notice::kCount)> notices{};
		std::optional<std::uint32_t>                              menu;
		std::uint32_t                                             addsToPlayer = 0;  // items the session put in our player's inventory
		GiftBook::Client                                          gifts{ *this };

		struct Save
		{
			Inventories               inventories;
			std::vector<std::uint8_t> record;  // the co-save's bytes ('DROP')
		};

		Game(PeerId a_peer, std::uint64_t a_identity) :
			peer(a_peer), identity(a_identity)
		{}

		// Connects and is welcomed, as Session does: Drops binds the record to the session and sends it, Gifts hears the rule.
		std::uint32_t Join(Fixture& a_f, const std::string& a_name)
		{
			const auto welcome = a_f.Join(peer, Fixture::MakeHello(a_name, identity));
			REQUIRE(welcome);
			sessionId = welcome->sessionId;
			localId = welcome->playerId;
			rule = welcome->rules.containerGifts;
			connected = true;
			record.Bind(sessionId);
			gifts.Welcome(localId, rule);
			SendRecord(a_f);
			return localId;
		}

		// The connection goes (Session::Reset: Drops' and Gifts'). What the server sent and we hadn't read is lost with it,
		// as is what we hadn't sent.
		void Disconnect(Fixture& a_f)
		{
			LinkLost();
			a_f.fake->Drop(peer);
			REQUIRE(a_f.fake->Settle());  // the server saw it go: nothing more is sent this connection
			a_f.fake->Take(peer);
		}

		// Our side of it alone (the server stopped: the host quit).
		void LinkLost()
		{
			gifts.Reset();
			record.Bind(0);
			outbox.clear();
			connected = false;
			menu.reset();
		}

		// ---- the player ----------------------------------------------------------------------------------------

		void Give(std::uint32_t a_ref, const GiftBook::Stack& a_stack) { AddStack(a_ref, a_stack); }

		void Open(Fixture& a_f, std::uint32_t a_holder)
		{
			menu = a_holder;
			gifts.Open(a_holder);
			SendOut(a_f);
		}

		void Close(Fixture& a_f)
		{
			menu.reset();
			gifts.Close();
			SendOut(a_f);
		}

		// The player moves one item of a_class from their inventory into a_holder (the menu's transfer; its event follows).
		bool Put(Fixture& a_f, std::uint32_t a_holder, const GiftBook::Class& a_class, bool a_frame = true)
		{
			return Move(a_f, PLAYER, a_holder, a_class, a_frame);
		}

		bool Take(Fixture& a_f, std::uint32_t a_holder, const GiftBook::Class& a_class, bool a_frame = true)
		{
			return Move(a_f, a_holder, PLAYER, a_class, a_frame);
		}

		// Gifts::Frame, and what it queued sent.
		void Frame(Fixture& a_f)
		{
			if (connected) {
				gifts.Frame();
			}
			SendOut(a_f);
		}

		// Time passes, a frame every 250 ms.
		void Wait(Fixture& a_f, Clock::duration a_time)
		{
			for (Clock::duration passed{}; passed < a_time; passed += std::chrono::milliseconds{ 250 }) {
				now += std::chrono::milliseconds{ 250 };
				Frame(a_f);
			}
		}

		// What the server sent us up to now. The server answers at most 10 heartbeats a second from a player (Sync is one): a
		// game that would ask more waits.
		Packets Fetch(Fixture& a_f)
		{
			Pace();
			Sync(a_f, peer);
			return a_f.fake->Take(peer);
		}

		// Session's packet loop: a gift's message to Gifts (Drops' to Drops, which these worlds have none of), then the frame.
		// a_send false: the link goes before the frame sends what the packets made us queue (Disconnect clears it).
		void Handle(Fixture& a_f, const Packets& a_packets, bool a_send = true)
		{
			for (const auto& packet : a_packets) {
				if (IsType(packet, MT::kDropPlaced)) {
					if (const auto msg = Protocol::DecodeDroppedItem(packet)) {
						gifts.Placed(*msg);
					}
				} else if (IsType(packet, MT::kDropGone)) {
					if (const auto msg = Protocol::DecodeDropGone(packet)) {
						gifts.Gone(*msg);
					}
				} else if (IsType(packet, MT::kDropList)) {
					if (const auto msg = Protocol::DecodeDropList(packet)) {
						gifts.List(*msg);
					}
				}
			}
			if (a_send) {
				Frame(a_f);
			}
		}

		void Receive(Fixture& a_f) { Handle(a_f, Fetch(a_f)); }

		// A save: Session::OnBeforeSaveOrLoad (no copy is saved), then the co-save record is written.
		Save SaveGame(Fixture& a_f)
		{
			menu.reset();
			gifts.BeforeSaveOrLoad();
			SendOut(a_f);
			return { inventories, record.Save() };
		}

		// A load while connected: Session::OnBeforeSaveOrLoad, the co-save record (F4SE reverts it first), then
		// Session::OnGameLoaded (Gifts' and Drops') and Drops::SendRecord.
		void LoadGame(Fixture& a_f, const Save& a_save)
		{
			menu.reset();
			gifts.BeforeSaveOrLoad();
			SendOut(a_f);
			Restore(a_save);
			gifts.GameLoaded();
			SendRecord(a_f);
		}

		// The game started again and loaded a_save before connecting (Join sends its record).
		void Restart(const Save& a_save)
		{
			outbox.clear();
			connected = false;
			menu.reset();
			Restore(a_save);
			gifts.GameLoaded();
		}

		// ---- counting --------------------------------------------------------------------------------------------

		// The items of a_class this world holds (our player's and every container's).
		std::int64_t Holds(const GiftBook::Class& a_class) const
		{
			std::int64_t n = 0;
			for (const auto& [ref, stacks] : inventories) {
				n += GiftBook::CountOf(GiftBook::Count(stacks), a_class);
			}
			return n;
		}

		std::int64_t In(std::uint32_t a_ref, const GiftBook::Class& a_class) const
		{
			const auto it = inventories.find(a_ref);
			return it != inventories.end() ? GiftBook::CountOf(GiftBook::Count(it->second), a_class) : 0;
		}

		std::int64_t Carried(const GiftBook::Class& a_class) const { return In(PLAYER, a_class); }

		// The gifts of a_class lying in the session, as this game heard.
		std::int64_t Lying(const GiftBook::Class& a_class) const
		{
			return static_cast<std::int64_t>(std::ranges::count_if(gifts.Lying(), [&](const auto& a_entry) { return GiftBook::ClassOf(StackOf(a_entry.second)) == a_class; }));
		}

		std::uint32_t Noticed(GiftBook::Notice a_notice) const { return notices[static_cast<std::size_t>(a_notice)]; }

		// ---- GiftBook::Engine ------------------------------------------------------------------------------------

		std::optional<std::vector<GiftBook::Stack>> Stacks(std::uint32_t a_ref, std::uint32_t a_base) override
		{
			if (away.contains(a_ref)) {
				return std::nullopt;
			}
			std::vector<GiftBook::Stack> out;
			if (const auto it = inventories.find(a_ref); it != inventories.end()) {
				for (const auto& stack : it->second) {
					if (a_base == 0 || stack.base == a_base) {
						out.push_back(stack);
					}
				}
			}
			return out;
		}

		bool Remove(std::uint32_t a_ref, std::uint32_t a_base, std::uint32_t a_stack) override
		{
			if (away.contains(a_ref)) {
				return false;
			}
			auto&         stacks = inventories[a_ref];
			std::uint32_t at = 0;
			for (auto it = stacks.begin(); it != stacks.end(); ++it) {
				if (it->base != a_base) {
					continue;
				}
				if (at++ != a_stack) {
					continue;
				}
				if (it->count <= 0) {
					return false;
				}
				if (--it->count == 0) {
					stacks.erase(it);
				}
				return true;
			}
			return false;
		}

		bool Add(std::uint32_t a_ref, const Protocol::DroppedItem& a_item) override
		{
			if (away.contains(a_ref) || !Placeable(a_item)) {
				return false;
			}
			if (a_ref == PLAYER) {
				++addsToPlayer;
			}
			AddStack(a_ref, StackOf(a_item));
			return true;
		}

		LootScope::HolderFacts Holder(std::uint32_t a_ref) override
		{
			const auto it = holderFacts.find(a_ref);
			return it != holderFacts.end() ? it->second : LootScope::HolderFacts{ .kind = LootScope::Holder::kContainer };
		}

		std::optional<GiftBook::Place> PlaceOf(std::uint32_t a_ref) override
		{
			if (away.contains(a_ref)) {
				return std::nullopt;
			}
			return GiftBook::Place{ .worldspace = 0x0000003C, .x = -79000.0f, .y = 89000.0f, .z = 7800.0f };  // Sanctuary
		}

		bool Placeable(const Protocol::DroppedItem& a_item) override
		{
			return !lacking.contains(a_item.base) && std::ranges::none_of(a_item.mods, [&](const Protocol::ItemMod& a_mod) { return lacking.contains(a_mod.mod); });
		}

		void Send(std::vector<std::uint8_t> a_packet) override { outbox.push_back(std::move(a_packet)); }

		void Notify(GiftBook::Notice a_notice) override { ++notices[static_cast<std::size_t>(a_notice)]; }

		std::uint64_t NewNonce() override { return (identity << 32) | ++nonces; }

		Clock::time_point Now() override { return now; }

		void Made(std::uint64_t a_nonce) override { record.Made(a_nonce); }
		bool HasMade(std::uint64_t a_nonce) override { return record.HasMade(a_nonce); }
		void Took(std::uint32_t a_dropId) override { record.Took(a_dropId); }
		bool HasTaken(std::uint32_t a_dropId) override { return record.HasTaken(a_dropId); }

		// Drops::OnWelcome and Drops::SendRecord: what our world holds of the transfers. The server answers a player's record no
		// sooner than DROP_RECORD_INTERVAL after their last (it holds a sooner one until then): a test waits that out here, so
		// the answer is there to read.
		void SendRecord(Fixture& a_f)
		{
			const auto since = std::chrono::steady_clock::now() - recordSentAt;
			if (since < std::chrono::milliseconds{ 300 }) {
				std::this_thread::sleep_for(std::chrono::milliseconds{ 300 } - since);
			}
			recordSentAt = std::chrono::steady_clock::now();
			outbox.push_back(Protocol::Encode(record.ForServer()));
			SendOut(a_f);
		}

		// What the session hasn't heard yet goes (Session::SendOut; nothing while unconnected).
		void SendOut(Fixture& a_f)
		{
			for (auto& packet : std::exchange(outbox, {})) {
				if (connected) {
					a_f.fake->Deliver(peer, std::move(packet));
				}
			}
		}

	private:
		std::chrono::steady_clock::time_point recordSentAt{};
		std::deque<std::chrono::steady_clock::time_point> syncs;

		void Pace()
		{
			using Steady = std::chrono::steady_clock;
			constexpr auto WINDOW = std::chrono::milliseconds{ 1100 };
			while (!syncs.empty() && Steady::now() - syncs.front() >= WINDOW) {
				syncs.pop_front();
			}
			if (syncs.size() >= 8) {
				std::this_thread::sleep_until(syncs.front() + WINDOW);
				syncs.pop_front();
			}
			syncs.push_back(Steady::now());
		}

		// One item more of a_stack's class in a_ref: on a stack of its class, as the engine merges like items.
		void AddStack(std::uint32_t a_ref, GiftBook::Stack a_stack)
		{
			auto&      stacks = inventories[a_ref];
			const auto cls = GiftBook::ClassOf(a_stack);
			for (auto& stack : stacks) {
				if (GiftBook::ClassOf(stack) == cls) {
					stack.count += a_stack.count;
					return;
				}
			}
			stacks.push_back(std::move(a_stack));
		}

		bool Move(Fixture& a_f, std::uint32_t a_from, std::uint32_t a_to, const GiftBook::Class& a_class, bool a_frame)
		{
			auto&      from = inventories[a_from];
			const auto it = std::ranges::find_if(from, [&](const GiftBook::Stack& a_stack) { return a_stack.count > 0 && GiftBook::ClassOf(a_stack) == a_class; });
			if (it == from.end()) {
				return false;
			}
			auto one = *it;
			one.count = 1;
			one.equipped = false;
			if (--it->count == 0) {
				from.erase(it);
			}
			AddStack(a_to, one);
			// WorldSync's ContainerWatcher: a move between our player and a container, posted to Gifts.
			const auto holder = a_from == PLAYER ? a_to : a_from;
			gifts.Moved(holder, a_class.base, a_from == PLAYER ? 1 : -1);
			if (a_frame) {
				Frame(a_f);
			}
			return true;
		}

		void Restore(const Save& a_save)
		{
			inventories = a_save.inventories;
			auto loaded = DropBook::Record::Load(a_save.record);
			record.Replace(loaded ? std::move(*loaded) : DropBook::Record{});
		}
	};
}
