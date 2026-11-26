#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace RE
{
	class TESHitEvent;
	template <class>
	class BSTEventSource;
	template <class>
	class BSTEventSink;
}

// Our player's hits reach the mod whichever other plugin's sink is ahead of ours (EventTapRules.h says why and what each
// part is; docs/DESIGN.md, "Event tap"). This patches the engine (1.11.240, by Address Library ID) and reads the event
// sources' sink lists; it never changes, stops or reorders what another plugin gets.
namespace EventTap
{
	// What the hook runs for a hit our sink never got (the same read-and-queue handler the sink runs, on the sender's
	// thread), set once by whoever owns the sink.
	using HitHandler = void (*)(const RE::TESHitEvent&);
	void SetHitHandler(HitHandler a_handler);

	// At plugin load: the engine's two calls that send a TESHitEvent go through a hook of ours, once each is checked to be
	// 1.11.240's. Where one isn't (another version, another plugin's patch), none is hooked, logged. Whether the hook is in.
	bool Install();

	// Our sink got a hit event (any thread): the hook is told, so the event is handled once. Whether it came through the
	// hook (a_event is the address the hook is sending).
	bool HitSunk(const RE::TESHitEvent* a_event);

	// Puts a_sink ahead of the other plugins' sinks on the hit source, right behind the engine's own (which finish the event), under
	// the source's lock, once it is registered there. Logged when it moved, and when it could not.
	void PutHitSinkFirst(RE::BSTEventSink<RE::TESHitEvent>* a_sink);

	// Once a session (the welcome): the front place is checked again (a plugin that registered since may have pushed ours
	// back), and the sinks on the sources we care about are listed, in the log, if this session has not yet.
	void OnWelcome();
	void OnSessionEnd();

	// For `net status`: whether the hook is in, how many hit events it sent, how many of them our sink never got (and the
	// tap handled), and how many our sink got that the hook did not send.
	[[nodiscard]] std::string Describe();

	// What CopyWatch's missing-hit-events warning adds: the cause named when hits were sent that our sink never got, or "".
	[[nodiscard]] std::string CauseNote();

	// Dev (`eventtap`): the counters and the sinks on the hit source as the log lists them. Main thread.
	[[nodiscard]] std::string Status();

	// Dev (`eventtap handle|front|starve on|off`): the rig plays a game whose hits another plugin's sink stops.
	// handle: whether the hook handles what our sink never got (off: as before the tap). front: whether our sink stands
	// ahead of the plugins' (off: moved last, and not put ahead again). starve: a sink of the rig's, ahead of ours (behind the engine's),
	// that stops every hit.
	void SetHandling(bool a_on);
	bool SetFront(bool a_on);
	bool SetStarve(bool a_on);
}
