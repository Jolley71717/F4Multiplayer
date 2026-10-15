#include "game/OpenMenus.h"

namespace OpenMenus
{
	namespace
	{
		Book book;

		// Only the book is written here (the event's thread).
		class Watcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				book.Seen(std::string_view{ a_event.menuName.c_str() }, a_event.opening);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Watcher watcher;
	}

	void Install()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
		}
	}

	bool Open(Menu a_menu)
	{
		return book.Open(a_menu);
	}

	bool AnyOpen(std::initializer_list<Menu> a_menus)
	{
		return book.AnyOpen(a_menus);
	}
}
