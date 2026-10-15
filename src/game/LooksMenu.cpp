#include "game/LooksMenu.h"

#include "game/BodyMorphs.h"
#include "game/Papyrus.h"

#include <mutex>

// CommonLibF4 declares the callback interface's destructor without defining it; a callback of our own needs one.
RE::BSScript::IStackCallbackFunctor::~IStackCallbackFunctor() = default;

namespace LooksMenu
{
	namespace
	{
		using Clock = BodyMorphs::Clock;
		using Variable = RE::BSScript::Variable;
		using Args = RE::BSScrapArray<Variable>;

		constexpr std::string_view SCRIPT = "BodyGen";

		// Told once what a call to BodyGen returned (null if the VM cancelled it), on one of the VM's threads.
		class Answer final :
			public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Answer(std::function<void(const Variable*)> a_then) :
				then(std::move(a_then))
			{}

			void CallQueued() override {}
			void CallCanceled() override { Give(nullptr); }
			void StartMultiDispatch() override {}
			void EndMultiDispatch() override {}
			void operator()(Variable a_result) override { Give(&a_result); }

		private:
			void Give(const Variable* a_result)
			{
				if (!given.exchange(true)) {
					then(a_result);
				}
			}

			std::function<void(const Variable*)> then;
			std::atomic<bool>                    given{ false };
		};

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> MakeAnswer(std::function<void(const Variable*)> a_then)
		{
			return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{ new Answer(std::move(a_then)) };
		}

		// A read of our player's sliders, filled in by the VM's threads: first the names, then each one's value.
		struct Read
		{
			std::mutex                       lock;
			std::uint32_t                    generation = 0;
			bool                             named = false;  // the names are in
			bool                             cancelled = false;  // the VM cancelled the call for them
			std::vector<std::string>         names;
			bool                             asked = false;  // the values were asked for (main thread only)
			std::size_t                      waiting = 0;    // values not yet answered
			std::vector<Protocol::BodyMorph> found;
		};

		// The sets queued for one stand-in.
		struct Setting
		{
			RE::ObjectRefHandle                         actor;
			std::uint32_t                               id = 0;
			std::shared_ptr<std::atomic<std::uint32_t>> waiting;
			Clock::time_point                           at{};
		};

		std::optional<bool>         installed;
		BodyMorphs::Reading         reading;
		std::shared_ptr<Read>       read;
		BodyMorphs::StandIns        standIns;
		std::vector<Setting>        settings;
		std::uint32_t               reads = 0;
		std::uint32_t               readsFailed = 0;
		std::uint32_t               applied = 0;
		std::uint32_t               cleared = 0;

		// The actor as BodyGen takes it. A stand-in is made at runtime and may have no script object yet.
		RE::BSTSmartPointer<RE::BSScript::Object> ActorObject(RE::Actor* a_actor)
		{
			return Papyrus::BoundObject(a_actor, "Actor", true);
		}

		bool RemoveAll(RE::Actor* a_actor)
		{
			const auto object = ActorObject(a_actor);
			if (!object) {
				return false;
			}
			bool queued = false;
			for (const bool female : { false, true }) {
				queued |= Papyrus::CallStatic(SCRIPT, "RemoveAllMorphs", [&](Args& a_out) {
					a_out = Args{ 2 };
					a_out[0] = object;
					a_out[1] = female;
				});
			}
			return queued;
		}

		// Asks for each named slider's value, under no keyword: LooksMenu's own sliders (other mods' keyworded morphs,
		// a pregnancy's say, are theirs to set in each game).
		void AskValues(const std::shared_ptr<Read>& a_read, RE::Actor* a_player, bool a_female, const std::vector<std::string>& a_names)
		{
			const auto object = ActorObject(a_player);
			{
				std::scoped_lock l{ a_read->lock };
				a_read->asked = true;
				a_read->waiting = object ? a_names.size() : 0;
			}
			if (!object) {
				return;
			}
			for (const auto& name : a_names) {
				const auto answer = MakeAnswer([a_read, name](const Variable* a_value) {
					std::scoped_lock l{ a_read->lock };
					if (a_value && a_value->is<float>()) {
						a_read->found.push_back({ name, RE::BSScript::get<float>(*a_value) });
					}
					--a_read->waiting;
				});
				const bool queued = Papyrus::CallStatic(SCRIPT, "GetMorph", [&](Args& a_out) {
					a_out = Args{ 4 };
					a_out[0] = object;
					a_out[1] = a_female;
					a_out[2] = RE::BSFixedString{ name };
					a_out[3] = nullptr;
				}, answer);
				if (!queued) {
					std::scoped_lock l{ a_read->lock };
					--a_read->waiting;
				}
			}
		}

		void StartRead(RE::Actor* a_player, bool a_female, Clock::time_point a_now)
		{
			const auto generation = reading.Start(a_now);
			const auto object = ActorObject(a_player);
			if (!object) {
				reading.Fail(generation, a_now);
				++readsFailed;
				return;
			}
			auto fresh = std::make_shared<Read>();
			fresh->generation = generation;
			const auto answer = MakeAnswer([fresh](const Variable* a_names) {
				std::scoped_lock l{ fresh->lock };
				if (a_names && a_names->is<RE::BSScript::Array>()) {
					if (const auto array = RE::BSScript::get<RE::BSScript::Array>(*a_names)) {
						for (std::uint32_t i = 0; i < array->size() && fresh->names.size() < BodyMorphs::MAX_NAMES_ASKED; ++i) {
							const auto& name = (*array)[i];
							if (name.is<RE::BSFixedString>()) {
								std::string text{ RE::BSScript::get<RE::BSFixedString>(name).c_str() };
								if (BodyMorphs::Sendable(text)) {
									fresh->names.push_back(std::move(text));
								}
							}
						}
					}
				}
				fresh->cancelled = !a_names;
				fresh->named = true;
			});
			const bool queued = Papyrus::CallStatic(SCRIPT, "GetMorphs", [&](Args& a_out) {
				a_out = Args{ 2 };
				a_out[0] = object;
				a_out[1] = a_female;
			}, answer);
			if (!queued) {
				reading.Fail(generation, a_now);
				++readsFailed;
				return;
			}
			read = std::move(fresh);
		}

		void FrameRead(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto npc = player ? player->GetNPC() : nullptr;
			if (!npc) {
				return;
			}
			if (reading.Expire(a_now)) {
				++readsFailed;
				REX::WARN("LooksMenu: reading our body sliders got no answer; trying again later");
				read.reset();
			}
			if (read) {
				bool askNow = false;
				bool cancelled = false;
				bool done = false;
				std::vector<std::string>         names;
				std::vector<Protocol::BodyMorph> found;
				{
					std::scoped_lock l{ read->lock };
					if (read->named && read->cancelled) {
						cancelled = true;
					} else if (read->named && !read->asked) {
						askNow = !read->names.empty();
						done = read->names.empty();
						names = read->names;
					} else if (read->asked && read->waiting == 0) {
						done = true;
						found = std::move(read->found);
					}
				}
				if (cancelled) {
					// Not a body with no sliders: what an earlier read found stays.
					reading.Fail(read->generation, a_now);
					++readsFailed;
					read.reset();
					return;
				}
				if (askNow) {
					AskValues(read, player, npc->IsFemale(), names);
					std::scoped_lock l{ read->lock };
					if (read->waiting == 0) {
						done = true;
						found = std::move(read->found);
					}
				}
				if (done) {
					const auto before = reading.Morphs();
					if (reading.Finish(read->generation, std::move(found), a_now)) {
						++reads;
						if (reading.Morphs() != before) {
							REX::INFO("LooksMenu: our body has {} sliders set", reading.Morphs().size());
						}
					}
					read.reset();
				}
				return;
			}
			const auto ui = RE::UI::GetSingleton();
			if (reading.ShouldStart(!ui || ui->menuMode != 0, a_now)) {
				StartRead(player, npc->IsFemale(), a_now);
			}
		}

		void FrameSettings(Clock::time_point a_now)
		{
			std::erase_if(settings, [&](const Setting& a_setting) {
				if (a_setting.waiting->load() != 0 && a_now - a_setting.at < BodyMorphs::ANSWER_TIMEOUT) {
					return false;
				}
				const auto ref = a_setting.actor.get();
				const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				switch (standIns.Answered(a_setting.id)) {
				case BodyMorphs::Then::kUpdate:
					// Its 3D built before the sets were answered has none of them: rebuilt (one not built yet takes
					// them as it is).
					if (actor && actor->Get3D()) {
						const auto object = ActorObject(actor);
						if (object) {
							Papyrus::CallStatic(SCRIPT, "UpdateMorphs", [&](Args& a_out) {
								a_out = Args{ 1 };
								a_out[0] = object;
							});
						}
					}
					break;
				case BodyMorphs::Then::kClear:
					if (actor && RemoveAll(actor)) {
						++cleared;
					}
					break;
				case BodyMorphs::Then::kNothing:
					break;
				}
				return true;
			});
		}
	}

	bool Installed()
	{
		// Plugins are loaded before the game runs a frame of ours, so the answer then holds.
		if (!installed) {
			installed = REX::W32::GetModuleHandleW(L"f4ee.dll") != nullptr;
			REX::INFO("LooksMenu: {}", *installed ? "found (body sliders travel)" : "not installed");
		}
		return *installed;
	}

	void Frame()
	{
		if (!Installed()) {
			return;
		}
		const auto now = Clock::now();
		FrameRead(now);
		FrameSettings(now);
	}

	bool Settled()
	{
		return BodyMorphs::FaceReady(Installed(), reading);
	}

	std::vector<Protocol::BodyMorph> PlayerMorphs()
	{
		return Installed() ? reading.Morphs() : std::vector<Protocol::BodyMorph>{};
	}

	void Apply(RE::Actor* a_standIn, bool a_female, const std::vector<Protocol::BodyMorph>& a_morphs)
	{
		if (!a_standIn || a_morphs.empty() || !Installed()) {
			return;
		}
		const auto object = ActorObject(a_standIn);
		if (!object) {
			return;
		}
		const auto waiting = std::make_shared<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(a_morphs.size()));
		for (const auto& morph : a_morphs) {
			const auto answer = MakeAnswer([waiting](const Variable*) { --*waiting; });
			const bool queued = Papyrus::CallStatic(SCRIPT, "SetMorph", [&](Args& a_out) {
				a_out = Args{ 5 };
				a_out[0] = object;
				a_out[1] = a_female;
				a_out[2] = RE::BSFixedString{ morph.name };
				a_out[3] = nullptr;  // no keyword: LooksMenu's own sliders
				a_out[4] = morph.value;
			}, answer);
			if (!queued) {
				--*waiting;
			}
		}
		standIns.Setting(a_standIn->GetFormID());
		settings.push_back({ a_standIn->GetHandle(), a_standIn->GetFormID(), waiting, Clock::now() });
		++applied;
	}

	void Remove(RE::Actor* a_standIn)
	{
		if (a_standIn && standIns.Leaving(a_standIn->GetFormID()) && RemoveAll(a_standIn)) {
			++cleared;
		}
	}

	void OnGameLoaded()
	{
		reading.Reset();
		read.reset();
		standIns.Reset();
		settings.clear();
	}

	std::string Describe()
	{
		if (!installed.value_or(false)) {
			return "looksmenu: no";
		}
		return std::format("looksmenu: sliders={} reads={} failed={} applied={} cleared={} standins={}", reading.Morphs().size(), reads, readsFailed, applied, cleared, standIns.Count());
	}
}
