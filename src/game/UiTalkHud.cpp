#include "game/UiTalkHud.h"

#include "Config.h"
#include "game/Hud.h"
#include "game/UiPanel.h"
#include "game/Voice.h"
#include "net/Session.h"

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>

namespace UiTalkHud
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;
		using Clock = std::chrono::steady_clock;

		constexpr const char* NAME = "F4MPTalkHud";
		constexpr const char* MOVIE = "Interface/F4Multiplayer/TalkHud.swf";
		constexpr auto        SHOW_EVERY = std::chrono::seconds(2);  // the engine rebuilds menus after a load: asked again, not at once

		struct State
		{
			bool              on = false;
			VoiceLevel::Look  look;
			std::uint32_t     color = 0;
			bool              operator==(const State&) const = default;
		};

		std::mutex           stateLock;
		std::optional<State> nextState;  // under stateLock
		std::atomic<std::uint32_t> applied{ 0 };
		std::atomic<int>     alive{ 0 };
		std::atomic<bool>    noMovie{ false };  // it could not be loaded: not asked for again this run
		std::atomic<bool>    fresh{ false };    // a new movie: it knows nothing yet

		State             lastState;  // main thread
		bool              asked = false;
		Clock::time_point nextShow{};

		class Panel final : public RE::GameMenuBase
		{
		public:
			Panel()
			{
				++alive;
				// Beside the game's HUD: no pause, cursor, input or menu context, as the developer HUD.
				menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving, RE::UI_MENU_FLAGS::kRequiresUpdate, RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu,
					RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);
				depthPriority = RE::UI_DEPTH_PRIORITY::kHUD;
				const auto scaleform = RE::BSScaleformManager::GetSingleton();
				if (!scaleform || !scaleform->LoadMovieEx(*this, MOVIE, "root")) {
					REX::ERROR("UiTalkHud: {} could not be loaded; is it in Data/Interface?", MOVIE);
					noMovie = true;
				}
				fresh = true;
			}

			~Panel() override
			{
				--alive;
			}

			// Flash objects are written only here. Nothing is written while the UI is held (UiHold.h).
			void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
			{
				if (uiMovie && Hud::IsSettled()) {
					std::optional<State> state;
					{
						std::scoped_lock l{ stateLock };
						state.swap(nextState);
					}
					if (state) {
						const std::array<GValue, 4> args{ GValue(state->on), GValue(static_cast<double>(state->look.inner)),
							GValue(static_cast<double>(state->look.outer)), GValue(static_cast<double>(state->color)) };
						if (menuObj.Invoke("SetState", nullptr, args.data(), args.size())) {
							++applied;
						} else {
							REX::WARN("UiTalkHud: the movie has no SetState");
						}
					}
				}
				GameMenuBase::AdvanceMovie(a_timeDelta, a_time);
			}
		};

		RE::IMenu* Create(const RE::UIMessage&)
		{
			return new Panel();
		}

		void Message(RE::UI_MESSAGE_TYPE a_type)
		{
			if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(NAME, a_type);
			}
		}

		// Whether our menu exists, kept by the menu itself. Asking the UI would take a reference to it, and could be what
		// destroys a closing menu on this thread (OpenMenus.h).
		bool IsOpen()
		{
			return alive.load() > 0;
		}
	}

	void Register()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterMenu(NAME, &Create);
		}
	}

	void Frame()
	{
		if (noMovie || !Hud::IsSettled()) {
			return;
		}
		const auto status = Session::GetStatus();
		Voice::SetMeterAlone(Config::Get().talkHud && status.hosting);  // the icon works before anyone has joined
		const auto talk = Voice::TalkState();
		if (!Config::Get().talkHud || !talk.mayTalk || !status.inWorld) {
			if (asked) {
				asked = false;
				Message(RE::UI_MESSAGE_TYPE::kHide);
			}
			return;
		}
		if (!IsOpen()) {
			const auto now = Clock::now();
			if (now >= nextShow) {
				nextShow = now + SHOW_EVERY;
				asked = true;
				Message(RE::UI_MESSAGE_TYPE::kShow);
			}
			return;
		}
		const State state{ talk.talking, talk.look, UiPanel::HudColor() };
		if (state != lastState || fresh.exchange(false)) {
			lastState = state;
			std::scoped_lock l{ stateLock };
			nextState = state;
		}
	}

	std::string Describe()
	{
		const auto talk = Voice::TalkState();
		return std::format("on={} mayTalk={} talking={} open={} applied={} noMovie={} inner={:.2f} outer={:.2f}", Config::Get().talkHud, talk.mayTalk, talk.talking, IsOpen(),
			applied.load(), noMovie.load(), talk.look.inner, talk.look.outer);
	}
}
