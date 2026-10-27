#include "game/Gifts.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Drops.h"
#include "game/Equipment.h"
#include "game/FrameLoans.h"
#include "game/GiftBook.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/StandInGear.h"
#include "game/WorldSync.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <random>
#include <vector>

namespace Gifts
{
	namespace
	{
		using Clock = GiftBook::Clock;
		using GiftBook::PLAYER;

		// The container menu is gone this long (its close event missed, or another menu took over) and our view closes.
		constexpr auto MENU_GONE = std::chrono::seconds{ 1 };

		thread_local bool applying = false;

		struct Guard
		{
			Guard() { applying = true; }
			~Guard() { applying = false; }
			Guard(const Guard&) = delete;
			Guard& operator=(const Guard&) = delete;
		};

		std::mutex                 inboxLock;
		GiftBook::Inbox            inbox;                  // under inboxLock
		std::atomic<bool>          listening{ false };     // welcomed: the menu's entry points post
		std::atomic<std::uint32_t> viewed{ 0 };            // the container our view watches (0: none): only its moves are kept

		struct Counts
		{
			std::uint32_t refusedOwn = 0;  // inventory changes OwnGear refused (must stay 0)
			std::uint32_t inboxDropped = 0;
		} counts;

		RE::TESObjectREFR* Ref(std::uint32_t a_id)
		{
			if (a_id == PLAYER) {
				return RE::PlayerCharacter::GetSingleton();
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			return ref && !ref->IsDeleted() ? ref : nullptr;
		}

		RE::TESBoundObject* Object(std::uint32_t a_item)
		{
			const auto form = RE::TESForm::GetFormByID(a_item);
			return form ? form->As<RE::TESBoundObject>() : nullptr;
		}

		std::int64_t CountIn(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_object)
		{
			return a_ref && a_object ? static_cast<std::int64_t>(a_ref->GetInventoryObjectCount(a_object)) : 0;
		}

		// The change OwnGear judges: our player's take of a gift, or a gift's copy in a container.
		bool MayChange(RE::TESObjectREFR* a_ref, std::uint32_t a_id)
		{
			if (Equipment::MayChange(a_ref, a_id == PLAYER ? OwnGear::Change::kGiftTake : OwnGear::Change::kGiftCopy)) {
				return true;
			}
			++counts.refusedOwn;
			return false;
		}

		std::string_view TextKey(GiftBook::Notice a_notice)
		{
			switch (a_notice) {
			case GiftBook::Notice::kGiven:
				return "gifts.given";
			case GiftBook::Notice::kTakenFirst:
				return "gifts.taken_first";
			default:
				return "gifts.not_given";
			}
		}

		std::vector<std::vector<std::uint8_t>> outgoing;

		class GameEngine final :
			public GiftBook::Engine
		{
		public:
			std::optional<std::vector<GiftBook::Stack>> Stacks(std::uint32_t a_ref, std::uint32_t a_base) override
			{
				const auto ref = Ref(a_ref);
				const auto list = ref ? ref->inventoryList : nullptr;
				if (!list) {
					return std::nullopt;
				}
				std::vector<GiftBook::Stack> out;
				std::vector<std::uint32_t>   seen;  // one entry per base, as the engine keeps them and Remove counts stacks (Drops::StacksOf)
				RE::BSAutoReadLock           l{ list->rwLock };
				for (auto& item : list->data) {
					const auto object = item.object;
					if (!object || (a_base != 0 && object->GetFormID() != a_base)) {
						continue;
					}
					const auto type = object->GetFormType();
					if (type != RE::ENUM_FORM_ID::kWEAP && type != RE::ENUM_FORM_ID::kARMO) {
						continue;
					}
					if (std::ranges::find(seen, object->GetFormID()) != seen.end()) {
						continue;
					}
					seen.push_back(object->GetFormID());
					for (auto stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
						GiftBook::Stack entry{ .base = object->GetFormID(),
							.kind = type == RE::ENUM_FORM_ID::kWEAP ? LootScope::Item::kWeapon : LootScope::Item::kArmor,
							.count = static_cast<std::int32_t>(stack->count),
							.templateIndex = StandInGear::NO_TEMPLATE };
						const auto extra = stack->extra.get();
						const auto instance = extra ? extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
						if (instance && instance->values) {
							entry.templateIndex = instance->itemIndex;
							for (const auto& data : instance->GetIndexData()) {
								if (!data.disabled && data.objectID != 0) {
									entry.mods.push_back({ data.objectID, data.index, data.rank });
								}
							}
						}
						if (extra) {
							if (extra->HasType(RE::EXTRA_DATA_TYPE::kHealth)) {
								if (const auto health = extra->GetHealthPerc(); std::isfinite(health)) {
									entry.health = std::clamp(health, 0.0f, 1.0f);
								}
							}
							// The player's own name only (a modded weapon's made-up name isn't one: it changes with how it is shown).
							if (const auto text = extra->GetByType<RE::ExtraTextDisplayData>();
								text && text->ownerInstance.get() == RE::ExtraTextDisplayData::DisplayDataType::kCustomName && !text->displayName.empty()) {
								entry.name = text->displayName.c_str();
							}
							entry.quest = extra->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
						}
						entry.lent = FrameLoans::IsLentItem(entry.base, entry.name);
						entry.equipped = stack->IsEquipped();
						out.push_back(std::move(entry));
					}
				}
				return out;
			}

			bool Remove(std::uint32_t a_ref, std::uint32_t a_base, std::uint32_t a_stack) override
			{
				const auto ref = Ref(a_ref);
				const auto object = Object(a_base);
				if (!ref || !object || !MayChange(ref, a_ref)) {
					return false;
				}
				const auto before = CountIn(ref, object);
				if (before <= 0) {
					return false;
				}
				Guard                              guard;
				RE::TESObjectREFR::RemoveItemData data{ object, 1 };
				data.stackData.push_back(a_stack);
				ref->RemoveItem(data);
				Equipment::RedrawContainerMenu();  // the open menu shows it gone now, not at the player's next move
				return CountIn(ref, object) == before - 1;
			}

			bool Add(std::uint32_t a_ref, const Protocol::DroppedItem& a_item) override
			{
				const auto ref = Ref(a_ref);
				const auto object = Object(a_item.base);
				if (!ref || !object || !MayChange(ref, a_ref)) {
					return false;
				}
				const auto before = CountIn(ref, object);
				Guard      guard;
				const bool added = Drops::AddOneTo(ref, a_item) && CountIn(ref, object) == before + 1;
				Equipment::RedrawContainerMenu();  // a copy put in, or a take granted, shows in the open menu now
				return added;
			}

			LootScope::HolderFacts Holder(std::uint32_t a_ref) override { return WorldSync::HolderFactsOf(a_ref); }

			std::optional<GiftBook::Place> PlaceOf(std::uint32_t a_ref) override
			{
				const auto ref = Ref(a_ref);
				const auto cell = ref ? ref->GetParentCell() : nullptr;
				if (!cell) {
					return std::nullopt;
				}
				GiftBook::Place place;
				if (cell->IsInterior()) {
					place.cell = cell->GetFormID();
				} else if (cell->worldSpace) {
					place.worldspace = cell->worldSpace->GetFormID();
				} else {
					return std::nullopt;
				}
				const auto& at = ref->data.location;
				if (!Protocol::InWorld(at.x) || !Protocol::InWorld(at.y) || !Protocol::InWorld(at.z)) {
					return std::nullopt;
				}
				place.x = at.x;
				place.y = at.y;
				place.z = at.z;
				return place;
			}

			bool Placeable(const Protocol::DroppedItem& a_item) override { return Drops::CanPlace(a_item); }

			std::optional<LootScope::Item> KindOf(std::uint32_t a_base) override
			{
				const auto object = Object(a_base);
				if (!object) {
					return std::nullopt;
				}
				switch (object->GetFormType()) {
				case RE::ENUM_FORM_ID::kWEAP:
					return LootScope::Item::kWeapon;
				case RE::ENUM_FORM_ID::kARMO:
					return LootScope::Item::kArmor;
				default:
					return std::nullopt;
				}
			}

			void Send(std::vector<std::uint8_t> a_packet) override { outgoing.push_back(std::move(a_packet)); }

			void Notify(GiftBook::Notice a_notice) override { Hud::Notify(Text::Get(TextKey(a_notice))); }

			std::uint64_t NewNonce() override
			{
				static std::mt19937_64 random{ (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}() };
				std::uint64_t          nonce = 0;
				while (nonce == 0) {
					nonce = random();
				}
				return nonce;
			}

			Clock::time_point Now() override { return Clock::now(); }

			void Made(std::uint64_t a_nonce) override { Drops::RecordMade(a_nonce); }
			bool HasMade(std::uint64_t a_nonce) override { return Drops::HasMade(a_nonce); }
			void Took(std::uint32_t a_dropId) override { Drops::RecordTook(a_dropId); }
			bool HasTaken(std::uint32_t a_dropId) override { return Drops::HasTaken(a_dropId); }
		};

		GameEngine        engine;
		GiftBook::Client  client{ engine };  // main thread
		bool              devView = false;   // `gifts view`: a view with no menu, until `gifts close`
		std::optional<Clock::time_point> menuGoneSince;

		// The container our view watches, for the event watch's thread.
		void Publish()
		{
			viewed = client.Viewing().value_or(0);
		}

		// Our player's moves the event watch saw, before anything here looks at the container.
		void Drain()
		{
			GiftBook::Inbox moves;
			{
				std::scoped_lock l{ inboxLock };
				std::swap(moves, inbox);
				counts.inboxDropped += moves.TakeDropped();
			}
			moves.Drain([](std::uint32_t a_holder, std::uint32_t a_base, std::int32_t a_count) { client.Moved(a_holder, a_base, a_count); });
		}

		void Close()
		{
			Drain();
			client.Close();
			devView = false;
			menuGoneSince.reset();
			Publish();
		}

		std::uint32_t ParseHex(std::string_view a_text)
		{
			std::uint32_t value = 0;
			std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			return value;
		}
	}

	void OnWelcome(std::uint32_t a_playerId, bool a_rule)
	{
		client.Welcome(a_playerId, a_rule);
		listening = true;
		REX::INFO("Gifts: weapons and armor put in a container are {}", a_rule ? "given (the host's bContainerGifts)" : "kept in our game (the host's bContainerGifts off)");
	}

	void OnPlaced(const Protocol::DroppedItem& a_drop)
	{
		Drain();
		client.Placed(a_drop);
		Publish();
	}

	bool OnGone(const Protocol::DropGone& a_gone)
	{
		Drain();
		const bool ours = client.Gone(a_gone);
		Publish();
		return ours;
	}

	void OnList(const Protocol::DropList& a_list)
	{
		Drain();
		client.List(a_list);
		Publish();
	}

	void Frame()
	{
		Drain();
		if (client.Viewing() && !devView) {
			if (OpenMenus::Open(OpenMenus::Menu::kContainer)) {
				menuGoneSince.reset();
			} else if (!menuGoneSince) {
				menuGoneSince = Clock::now();
			} else if (Clock::now() - *menuGoneSince > MENU_GONE) {
				Close();
			}
		}
		client.Frame();
		Publish();
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void BeforeSaveOrLoad()
	{
		Close();
		client.BeforeSaveOrLoad();
	}

	void OnGameLoaded()
	{
		{
			std::scoped_lock l{ inboxLock };
			inbox.Clear();
		}
		client.GameLoaded();
		devView = false;
		menuGoneSince.reset();
		Publish();
	}

	void Reset()
	{
		listening = false;
		Drain();
		client.Reset();
		outgoing.clear();
		devView = false;
		menuGoneSince.reset();
		Publish();
	}

	void OnMenuShown(RE::ObjectRefHandle a_container)
	{
		if (!listening) {
			return;
		}
		MainThread::Post([a_container] {
			if (const auto ref = a_container.get()) {
				Drain();
				devView = false;
				menuGoneSince.reset();
				client.Open(ref->GetFormID());
				Publish();
			}
		});
	}

	void OnMenuClosed()
	{
		if (!listening) {
			return;
		}
		MainThread::Post([] {
			if (!devView) {
				Close();
			}
		});
	}

	void OnMoved(std::uint32_t a_holder, std::uint32_t a_base, std::int32_t a_count)
	{
		// Only the container our view watches: a move anywhere else changes nothing here (and the host's rule off, with no
		// gift in it, no container is watched).
		if (!listening || a_holder == 0 || a_holder != viewed.load()) {
			return;
		}
		std::scoped_lock l{ inboxLock };
		inbox.Add(a_holder, a_base, a_count);
	}

	bool Applying()
	{
		return applying;
	}

	bool GivesIn(std::uint32_t a_holder)
	{
		return client.Rule() && Protocol::IsShareableRef(a_holder) && GiftBook::GiftPlace(WorldSync::HolderFactsOf(a_holder));
	}

	std::string Describe()
	{
		const auto& s = client.Counters();
		const auto  viewing = client.Viewing();
		return std::format("gifts={} viewing={} copies={} claims={} puts={} lying={} views={} put={} notGiven={} overflow={} reported={} refused={} given={} kept={} "
						   "lateOffers={} shown={} unplaceable={} notGear={} addUndone={} copyMissing={} taken={} unsignaled={} unheld={} received={} giveFailed={} takenFirst={} givenBack={} takenBack={} "
						   "dupRisk={} inboxDropped={} ownRefused={} {}",
			client.Rule() ? "on" : "off", viewing ? std::format("{:08X}", *viewing) : "-", client.Copies(), client.Claims(), client.Puts(), client.Lying().size(), s.views,
			s.puts, s.notGiven, s.overflow, s.reported, s.refused, s.given, s.kept, s.lateOffers, s.shown, s.unplaceable, s.notGear, s.addUndone, s.copyMissing, s.taken,
			s.unsignaled, s.unheld, s.received,
			s.giveFailed, s.takenFirst, s.givenBack, s.takenBack, s.dupRisk, counts.inboxDropped, counts.refusedOwn, Equipment::DescribeContainerRedraw());
	}

	std::string Command(std::string_view a_args)
	{
		std::vector<std::string_view> args;
		for (std::size_t at = 0; at < a_args.size();) {
			const auto end = a_args.find(' ', at);
			const auto word = a_args.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
			if (!word.empty()) {
				args.push_back(word);
			}
			if (end == std::string_view::npos) {
				break;
			}
			at = end + 1;
		}
		if (args.empty() || args[0] == "status") {
			return Describe();
		}
		if (args[0] == "list") {
			std::string out = std::format("lying={}", client.Lying().size());
			for (const auto& [id, drop] : client.Lying()) {
				out += std::format("; gift {} base={:08X} mods={} health={:.2f} name='{}' in {:08X} owner={} shown={} claiming={}", id, drop.base, drop.mods.size(), drop.health,
					drop.name, drop.holder, drop.ownerId, client.Shows(id) ? "yes" : "no", client.Claiming(id) ? "yes" : "no");
			}
			return out;
		}
		if (args[0] == "close") {
			Close();
			return "closed";
		}
		if (args.size() < 2) {
			return "error: gifts [status | list | view <ref> | close | count <ref> <item> | take <ref> <item> [count] | put <ref> <item> [count]]";
		}
		const auto id = ParseHex(args[1]);
		const auto ref = Ref(id);
		if (!ref) {
			return std::format("error: no reference {:08X}", id);
		}
		if (args[0] == "view") {
			Drain();
			client.Open(id);
			Publish();
			devView = client.Viewing() == id;
			return devView ? std::format("viewing {:08X}: {} copies", id, client.Copies()) : std::format("not viewed: {:08X} is no gift's place (or the session isn't on)", id);
		}
		if (args.size() < 3) {
			return "error: missing item";
		}
		const auto item = ParseHex(args[2]);
		const auto object = Object(item);
		if (!object) {
			return std::format("error: no item {:08X}", item);
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (args[0] == "count") {
			return std::format("{:08X} holds {} of {:08X}; our player {}", id, CountIn(ref, object), item, CountIn(player, object));
		}
		std::int32_t count = 1;
		if (args.size() > 3) {
			std::from_chars(args[3].data(), args[3].data() + args[3].size(), count);
		}
		if (count <= 0 || !player) {
			return "error: count";
		}
		// A transfer between our player and the container, as the menu makes one (its event comes to OnMoved).
		if (args[0] == "take") {
			RE::TESObjectREFR::RemoveItemData data{ object, (std::min)(count, static_cast<std::int32_t>(CountIn(ref, object))) };
			data.otherContainer = player;
			if (data.count > 0) {
				ref->RemoveItem(data);
			}
			return std::format("took {}", data.count);
		}
		if (args[0] == "put") {
			RE::TESObjectREFR::RemoveItemData data{ object, (std::min)(count, static_cast<std::int32_t>(CountIn(player, object))) };
			data.otherContainer = ref;
			if (data.count > 0) {
				player->RemoveItem(data);
			}
			return std::format("put {}", data.count);
		}
		return "error: unknown gifts command";
	}
}
