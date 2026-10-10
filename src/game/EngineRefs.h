#pragma once

// Small guards around engine singletons that may not exist yet (before the game data is loaded).
namespace EngineRefs
{
	// A reference placed in the world, or an empty handle when the data handler is not there.
	inline RE::ObjectRefHandle CreateReferenceAtLocation(RE::NEW_REFR_DATA& a_data)
	{
		const auto handler = RE::TESDataHandler::GetSingleton();
		return handler ? handler->CreateReferenceAtLocation(a_data) : RE::ObjectRefHandle{};
	}

	// Memory the engine will free with its own allocator must come from it: an engine-owned struct
	// made with plain new would be freed by the wrong heap.
	template <class T, class... Args>
	T* New(Args&&... a_args)
	{
		const auto memory = RE::malloc(sizeof(T));
		return memory ? new (memory) T(std::forward<Args>(a_args)...) : nullptr;
	}
}
