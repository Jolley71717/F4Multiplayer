// Which menus are open, from their open and close events (game/OpenMenus.h), and the guard that keeps the
// plugin from asking the UI instead: a reference to a menu taken beside the game's drawing froze a host
// scrapping in the workshop (the workshop menu was destroyed mid-draw and waited on its own lock).

#include "Test.h"

#include "SourceTree.h"
#include "game/OpenMenus.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <thread>

using OpenMenus::Book;
using enum OpenMenus::Menu;

TEST("open menus: each menu is known by the game's own name (in any case), and is open from its opening to its closing")
{
	for (std::size_t i = 0; i < OpenMenus::NAMES.size(); ++i) {
		Book       book;
		const auto menu = static_cast<OpenMenus::Menu>(i);
		CHECK(!book.Open(menu));
		book.Seen(OpenMenus::NAMES[i], true);
		CHECK(book.Open(menu));
		for (std::size_t j = 0; j < OpenMenus::NAMES.size(); ++j) {
			CHECK(book.Open(static_cast<OpenMenus::Menu>(j)) == (i == j));  // only that one
		}
		book.Seen(OpenMenus::NAMES[i], false);
		CHECK(!book.Open(menu));
	}
	Book book;
	book.Seen("WorkshopMenu", true);
	CHECK(book.Open(kWorkshop));
	book.Seen("workshopmenu", false);  // case-blind, as the game's strings are
	CHECK(!book.Open(kWorkshop));
	book.Seen("WORKSHOPMENU", true);
	CHECK(book.Open(kWorkshop));
	book.Seen("WorkshopMenu2", false);  // another name, however it starts
	book.Seen("Workshop", false);
	CHECK(book.Open(kWorkshop));
}

TEST("open menus: a scrapping session in the workshop, with its prompts and the HUD around it, leaves the workshop closed when it closes")
{
	Book book;
	// The menus the game opens around workshop mode that are no business of the book's.
	for (const auto other : { "HUDMenu", "CursorMenu", "MessageBoxMenu", "ExamineMenu", "FaderMenu", "LoadingMenu" }) {
		book.Seen(other, true);
	}
	CHECK(!book.AnyOpen({ kDialogue, kPipboy, kWorkshop, kLooks, kPowerArmorMod }));
	for (int round = 0; round < 50; ++round) {
		book.Seen("WorkshopMenu", true);
		CHECK(book.AnyOpen({ kDialogue, kPipboy, kWorkshop, kLooks, kPowerArmorMod }));  // the compass is left alone
		book.Seen("MessageBoxMenu", true);  // "scrap this?"
		book.Seen("MessageBoxMenu", false);
		CHECK(book.Open(kWorkshop));
		book.Seen("WorkshopMenu", false);
		CHECK(!book.Open(kWorkshop));
		CHECK(!book.AnyOpen({ kDialogue, kPipboy, kWorkshop, kLooks, kPowerArmorMod }));
	}
	// A conversation outlasts the Pip-Boy opened and closed during it.
	book.Seen("DialogueMenu", true);
	book.Seen("PipboyMenu", true);
	book.Seen("PipboyMenu", false);
	CHECK(book.Open(kDialogue));
	CHECK(!book.Open(kPipboy));
	CHECK(book.AnyOpen({ kPipboy, kDialogue }));
	CHECK(!book.AnyOpen({ kPipboy, kVats }));
	CHECK(!book.AnyOpen({}));
	// A close the book never saw open (it was up before the book watched) is no harm.
	book.Seen("Console", false);
	CHECK(!book.Open(kConsole));
	CHECK(book.Open(kDialogue));
}

TEST("open menus: menus opening and closing on the UI's thread while other threads ask lose no menu's state")
{
	Book              book;
	std::atomic<bool> stop{ false };
	std::atomic<bool> lost{ false };
	book.Seen("DialogueMenu", true);
	// The workshop opened and closed over and over, beside the console and the Pip-Boy doing the same, as two
	// threads: had one menu's write been done as a read and a store, it would lose the others' now and then.
	std::thread workshop{ [&] {
		for (int i = 0; i < 100'000; ++i) {
			book.Seen("WorkshopMenu", true);
			book.Seen("WorkshopMenu", false);
		}
	} };
	std::thread others{ [&] {
		for (int i = 0; i < 100'000; ++i) {
			book.Seen("Console", true);
			book.Seen("PipboyMenu", (i % 2) == 0);
			book.Seen("Console", false);
		}
	} };
	std::thread reader{ [&] {
		do {
			if (!book.Open(kDialogue)) {
				lost = true;  // the conversation never closed
			}
		} while (!stop);
	} };
	workshop.join();
	others.join();
	stop = true;
	reader.join();
	CHECK(!lost);
	CHECK(book.Open(kDialogue));
	CHECK(!book.Open(kWorkshop));
	CHECK(!book.Open(kConsole));
	CHECK(!book.Open(kPipboy));  // the last of it was a close
}

// The plugin asks OpenMenus, never the UI. Each lookup of a menu through the UI takes a reference to it, and the
// one who lets go of a closing menu's last reference destroys it, there and then: the workshop menu destroyed
// while the menus were being drawn froze the game. The guard is on the source, as the game can't run in a test.
namespace
{
	// a_text without its comments and its string and character literals, line breaks kept.
	std::string CodeOnly(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const char c = a_text[i];
			if (c == '/' && i + 1 < a_text.size() && a_text[i + 1] == '/') {
				while (i < a_text.size() && a_text[i] != '\n') {
					++i;
				}
				out += '\n';
				continue;
			}
			if (c == '/' && i + 1 < a_text.size() && a_text[i + 1] == '*') {
				for (i += 2; i + 1 < a_text.size() && !(a_text[i] == '*' && a_text[i + 1] == '/'); ++i) {
					out += a_text[i] == '\n' ? "\n" : "";
				}
				++i;
				continue;
			}
			if (c == '"' || c == '\'') {
				for (++i; i < a_text.size() && a_text[i] != c; ++i) {
					i += a_text[i] == '\\';  // what a backslash escapes is skipped
				}
				out += ' ';
				continue;
			}
			out += c;
		}
		return out;
	}

	// How often a_code uses any of a_names as a whole name.
	int Uses(std::string_view a_code, std::initializer_list<std::string_view> a_names)
	{
		const auto ident = [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_'; };
		int        found = 0;
		for (const auto name : a_names) {
			for (auto at = a_code.find(name); at != std::string_view::npos; at = a_code.find(name, at + 1)) {
				const auto end = at + name.size();
				if ((at > 0 && ident(a_code[at - 1])) || (end < a_code.size() && ident(a_code[end]))) {
					continue;  // part of another name (GetMenuOpen is not a GetMenu)
				}
				++found;
			}
		}
		return found;
	}

	// How often a_code looks a menu up through the UI (GetMenu, GetMenuOpen, or the UI's menu stack and map).
	int MenuLookups(std::string_view a_code)
	{
		return Uses(a_code, { "GetMenu", "GetMenuOpen", "menuStack", "menuMap" });
	}

	// How often a_code hands work to an F4SE UI task.
	int UiTasks(std::string_view a_code)
	{
		return Uses(a_code, { "AddUITask" });
	}

	// a_fn(file, code) for every source file under src, the file relative to the repository.
	template <class Fn>
	void ForEachSource(const std::filesystem::path& a_root, Fn a_fn)
	{
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ a_root / "src" }) {
			const auto ext = entry.path().extension();
			if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
				continue;
			}
			a_fn(std::filesystem::relative(entry.path(), a_root).generic_string(), CodeOnly(SourceTree::Read(entry.path())));
		}
	}
}

TEST("open menus: the guard finds a menu looked up through the UI, and not one named in a comment or a string")
{
	CHECK(MenuLookups(CodeOnly(R"x(const bool open = ui && ui->GetMenuOpen("WorkshopMenu"sv);)x")) == 1);
	CHECK(MenuLookups(CodeOnly(R"x(const auto hud = ui->GetMenu("HUDMenu");)x")) == 1);
	CHECK(MenuLookups(CodeOnly(R"x(if (ui->GetMenuOpen<RE::WorkshopMenu>()) {)x")) == 1);
	CHECK(MenuLookups(CodeOnly(R"x(for (const auto& menu : ui->menuStack) {)x")) == 1);
	CHECK(MenuLookups(CodeOnly(R"x(// Asking the UI instead (GetMenuOpen) takes a reference to the menu
		/* nor GetMenu */ REX::INFO("no GetMenu here"); const char c = 'x';)x")) == 0);
	CHECK(MenuLookups(CodeOnly(R"x(OpenMenus::Open(OpenMenus::Menu::kWorkshop); GetMenuName(); myGetMenu();)x")) == 0);
	CHECK(CodeOnly("a // b\nc /* d\ne */ f \"g\\\"h\" i") == "a \nc \n f   i");
}

TEST("open menus: the plugin looks no menu up through the UI, but for the HUD once, to hook it, and the developer's menu commands")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder
	// Compass's lookup is on the main thread, between frames, of the HUD (which the UI keeps for the whole game),
	// and only until its update is hooked. The gfx commands (dev channel) look up what a developer names, and
	// "dialogue menu" the dialogue menu and the menu stack: on the main thread, between frames, when a developer
	// asks, and the reference is let go before the command returns.
	const std::map<std::string, int> allowed{ { "src/game/Compass.cpp", 1 }, { "src/DevCommands.cpp", 4 } };
	ForEachSource(*root, [&](const std::string& a_file, const std::string& a_code) {
		const auto lookups = MenuLookups(a_code);
		const auto it = allowed.find(a_file);
		const int  expected = it == allowed.end() ? 0 : it->second;
		if (lookups != expected) {
			Test::Fail(__FILE__, __LINE__, std::format("{} looks a menu up through the UI {} time(s), not {}: ask OpenMenus", a_file, lookups, expected));
		}
	});
}

// An F4SE UI task runs on a job thread beside the main thread's drawing: a menu looked up there (the gfx commands
// did, by any name a developer gives, the workshop menu's included) held a reference to it at the one moment that
// can hand the engine's deadlock its chance. The plugin's work with the menus runs on the main thread between
// frames (DevCommands, Compass's hook) or inside the HUD's own update (Compass, Hud), never in a UI task.
TEST("open menus: the plugin hands no work to an F4SE UI task")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);
	ForEachSource(*root, [&](const std::string& a_file, const std::string& a_code) {
		if (const auto tasks = UiTasks(a_code)) {
			Test::Fail(__FILE__, __LINE__, std::format("{} hands work to an F4SE UI task {} time(s): run it on the main thread between frames", a_file, tasks));
		}
	});
	CHECK(UiTasks(CodeOnly(R"x(F4SE::GetTaskInterface()->AddUITask([] {});)x")) == 1);
	CHECK(UiTasks(CodeOnly(R"x(// AddUITask is not used; MyAddUITask(); AddUITasks();)x")) == 0);
}
