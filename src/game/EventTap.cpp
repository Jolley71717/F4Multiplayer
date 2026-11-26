#include "game/EventTap.h"

#include "game/EventTapRules.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace EventTap
{
	namespace
	{
		// 1.11.240, by Address Library ID (EventTapRules.h: how each was read).
		constexpr REL::ID     NOTIFY{ 2202308 };        // BSTEventSource<TESHitEvent>::Notify (+546930): (source, const event*)
		constexpr REL::ID     SEND_PLAIN{ 2201637 };    // the sender with no hit data (+52CF80)
		constexpr REL::ID     SEND_HITDATA{ 2201638 };  // the sender with a HitData (+52D0B0)
		constexpr std::size_t CALL_STUB = 14;           // what a write_call<5> takes of the trampoline (the two calls share it)

		using NotifyFn = void (*)(RE::BSTEventSource<RE::TESHitEvent>*, const RE::TESHitEvent*);
		NotifyFn originalNotify = nullptr;

		std::atomic<bool>                  installed{ false };
		std::atomic<bool>                  handling{ true };
		std::atomic<bool>                  front{ true };
		HitHandler                         handler = nullptr;
		EventTapRules::Tally               tally;
		std::atomic<std::uint32_t>         loggedHandled{ 0 };
		RE::BSTEventSink<RE::TESHitEvent>* ourSink = nullptr;
		bool                               sinksListed = false;  // this session's list is in the log
		std::mutex                         listLock;

		thread_local EventTapRules::Track track;

		// BSTEventSource<Event>'s layout (CommonLibF4's BSTEvent.h, 0x58 bytes): the lock, then the sink list.
		constexpr std::size_t   SINKS_AT = 0x08;      // BSTArray: data, capacity, size
		constexpr std::size_t   SINKS_SIZE = 0x10;    // ... the count, at +0x10 of the array
		constexpr std::size_t   NOTIFYING_AT = 0x50;  // how many notifies are running
		constexpr std::uint32_t MAX_SINKS = 512;      // a list longer than this is not read as one

		struct View
		{
			RE::BSSpinLock* lock;
			void**          data;
			std::uint32_t*  size;
			std::int8_t*    notifying;
		};

		View ViewOf(void* a_source)
		{
			const auto at = reinterpret_cast<std::uintptr_t>(a_source);
			return {
				reinterpret_cast<RE::BSSpinLock*>(at),
				*reinterpret_cast<void***>(at + SINKS_AT),
				reinterpret_cast<std::uint32_t*>(at + SINKS_AT + SINKS_SIZE),
				reinterpret_cast<std::int8_t*>(at + NOTIFYING_AT),
			};
		}

		// The module a sink's vtable is in, and its offset there.
		EventTapRules::SinkRow RowOf(void* a_sink, bool a_ours)
		{
			EventTapRules::SinkRow row;
			row.ours = a_ours;
			const auto vtable = a_sink ? *reinterpret_cast<const std::uintptr_t*>(a_sink) : 0;
			REX::W32::MEMORY_BASIC_INFORMATION info{};
			if (vtable && REX::W32::VirtualQuery(reinterpret_cast<const void*>(vtable), &info, sizeof(info)) != 0 && info.allocationBase) {
				// An image's allocation base is its module handle.
				const auto module = reinterpret_cast<REX::W32::HMODULE>(info.allocationBase);
				wchar_t    path[REX::W32::MAX_PATH]{};
				if (const auto n = REX::W32::GetModuleFileNameW(module, path, static_cast<std::uint32_t>(std::size(path))); n > 0 && n < std::size(path)) {
					std::wstring name{ path, n };
					if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring::npos) {
						name.erase(0, slash + 1);
					}
					for (const auto c : name) {
						row.module += c < 0x80 ? static_cast<char>(c) : '?';
					}
				}
				row.offset = vtable - reinterpret_cast<std::uintptr_t>(info.allocationBase);
			} else {
				row.offset = vtable;
			}
			return row;
		}

		std::vector<EventTapRules::SinkRow> RowsOf(void* a_source, void* a_ours)
		{
			std::vector<EventTapRules::SinkRow> rows;
			if (!a_source) {
				return rows;
			}
			const auto                     view = ViewOf(a_source);
			RE::BSAutoLock<RE::BSSpinLock> locker(*view.lock);
			const auto                     size = *view.size;
			if (size > MAX_SINKS || (size != 0 && !view.data)) {
				return rows;
			}
			for (std::uint32_t i = 0; i < size; ++i) {
				rows.push_back(RowOf(view.data[i], view.data[i] == a_ours));
			}
			return rows;
		}

		// Whether a sink is the engine's own: its vtable is in the game's executable.
		bool IsEngineSink(void* a_sink)
		{
			const auto vtable = a_sink ? *reinterpret_cast<const std::uintptr_t*>(a_sink) : 0;
			REX::W32::MEMORY_BASIC_INFORMATION info{};
			static const auto exe = reinterpret_cast<void*>(REX::W32::GetModuleHandleW(nullptr));
			return vtable != 0 && REX::W32::VirtualQuery(reinterpret_cast<const void*>(vtable), &info, sizeof(info)) != 0 && info.allocationBase == exe;
		}

		// Our sink ahead of every other plugin's and behind the engine's own, under the source's lock (EventTapRules.h: the
		// engine's first sink finishes the hit event). 1: moved, 0: already there, -1: not there, or a notify is running (this
		// thread's own, from inside a sink: asked again later).
		int FrontRaw(void* a_source, void* a_sink)
		{
			const auto                     view = ViewOf(a_source);
			RE::BSAutoLock<RE::BSSpinLock> locker(*view.lock);
			if (*view.notifying != 0 || *view.size > MAX_SINKS || !view.data) {
				return -1;
			}
			bool moved = false;
			if (!EventTapRules::PlaceAfterLeading(view.data, *view.size, a_sink, [](void* a_other) { return IsEngineSink(a_other); }, &moved)) {
				return -1;
			}
			return moved ? 1 : 0;
		}

		// The rig's: a sink last in the list.
		int BackRaw(void* a_source, void* a_sink)
		{
			const auto                     view = ViewOf(a_source);
			RE::BSAutoLock<RE::BSSpinLock> locker(*view.lock);
			if (*view.notifying != 0 || *view.size > MAX_SINKS || !view.data) {
				return -1;
			}
			const auto size = *view.size;
			const auto at = EventTapRules::Position(view.data, size, a_sink);
			if (at >= size) {
				return -1;
			}
			std::rotate(view.data + at, view.data + at + 1, view.data + size);
			return 1;
		}

		void ListSinks(std::string_view a_event, void* a_source, void* a_ours)
		{
			REX::INFO("EventTap: the {} event source has {}", a_event, EventTapRules::DescribeSinks(RowsOf(a_source, a_ours)));
		}

		// The rig's sink: stops every hit.
		class StarveSink :
			public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent&, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				++got;
				return RE::BSEventNotifyControl::kStop;
			}
			std::atomic<std::uint32_t> got{ 0 };
		};
		std::atomic<std::uint32_t> sentByPlayer{ 0 };

		StarveSink starveSink;
		bool       starving = false;

		void NotifyHook(RE::BSTEventSource<RE::TESHitEvent>* a_source, const RE::TESHitEvent* a_event)
		{
			// Reads only: the event goes on unchanged, and our own work for one our sink never got is the handler's,
			// which queues it for our frame as the sink's does.
			const bool following = a_event && track.Begin(a_event);
			if (following) {
				tally.Sent();
				if (const auto cause = a_event->cause.get(); cause && cause->GetFormID() == 0x14) {
					++sentByPlayer;
				}
			}
			originalNotify(a_source, a_event);
			if (!following) {
				return;
			}
			if (track.End()) {
				tally.Reached();
				return;
			}
			tally.Stopped();
			if (handling.load(std::memory_order_relaxed) && handler) {
				if (const auto n = ++loggedHandled; n <= 5) {
					REX::WARN("EventTap: hit event {} never reached our sink (a sink ahead of ours stopped it): handled from the tap", n);
				}
				handler(*a_event);
			}
		}
	}

	void SetHitHandler(HitHandler a_handler)
	{
		handler = a_handler;
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("EventTap: no room for the hook ({} bytes free); a hit another plugin's sink stops before ours is not seen", trampoline.free_size());
			return false;
		}
		const std::array<std::uintptr_t, 2>        bases{ SEND_PLAIN.address(), SEND_HITDATA.address() };
		std::array<std::uintptr_t, 2>              at{};
		std::array<std::array<std::uint8_t, 5>, 2> bytes{};
		for (std::size_t i = 0; i < at.size(); ++i) {
			at[i] = bases[i] + EventTapRules::SITES[i].offset;
			std::memcpy(bytes[i].data(), reinterpret_cast<const void*>(at[i]), bytes[i].size());
		}
		const auto sites = EventTapRules::Check(bytes, at, NOTIFY.address());
		if (!sites.All()) {
			REX::WARN("EventTap: {} of the {} calls that send a hit event are not where 1.11.240 has them (another plugin's patch?); none patched, so a hit another plugin's sink "
			          "stops before ours is not seen",
				sites.total - sites.calling, sites.total);
			return false;
		}
		for (const auto site : at) {
			// The same function at both: the first patch's answer is the original.
			const auto original = trampoline.write_call<5>(site, &NotifyHook);
			if (!originalNotify) {
				originalNotify = reinterpret_cast<NotifyFn>(original);
			}
		}
		installed = true;
		REX::INFO("EventTap: the engine's two sends of a hit event (+{:X}+{:X}, +{:X}+{:X}) go through the tap, which hands each on unchanged", SEND_PLAIN.offset(),
			EventTapRules::SITES[0].offset, SEND_HITDATA.offset(), EventTapRules::SITES[1].offset);
		return true;
	}

	bool HitSunk(const RE::TESHitEvent* a_event)
	{
		const bool sent = track.Sunk(a_event);
		if (!sent) {
			tally.Direct();
		}
		return sent;
	}

	void PutHitSinkFirst(RE::BSTEventSink<RE::TESHitEvent>* a_sink)
	{
		ourSink = a_sink;
		const auto source = RE::TESHitEvent::GetEventSource();
		if (!source || !a_sink || !front) {
			return;
		}
		switch (FrontRaw(source, a_sink)) {
		case 1:
			REX::INFO("EventTap: our hit sink moved ahead of the other plugins' sinks (behind the engine's own)");
			break;
		case 0:
			break;
		default:
			REX::WARN("EventTap: could not place our hit sink ahead of the other plugins' (not registered, or the source was being notified); the tap still handles the hits it misses");
			break;
		}
	}

	void OnWelcome()
	{
		const auto source = RE::TESHitEvent::GetEventSource();
		if (!source) {
			return;
		}
		if (ourSink && front && FrontRaw(source, ourSink) == 1) {
			REX::INFO("EventTap: our hit sink's place in the list had changed since (a sink registered); put ahead of the other plugins' again");
		}
		std::scoped_lock l{ listLock };
		if (sinksListed) {
			return;
		}
		sinksListed = true;
		ListSinks("hit", source, ourSink);
		// The other sources our sinks sit on are only listed: nothing says another sink stops them, so none is moved.
		if (const auto deaths = RE::TESDeathEvent::GetEventSource()) {
			ListSinks("death", deaths, nullptr);
		}
		if (const auto containers = RE::TESContainerChangedEvent::GetEventSource()) {
			ListSinks("container-changed", containers, nullptr);
		}
		if (const auto activates = RE::TESActivateEvent::GetEventSource()) {
			ListSinks("activate", activates, nullptr);
		}
	}

	void OnSessionEnd()
	{
		std::scoped_lock l{ listLock };
		sinksListed = false;  // listed again in the next session
	}

	std::string Describe()
	{
		return std::format("hitTap={} hitSent={} hitStopped={} hitDirect={}", installed ? (handling ? "on" : "counting") : "unhooked", tally.SentCount(), tally.StoppedCount(),
			tally.DirectCount());
	}

	std::string CauseNote()
	{
		return EventTapRules::CauseNote(tally.SentCount(), tally.StoppedCount());
	}

	std::string Status()
	{
		std::string out = Describe();
		if (const auto source = RE::TESHitEvent::GetEventSource()) {
			out += std::format(" | front={} handle={} starve={} (stopper got {}, by our player {}) | the hit source has {}", front ? "on" : "off", handling ? "on" : "off", starving ? "on" : "off",
				starveSink.got.load(), sentByPlayer.load(),
				EventTapRules::DescribeSinks(RowsOf(source, ourSink)));
		}
		return out;
	}

	void SetHandling(bool a_on)
	{
		handling = a_on;
	}

	bool SetFront(bool a_on)
	{
		front = a_on;
		const auto source = RE::TESHitEvent::GetEventSource();
		if (!source || !ourSink) {
			return false;
		}
		return (a_on ? FrontRaw(source, ourSink) : BackRaw(source, ourSink)) >= 0;
	}

	bool SetStarve(bool a_on)
	{
		const auto source = RE::TESHitEvent::GetEventSource();
		if (!source) {
			return false;
		}
		if (a_on == starving) {
			return true;
		}
		if (a_on) {
			source->RegisterSink(&starveSink);
			starving = FrontRaw(source, &starveSink) >= 0;
			return starving;
		}
		source->UnregisterSink(&starveSink);
		starving = false;
		return true;
	}
}
