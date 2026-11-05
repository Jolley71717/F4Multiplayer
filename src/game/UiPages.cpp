#include "game/UiPages.h"

#include "Config.h"
#include "game/HostRules.h"
#include "game/Hotkeys.h"
#include "game/Hud.h"
#include "game/UiKeyNames.h"
#include "game/UiPasswordEntry.h"
#include "game/UiReleaseNotes.h"
#include "game/UiPanel.h"
#include "game/OpenMenus.h"
#include "game/Party.h"
#include "game/RemotePlayers.h"
#include "net/Session.h"
#include "Platform.h"
#include "steam/Steam.h"
#include "steam/SteamTransport.h"
#include "Text.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

namespace UiPages
{
	namespace
	{
		constexpr std::size_t MAX_BUTTONS = 4;  // what MessageMenuManager::Create takes
		constexpr auto        HEADER = "F4Multiplayer";

		// A button and what it does. The action runs on the main thread in Frame, after the box has
		// closed; an empty action just closes the menu.
		struct Button
		{
			std::string           label;
			std::function<void()> action;
			std::string           hint = {};  // the panel shows it while the button is chosen; a box has only its body
			std::string           value = {};  // the panel shows it in a column of its own on the row (a key)
			bool                  listed = false;  // not one of the rows: the panel's list of players presses it (ShowMainPage)
		};

		struct ButtonPress
		{
			std::uint32_t serial;
			std::uint32_t button;
		};

		// The engine calls this with the index of the button pressed, from whichever thread runs the
		// menu; the press waits here for Frame. The engine owns the object (reference counted with
		// the box's data), and a serial tells a stale box's press apart from the current page's.
		class Callback final : public RE::IMessageBoxCallback
		{
		public:
			explicit Callback(std::uint32_t a_serial) :
				serial(a_serial)
			{}

			void operator()(std::uint8_t a_button) override
			{
				std::scoped_lock l{ pressedLock };
				pressed = ButtonPress{ serial, a_button };
			}

			static inline std::mutex           pressedLock;
			static inline std::optional<ButtonPress> pressed;

		private:
			std::uint32_t serial;
		};

		std::uint32_t       serial = 0;
		std::vector<Button> buttons;  // the page showing, by button index
		std::uint32_t       pagesShown = 0;
		std::uint32_t       pressesHandled = 0;
		std::string         notice;    // shown once at the top of the main page (why hosting failed)
		std::string         lastBody;  // the page showing, for Describe
		// The menu key was pressed while the UI was held (a loading screen, a save or load, the moments after; see
		// UiHold): the box is one of the game's menus, and is shown once the UI has settled, from Frame.
		bool                openWaiting = false;
		bool                keyWaiting = false;  // the same for the panel, asked for with the menu key
		bool                inPanel = false;  // the pages go to the panel (MPMenu) rather than to message boxes
		std::string         typedPassword;           // the password page's text so far
		bool                typingPassword = false;  // that page is showing, with the keys going to it
		UiPanel::Page        nextPage;         // what the next page has besides its text and rows, in the panel: Show takes it, panel or not

		// "Join a friend" first asks Steam for the friends' rich presence (a host's names their lobby,
		// and Steam hands it out only once asked), then shows the page when the answers are in or
		// after a moment. Nothing is on screen meanwhile, so the moment is short.
		using Clock = std::chrono::steady_clock;
		constexpr auto PRESENCE_WAIT = 1500ms;
		struct PendingJoin
		{
			std::size_t       page;
			Clock::time_point deadline;
			std::uint32_t     updatesBefore;
			std::size_t       asked;
		};
		std::optional<PendingJoin> pendingJoin;

		// Host controls: a command goes to our own server, and the page after it waits for the server's answer (a new
		// Session::HostControls) or a moment, so it shows what is true. a_done hears the answer (a HUD line saying what
		// happened); nothing is on screen meanwhile, as for the join page.
		constexpr auto HOST_WAIT = 1000ms;
		struct PendingHost
		{
			std::function<void()>                          show;
			std::function<void(const Protocol::HostInfo&)> done;
			Clock::time_point                              deadline;
			std::uint32_t                                  serial;
		};
		std::optional<PendingHost> pendingHost;
		// Players just kicked or banned: their leaving comes a moment after the server's answer, so the player list
		// leaves them out meanwhile. Forgotten when the host controls are opened again from the main page.
		std::vector<std::uint32_t> justRemoved;

		// Names and reject reasons come from other players: the box's text field reads markup, so
		// '<' and '>' and control characters (other than line breaks) are dropped, as the HUD does.
		std::string Clean(std::string_view a_text)
		{
			std::string clean;
			clean.reserve(a_text.size());
			for (const unsigned char c : a_text) {
				if ((c >= 0x20 && c != 0x7F && c != '<' && c != '>') || c == '\n') {
					clean.push_back(static_cast<char>(c));
				}
			}
			return clean;
		}

		bool mainShowing = false;  // the main page is the one showing
		int listeningFor = -1;     // the Keybinds row waiting for its key
		bool modsShowing = false;  // the Mods page is the one showing, in the panel
		std::string modsBody;      // what it says, to redraw when the host's answer changes that
		Clock::time_point nextModsCheck{};

		void Show(const std::string& a_body, std::vector<Button> a_buttons)
		{
			auto page = std::exchange(nextPage, {});
			mainShowing = false;  // ShowMainPage says so after this
			listeningFor = -1;    // ShowKeybinds says so after this
			modsShowing = false;  // ShowMods says so after this
			// The panel: no limit on buttons, and the first, the page's way back (see ShowPaged), is Esc rather than a row.
			if (inPanel) {
				buttons = std::move(a_buttons);
				std::vector<std::string> rows;
				std::vector<std::string> hints;
				for (std::size_t i = 0; i < buttons.size(); ++i) {
					buttons[i].label = Clean(buttons[i].label);
					if (i > 0 && !buttons[i].listed) {
						// Clean leaves no tab in either, so the movie can split them.
						rows.push_back(buttons[i].value.empty() ? buttons[i].label : buttons[i].label + "\t" + Clean(buttons[i].value));
						hints.push_back(Clean(buttons[i].hint));
					}
				}
				++serial;
				++pagesShown;
				lastBody = Clean(a_body);
				page.serial = serial;
				page.body = lastBody;
				page.rows = std::move(rows);
				page.hints = std::move(hints);
				UiPanel::SetPage(std::move(page));
				UiPanel::CaptureKey(false);
				UiPanel::CaptureText(false);
				typingPassword = false;
				return;
			}
			const auto manager = RE::MessageMenuManager::GetSingleton();
			if (!manager) {
				Hud::Notify(Text::Get("menu.unavailable"));
				return;
			}
			if (a_buttons.size() > MAX_BUTTONS) {
				a_buttons.resize(MAX_BUTTONS);
			}
			buttons = std::move(a_buttons);
			for (auto& button : buttons) {
				button.label = Clean(button.label);  // friends' names end up on buttons
			}
			++serial;
			++pagesShown;
			const auto label = [&](std::size_t a_index) -> const char* { return a_index < buttons.size() ? buttons[a_index].label.c_str() : nullptr; };
			const auto body = Clean(a_body);
			lastBody = body;
			// The game's own warning dialog, with our text and buttons. It copies the strings and keeps
			// the callback alive for as long as the box exists.
			manager->Create(HEADER, body.c_str(), new Callback(serial), RE::WARNING_TYPES::kDefault, label(0), label(1), label(2), label(3));
		}

		// The game's message box over the panel, with one button that just closes it: the serial no page ever has, so no press.
		void ShowMain();

		// Over the page showing, in the panel; in a box it's the page itself, the title dropped. The first of a_buttons is the way back.
		void ShowDialog(std::string a_title, const std::string& a_text, std::vector<Button> a_buttons)
		{
			nextPage.dialog = true;
			nextPage.title = Clean(a_title);
			Show(a_text, std::move(a_buttons));
		}

		void Popup(const std::string& a_text)
		{
			if (inPanel) {
				const auto back = [] {
					nextPage.keepRow = true;
					ShowMain();
				};
				ShowDialog(HEADER, a_text, { { Text::Get("menu.back"), back }, { Text::Get("menu.close"), back } });
				return;
			}
			if (const auto manager = RE::MessageMenuManager::GetSingleton()) {
				const auto text = Clean(a_text);
				const auto close = Text::Get("menu.close");
				manager->Create(HEADER, text.c_str(), new Callback(0), RE::WARNING_TYPES::kDefault, close.c_str(), nullptr, nullptr, nullptr);
			}
		}

		void ShowAbout();
		void ShowKeybinds(int a_listening, std::string a_said);
		void ShowJoin(std::size_t a_page);
		void ShowSettings(std::size_t a_page);

		// The way to the join page: ask Steam about the friends first (see PendingJoin).
		void RequestJoin(std::size_t a_page)
		{
			const auto asked = Steam::RefreshFriends();
			if (asked == 0) {
				ShowJoin(a_page);  // nobody online, or no Steam: nothing to wait for
				return;
			}
			// The panel shows the wait; a box can't, and nothing is on screen for the moment.
			if (inPanel) {
				Show(Text::Get("menu.searching"), { { Text::Get("menu.back"), [] {
														 pendingJoin.reset();
														 ShowMain();
													 } } });
			}
			pendingJoin = PendingJoin{ a_page, Clock::now() + PRESENCE_WAIT, Steam::PresenceUpdates(), asked };
		}

		// The first button of every page is the harmless one (Back or Close): the game's back key, Tab,
		// presses the first button of a message box, and a player who leaves this menu the way they
		// leave every other menu must not host, leave or stop hosting by accident (two players did).
		// A list longer than a page: two entries per page with "More" to turn it (three when they all
		// fit on one), and "Back" first. a_page wraps around.
		void ShowPaged(const std::string& a_body, std::vector<Button> a_entries, std::size_t a_page, void (*a_turn)(std::size_t), Button a_back)
		{
			if (inPanel) {  // its list scrolls: every entry on the one page
				a_entries.insert(a_entries.begin(), std::move(a_back));
				Show(a_body, std::move(a_entries));
				return;
			}
			const std::size_t perPage = a_entries.size() > MAX_BUTTONS - 1 ? MAX_BUTTONS - 2 : MAX_BUTTONS - 1;
			const std::size_t pages = (std::max<std::size_t>)(1, (a_entries.size() + perPage - 1) / perPage);
			const std::size_t page = a_page % pages;
			std::vector<Button> shown;
			shown.push_back(std::move(a_back));
			for (std::size_t i = page * perPage; i < a_entries.size() && i < (page + 1) * perPage; ++i) {
				shown.push_back(std::move(a_entries[i]));
			}
			if (pages > 1) {
				shown.push_back({ Text::Format("menu.more", { { "page", page + 1 }, { "pages", pages } }), [=] { a_turn(page + 1); } });
			}
			Show(a_body, std::move(shown));
		}

		std::string OnOff(bool a_on)
		{
			return Text::Get(a_on ? "menu.on" : "menu.off");
		}

		// "Sam's game (Steam)" or "192.168.1.5:7779".
		std::string DescribeAddress(const std::string& a_address)
		{
			if (const auto id = SteamTransport::ParseAddress(a_address)) {
				const auto name = Steam::NameOf(id);
				return name.empty() ? Text::Format("menu.address_steam_unnamed", { { "id", id } }) : Text::Format("menu.address_steam_named", { { "player", name } });
			}
			return a_address;
		}

		std::string StatusBody()
		{
			const auto& settings = Config::Get();
			const auto  status = Session::GetStatus();
			std::string body;
			if (status.hosting) {
				if (!status.steam) {
					body += Text::Format(settings.transport == "enet" ? "menu.hosting_udp" : "menu.hosting_udp_nosteam", { { "port", settings.port } });
				} else if (Steam::LobbyOpen()) {
					body += Text::Get("menu.hosting_steam");
				} else {
					body += Text::Get("menu.hosting_steam_opening");
				}
				if (status.link != Session::Status::Link::kJoined) {
					body += "\n" + Text::Get("menu.own_game_connecting");
				}
			} else {
				switch (status.link) {
				case Session::Status::Link::kNone:
					body += Text::Get(status.hostWaiting ? "menu.host_waiting" : "menu.not_in_session");
					break;
				case Session::Status::Link::kConnecting:
					body += Text::Format(status.inWorld ? "menu.connecting" : "menu.join_waiting", { { "address", DescribeAddress(status.address) } });
					if (!status.rejectReason.empty()) {
						body += "\n" + Text::Format(status.retrySoon ? "menu.refused_soon" : "menu.refused_later", { { "reason", Text::Reject(status.rejectReason) } });
					}
					break;
				case Session::Status::Link::kConnected:
					body += Text::Format("menu.connected", { { "address", DescribeAddress(status.address) } });
					break;
				case Session::Status::Link::kJoined:
					body += Text::Format("menu.in_session", { { "address", DescribeAddress(status.address) } });
					if (const auto ping = Party::RoundTripMs()) {
						body += " (" + Text::Format("party.ping_info", { { "ms", ping } }) + ")";
					}
					break;
				}
			}
			// The panel lists them beside its rows (PanelPlayersList), and keeps this to a line.
			const auto players = Party::PlayerLines();
			if (status.link == Session::Status::Link::kJoined && !inPanel) {
				body += players.empty() ? "\n" + Text::Get("menu.nobody_else") : "\n" + Text::Format("menu.with_you", { { "n", players.size() } });
				for (const auto& line : players) {
					body += "\n" + line;
				}
				// The host's rules for being down, when they are not the defaults.
				if (const auto rules = HostRules::Summary(HostRules::Get()); HostRules::InSession() && !rules.empty()) {
					body += "\n" + rules;
				}
			}
			return body;
		}

		// The list beside the main page's rows, while in a session: the players, or that there are none, and the host's rules.
		std::vector<std::string> PanelPlayersList()
		{
			std::vector<std::string> rows;
			if (Session::GetStatus().link != Session::Status::Link::kJoined) {
				return rows;
			}
			rows = Party::PanelPlayers();
			if (rows.empty()) {
				rows.push_back(Text::Get("menu.nobody_else"));
			}
			if (const auto rules = HostRules::Summary(HostRules::Get()); HostRules::InSession() && !rules.empty()) {
				rows.push_back(rules);
			}
			return rows;
		}

		// What the main page says, to redraw it when that changes.
		std::string MainPageNow()
		{
			auto now = StatusBody();
			for (const auto& row : PanelPlayersList()) {
				now += "\n" + row;
			}
			return now;
		}

		void Host()
		{
			std::string failed;
			if (const auto error = Session::StartHosting(); !error.empty()) {
				failed = Text::Format("menu.host_failed", { { "error", error } });
				if (!inPanel) {
					notice = failed;
				}
			} else {
				// As bHost would at startup, and remembered: next time the game starts, it hosts again.
				Config::Mutable().host = true;
				Config::Write("bHost", "true");
			}
			nextPage.keepRow = inPanel;
			ShowMain();
			if (inPanel && !failed.empty()) {
				Popup(failed);  // the panel's text is one line of status
			}
		}

		void StopHosting()
		{
			Session::StopHosting();
			Config::Mutable().host = false;
			Config::Write("bHost", "false");
			ShowMain();
		}

		// Our mods compared with the host's (Session::HostMods), a page of names at a time (ModDiff::Pages). "Back"
		// first; "Ask the host again" while in the session; "More" to turn the page.
		void ShowMods(std::size_t a_page)
		{
			const auto diff = Session::HostMods();
			if (!diff) {
				ShowMain();  // gone meanwhile (another server, or let in with the host's load order)
				return;
			}
			// The panel scrolls: everything on one page, and asking the host again stays on it until the answer redraws it.
			constexpr std::size_t ALL_NAMES = 1'000'000;
			const auto  pages = inPanel ? ModDiff::Pages(*diff, ALL_NAMES) : ModDiff::Pages(*diff);
			const auto  page = a_page % pages.size();
			std::vector<Button> choices;
			choices.push_back({ Text::Get("menu.back"), ShowMain });
			if (Session::GetStatus().link == Session::Status::Link::kJoined && !Session::GetStatus().hosting) {
				choices.push_back({ Text::Get("menu.mods_ask"), [page] {
									   Session::AskHostMods();
									   if (inPanel) {
										   nextPage.keepRow = true;
										   ShowMods(page);
									   }
								   } });
			}
			if (pages.size() > 1) {
				choices.push_back({ Text::Format("menu.more", { { "page", page + 1 }, { "pages", pages.size() } }), [=] { ShowMods(page + 1); } });
			}
			Show(pages[page], std::move(choices));
			modsShowing = inPanel;
			modsBody = pages[page];
		}

		// --- Host controls (while hosting): kick, ban and unban players, lock the session, set or remove the password. Each
		// goes to our own server (Session::SendHostCommand), which takes them from the host's game only.

		void ShowHostControls(std::size_t a_page);
		void ShowPasswordPage();
		void ShowHostPlayers(std::size_t a_page);
		void ShowBans(std::size_t a_page);

		// Sends a_command, then shows a_show once the server has answered (a_done hears the answer first), or after a
		// moment without one. Not hosting any more: the main page.
		void HostDo(const Protocol::HostCommand& a_command, std::function<void()> a_show, std::function<void(const Protocol::HostInfo&)> a_done = {})
		{
			if (!Session::SendHostCommand(a_command)) {
				ShowMain();
				return;
			}
			pendingHost = PendingHost{ std::move(a_show), std::move(a_done), Clock::now() + HOST_WAIT, Session::HostControlsSerial() };
		}

		// What the HUD says of a command's answer: a_doneKey (with a_args) when it went through, else why not.
		void SayResult(const Protocol::HostInfo& a_info, const char* a_doneKey, std::initializer_list<Text::Arg> a_args)
		{
			switch (a_info.result) {
			case Protocol::HostResult::kDone:
				if (a_doneKey) {
					Hud::Notify(Text::Format(a_doneKey, a_args));
				}
				break;
			case Protocol::HostResult::kNotSaved:
				Hud::Notify(Text::Get("menu.hc_not_saved"));
				break;
			case Protocol::HostResult::kNoIdentity:
				Hud::Notify(Text::Format("menu.hc_no_identity", a_args));
				break;
			case Protocol::HostResult::kNoSuchPlayer:
				Hud::Notify(Text::Get("menu.hc_gone"));
				break;
			default:
				break;
			}
		}

		// "Password: k7m2qp9x ..." or "No password ...": the password is the ini's (the server only says one is set).
		std::string PasswordLine(const Protocol::HostInfo& a_info)
		{
			const auto& password = Config::Get().password;
			if (!a_info.password) {
				return Text::Get("menu.hc_no_password");
			}
			if (inPanel) {
				return Text::Get("menu.pw_line");  // shown only when asked for (ShowPasswordPage)
			}
			return Text::Format("menu.hc_password", { { "password", password.empty() ? std::string{ "?" } : password } });
		}

		// A made-up password (the box can't take typing): eight letters and digits that can't be mistaken for each other.
		std::string NewPassword()
		{
			static constexpr std::string_view LETTERS = "abcdefghjkmnpqrstuvwxyz23456789";
			std::random_device                rd;
			std::string                       password;
			for (int i = 0; i < 8; ++i) {
				password += LETTERS[rd() % LETTERS.size()];
			}
			return password;
		}

		// The password the host chose, for the ini (read at the next start) and the server (for the next join).
		void SetPassword(const std::string& a_password)
		{
			Config::Mutable().password = a_password;
			Config::Write("sPassword", a_password);
			HostDo({ Protocol::HostAction::kSetPassword, 0, 0, a_password }, [] {
				if (inPanel) {
					ShowPasswordPage();
				} else {
					ShowHostControls(0);
				}
			});
		}

		// Back to where the password page was opened from.
		void LeavePasswordPage()
		{
			if (Session::GetStatus().hosting) {
				ShowHostControls(0);
			} else {
				nextPage.keepRow = true;
				ShowSettings(0);
			}
		}

		void SavePassword(const std::string& a_password)
		{
			if (Session::GetStatus().hosting) {
				SetPassword(a_password);
			} else {
				Config::Mutable().password = a_password;
				Config::Write("sPassword", a_password);
				ShowPasswordPage();
			}
		}

		// The dialog to type one in: what's typed shows as it comes.
		void ShowPasswordInput()
		{
			std::vector<Button> rows{ { Text::Get("menu.back"), ShowPasswordPage } };
			rows.push_back({ Text::Get("menu.pw_save"), [] {
								if (typedPassword.empty()) {
									nextPage.keepRow = true;
									ShowPasswordInput();
								} else {
									SavePassword(typedPassword);
								}
							} });
			// For a gamepad, which can't type: it goes in the box, and the row chosen goes back to Save.
			rows.push_back({ Text::Get("menu.pw_make_up"), [] {
								typedPassword = NewPassword();
								ShowPasswordInput();
							} });
			nextPage.input = true;
			nextPage.inputText = typedPassword;
			ShowDialog(Text::Get("menu.hc_password_button"), Text::Get("menu.pw_save_hint"), std::move(rows));
			typingPassword = true;
			UiPanel::CaptureText(true);
		}

		// The password is for hosting and for joining alike: the ini has one.
		void ShowPasswordPage()
		{
			const bool          any = !Config::Get().password.empty();
			std::vector<Button> rows{ { Text::Get("menu.back"), LeavePasswordPage } };
			if (any) {
				rows.push_back({ Text::Get("menu.pw_view"), [] {
									ShowDialog(Text::Get("menu.hc_password_button"), Config::Get().password,
										{ { Text::Get("menu.back"), ShowPasswordPage }, { Text::Get("menu.close"), ShowPasswordPage } });
									lastBody = {};  // not for the dev channel's report (Describe)
								},
					Text::Get("menu.pw_view_hint") });
			}
			rows.push_back({ Text::Get(any ? "menu.pw_change" : "menu.pw_set"), [] {
								typedPassword.clear();
								ShowPasswordInput();
							},
				Text::Get("menu.pw_setting_hint") });
			if (any) {
				rows.push_back({ Text::Get("menu.pw_remove"), [] {
									ShowDialog(Text::Get("menu.pw_remove"), Text::Get("menu.pw_remove_ask"),
										{ { Text::Get("menu.back"), ShowPasswordPage }, { Text::Get("menu.pw_remove"), [] { SavePassword({}); } }, { Text::Get("menu.back"), ShowPasswordPage } });
								},
					Text::Get("menu.hc_no_password") });
			}
			Show({}, std::move(rows));
		}

		void ShowHostPassword()
		{
			if (inPanel) {
				ShowPasswordPage();
				return;
			}
			const auto info = Session::HostControls();
			if (!info) {
				ShowHostControls(0);
				return;
			}
			const auto body = PasswordLine(*info) + "\n" + Text::Get("menu.hc_password_page") + "\n" + Text::Get("menu.settings_saved");
			std::vector<Button> choices{ { Text::Get("menu.back"), [] { ShowHostControls(0); } }, { Text::Get("menu.hc_new_password"), [] { SetPassword(NewPassword()); } } };
			if (info->password) {
				choices.push_back({ Text::Get("menu.hc_remove_password"), [] { SetPassword({}); } });
			}
			Show(body, std::move(choices));
		}

		// The action that kicks or bans a player, then shows a_after.
		std::function<void()> RemovePlayer(std::uint32_t a_id, const std::string& a_name, bool a_ban, std::function<void()> a_after)
		{
			return [a_id, a_name, a_ban, a_after] {
				HostDo({ a_ban ? Protocol::HostAction::kBan : Protocol::HostAction::kKick, a_id }, a_after,
					[a_id, a_name, a_ban](const Protocol::HostInfo& a_info) {
						if (a_info.result == Protocol::HostResult::kDone || a_info.result == Protocol::HostResult::kNotSaved ||
							a_info.result == Protocol::HostResult::kNoIdentity) {
							justRemoved.push_back(a_id);
						}
						SayResult(a_info, a_ban ? "menu.hc_banned_hud" : "menu.hc_kicked", { { "player", a_name } });
					});
			};
		}

		// A player chosen in the panel's list: what can be done with them.
		void ShowPlayer(std::uint32_t a_id)
		{
			const auto name = RemotePlayers::NameOf(a_id);
			const auto line = Party::PlayerLine(a_id);
			if (name.empty() || line.empty()) {
				ShowMain();  // gone meanwhile
				return;
			}
			auto problem = Party::TeleportProblem();
			std::vector<Button> rows{ { Text::Get("menu.back"), ShowMain },
				{ Text::Format("menu.teleport_to", { { "player", name } }), [a_id] { Party::TeleportTo(a_id); }, std::move(problem) } };
			if (Session::GetStatus().hosting) {
				// What each does: the Kick or ban page's text, a line each after the player's name.
				const auto explained = Text::Format("menu.hc_player", { { "player", name } });
				const auto kick = explained.find('\n');
				const auto ban = kick == std::string::npos ? std::string::npos : explained.find('\n', kick + 1);
				rows.push_back({ Text::Format("menu.hc_kick", { { "player", name } }), RemovePlayer(a_id, name, false, ShowMain), ban == std::string::npos ? std::string{} : explained.substr(kick + 1, ban - kick - 1) });
				rows.push_back({ Text::Format("menu.hc_ban", { { "player", name } }), RemovePlayer(a_id, name, true, ShowMain), ban == std::string::npos ? std::string{} : explained.substr(ban + 1) });
			}
			nextPage.header = Party::PanelPlayer(a_id);
			Show({}, std::move(rows));
		}

		// One player: kick or ban them.
		void ShowHostPlayer(std::uint32_t a_id)
		{
			const auto name = RemotePlayers::NameOf(a_id);
			if (name.empty() || std::ranges::contains(justRemoved, a_id)) {
				ShowHostPlayers(0);  // gone meanwhile
				return;
			}
			const auto removed = [a_id, name](bool a_ban) { return RemovePlayer(a_id, name, a_ban, [] { ShowHostPlayers(0); }); };
			Show(Text::Format("menu.hc_player", { { "player", name } }),
				{ { Text::Get("menu.back"), [] { ShowHostPlayers(0); } }, { Text::Format("menu.hc_kick", { { "player", name } }), removed(false) },
					{ Text::Format("menu.hc_ban", { { "player", name } }), removed(true) } });
		}

		// Everyone else in the session, a button each.
		void ShowHostPlayers(std::size_t a_page)
		{
			std::string         body = Text::Get("menu.hc_pick_player");
			std::vector<Button> entries;
			for (const auto& player : RemotePlayers::List()) {
				if (std::ranges::contains(justRemoved, player.id)) {
					continue;
				}
				if (!inPanel) {
					body += "\n" + player.name;  // the panel has room for them on its rows alone
				}
				entries.push_back({ player.name, [id = player.id] { ShowHostPlayer(id); } });
			}
			if (entries.empty()) {
				Show(Text::Get("menu.hc_nobody"), { { Text::Get("menu.back"), [] { ShowHostControls(0); } } });
				return;
			}
			ShowPaged(body, std::move(entries), a_page, ShowHostPlayers, { Text::Get("menu.back"), [] { ShowHostControls(0); } });
		}

		// "Sam (~3fa2c1)" while the server that banned them runs, "~3fa2c1, banned 2026-10-18" after.
		std::string BanLabel(const Protocol::HostInfo::Ban& a_ban)
		{
			if (!a_ban.name.empty()) {
				return Text::Format("menu.hc_ban_named", { { "player", a_ban.name }, { "alias", a_ban.alias } });
			}
			const std::chrono::year_month_day day{ std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{ std::chrono::seconds{ a_ban.bannedAt } }) };
			const auto date = std::format("{:04}-{:02}-{:02}", static_cast<int>(day.year()), static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
			return Text::Format("menu.hc_ban_unnamed", { { "alias", a_ban.alias }, { "date", date } });
		}

		// The bans, newest first: pressing one lifts it.
		void ShowBans(std::size_t a_page)
		{
			const auto info = Session::HostControls();
			if (!info) {
				ShowHostControls(0);
				return;
			}
			if (info->list.empty()) {
				Show(Text::Get("menu.hc_no_bans"), { { Text::Get("menu.back"), [] { ShowHostControls(0); } } });
				return;
			}
			std::string         body = Text::Format("menu.hc_bans_page", { { "n", info->bans } });
			std::vector<Button> entries;
			for (const auto& ban : info->list) {
				const auto label = BanLabel(ban);
				if (!inPanel) {
					body += "\n" + label;  // the panel has room for them on its rows alone
				}
				entries.push_back({ Text::Format("menu.hc_unban", { { "who", label } }), [id = ban.id, label] {
									   HostDo({ Protocol::HostAction::kUnban, 0, id }, [] { ShowBans(0); },
										   [label](const Protocol::HostInfo& a_info) { SayResult(a_info, "menu.hc_unbanned", { { "who", label } }); });
								   } });
			}
			if (info->list.size() > 1) {
				entries.push_back({ Text::Get("menu.hc_unban_all"), [] {
									   HostDo({ Protocol::HostAction::kUnban, 0, 0 }, [] { ShowHostControls(0); },
										   [](const Protocol::HostInfo& a_info) { SayResult(a_info, nullptr, {}); });
								   } });
			}
			ShowPaged(body, std::move(entries), a_page, ShowBans, { Text::Get("menu.back"), [] { ShowHostControls(0); } });
		}

		// The host controls' first page: the session's lock, password and bans, and a button for each.
		void ShowHostControls(std::size_t a_page)
		{
			if (!Session::GetStatus().hosting) {
				ShowMain();
				return;
			}
			const auto info = Session::HostControls();
			if (!info) {
				// Not heard from our server yet (our own game still joining it): asked again, and said so.
				Session::AskHostControls();
				Show(Text::Get("menu.hc_waiting"), { { Text::Get("menu.back"), ShowMain }, { Text::Get("menu.look_again"), [] { ShowHostControls(0); } } });
				return;
			}
			std::size_t others = 0;
			for (const auto& player : RemotePlayers::List()) {
				others += !std::ranges::contains(justRemoved, player.id);
			}
			// In the panel, what each row is about shows beside it when chosen.
			const auto lockLine = Text::Get(info->locked ? "menu.hc_locked" : "menu.hc_open");
			const auto bansLine = Text::Format("menu.hc_banned", { { "n", info->bans } });
			const auto body = inPanel ? std::string{} : lockLine + "\n" + PasswordLine(*info) + "\n" + bansLine;
			std::vector<Button> entries;
			entries.push_back({ Text::Format("menu.hc_players", { { "n", others } }), [] { ShowHostPlayers(0); }, Text::Get("menu.hc_pick_player") });
			entries.push_back({ Text::Get(info->locked ? "menu.hc_unlock" : "menu.hc_lock"), [locked = info->locked, a_page] {
								   HostDo({ locked ? Protocol::HostAction::kUnlock : Protocol::HostAction::kLock }, [a_page] {
									   nextPage.keepRow = true;
									   ShowHostControls(a_page);
								   });
							   },
				lockLine });
			entries.push_back({ Text::Get("menu.hc_password_button"), ShowHostPassword, PasswordLine(*info) });
			entries.push_back({ Text::Format("menu.hc_bans", { { "n", info->bans } }), [] { ShowBans(0); }, bansLine });
			ShowPaged(body, std::move(entries), a_page, ShowHostControls, { Text::Get("menu.back"), ShowMain });
		}

		void ShowMainPage(std::size_t a_page);

		void ShowMain()
		{
			ShowMainPage(0);
		}

		// More buttons than a box has (the Mods button, when our mods differ from the host's, besides Leave, Join
		// someone else and Settings) turn into pages, "Close" first on each.
		// What the mod is, from a file of ours, with its name and version over it.
		// The latest release notes, as packaged beside the about text, as one markdown text; "" if there are none.
		std::string Changelog()
		{
			constexpr std::size_t VERSIONS_SHOWN = 10;
			const auto            folder = Platform::GamePath("Data/F4SE/Plugins/F4Multiplayer/changelog");
			std::vector<std::string> files;
			std::error_code          error;
			for (const auto& entry : std::filesystem::directory_iterator{ folder, error }) {
				files.push_back(entry.path().filename().string());
			}
			const auto  running = F4SE::GetPluginVersion();
			std::string text;
			for (const auto& file : UiReleaseNotes::Pick(files, { running.major(), running.minor(), running.patch() }, VERSIONS_SHOWN)) {
				std::ifstream in{ folder / file, std::ios::binary };
				text.append(std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{});
				text += "\n\n";
			}
			return text;
		}

		void ShowChangelog()
		{
			nextPage.framed = true;
			nextPage.markdown = true;
			Show(Changelog(), { { Text::Get("menu.back"), ShowAbout } });
		}

		void ShowAbout()
		{
			std::ifstream in{ Platform::GamePath("Data/F4SE/Plugins/F4Multiplayer/about.txt"), std::ios::binary };
			std::string   body{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
			if (body.empty()) {
				body = Text::Get("menu.about_missing");
			}
			nextPage.framed = true;
			std::vector<Button> rows{ { Text::Get("menu.back"), ShowMain } };
			if (inPanel && !Changelog().empty()) {
				rows.push_back({ Text::Get("menu.changelog"), ShowChangelog });
			}
			Show(body, std::move(rows));
		}

		// The main page in the panel stays true: redrawn when what it says changes (the second it's seen, not at once).
		std::size_t       mainPage = 0;
		std::string       mainStatus;
		Clock::time_point nextMainCheck{};

		void ShowMainPage(std::size_t a_page)
		{
			const auto          status = Session::GetStatus();
			const auto          said = StatusBody();
			const auto          body = notice.empty() ? said : std::exchange(notice, {}) + "\n" + said;
			std::vector<Button> entries;
			if (const auto mods = Session::HostMods(); mods && !mods->Empty() && !status.hosting) {
				entries.push_back({ Text::Format("menu.mods", { { "n", mods->Count() } }), [] { ShowMods(0); } });
			}
			if (status.hosting) {
				if (status.steam && Steam::LobbyOpen()) {
					entries.push_back({ Text::Get("menu.invite"), [] { Steam::OpenInviteDialog(); } });
				}
				entries.push_back({ Text::Get("menu.host_controls"), [] {
									   justRemoved.clear();
									   ShowHostControls(0);
								   } });
				entries.push_back({ Text::Get("menu.stop_hosting"), StopHosting });
			} else if (status.hostWaiting) {
				entries.push_back({ Text::Get("menu.stop_hosting"), StopHosting });  // bHost off: it won't start
			} else if (status.link != Session::Status::Link::kNone) {
				entries.push_back({ Text::Get("menu.leave"), [] { Session::Leave(); ShowMain(); } });
				entries.push_back({ Text::Get("menu.join_other"), [] { RequestJoin(0); } });
			} else {
				entries.push_back({ Text::Get("menu.host"), Host });
				entries.push_back({ Text::Get("menu.join"), [] { RequestJoin(0); } });
			}
			entries.push_back({ Text::Get("menu.settings"), [] { ShowSettings(0); } });
			if (inPanel) {
				entries.push_back({ Text::Get("menu.keybinds"), [] { ShowKeybinds(-1, {}); } });
				entries.push_back({ Text::Get("menu.about"), ShowAbout });
			}
			Button close{ Text::Get("menu.close"), {} };  // first: Tab closes the menu (see ShowPaged)
			if (inPanel) {
				nextPage.players = PanelPlayersList();
				// After the rows, in the list's order: the movie presses one by its place past the last row.
				if (status.link == Session::Status::Link::kJoined) {
					for (const auto& player : RemotePlayers::List()) {
						Button row{ player.name, [id = player.id] { ShowPlayer(id); } };
						row.listed = true;
						entries.push_back(std::move(row));
					}
				}
			}
			if (entries.size() + 1 > MAX_BUTTONS) {
				ShowPaged(body, std::move(entries), a_page, ShowMainPage, std::move(close));
			} else {
				entries.insert(entries.begin(), std::move(close));
				Show(body, std::move(entries));
			}
			mainShowing = true;
			mainPage = a_page;
			mainStatus = MainPageNow();
		}

		void ShowJoin(std::size_t a_page)
		{
			const auto& settings = Config::Get();
			const auto  scan = Steam::ScanFriends();
			std::string body;
			std::vector<Button> entries;
			if (!scan.hosting.empty()) {
				body = Text::Get("menu.friends_hosting");
				for (const auto& host : scan.hosting) {
					if (!inPanel) {
						body += "\n" + host.name;  // the panel has room for them on its rows alone
					}
					entries.push_back({ host.name, [host] {
										   Steam::Join(host.lobby);
										   Hud::Notify(Text::Format("menu.joining_host", { { "player", host.name } }));
									   } });
				}
			} else if (!Steam::Available()) {
				body = Text::Get("menu.steam_missing");
			} else {
				// The counts tell a player (and us) where the list stops: no friends online, none
				// in the game, or in the game but not hosting (or hosting with private game details).
				body = Text::Format("menu.none_hosting", { { "online", scan.online }, { "friends", scan.friends }, { "ingame", scan.inGame } });
			}
			// The panel: the first line, with the rest a page of its own, and looking again always there (it has the room).
			std::string help;
			if (inPanel) {
				if (const auto line = body.find('\n'); line != std::string::npos) {
					help = body.substr(line + 1);
					body.resize(line);
				}
			}
			if (!settings.serverAddress.empty()) {
				if (!inPanel) {
					body += "\n" + Text::Format("menu.ini_address", { { "address", settings.serverAddress } });
				}
				entries.push_back({ Text::Format("menu.join_address", { { "address", settings.serverAddress } }), [address = settings.serverAddress] {
									   Session::ConnectTo(address);
									   Hud::Notify(Text::Format("menu.connecting_hud", { { "address", address } }));
								   } });
			}
			if (inPanel) {
				entries.push_back({ Text::Get("menu.look_again"), [] { RequestJoin(0); } });
				if (!help.empty()) {
					entries.push_back({ Text::Get("menu.join_help"), [help, a_page] { Show(help, { { Text::Get("menu.back"), [a_page] { ShowJoin(a_page); } } }); } });
				}
			}
			if (entries.empty()) {
				Show(body, { { Text::Get("menu.back"), ShowMain }, { Text::Get("menu.look_again"), [] { RequestJoin(0); } } });
				return;
			}
			ShowPaged(body, std::move(entries), a_page, ShowJoin, { Text::Get("menu.back"), ShowMain });
		}

		// A setting the menu toggles: shown in the body and on its button, applied to the running
		// game at once and written to the ini.
		struct Toggle
		{
			const char* name;  // the key of its name
			const char* what;  // the key of what "On" means
			bool Config::Settings::*value;
			const char* key;
			const char* onValue;
			const char* offValue;
		};
		constexpr std::array<Toggle, 8> TOGGLES{ {
			{ "menu.friendly_fire", "menu.friendly_fire_what", &Config::Settings::friendlyFire, "bFriendlyFire", "true", "false" },
			{ "menu.shared_story", "menu.shared_story_what", &Config::Settings::sharedStory, "sStory", "shared", "own" },
			{ "menu.safe_quests", "menu.safe_quests_what", &Config::Settings::safeQuests, "sQuests", "safe", "story" },
			{ "menu.voice_chat", "menu.voice_chat_what", &Config::Settings::voiceChat, "bVoiceChat", "true", "false" },
			{ "menu.spoken_lines", "menu.spoken_lines_what", &Config::Settings::spokenLines, "bSpokenLines", "true", "false" },
			{ "menu.compass_markers", "menu.compass_markers_what", &Config::Settings::compassMarkers, "bCompassMarkers", "true", "false" },
			{ "menu.fast_loading", "menu.fast_loading_what", &Config::Settings::fastLoading, "bFastLoading", "true", "false" },
			{ "menu.lend_power_armor", "menu.lend_power_armor_what", &Config::Settings::lendPowerArmor, "bLendPowerArmor", "true", "false" },
		} };

		// --- Keybinds (the panel only: it can hear a key). Hotkeys and Voice read the settings as they go, so a change is
		// live at once, and it goes to the ini as the rest of the settings do.
		struct Keybind
		{
			const char* name;  // the key of its name
			std::uint32_t Config::Settings::*value;
			const char* key;
		};
		constexpr std::array<Keybind, 9> KEYBINDS{ {
			{ "menu.kb_menu", &Config::Settings::keyMenu, "iKeyMenu" },
			{ "menu.kb_list", &Config::Settings::keyPlayerList, "iKeyPlayerList" },
			{ "menu.kb_teleport", &Config::Settings::keyTeleport, "iKeyTeleport" },
			{ "menu.kb_ping", &Config::Settings::keyPing, "iKeyPing" },
			{ "menu.kb_where", &Config::Settings::keyWhere, "iKeyWhere" },
			{ "menu.kb_point", &Config::Settings::keyEmotePoint, "iKeyEmotePoint" },
			{ "menu.kb_cheer", &Config::Settings::keyEmoteCheer, "iKeyEmoteCheer" },
			{ "menu.kb_clap", &Config::Settings::keyEmoteClap, "iKeyEmoteClap" },
			{ "menu.kb_voice", &Config::Settings::keyVoice, "iKeyVoice" },
		} };
		std::string KeyText(std::uint32_t a_vk)
		{
			return a_vk == 0 ? Text::Get("menu.key_off") : UiKeyNames::Name(a_vk) + ")";  // W), as the game's Controls page has it
		}

		// a_listening: the row waiting for a key, or -1. a_said: what happened to the last key, in the status line.
		void ShowKeybinds(int a_listening, std::string a_said)
		{
			const auto&         settings = Config::Get();
			std::vector<Button> rows{ { Text::Get("menu.back"), a_listening < 0 ? std::function<void()>{ ShowMain } : std::function<void()>{ [] {
																   nextPage.keepRow = true;
																   ShowKeybinds(-1, {});
															   } } } };
			for (int i = 0; i < static_cast<int>(KEYBINDS.size()); ++i) {
				const auto& bind = KEYBINDS[i];
				rows.push_back({ Text::Get(bind.name), [i] {
									nextPage.keepRow = true;
									ShowKeybinds(i, {});
								},
					{}, i == a_listening ? Text::Get("menu.key_waiting") : KeyText(settings.*bind.value) });
			}
			Show(a_listening >= 0 ? Text::Get("menu.key_how") : !a_said.empty() ? std::move(a_said) : Text::Get("menu.settings_saved"), std::move(rows));
			listeningFor = a_listening;
			if (a_listening >= 0) {
				UiPanel::CaptureKey(true);
			}
		}

		void ShowSettings(std::size_t a_page)
		{
			const auto& settings = Config::Get();
			std::string body;
			std::vector<Button> entries;
			for (const auto& toggle : TOGGLES) {
				const bool on = settings.*toggle.value;
				const auto name = Text::Get(toggle.name);
				if (!inPanel) {
					body += (body.empty() ? "" : "\n") + Text::Format("menu.setting_line", { { "name", name }, { "state", OnOff(on) }, { "what", Text::Get(toggle.what) } });
				}
				entries.push_back({ Text::Format("menu.setting_button", { { "name", name }, { "state", OnOff(on) } }), [&toggle, a_page] {
									   auto&      s = Config::Mutable();
									   const bool now = !(s.*toggle.value);
									   s.*toggle.value = now;
									   Config::Write(toggle.key, now ? toggle.onValue : toggle.offValue);
									   nextPage.keepRow = true;
									   ShowSettings(a_page);
								   },
					inPanel ? Text::Get(toggle.what) : std::string{} });
			}
			if (inPanel) {
				entries.push_back({ Text::Get("menu.hc_password_button"), ShowPasswordPage, Text::Get("menu.pw_setting_hint") });
			}
			body += (body.empty() ? "" : "\n") + Text::Get("menu.settings_saved");
			ShowPaged(body, std::move(entries), a_page, ShowSettings, { Text::Get("menu.back"), ShowMain });
		}
	}

	void Open()
	{
		if (std::exchange(inPanel, false)) {
			UiPanel::Close();  // the panel could not be shown, or a box was asked for over it
		}
		if (!Hud::IsSettled()) {
			openWaiting = true;  // shown from Frame once the UI has settled
			return;
		}
		pendingHost.reset();  // the menu key again: the main page, not the page a command was waiting to show
		ShowMain();
	}

	void OpenFromKey()
	{
		if (UiPanel::Showing()) {
			return;
		}
		if (!Hud::IsSettled()) {
			keyWaiting = true;  // opened from Frame once the UI has settled
			return;
		}
		UiPanel::OpenCentered();
	}

	void OpenInPanel()
	{
		inPanel = true;
		if (!Hud::IsSettled()) {
			openWaiting = true;
			return;
		}
		pendingHost.reset();
		ShowMain();
	}

	void PanelPress(std::uint32_t a_serial, std::uint32_t a_row)
	{
		std::scoped_lock l{ Callback::pressedLock };
		Callback::pressed = ButtonPress{ a_serial, a_row + 1 };
	}

	void PanelKey(bool a_keyboard, std::uint32_t a_code)
	{
		if (!inPanel || listeningFor < 0 || listeningFor >= static_cast<int>(KEYBINDS.size())) {
			return;
		}
		const auto vk = a_keyboard ? a_code : UiKeyNames::FromMouse(a_code);
		const auto& bind = KEYBINDS[listeningFor];
		REX::INFO("Menu: {} heard {} code {:X}", bind.key, a_keyboard ? "key" : "mouse button", a_code);
		std::string said;
		if (vk == 0 || vk == UiKeyNames::ESCAPE) {
			// nothing to do: a button of no use, or the way out
		} else if (UiKeyNames::Reserved(vk)) {
			said = Text::Format("menu.key_reserved", { { "key", UiKeyNames::Name(vk) } });
		} else {
			auto&      settings = Config::Mutable();
			const auto next = vk == UiKeyNames::DELETE_KEY ? 0u : vk;
			const auto taken = next == 0 ? KEYBINDS.end() : std::ranges::find_if(KEYBINDS, [&](const Keybind& a_other) { return &a_other != &bind && settings.*a_other.value == next; });
			if (taken != KEYBINDS.end()) {
				said = Text::Format("menu.key_taken", { { "key", UiKeyNames::Name(next) }, { "name", Text::Get(taken->name) } });
			} else {
				settings.*bind.value = next;
				Config::Write(bind.key, std::format("{:02X}", next));
			}
		}
		nextPage.keepRow = true;
		ShowKeybinds(-1, std::move(said));
	}

	void PanelTyped(std::uint32_t a_key, std::uint32_t a_character)
	{
		if (!inPanel || !typingPassword) {
			return;
		}
		if (a_key == UiKeyNames::BACKSPACE) {
			if (typedPassword.empty()) {
				return;
			}
			typedPassword.pop_back();
		} else if (!UiPasswordEntry::Add(typedPassword, a_character, Protocol::MAX_PASSWORD_LENGTH)) {
			return;
		}
		nextPage.keepRow = true;
		ShowPasswordInput();
	}

	void PanelBack()
	{
		if (!inPanel) {
			return;
		}
		if (buttons.empty() && !pendingJoin && !pendingHost) {
			inPanel = false;
			UiPanel::Close();
			return;
		}
		std::scoped_lock l{ Callback::pressedLock };
		Callback::pressed = ButtonPress{ serial, 0 };
	}

	void Frame()
	{
		// The panel went (the game closed it for a load, say): the pages with it.
		if (inPanel && !UiPanel::Showing()) {
			REX::INFO("Menu: the panel has gone");
			inPanel = false;
			openWaiting = false;
			buttons.clear();
			pendingJoin.reset();
			pendingHost.reset();
			++serial;
		}
		// No page while the UI is held (the engine is tearing its menus down or building them again): a press
		// made before waits here, as does a pending join and the menu key.
		if (!Hud::IsSettled()) {
			return;
		}
		// In the world the game takes Esc before any menu hears it, so with the panel in the middle of the screen it's read here.
		// It counts the frame after the key comes back up: closed any sooner, the game's own Esc finds no menu in its way and
		// opens the pause menu.
		static bool escWasDown = false;
		static bool escArmed = false;  // went down with the panel open
		static bool escUp = false;     // and has come up
		const bool  escDown = Hotkeys::Held(UiKeyNames::ESCAPE);
		// Not with a message box over the panel: Esc is that box's.
		const bool  panelOpen = inPanel && UiPanel::Centered() && UiPanel::Showing() && !OpenMenus::Open(OpenMenus::Menu::kMessageBox);
		if (std::exchange(escUp, false) && panelOpen) {
			UiPanel::EscPressed();
		}
		if (escDown && !escWasDown) {
			escArmed = panelOpen;
		} else if (!escDown && escWasDown) {
			escUp = std::exchange(escArmed, false);
		}
		escWasDown = escDown;
		if (std::exchange(keyWaiting, false)) {
			UiPanel::OpenCentered();
		}
		if (std::exchange(openWaiting, false)) {
			pendingHost.reset();
			ShowMain();
			return;  // a press from the page that was up before is stale by now
		}
		if (pendingJoin && (Clock::now() >= pendingJoin->deadline || Steam::PresenceUpdates() - pendingJoin->updatesBefore >= pendingJoin->asked)) {
			const auto page = pendingJoin->page;
			pendingJoin.reset();
			ShowJoin(page);
		}
		if (pendingHost) {
			const auto info = Session::HostControls();
			const bool answered = info.has_value() && Session::HostControlsSerial() != pendingHost->serial;
			if (answered || Clock::now() >= pendingHost->deadline) {
				auto pending = std::move(*pendingHost);
				pendingHost.reset();
				if (answered && pending.done) {
					pending.done(*info);
				}
				pending.show();
			}
		}
		std::optional<ButtonPress> press;
		{
			std::scoped_lock l{ Callback::pressedLock };
			press = std::exchange(Callback::pressed, std::nullopt);
		}
		if (!press || press->serial != serial) {
			// Only with no press to take: a redraw makes a new page, and the press would be for the old one.
			if (inPanel && modsShowing && Clock::now() >= nextModsCheck) {
				nextModsCheck = Clock::now() + 1s;
				const auto diff = Session::HostMods();
				constexpr std::size_t ALL_NAMES = 1'000'000;
				if (!diff || ModDiff::Pages(*diff, ALL_NAMES).front() != modsBody) {
					nextPage.keepRow = true;
					ShowMods(0);
				}
			}
			if (inPanel && mainShowing && !pendingJoin && !pendingHost && Clock::now() >= nextMainCheck) {
				nextMainCheck = Clock::now() + 1s;
				if (MainPageNow() != mainStatus) {
					nextPage.keepRow = true;
					ShowMainPage(mainPage);
				}
			}
			return;
		}
		++pressesHandled;
		// The action may show the next page, which replaces the buttons: take ours out first.
		auto pressed = std::exchange(buttons, {});
		const auto shown = serial;
		if (press->button < pressed.size() && pressed[press->button].action) {
			pressed[press->button].action();
		}
		// A box has closed by now and the panel hasn't: closed too if nothing follows (Close, an invite, a join).
		if (inPanel && serial == shown && !pendingJoin && !pendingHost) {
			inPanel = false;
			UiPanel::Close();
		}
	}

	void Press(std::uint32_t a_index)
	{
		std::scoped_lock l{ Callback::pressedLock };
		Callback::pressed = ButtonPress{ serial, a_index };
	}

	std::string Describe()
	{
		std::string labels;
		for (const auto& button : buttons) {
			labels += (labels.empty() ? "" : "|") + button.label;
		}
		// The body on one line (the dev channel's reply is one line).
		std::string body = lastBody;
		std::ranges::replace(body, '\n', '/');
		return std::format("menu: pages={} presses={} serial={} panel={} centered={} showing={} pendingJoin={} pendingHost={} openWaiting={} buttons='{}' body='{}'", pagesShown, pressesHandled, serial, inPanel, UiPanel::Centered(), UiPanel::Showing(), pendingJoin.has_value(), pendingHost.has_value(), openWaiting, labels, body);
	}
}
