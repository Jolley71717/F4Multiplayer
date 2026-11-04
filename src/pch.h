#pragma once

#include <RE/Fallout.h>
#include <F4SE/F4SE.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>
#include <numbers>
#include <thread>

using namespace std::literals;

#if defined(__clang__)
// Clang fix for the Mac build (tools/xwin.lua). A plain delete makes clang write the manager's destructor, which
// needs types CommonLibF4 only declares; deleting through the first base is the same virtual call MSVC makes.
namespace RE
{
	template <>
	class BSTSmartPointerIntrusiveRefCount<BSAnimationGraphManager>
	{
	public:
		static void Acquire(BSAnimationGraphManager* a_ptr) { a_ptr->IncRef(); }

		static void Release(BSAnimationGraphManager* a_ptr)
		{
			if (a_ptr->DecRef() == 0) {
				delete static_cast<BSTEventSink<BSAnimationGraphEvent>*>(a_ptr);
			}
		}
	};
}
#endif
