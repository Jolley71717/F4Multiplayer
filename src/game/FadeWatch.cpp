#include "game/FadeWatch.h"

#include "game/FadeGuard.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/Papyrus.h"
#include "game/Story.h"

namespace FadeWatch
{
	namespace
	{
		using Clock = FadeGuard::Clock;
		using Args = RE::BSScrapArray<RE::BSScript::Variable>;

		FadeGuard::Guard                 guard;
		std::string                      lastWhat;  // what the session last did that armed the guard
		std::optional<Clock::time_point> lastAt;

		std::string Seconds(Clock::duration a_duration)
		{
			return std::format("{:.1f}", std::chrono::duration<float>(a_duration).count());
		}

		std::string SinceArmed(Clock::time_point a_now)
		{
			if (!lastAt) {
				return "the session has set no stage here nor moved the player";
			}
			return std::format("the session last {} {} s ago", lastWhat, Seconds(a_now - *lastAt));
		}

		// Game.FadeOutGame(abFadingOut, abBlackFade, afSecsBeforeFade, afFadeDuration, abStayFaded).
		bool FadeOutGame(bool a_fadingOut, bool a_stayFaded)
		{
			return Papyrus::CallStatic("Game", "FadeOutGame", [&](Args& a_out) {
				a_out = Args{ 5 };
				a_out[0] = a_fadingOut;
				a_out[1] = true;
				a_out[2] = 0.0f;
				a_out[3] = 1.0f;
				a_out[4] = a_stayFaded;
			});
		}
	}

	void Arm(std::string_view a_what)
	{
		guard.Arm();
		lastWhat = a_what;
		lastAt = Clock::now();
	}

	void Frame()
	{
		const auto now = Clock::now();
		// Play: not a loading screen (nor the moment after one, Hud::IsSettled) nor a menu that pauses the game, and out
		// of the opening, whose own scripts hold the screen black on the way out of the vault (the lift up) until a
		// trigger at the top fades it back in.
		FadeGuard::Screen screen;
		screen.faded = OpenMenus::Open(OpenMenus::Menu::kFader);
		screen.paused = Hud::LoadingScreenUp() || !Hud::IsSettled() || OpenMenus::AnyOpen({ OpenMenus::Menu::kPause, OpenMenus::Menu::kPipboy, OpenMenus::Menu::kConsole }) ||
		                Story::InOpening();
		switch (guard.Step(now, screen)) {
		case FadeGuard::Act::kFadeIn:
			REX::WARN("FadeWatch: the screen has been faded out for {} s of play, since soon after {}; fading it back in (try {} of {})", Seconds(guard.Up()),
				SinceArmed(now), guard.Tries(), FadeGuard::MAX_TRIES);
			if (!FadeIn()) {
				REX::WARN("FadeWatch: the fade-in couldn't be queued (no script VM)");
			}
			break;
		case FadeGuard::Act::kLongFade:
			REX::WARN("FadeWatch: the screen has been faded out for {} s of play, and not after anything of the session's ({}); left to the game", Seconds(guard.Up()),
				SinceArmed(now));
			break;
		case FadeGuard::Act::kCleared:
			REX::INFO("FadeWatch: the screen faded back in after {} s of play{}", Seconds(guard.LastUp()),
				guard.LastCaused() ? (guard.LastTries() ? std::format(" (the session's fade; faded in by the guard {} time(s))", guard.LastTries()) : std::string(" (the session's fade)")) : std::string());
			break;
		case FadeGuard::Act::kNone:
			break;
		}
	}

	bool FadeIn()
	{
		return FadeOutGame(false, false);
	}

	bool FadeOutAndStay()
	{
		return FadeOutGame(true, true);
	}

	std::string Describe()
	{
		return std::format("fade: {}{} armed={} fadedIn={} longFades={}", guard.Faded() ? "up " : "down", guard.Faded() ? std::format("{}s{}", Seconds(guard.Up()), guard.Caused() ? " session's" : "") : std::string(),
			guard.Armed(), guard.FadedIn(), guard.LongFades());
	}
}
