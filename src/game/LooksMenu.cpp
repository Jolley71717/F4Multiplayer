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
		// LooksMenu's overlay script and its entry (Overlays.psc): uid, priority, template, red, green, blue, alpha,
		// offset_u, offset_v, scale_u, scale_v. Add, GetAll, RemoveAll and Update take the actor and, but for Update,
		// its sex (LooksMenu keeps a character's overlays per sex, as its sliders).
		constexpr std::string_view OVERLAYS = "Overlays";
		constexpr std::string_view ENTRY = "Overlays#Entry";

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

		// A read of our player's sliders, filled in by the VM's threads: first the names, then each one's value; and our
		// overlays, in one call beside the names.
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
			bool                             overlaysIn = false;  // the overlays are in, or will not come
			std::optional<std::vector<LooksOverlays::Entry>> overlays;  // none if that call was not answered
		};

		// The sets and adds queued for one stand-in.
		struct Setting
		{
			RE::ObjectRefHandle                         actor;
			std::uint32_t                               id = 0;
			std::shared_ptr<std::atomic<std::uint32_t>> waiting;
			Clock::time_point                           at{};
			bool                                        morphs = false;
			bool                                        overlays = false;
		};

		std::optional<bool>         installed;
		BodyMorphs::Reading         reading;
		std::shared_ptr<Read>       read;
		BodyMorphs::StandIns        standIns;
		std::vector<Setting>        settings;
		std::uint32_t               reads = 0;
		std::uint32_t               readsFailed = 0;
		std::uint32_t               applied = 0;
		std::atomic<std::uint32_t>  overlaysAdded{ 0 };    // counted as Add answers, on the VM's threads
		std::atomic<std::uint32_t>  overlaysRefused{ 0 };
		std::uint32_t               cleared = 0;
		bool                        toldNoEntry = false;

		// The actor as BodyGen takes it. A stand-in is made at runtime and may have no script object yet.
		RE::BSTSmartPointer<RE::BSScript::Object> ActorObject(RE::Actor* a_actor)
		{
			return Papyrus::BoundObject(a_actor, "Actor", true);
		}

		// Its sliders and overlays out of LooksMenu's store, under both sexes.
		bool RemoveAll(RE::Actor* a_actor)
		{
			const auto object = ActorObject(a_actor);
			if (!object) {
				return false;
			}
			bool queued = false;
			for (const bool female : { false, true }) {
				for (const auto [script, function] : { std::pair{ SCRIPT, std::string_view{ "RemoveAllMorphs" } }, std::pair{ OVERLAYS, std::string_view{ "RemoveAll" } } }) {
					queued |= Papyrus::CallStatic(script, function, [&](Args& a_out) {
						a_out = Args{ 2 };
						a_out[0] = object;
						a_out[1] = female;
					});
				}
			}
			return queued;
		}

		float FloatField(RE::BSScript::Struct& a_entry, std::string_view a_name, float a_default)
		{
			const auto field = Papyrus::Field(a_entry, a_name);
			return field && field->is<float>() ? RE::BSScript::get<float>(*field) : a_default;
		}

		// One of Overlays.GetAll's entries, or none if it is not one (no template, say).
		std::optional<LooksOverlays::Entry> ReadEntry(const Variable& a_value)
		{
			if (!a_value.is<RE::BSScript::Struct>()) {
				return std::nullopt;
			}
			const auto entry = RE::BSScript::get<RE::BSScript::Struct>(a_value);
			if (!entry) {
				return std::nullopt;
			}
			const auto templ = Papyrus::Field(*entry, "template");
			const auto priority = Papyrus::Field(*entry, "priority");
			if (!templ || !templ->is<RE::BSFixedString>() || !priority || !priority->is<std::int32_t>()) {
				return std::nullopt;
			}
			LooksOverlays::Entry out;
			out.templateId = RE::BSScript::get<RE::BSFixedString>(*templ).c_str();
			out.priority = RE::BSScript::get<std::int32_t>(*priority);
			out.tint[0] = FloatField(*entry, "red", 1.0f);
			out.tint[1] = FloatField(*entry, "green", 1.0f);
			out.tint[2] = FloatField(*entry, "blue", 1.0f);
			out.tint[3] = FloatField(*entry, "alpha", 1.0f);
			out.offset[0] = FloatField(*entry, "offset_u", 0.0f);
			out.offset[1] = FloatField(*entry, "offset_v", 0.0f);
			out.scale[0] = FloatField(*entry, "scale_u", 1.0f);
			out.scale[1] = FloatField(*entry, "scale_v", 1.0f);
			return out;
		}

		// A friend's overlay as Overlays.Add takes it, or null if this LooksMenu has no such entry.
		RE::BSTSmartPointer<RE::BSScript::Struct> MakeEntry(const Protocol::Overlay& a_overlay)
		{
			auto entry = Papyrus::NewStruct(ENTRY);
			if (!entry) {
				if (!toldNoEntry) {
					toldNoEntry = true;
					REX::WARN("LooksMenu: no Overlays#Entry in the scripts (an older LooksMenu?); friends' overlays not shown");
				}
				return {};
			}
			const auto from = LooksOverlays::ToEntry(a_overlay);
			const auto set = [&](std::string_view a_name, auto a_value) {
				if (const auto field = Papyrus::Field(*entry, a_name)) {
					*field = a_value;
				}
			};
			set("priority", from.priority);
			set("template", RE::BSFixedString{ from.templateId });
			set("red", from.tint[0]);
			set("green", from.tint[1]);
			set("blue", from.tint[2]);
			set("alpha", from.tint[3]);
			set("offset_u", from.offset[0]);
			set("offset_v", from.offset[1]);
			set("scale_u", from.scale[0]);
			set("scale_v", from.scale[1]);
			return entry;
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

		// Our overlays, beside the slider names. Not answered (no such script), the read still goes with the sliders
		// and the overlays read before.
		void AskOverlays(const std::shared_ptr<Read>& a_read, const RE::BSTSmartPointer<RE::BSScript::Object>& a_object, bool a_female)
		{
			const auto answer = MakeAnswer([a_read](const Variable* a_all) {
				std::scoped_lock l{ a_read->lock };
				if (a_all && a_all->is<RE::BSScript::Array>()) {
					std::vector<LooksOverlays::Entry> entries;
					if (const auto array = RE::BSScript::get<RE::BSScript::Array>(*a_all)) {
						for (std::uint32_t i = 0; i < array->size() && entries.size() < 2 * Protocol::MAX_OVERLAYS; ++i) {
							if (auto entry = ReadEntry((*array)[i])) {
								entries.push_back(std::move(*entry));
							}
						}
					}
					a_read->overlays = std::move(entries);
				} else if (a_all && a_all->is<std::nullptr_t>()) {
					a_read->overlays = std::vector<LooksOverlays::Entry>{};  // a None array: none worn
				}
				a_read->overlaysIn = true;
			});
			const bool queued = Papyrus::CallStatic(OVERLAYS, "GetAll", [&](Args& a_out) {
				a_out = Args{ 2 };
				a_out[0] = a_object;
				a_out[1] = a_female;
			}, answer);
			if (!queued) {
				std::scoped_lock l{ a_read->lock };
				a_read->overlaysIn = true;
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
			AskOverlays(fresh, object, a_female);
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
				REX::WARN("LooksMenu: reading our body sliders and overlays got no answer; trying again later");
				read.reset();
			}
			if (read) {
				bool askNow = false;
				bool cancelled = false;
				std::vector<std::string> names;
				{
					std::scoped_lock l{ read->lock };
					if (read->named && read->cancelled) {
						cancelled = true;
					} else if (read->named && !read->asked && !read->names.empty()) {
						askNow = true;
						names = read->names;
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
				}
				// Done once every value and the overlays are in (a body with no sliders has no values to wait for).
				bool done = false;
				std::vector<Protocol::BodyMorph>                 found;
				std::optional<std::vector<LooksOverlays::Entry>> overlays;
				{
					std::scoped_lock l{ read->lock };
					const bool valuesIn = read->named && (read->asked ? read->waiting == 0 : read->names.empty());
					if (valuesIn && read->overlaysIn) {
						done = true;
						found = std::move(read->found);
						overlays = std::move(read->overlays);
					}
				}
				if (done) {
					const auto before = reading.Morphs();
					const auto overlaysBefore = reading.Overlays();
					if (reading.Finish(read->generation, std::move(found), a_now, overlays)) {
						++reads;
						if (reading.Morphs() != before) {
							REX::INFO("LooksMenu: our body has {} sliders set", reading.Morphs().size());
						}
						if (reading.Overlays() != overlaysBefore) {
							REX::INFO("LooksMenu: we wear {} overlays", reading.Overlays().size());
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
					// Its 3D built before the sets were answered has none of them: rebuilt, and the overlays laid on it
					// (one not built yet takes them as it is).
					if (actor && actor->Get3D()) {
						const auto object = ActorObject(actor);
						if (object) {
							if (a_setting.morphs) {
								Papyrus::CallStatic(SCRIPT, "UpdateMorphs", [&](Args& a_out) {
									a_out = Args{ 1 };
									a_out[0] = object;
								});
							}
							if (a_setting.overlays) {
								Papyrus::CallStatic(OVERLAYS, "Update", [&](Args& a_out) {
									a_out = Args{ 1 };
									a_out[0] = object;
								});
							}
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
			REX::INFO("LooksMenu: {}", *installed ? "found (body sliders and overlays travel)" : "not installed");
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

	std::vector<Protocol::Overlay> PlayerOverlays()
	{
		return Installed() ? reading.Overlays() : std::vector<Protocol::Overlay>{};
	}

	void Apply(RE::Actor* a_standIn, bool a_female, const std::vector<Protocol::BodyMorph>& a_morphs, const std::vector<Protocol::Overlay>& a_overlays)
	{
		if (!a_standIn || (a_morphs.empty() && a_overlays.empty()) || !Installed()) {
			return;
		}
		const auto object = ActorObject(a_standIn);
		if (!object) {
			return;
		}
		const auto waiting = std::make_shared<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(a_morphs.size() + a_overlays.size()));
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
		// Each overlay LooksMenu here has the template for (Add answers 0 for one from an overlay mod this game lacks).
		for (const auto& overlay : a_overlays) {
			const auto entry = MakeEntry(overlay);
			if (!entry) {
				--*waiting;
				continue;
			}
			const auto answer = MakeAnswer([waiting](const Variable* a_uid) {
				if (a_uid && a_uid->is<std::int32_t>() && RE::BSScript::get<std::int32_t>(*a_uid) != 0) {
					++overlaysAdded;
				} else {
					++overlaysRefused;
				}
				--*waiting;
			});
			const bool queued = Papyrus::CallStatic(OVERLAYS, "Add", [&](Args& a_out) {
				a_out = Args{ 3 };
				a_out[0] = object;
				a_out[1] = a_female;
				a_out[2] = entry;
			}, answer);
			if (!queued) {
				--*waiting;
			}
		}
		standIns.Setting(a_standIn->GetFormID());
		settings.push_back({ a_standIn->GetHandle(), a_standIn->GetFormID(), waiting, Clock::now(), !a_morphs.empty(), !a_overlays.empty() });
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
		return std::format("looksmenu: sliders={} overlays={} reads={} failed={} applied={} added={} refused={} cleared={} standins={}", reading.Morphs().size(), reading.Overlays().size(), reads,
			readsFailed, applied, overlaysAdded.load(), overlaysRefused.load(), cleared, standIns.Count());
	}
}
