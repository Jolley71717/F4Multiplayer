#include "game/Conversations.h"

#include "Config.h"

#include "game/Hud.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"

namespace Conversations
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;

		// How close you must be to a speaker (your copy of the NPC, or a friend's stand-in) to
		// see what they say. About 20 m.
		constexpr float HEAR_RANGE = 1400.0f;
		constexpr auto  REPEAT_WINDOW = 5s;  // the same speaker saying the same thing again is skipped
		constexpr auto  SHOW_WINDOW = 3s;    // at most MAX_SHOWN lines per window (combat barks pile up)
		constexpr int   MAX_SHOWN = 3;

		struct Speaking
		{
			std::string text;
			bool        talking = false;
		};

		struct Shown
		{
			std::uint64_t     key = 0;  // player id and speaker
			std::string       text;
			Clock::time_point at{};
		};

		std::unordered_map<std::uint32_t, Speaking> speaking;  // what each actor said last frame
		std::vector<std::vector<std::uint8_t>>      outgoing;
		std::deque<Shown>                           shown;
		std::uint32_t                               heard = 0;
		std::uint32_t                               reported = 0;
		std::uint32_t                               received = 0;
		std::uint32_t                               displayed = 0;
		std::uint32_t                               voiced = 0;

		// The actor's latest subtitle. The game keeps it after the line ends; IsTalking() says
		// whether it's being spoken now.
		std::string SubtitleOf(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto high = process ? process->high : nullptr;
			const char* text = high ? high->strVoiceSubtitle.c_str() : nullptr;
			return text ? text : "";
		}

		// The topic of the line the actor is saying, when the engine still has it (greetings and most
		// conversation lines keep their dialogue item on the actor), so the hearer's copy can say it too.
		std::uint32_t TopicOf(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto high = process ? process->high : nullptr;
			if (!high) {
				return 0;
			}
			if (high->greetTopic && high->greetTopic->topic) {
				return high->greetTopic->topic->GetFormID();
			}
			if (high->lastGreeting && high->lastGreeting->parentTopic) {
				return high->lastGreeting->parentTopic->GetFormID();
			}
			return 0;
		}

		// Cut to the protocol's limit without splitting a UTF-8 character.
		std::string Fit(std::string a_text)
		{
			if (a_text.size() > Protocol::MAX_LINE_LENGTH) {
				auto length = Protocol::MAX_LINE_LENGTH;
				while (length > 0 && (static_cast<unsigned char>(a_text[length]) & 0xC0) == 0x80) {
					--length;
				}
				a_text.resize(length);
			}
			return a_text;
		}

		// Who reports a line: the player for their own lines; for an NPC, the game that runs it,
		// or the player talking to it (their copy is the one in the conversation). Combat barks
		// and grunts aren't shared: they'd bury the conversations in notifications.
		std::optional<std::uint32_t> Reportable(RE::Actor* a_actor, bool a_isPlayer)
		{
			if (a_actor->IsInCombat()) {
				return std::nullopt;
			}
			if (a_isPlayer) {
				return 0u;
			}
			const auto id = a_actor->GetFormID();
			if (Protocol::IsShareableRef(id) && (NpcSync::RunsLocally(id) || NpcSync::TalkingTo() == id)) {
				return id;
			}
			return std::nullopt;
		}

		void Watch(RE::Actor* a_actor, bool a_isPlayer, bool a_connected, std::unordered_map<std::uint32_t, Speaking>& a_now)
		{
			const bool talking = a_actor->IsTalking();
			auto       text = SubtitleOf(a_actor);
			const auto id = a_actor->GetFormID();
			const auto before = speaking.find(id);
			const bool wasTalking = before != speaking.end() && before->second.talking;
			const bool changed = before == speaking.end() || before->second.text != text;
			if (talking && !text.empty() && (!wasTalking || changed)) {
				++heard;
				const auto speaker = Reportable(a_actor, a_isPlayer);
				REX::DEBUG("Conversations: {:08X} says '{}'{}", id, text, speaker ? " (reported)" : "");
				if (speaker && a_connected) {
					outgoing.push_back(Protocol::Encode(Protocol::Line{ 0, *speaker, Fit(text), a_isPlayer ? 0u : TopicOf(a_actor) }, Protocol::MessageType::kReportLine));
					++reported;
				}
			}
			if (talking || before != speaking.end()) {
				a_now.emplace(id, Speaking{ std::move(text), talking });
			}
		}

		bool SameSpace(RE::Actor* a_a, RE::Actor* a_b)
		{
			const auto a = a_a->GetParentCell();
			const auto b = a_b->GetParentCell();
			if (!a || !b) {
				return false;
			}
			if (a->IsInterior() || b->IsInterior()) {
				return a == b;
			}
			return a->worldSpace == b->worldSpace;
		}

		// Our copy of who said it, and the name to show.
		std::pair<RE::Actor*, std::string> FindSpeaker(const Protocol::Line& a_line)
		{
			if (a_line.speaker == 0) {
				for (auto& info : RemotePlayers::List()) {
					if (info.id == a_line.playerId) {
						return { info.actor, info.name };
					}
				}
				return { nullptr, {} };
			}
			const auto actor = RE::TESForm::GetFormByID<RE::Actor>(a_line.speaker);
			if (!actor || !actor->Get3D() || actor->IsDead(false)) {
				return { nullptr, {} };
			}
			const char* name = actor->GetDisplayFullName();
			return { actor, name && *name ? name : "Someone" };
		}

		bool Throttled(std::uint64_t a_key, const std::string& a_text, Clock::time_point a_now)
		{
			while (!shown.empty() && a_now - shown.front().at > REPEAT_WINDOW) {
				shown.pop_front();
			}
			int recent = 0;
			for (const auto& entry : shown) {
				if (entry.key == a_key && entry.text == a_text) {
					return true;
				}
				recent += a_now - entry.at < SHOW_WINDOW;
			}
			return recent >= MAX_SHOWN;
		}
	}

	void Frame(bool a_connected)
	{
		std::unordered_map<std::uint32_t, Speaking> now;
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			Watch(player, true, a_connected, now);
		}
		if (const auto lists = RE::ProcessLists::GetSingleton()) {
			for (const auto& handle : lists->highActorHandles) {
				if (const auto actor = handle.get(); actor && actor.get() != RE::PlayerCharacter::GetSingleton()) {
					Watch(actor.get(), false, a_connected, now);
				}
			}
		}
		speaking = std::move(now);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Apply(const Protocol::Line& a_line)
	{
		++received;
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto [actor, name] = FindSpeaker(a_line);
		if (!player || !actor || !SameSpace(player, actor) ||
			actor->data.location.GetDistance(player->data.location) > HEAR_RANGE) {
			return;
		}
		// Our own copy is saying it too (or we're in this conversation ourselves): we hear it already.
		if (a_line.speaker != 0 && (actor->IsTalking() || NpcSync::TalkingTo() == a_line.speaker)) {
			return;
		}
		const auto key = (static_cast<std::uint64_t>(a_line.playerId) << 32) | a_line.speaker;
		const auto now = Clock::now();
		if (Throttled(key, a_line.text, now)) {
			return;
		}
		shown.push_back({ key, a_line.text, now });
		++displayed;
		// An NPC's line is said aloud by our copy when we know its topic (the engine picks the line
		// for the topic by its conditions, which match the speaker's game in all but odd cases).
		if (a_line.speaker != 0 && a_line.topic != 0 && Config::Get().voiceLines) {
			if (const auto topic = RE::TESForm::GetFormByID<RE::TESTopic>(a_line.topic)) {
				// Through the console, not Papyrus: packing the call's arguments for the script VM crashed
				// both games in testing (BSScript::PackVariable), while the console's Say is plain engine code.
				RE::Console::ExecuteCommand(std::format("{:08X}.say {:08X}", actor->GetFormID(), topic->GetFormID()).c_str());
				++voiced;
			}
		}
		Hud::Notify(std::format("{}: {}", name, a_line.text));
	}

	void Reset()
	{
		outgoing.clear();
		shown.clear();
	}

	std::string Describe()
	{
		return std::format("conversations: heard={} reported={} received={} shown={} voiced={}", heard, reported, received, displayed, voiced);
	}
}
