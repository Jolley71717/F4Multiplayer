#include "game/Conversations.h"
#include "game/Official.h"

#include "Config.h"

#include "game/Dialogue.h"
#include "game/Hud.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"
#include "game/VoicePath.h"

#include <bit>

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
		std::uint32_t                               placed = 0;    // lines reported with their recording
		std::uint32_t                               voiced = 0;    // friends' lines played
		std::uint32_t                               unplaced = 0;  // friends' lines that came without a recording (text only)
		std::uint32_t                               missing = 0;   // ... with one we have no file for (an unvoiced line, or a mod's)
		std::uint32_t                               unplayed = 0;  // ... with a file the audio manager gave no sound for

		// The sound each friend's speaker is saying now, keyed like Shown (a speaker says one line at a time).
		std::unordered_map<std::uint64_t, RE::BSSoundHandle> voices;

		// How a friend's line is played: a positioned (3D) sound, heard from where the speaker is and
		// fading with distance through the game's own 3D dialogue output model.
		constexpr std::uint32_t VOICE_FLAGS = 0x10;  // as WeaponFire's shots: a 3D sound, heard from where it is
		constexpr std::uint8_t  VOICE_PRIORITY = 128;
		constexpr std::uint16_t VOICE_CUT_MS = 100;  // a speaker's next line fades the one before out this fast

		// The actor's latest subtitle. The game keeps it after the line ends; IsTalking() says
		// whether it's being spoken now.
		std::string SubtitleOf(RE::Actor* a_actor)
		{
			const auto process = a_actor->currentProcess;
			const auto high = process ? process->high : nullptr;
			const char* text = high ? high->strVoiceSubtitle.c_str() : nullptr;
			return text ? text : "";
		}

		// The recorded line an actor is saying: its INFO, the response number its voice file is named
		// with, and the voice it is said in. All 0 when the engine no longer has the line.
		struct Recording
		{
			std::uint32_t info = 0;
			std::uint8_t  response = 0;
			std::uint32_t voiceType = 0;
		};

		// The info whose responses (text and recordings) an info uses: its own, or those of the info it
		// shares them with.
		RE::TESTopicInfo* ResponsesOf(RE::TESTopicInfo* a_info)
		{
			return a_info->responses.head || !a_info->dataInfo ? a_info : a_info->dataInfo;
		}

		// The number the voice file of the a_index-th response (from 0) is named with: the response's own
		// number (TRDT), or its place in the list from 1 when that is unset.
		std::uint8_t ResponseNumber(RE::TESTopicInfo* a_info, std::uint32_t a_index)
		{
			struct RespData  // RE::TESResponse::RespData, whose members CommonLibF4 keeps private
			{
				std::uint16_t percent;
				std::uint8_t  responseID;
				bool          useEmotion;
			};
			static_assert(sizeof(RespData) == sizeof(RE::TESResponse::RespData));
			std::uint32_t index = 0;
			for (auto response = ResponsesOf(a_info)->responses.head; response; response = response->pNext, ++index) {
				if (index == a_index) {
					const auto number = std::bit_cast<RespData>(response->data).responseID;
					return number != 0 ? number : static_cast<std::uint8_t>(a_index < 255 ? a_index + 1 : 0);
				}
			}
			return 0;
		}

		// The response of an info whose text is the line, as a number for its voice file (0: none is).
		std::uint8_t ResponseSaying(RE::TESTopicInfo* a_info, const std::string& a_text)
		{
			std::uint32_t index = 0;
			for (auto response = ResponsesOf(a_info)->responses.head; response; response = response->pNext, ++index) {
				const char* text = response->GetResponseText();
				if (text && a_text == text) {
					return ResponseNumber(a_info, index);
				}
			}
			return 0;
		}

		RE::BGSVoiceType* VoiceTypeOf(RE::Actor* a_actor, bool a_isPlayer)
		{
			if (a_isPlayer) {  // the player character's lines are recorded in the male and the female player voice
				const auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
				const auto which = a_actor->GetSex() == RE::SEX::kFemale ? RE::DEFAULT_OBJECT::kPlayerVoiceFemale : RE::DEFAULT_OBJECT::kPlayerVoiceMale;
				return defaults ? defaults->GetDefaultObject<RE::BGSVoiceType>(which) : nullptr;
			}
			const auto npc = a_actor->GetNPC();
			return npc ? npc->voiceType : nullptr;
		}

		// The line the actor is saying, when the engine still has it: greetings and most conversation
		// lines keep their dialogue item (the info and the response being said) on the actor. The item
		// counts only while its response is the line on screen; otherwise the actor's last greeting
		// does if one of its responses is the line.
		Recording RecordingOf(RE::Actor* a_actor, bool a_isPlayer, const std::string& a_text)
		{
			const auto process = a_actor->currentProcess;
			const auto high = process ? process->high : nullptr;
			if (!high) {
				return {};
			}
			RE::TESTopicInfo* info = nullptr;
			std::uint8_t      response = 0;
			if (const auto item = high->greetTopic; item && item->info) {
				const auto current = item->GetCurrentResponse();
				const char* text = current ? current->text.c_str() : nullptr;
				if (text && a_text == text) {
					info = item->info;
					response = ResponseNumber(info, item->currentResponse);
					const char* file = current->voice.c_str();
					REX::DEBUG("Conversations: {:08X} line is {:08X} response {} (the engine's voice file '{}')", a_actor->GetFormID(), info->GetFormID(), response, file ? file : "");
				}
			}
			if ((!info || response == 0) && high->lastGreeting) {
				info = high->lastGreeting;
				response = ResponseSaying(info, a_text);
			}
			const auto voice = VoiceTypeOf(a_actor, a_isPlayer);
			if (!info || response == 0 || !voice || !Protocol::IsShareableRef(info->GetFormID()) || !Protocol::IsShareableRef(voice->GetFormID())) {
				return {};  // the line is still shown, as text
			}
			return { info->GetFormID(), response, voice->GetFormID() };
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
					const auto recording = RecordingOf(a_actor, a_isPlayer, text);
					outgoing.push_back(Protocol::Encode(Protocol::Line{ 0, *speaker, Fit(text), recording.info, recording.response, recording.voiceType },
						Protocol::MessageType::kReportLine));
					++reported;
					placed += recording.info != 0;
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

		// Whether the game has the file, loose or in an archive.
		bool Exists(const std::string& a_path)
		{
			RE::BSTSmartPointer<RE::BSResource::Stream> stream;
			return RE::BSResource::GetOrCreateStream(a_path.c_str(), stream) == RE::BSResource::ErrorCode::kNone && stream;
		}

		// The voice file of a response of an info, said in a voice ("" if its plugin or the voice has no usable name).
		std::string FileOf(const RE::TESTopicInfo* a_info, const RE::BGSVoiceType* a_voice, std::uint8_t a_response, std::string_view a_extension = "fuz")
		{
			const auto file = a_info->GetFile(0);  // the plugin that made the info: its recordings are under that plugin's name
			const char* voice = a_voice->formEditorID.c_str();
			if (!file || !voice) {
				return {};
			}
			return VoicePath::Build(file->GetFilename(), voice, VoicePath::LocalId(a_info->GetFormID(), file->IsLight()), a_response, a_extension);
		}

		struct Resolved
		{
			const RE::TESTopicInfo* info = nullptr;  // the info whose recording it is (the line's own, or the one it shares)
			std::string             path;            // its voice file, "" if the game has none
		};

		// The voice file of a friend's line, resolved by us from what the line names: only the game's
		// own infos and voice types (Official.h), as for everything said or set from a friend's game.
		Resolved Resolve(const Protocol::Line& a_line)
		{
			const auto info = RE::TESForm::GetFormByID<RE::TESTopicInfo>(a_line.info);
			const auto voice = RE::TESForm::GetFormByID<RE::BGSVoiceType>(a_line.voiceType);
			if (!info || !voice || !Official::IsOfficialForm(info) || !Official::IsOfficialForm(voice)) {
				return {};
			}
			for (const RE::TESTopicInfo* candidate : { info, info->dataInfo }) {
				if (candidate && Official::IsOfficialForm(candidate)) {
					if (auto path = FileOf(candidate, voice, a_line.response); !path.empty() && Exists(path)) {
						return { candidate, std::move(path) };
					}
				}
			}
			return { info, {} };
		}

		// A sound for a voice file. The archives hold .fuz files (the recording with its lip sync); should
		// the audio manager want one of the other names a recording can go by, it is asked for those too.
		RE::BSSoundHandle SoundFor(const std::string& a_path, std::uint32_t a_flags, std::uint8_t a_priority)
		{
			RE::BSSoundHandle handle;
			const auto        audio = RE::BSAudioManager::GetSingleton();
			if (!audio || a_path.size() < 4) {
				return handle;
			}
			const auto stem = a_path.substr(0, a_path.size() - 3);
			for (const auto extension : { "fuz"sv, "wav"sv, "xwm"sv }) {
				handle = {};
				audio->GetSoundHandleByFile(handle, RE::BSResource::ID((stem + std::string(extension)).c_str()), a_flags, a_priority);
				if (handle.IsValid()) {
					break;
				}
			}
			return handle;
		}

		// Plays a voice file at the actor and follows them while it lasts: a sound like any other, so
		// nothing of the dialogue system (no conversation, Say, scene or lip sync) is involved.
		bool Play(RE::Actor* a_actor, const std::string& a_path, std::uint64_t a_key, std::uint32_t a_flags = VOICE_FLAGS, std::uint8_t a_priority = VOICE_PRIORITY)
		{
			auto handle = SoundFor(a_path, a_flags, a_priority);
			if (!handle.IsValid()) {
				return false;
			}
			const auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
			if (const auto model = defaults ? defaults->GetDefaultObject<RE::BGSSoundOutput>(RE::DEFAULT_OBJECT::kDialogueOutputModel3D) : nullptr) {
				handle.SetOutputModel(model);  // how far an NPC's own lines carry, and how they fade
			}
			handle.SetPosition(a_actor->data.location);
			if (const auto root = a_actor->Get3D()) {
				handle.SetObjectToFollow(root);
			}
			// A speaker says one line at a time: the one before, if still playing, is cut short. Lines
			// that have ended are let go.
			if (const auto before = voices.find(a_key); before != voices.end() && before->second.IsPlaying()) {
				before->second.FadeOutAndRelease(VOICE_CUT_MS);
			}
			std::erase_if(voices, [](auto& a_entry) { return !a_entry.second.IsPlaying(); });
			if (!handle.Play()) {
				return false;
			}
			voices[a_key] = handle;
			return true;
		}

		// Says a friend's line at our copy of its speaker, if it names a recording we have.
		void Speak(RE::Actor* a_actor, const Protocol::Line& a_line, std::uint64_t a_key)
		{
			if (a_line.info == 0) {
				++unplaced;
				return;
			}
			const auto file = Resolve(a_line);
			if (file.path.empty()) {
				++missing;
				REX::DEBUG("Conversations: no voice file for {:08X} response {} voice {:08X}", a_line.info, a_line.response, a_line.voiceType);
				return;
			}
			if (!Play(a_actor, file.path, a_key)) {
				++unplayed;
				REX::DEBUG("Conversations: no sound for '{}'", file.path);
				return;
			}
			++voiced;
			REX::DEBUG("Conversations: {:08X} says '{}'", a_actor->GetFormID(), file.path);
		}
	}

	void Frame(bool a_connected)
	{
		Dialogue::Frame();  // a conversation left without its speaker is ended

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
		// The line is heard in the speaker's own recording, played as a plain 3D sound at our copy of
		// them (a friend's own lines at their stand-in): whoever is close enough hears it, in a
		// conversation or a scene or not, fading with distance. It used to be the console's Say on
		// our copy of the NPC, which went through the dialogue system: a Say into a running
		// conversation or scene could leave it unfinished, and the game then held the player as if
		// still talking (no weapon, Pip-Boy or workshop). A sound can't, and nothing here touches the
		// dialogue system: no conversation, Say, scene, topic or script.
		if (Config::Get().spokenLines) {
			Speak(actor, a_line, key);
		}
		Hud::Notify(std::format("{}: {}", name, a_line.text));
	}

	void Reset()
	{
		outgoing.clear();
		shown.clear();
		for (auto& [key, handle] : voices) {
			if (handle.IsPlaying()) {
				handle.FadeOutAndRelease(VOICE_CUT_MS);
			}
		}
		voices.clear();
	}

	std::string Describe()
	{
		return std::format("conversations: heard={} reported={} placed={} received={} shown={} voiced={} unplaced={} missing={} unplayed={} {}", heard, reported, placed,
			received, displayed, voiced, unplaced, missing, unplayed, Dialogue::Describe());
	}

	std::string DescribeLine(RE::Actor* a_actor)
	{
		const auto process = a_actor->currentProcess;
		const auto high = process ? process->high : nullptr;
		const char* subtitle = high ? high->strVoiceSubtitle.c_str() : nullptr;
		const auto  isPlayer = a_actor == RE::PlayerCharacter::GetSingleton();
		const auto  recording = RecordingOf(a_actor, isPlayer, subtitle ? subtitle : "");
		const auto  item = high ? high->greetTopic : nullptr;
		const auto  current = item ? item->GetCurrentResponse() : nullptr;
		const char* engineFile = current ? current->voice.c_str() : nullptr;
		std::string ours;
		if (recording.info != 0) {
			ours = Resolve(Protocol::Line{ 0, 0, "-", recording.info, recording.response, recording.voiceType }).path;
		}
		return std::format("talking={} subtitle='{}' info={:08X} response={} voiceType={:08X} item={:08X}/{} engineFile='{}' file='{}'", a_actor->IsTalking(),
			subtitle ? subtitle : "", recording.info, recording.response, recording.voiceType, item && item->info ? item->info->GetFormID() : 0,
			item ? static_cast<std::int64_t>(static_cast<std::int32_t>(item->currentResponse)) : -1, engineFile ? engineFile : "", ours);
	}

	std::string PlayLine(RE::Actor* a_actor, std::uint32_t a_info, std::uint8_t a_response, std::uint32_t a_voiceType, std::uint32_t a_flags, std::uint8_t a_priority)
	{
		const auto file = Resolve(Protocol::Line{ 0, 0, "-", a_info, a_response, a_voiceType });
		if (file.path.empty()) {
			return std::format("no file (info {:08X} official={}, voice type {:08X} official={})", a_info,
				Official::IsOfficialForm(RE::TESForm::GetFormByID<RE::TESTopicInfo>(a_info)), a_voiceType,
				Official::IsOfficialForm(RE::TESForm::GetFormByID<RE::BGSVoiceType>(a_voiceType)));
		}
		const bool played = Play(a_actor, file.path, (static_cast<std::uint64_t>(0xFFFFFFFF) << 32) | a_actor->GetFormID(), a_flags, a_priority);
		return std::format("file='{}' played={}", file.path, played);
	}
}
