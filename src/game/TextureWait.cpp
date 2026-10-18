#include "game/TextureWait.h"

#include "HangWatch.h"
#include "game/TextureFlush.h"

#include <array>
#include <atomic>
#include <cstring>

// The engine's texture update, as read from Fallout4.exe 1.11.240 (docs/HANG-20261017.md, with the walk's pool-full
// hand-over read since): its state block at +32FF858 and the functions the walk and the wait use.
namespace TextureWait
{
	namespace
	{
		using TextureFlush::Counters;

		// A batch (BSTextureStreamer::TextureModifyBatch): a vtable, then a singly linked list of requests (head, and the
		// address of the last link). A request's next link is at +B8.
		constexpr std::uintptr_t BATCH_HEAD = 0x8;
		constexpr std::uintptr_t REQUEST_NEXT = 0xB8;
		constexpr std::uint32_t  LIST_LIMIT = 100000;  // a walk along a list stops here (a pool holds far fewer)

		// NiTexture's flags: 0x10 while a request for it is in a batch (the walk sets it, ReleaseBatch clears it); the walk
		// leaves alone a texture with 0x400 too.
		constexpr std::uint32_t IN_BATCH = 0x10;
		constexpr std::uint32_t WALK_SKIPS = 0x410;
		constexpr std::uintptr_t RENDERER_LEVEL = 0x3D;  // BSGraphics::Texture: the degrade level it is at

		// The functions a look checks the stacks for, and their lengths (tools/addrcheck.ps1 checks both).
		constexpr std::uintptr_t PUMP_LENGTH = 0xEF;   // PositionPlayerJob::Run
		constexpr std::uintptr_t WAIT_LENGTH = 0x310;  // the texture update's blocking wait, to its last ret
		constexpr std::size_t    LOOK_CALLERS = 64;

		struct Engine
		{
			std::uint8_t*  pending = reinterpret_cast<std::uint8_t*>(REL::ID(2698346).address());
			std::uint32_t* done = reinterpret_cast<std::uint32_t*>(REL::ID(2698349).address());
			void**         second = reinterpret_cast<void**>(REL::ID(2698350).address());
			void**         current = reinterpret_cast<void**>(REL::ID(2698351).address());
			std::uintptr_t mainBatch = REL::ID(2698368).address();  // the main thread's batch (a static one): current but while the wait runs
			std::uint32_t* total = reinterpret_cast<std::uint32_t*>(REL::ID(2698352).address());
			std::uint32_t* stage = reinterpret_cast<std::uint32_t*>(REL::ID(2698354).address());
			std::uint32_t* owner = reinterpret_cast<std::uint32_t*>(REL::ID(2698358).address());  // the lock's owner thread
			std::uint32_t* count = reinterpret_cast<std::uint32_t*>(REL::ID(2698359).address());  // and its count
			RE::NiTexture** textures = reinterpret_cast<RE::NiTexture**>(REL::ID(2703490).address());  // every NiTexture, linked by next
			std::uintptr_t pump = REL::ID(2228998).address();
			std::uintptr_t wait = REL::ID(2229488).address();

			// SubmitBatch: hands the batch's list to the streamer, empties it and wakes the streamer.
			bool Submit(void* a_batch) const
			{
				using Fn = bool (*)(void*);
				static REL::Relocation<Fn> fn{ REL::ID(2275477) };
				return fn(a_batch);
			}

			// AddToBatch: takes a request from the streamer's pool (none: false), links it into the batch for a_texture at
			// a_level, and counts it into *a_total at once.
			bool Add(void* a_batch, RE::NiTexture* a_texture, std::uint32_t a_level) const
			{
				using Fn = bool (*)(void*, RE::NiTexture*, std::uint32_t*, std::uint32_t*, std::uint32_t);
				static REL::Relocation<Fn> fn{ REL::ID(2275476) };
				return fn(a_batch, a_texture, done, total, a_level);
			}
		};

		const Engine& Game()
		{
			static const Engine engine;
			return engine;
		}

		template <class T>
		T Load(T* a_at)
		{
			return std::atomic_ref<T>{ *a_at }.load();
		}

		template <class T>
		void Store(T* a_at, T a_value)
		{
			std::atomic_ref<T>{ *a_at }.store(a_value);
		}

		Counters Read()
		{
			const auto& e = Game();
			return { .pending = Load(e.pending) != 0, .done = Load(e.done), .total = Load(e.total) };
		}

		std::string Describe(const Counters& a_counters)
		{
			return std::format("pending={} done={} total={}", a_counters.pending ? 1 : 0, a_counters.done, a_counters.total);
		}

		// Requests linked into a batch's list, read through ReadMemory (another thread may own the list): none when the
		// batch can't be read.
		std::optional<std::uint32_t> Owed(std::uintptr_t a_batch)
		{
			std::uintptr_t at = 0;
			if (!HangWatch::ReadMemory(a_batch + BATCH_HEAD, &at, sizeof(at))) {
				return std::nullopt;
			}
			std::uint32_t owed = 0;
			while (at != 0 && owed < LIST_LIMIT) {
				++owed;
				if (!HangWatch::ReadMemory(at + REQUEST_NEXT, &at, sizeof(at))) {
					break;
				}
			}
			return owed;
		}

		// The texture update's lock, taken the way the engine takes it: a recursive spin lock (owner thread, count), tried
		// once. The per-frame update and the wait both give up when another thread holds it; so do we.
		class Lock
		{
		public:
			Lock()
			{
				const auto& e = Game();
				const auto  me = REX::W32::GetCurrentThreadId();
				if (Load(e.owner) == me) {
					std::atomic_ref{ *e.count }.fetch_add(1);
					held = true;
					return;
				}
				std::uint32_t expected = 0;
				if (std::atomic_ref{ *e.count }.compare_exchange_strong(expected, 1)) {
					Store(e.owner, me);
					held = true;
				}
			}

			~Lock()
			{
				if (!held) {
					return;
				}
				const auto& e = Game();
				std::atomic_ref count{ *e.count };
				if (count.load() == 1) {
					Store(e.owner, 0u);
					count.store(0);
				} else {
					count.fetch_sub(1);
				}
			}

			Lock(const Lock&) = delete;
			Lock& operator=(const Lock&) = delete;

			explicit operator bool() const { return held; }

		private:
			bool held = false;
		};

		std::atomic<bool>          flushOn{ true };  // the dev channel can turn it off, to show the hang it prevents
		std::atomic<std::uint32_t> handedOver{ 0 };  // moves before which requests were handed over, for `texwait`

		// After a strand, for a_seconds: every change in what the hang turns on, logged from a thread of its own (the
		// main thread may be about to stop): pending, the stage, the lock's owner, whether the main thread's batch is
		// empty, whether the main thread is in the pump, and each reset of the counters. Shows whether a frame of the
		// walk comes between the strand and the move's wait.
		void Watch(int a_seconds)
		{
			std::thread([a_seconds]() {
				const auto& e = Game();
				const auto  main = RE::Main::GetSingleton();
				const auto  mainThread = HangWatch::WindowThread(main ? reinterpret_cast<void*>(main->hwnd) : nullptr);
				struct State
				{
					bool          pending, owes, pump;
					std::uint32_t stage, owner, total;
				};
				const auto start = std::chrono::steady_clock::now();
				const auto end = start + std::chrono::seconds{ a_seconds };
				std::optional<State> last;
				int                  lines = 0, samples = 0;
				bool                 pump = false;
				while (std::chrono::steady_clock::now() < end && lines < 200) {
					std::uintptr_t head = 0;
					if (!HangWatch::ReadMemory(reinterpret_cast<std::uintptr_t>(Load(e.current)) + BATCH_HEAD, &head, sizeof(head))) {
						head = 0;
					}
					if (samples++ % 5 == 0) {
						pump = TextureFlush::OnStack(HangWatch::LookAtThread(mainThread, LOOK_CALLERS).callers, e.pump, e.pump + PUMP_LENGTH);
					}
					const auto  counters = Read();
					const State now{ counters.pending, head != 0, pump, Load(e.stage), Load(e.owner), counters.total };
					if (!last || now.pending != last->pending || now.owes != last->owes || now.pump != last->pump || now.stage != last->stage || now.owner != last->owner ||
						now.total < last->total) {
						const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
						REX::INFO("Texture wait: watch +{} ms: {} stage={} main batch {} lock={} pump={}", ms, Describe(counters), now.stage, now.owes ? "owes" : "empty",
							now.owner == 0 ? "none"s : now.owner == mainThread ? "main"s : std::format("0x{:x}", now.owner), now.pump ? "yes" : "no");
						++lines;
					}
					last = now;
					std::this_thread::sleep_for(1ms);
				}
				REX::INFO("Texture wait: watch ends after {} samples: {}", samples, Describe(Read()));
			}).detach();
		}

		// strand: what the walk leaves when the streamer's pool ran out (it handed its batch over and set pending) and it
		// then counted one more request into the main thread's batch, not handed over. A real request, for a real texture,
		// at the level it is at, the way the walk adds one. The next move hangs on it unless it is handed over first.
		std::string Strand()
		{
			const auto& e = Game();
			const Lock  lock;
			if (!lock) {
				return std::format("error: the texture update's lock is held by thread 0x{:x}", Load(e.owner));
			}
			const auto batch = Load(e.current);
			if (!batch) {
				return "error: no texture batch";
			}
			RE::NiTexture* texture = Load(e.textures);
			std::uint32_t  seen = 0;
			for (; texture && seen < LIST_LIMIT; texture = texture->next, ++seen) {
				if (texture->rendererTexture && (texture->flags & WALK_SKIPS) == 0) {
					break;
				}
			}
			if (!texture || seen == LIST_LIMIT) {
				return "error: no texture to request";
			}
			const auto level = *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(texture->rendererTexture) + RENDERER_LEVEL);
			const auto before = Read();
			if (!e.Add(batch, texture, level)) {
				return "error: the streamer's pool of requests is empty right now; try again";
			}
			texture->flags |= IN_BATCH;  // as the walk does: no second request for it
			texture->desiredDegradeLevel = static_cast<std::int8_t>(level);
			Store(e.pending, std::uint8_t{ 1 });
			const auto after = Read();
			REX::WARN("Texture wait: strand (dev): {} before, {} now, stage {}; one request counted in the main thread's batch and not handed over", Describe(before), Describe(after),
				Load(e.stage));
			return std::format("stranded: {} (was {}), stage {}, {} request(s) owed; the next move hangs unless they are handed over first (texwait flush is {})", Describe(after),
				Describe(before), Load(e.stage), Owed(reinterpret_cast<std::uintptr_t>(batch)).value_or(0), flushOn ? "on" : "off");
		}

		std::string Status()
		{
			const auto& e = Game();
			const auto  batch = reinterpret_cast<std::uintptr_t>(Load(e.current));
			const auto  owed = Owed(batch);
			return std::format("{} stage={} lock=0x{:x}/{} owed={} flush={} handed-over-before={}", Describe(Read()), Load(e.stage), Load(e.owner), Load(e.count),
				owed ? std::to_string(*owed) : "?"s, flushOn ? "on" : "off", handedOver.load());
		}

		// sample <s>: reads the state once a millisecond for a_args seconds, from the channel's thread, and counts how
		// often the main thread's batch held requests counted and not handed over while pending was set: the moments in
		// which a move would hang.
		std::string Sample(std::string_view a_args)
		{
			int seconds = 10;
			if (std::from_chars(a_args.data(), a_args.data() + a_args.size(), seconds).ec != std::errc{}) {
				seconds = 10;
			}
			seconds = std::clamp(seconds, 1, 60);
			const auto& e = Game();
			std::uint64_t samples = 0, pending = 0, hazard = 0;
			std::uint32_t mostOwed = 0, mostGap = 0;
			const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{ seconds };
			while (std::chrono::steady_clock::now() < end) {
				const auto counters = Read();
				++samples;
				if (counters.pending) {
					++pending;
					std::uintptr_t head = 0;
					if (HangWatch::ReadMemory(reinterpret_cast<std::uintptr_t>(Load(e.current)) + BATCH_HEAD, &head, sizeof(head)) && head != 0) {
						++hazard;
						mostOwed = (std::max)(mostOwed, Owed(reinterpret_cast<std::uintptr_t>(Load(e.current))).value_or(0));
						mostGap = (std::max)(mostGap, counters.total > counters.done ? counters.total - counters.done : 0u);
					}
				}
				std::this_thread::sleep_for(1ms);
			}
			return std::format("{} samples in {} s: pending in {}, pending with requests owed in {} (the hang's window), at most {} owed, total-done at most {} then", samples, seconds, pending,
				hazard, mostOwed, mostGap);
		}

		// unstick: the way out of a wait that is stuck (a dev command for the test, not done on its own): clears pending,
		// the wait's loop ends, and the wait does what it does after it (releases its batch, zeroes the counters, puts the
		// main thread's batch back). Only when the lock's owner, another thread than the main one, is in the wait.
		std::string Unstick()
		{
			const auto& e = Game();
			const auto  main = RE::Main::GetSingleton();
			const auto  mainThread = HangWatch::WindowThread(main ? reinterpret_cast<void*>(main->hwnd) : nullptr);
			const auto  counters = Read();
			const auto  owner = Load(e.owner);
			if (!TextureFlush::Waiting(counters)) {
				return std::format("nothing to do: {} (the wait's loop would end)", Describe(counters));
			}
			if (owner == 0 || owner == mainThread) {
				return std::format("nothing to do: {}, the lock held by 0x{:x}, not a thread in the wait", Describe(counters), owner);
			}
			const auto look = HangWatch::LookAtThread(owner, LOOK_CALLERS);
			if (!TextureFlush::OnStack(look.callers, e.wait, e.wait + WAIT_LENGTH)) {
				return std::format("nothing to do: {}, the lock's owner 0x{:x} is not in the wait", Describe(counters), owner);
			}
			Store(e.pending, std::uint8_t{ 0 });
			REX::WARN("Texture wait: unstick (dev): cleared pending at {}, the lock's owner 0x{:x} in the wait", Describe(counters), owner);
			return std::format("cleared pending at {}; the wait on thread 0x{:x} ends", Describe(counters), owner);
		}
	}

	namespace
	{
		constexpr std::uintptr_t PUMP_CALL = 0x37F;  // in Main::OnIdle: `call PositionPlayerJob::Run`, after the position flag's test

		// Main thread, right before a move starts (nothing walks the textures from here until the job is done).
		void HandOver()
		{
			const auto& e = Game();
			if (!flushOn) {
				REX::WARN("Texture wait: before the move, handing over is off (dev): {}, {} request(s) owed", Describe(Read()),
					Owed(reinterpret_cast<std::uintptr_t>(Load(e.current))).value_or(0));
				return;
			}
			const Lock lock;
			if (!lock) {
				REX::WARN("Texture wait: before the move, the texture update's lock is held by thread 0x{:x}; nothing handed over", Load(e.owner));
				return;
			}
			const auto batch = Load(e.current);
			const auto counters = Read();
			const auto owed = batch ? Owed(reinterpret_cast<std::uintptr_t>(batch)).value_or(0) : 0;
			if (!TextureFlush::NeedsFlush(counters, owed)) {
				REX::DEBUG("Texture wait: before the move, nothing owed ({}, {} request(s) in the main thread's batch)", Describe(counters), owed);
				return;
			}
			e.Submit(batch);
			++handedOver;
			REX::INFO("Texture wait: before the move, handed the streamer the {} request(s) the texture walk had counted and not handed over ({})", owed, Describe(counters));
		}

		bool (*runPump)() = nullptr;  // PositionPlayerJob::Run, as OnIdle called it

		bool BeforePump()
		{
			// An exception must never escape into the game's frame loop; the move goes ahead either way.
			try {
				HandOver();
			} catch (...) {
				REX::ERROR("Texture wait: the hand-over before a move failed");
			}
			return runPump();
		}
	}

	bool Install()
	{
		const auto& e = Game();
		const auto  site = REL::ID(2228917).address() + PUMP_CALL;  // Main::OnIdle
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != e.pump) {
			REX::WARN("Texture wait: Main::OnIdle doesn't call PositionPlayerJob::Run where 1.11.240 does (another plugin's patch?); no hand-over before a move");
			return false;
		}
		runPump = reinterpret_cast<bool (*)()>(REL::GetTrampoline().write_call<5>(site, &BeforePump));
		REX::INFO("Texture wait: the texture walk's owed requests are handed over before every move");
		return true;
	}

	std::string DescribePump(void* a_window)
	{
		const auto& e = Game();
		const auto  mainThread = HangWatch::WindowThread(a_window);
		const auto  main = HangWatch::LookAtThread(mainThread, LOOK_CALLERS);
		if (!TextureFlush::OnStack(main.callers, e.pump, e.pump + PUMP_LENGTH)) {
			return {};
		}
		const auto counters = Read();
		const auto owner = Load(e.owner);
		std::string text = std::format("the main thread is in the engine's pump, moving the player; the texture wait: {} ({}), the lock held by ", Describe(counters),
			TextureFlush::Waiting(counters) ? "its loop goes on" : "its loop would end");
		if (owner == 0) {
			return text + "no thread";
		}
		if (owner == mainThread) {
			return text + "the main thread";
		}
		const auto look = HangWatch::LookAtThread(owner, LOOK_CALLERS);
		if (!TextureFlush::OnStack(look.callers, e.wait, e.wait + WAIT_LENGTH)) {
			return text + std::format("thread 0x{:x} (count {}), not in the wait{}", owner, Load(e.count), look.looked ? "" : " (couldn't be looked at)");
		}
		text += std::format("thread 0x{:x} (count {}), in the wait", owner, Load(e.count));
		// The wait sets the main thread's batch aside (in rsi, +C549CB..+C54BCF: a register Sleep's own frames save, so
		// not read from the thread) and makes its own current; the main thread's is a static one, read where it is.
		if (reinterpret_cast<std::uintptr_t>(Load(e.current)) != e.mainBatch) {
			if (const auto owed = Owed(e.mainBatch)) {
				return text + std::format("; the main thread's batch, set aside by the wait, holds {} request(s) never handed to the streamer", *owed);
			}
		}
		return text + "; the batch it set aside couldn't be read";
	}

	bool OnMainThread(std::string_view a_args)
	{
		return a_args.starts_with("strand");
	}

	std::string Command(std::string_view a_args)
	{
		const auto split = a_args.find(' ');
		const auto verb = a_args.substr(0, split);
		const auto rest = split == std::string_view::npos ? std::string_view{} : a_args.substr(split + 1);
		if (verb.empty() || verb == "status") {
			return Status();
		}
		if (verb == "strand") {
			// strand <console command>: the command (a coc) right after, in the same call on the main thread, and a watch
			// of what follows in the log.
			auto out = Strand();
			if (!out.starts_with("error")) {
				Watch(10);
			}
			if (!rest.empty() && !out.starts_with("error")) {
				const std::string command{ rest };
				REX::INFO("Texture wait: strand, then console {} (dev)", command);
				RE::Console::ExecuteCommand(command.c_str());
				out += "; then " + command;
			}
			return out;
		}
		if (verb == "sample") {
			return Sample(rest);
		}
		if (verb == "unstick") {
			return Unstick();
		}
		if (verb == "flush" && (rest == "on" || rest == "off")) {
			flushOn = rest == "on";
			REX::INFO("Texture wait: handing over before a move is {} (dev)", rest);
			return std::format("flush {}", rest);
		}
		return "error: texwait [status | sample <s> | strand [<console command>] | unstick | flush on|off]";
	}
}
