#include "game/Conversations.h"
#include "game/Official.h"

#include "Config.h"

#include "game/Dialogue.h"
#include "game/Hud.h"
#include "game/LineHold.h"
#include "game/LineShare.h"
#include "game/LineSounds.h"
#include "game/NpcSync.h"
#include "game/QuestActors.h"
#include "game/RemotePlayers.h"
#include "game/Spawns.h"
#include "game/SubtitleTime.h"
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
		std::uint32_t                               subtitled = 0;  // friends' lines shown in the game's subtitle area
		std::uint32_t                               timed = 0;      // ... timed by their recording's length
		std::uint32_t                               noticed = 0;    // ... shown as a HUD notice instead (subtitles off, in a conversation)
		std::uint32_t                               dropped = 0;    // ... gone from the subtitle list at once, and shown as a notice
		std::uint32_t                               cutLong = 0;     // friends' lines' sounds still sounding at their hard end, ended then
		std::uint32_t                               cutCrowded = 0;  // ... still sounding when LineSounds::MAX_PLAYING newer ones came
		std::uint32_t                               cutUnlogged = 0;  // ... of them, not logged (CUT_LOG_EVERY) since the last cut logged
		std::optional<Clock::time_point>            cutLoggedAt;      // when a cut was last logged

		// Friends' lines held for a copy of their speaker about to be placed (Apply, HeldLinesFrame; LineHold.h): a spawn's
		// first line came 0.3 s before the copies stood on the rig (0.14.0), and nobody was there to say it.
		struct Held
		{
			Protocol::Line    line;
			Clock::time_point since{};
		};
		std::vector<Held> heldLines;
		std::uint32_t     held = 0, heldDropped = 0;  // lines held; of them, let go unsaid (no copy stood in time)

		// Our copy of a friend's spawn, as a line's speaker: placed (Spawns::LocalOf names it), and standing with its 3D.
		struct CopyState
		{
			bool placed = false;
			bool loaded = false;
		};

		CopyState CopyOf(std::uint32_t a_spawn)
		{
			const auto local = Spawns::LocalOf(a_spawn);
			const auto actor = local != 0 ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
			return { local != 0, actor && actor->Get3D() };
		}

		// The sounds of friends' lines, keyed like Shown (a speaker says one line at a time), each with a hard end.
		using Sounds = LineSounds<RE::BSSoundHandle>;
		Sounds lineSounds;

		// A friend's line in the game's own subtitle area, the way the game shows a nearby NPC's line: the
		// SubtitleManager's list of who is saying what (speaker, text, priority, distance), from which the engine
		// shows the nearest line of the highest priority each frame, within fMaxSubtitleDistance (1500 units) and
		// only as the player's subtitle settings allow (bGeneralSubtitles out of a conversation, bDialogueSubtitles
		// in one, for the lines of the player's own conversation alone). An NPC's own line leaves the list when its
		// recording ends; ours has no recording playing through the dialogue system, so we take it out ourselves
		// after as long as its recording lasts (SubtitleTime::ShowFor).
		//
		// The engine's own two calls, by Address Library ID (1.11.240), read from the game's code:
		// - SubtitleManager::AddSubtitle (ID 2249542, +107DE80): (manager, speaker, const BSFixedStringCS& text,
		//   TESTopicInfo* info, bool high). Nothing for a blank text. Out of a conversation it adds a line only with
		//   bGeneralSubtitles on (or the line of the conversation's speaker); in one, with bDialogueSubtitles on, only
		//   the lines of the player and of who they are talking to. Priority: normal, high with a_high, or the
		//   info's own when that is low or forced (none here: the speaker's own name is shown, and the line's
		//   priority is normal). It takes the manager's lock itself.
		// - SubtitleManager::RemoveSubtitle (ID 2249543, +107E1B0): (manager, speaker): takes every line of the
		//   speaker out of the list, and off the screen if it is the one shown. The engine calls it when an
		//   actor's line ends (the voice state update, ID 2229974).
		// The list itself is read only, under the manager's lock, to check a line we added is in it.
		constexpr REL::ID ADD_SUBTITLE{ 2249542 };
		constexpr REL::ID REMOVE_SUBTITLE{ 2249543 };

		// How soon after adding a line we check the engine kept it.
		constexpr auto SUBTITLE_CHECK = 200ms;

		// The largest part of a voice file read for its length (the audio's header and packet table come first).
		constexpr std::uint64_t MAX_VOICE_READ = 4u << 20;

		// A line's sound cut while it still played is logged at most this often (net status counts them all).
		constexpr auto CUT_LOG_EVERY = 30s;
		// The player ID the voiceline dev command plays its recordings under (PlayLine).
		constexpr std::uint32_t DEV_PLAYER = 0xFFFFFFFF;

		struct Subtitle
		{
			std::uint64_t       key = 0;  // as Shown: player id and speaker
			RE::ObjectRefHandle speaker;
			std::string         text;
			std::string         notice;  // the HUD notice to show instead, should the engine drop it
			Clock::time_point   at{};
			Clock::time_point   until{};
			bool                checked = false;
		};
		std::vector<Subtitle> subtitlesUp;

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

		// The game's default objects, the form 00000800. Not RE::BGSDefaultObjectManager::GetSingleton():
		// the function it calls (ID 2192850) is no plain getter in this game version: it read the default
		// object array at an index it was never given and crashed the game the first time a friend's NPC
		// line was played.
		RE::BGSDefaultObjectManager* DefaultObjects()
		{
			const auto form = RE::TESForm::GetFormByID(0x800);
			return form && form->GetFormType() == RE::ENUM_FORM_ID::kDOBJ ? static_cast<RE::BGSDefaultObjectManager*>(form) : nullptr;
		}

		RE::BGSVoiceType* VoiceTypeOf(RE::Actor* a_actor, bool a_isPlayer)
		{
			if (a_isPlayer) {  // the player character's lines are recorded in the male and the female player voice
				const auto defaults = DefaultObjects();
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

		// Whether our own game has the recording a line names (Resolve finds its voice file, loose or in an archive): what
		// a creature's line must have to be shared (LineShare), since its growls name sound descriptors and no file.
		bool HasRecording(const Recording& a_recording);

		// A creature: a race our player can't talk to (a feral dog, a stingwing, a mutant hound; not an eyebot's or
		// Liberty Prime's recorded speech, which LineShare keeps by their recordings).
		bool IsCreature(const RE::Actor* a_actor)
		{
			return a_actor->race && (static_cast<std::uint32_t>(a_actor->race->data.flags) & QuestActors::RACE_ALLOWS_DIALOGUE) == 0;
		}

		// Whose line it is to report, and under what id: the player's own (0); an NPC's when our game runs it, or our
		// player talks to it (their copy is the one in the conversation), under the NPC's id or its spawn id. Nothing for
		// an NPC another game runs. Whether the line goes at all is LineShare::Reported (Watch).
		std::optional<std::uint32_t> Reportable(RE::Actor* a_actor, bool a_isPlayer)
		{
			if (a_isPlayer) {
				return 0u;
			}
			const auto id = a_actor->GetFormID();
			if (Protocol::IsShareableRef(id) && (NpcSync::RunsLocally(id) || NpcSync::TalkingTo() == id)) {
				return id;
			}
			// An NPC our game made and shares as a spawn (a random encounter's actor, Encounters.h): under its spawn id, which
			// the hearers resolve to their copy of it. A copy of a friend's spawn is theirs to report.
			if (Protocol::IsSpawnRef(id)) {
				if (const auto spawn = Spawns::OwnIdOf(id)) {
					return spawn;
				}
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
				const auto recording = speaker ? RecordingOf(a_actor, a_isPlayer, text) : Recording{};
				// A creature's line goes only with a recording our game has (one archive lookup, for creatures' lines alone).
				const bool creature = speaker && !a_isPlayer && IsCreature(a_actor);
				const bool inCombat = a_actor->IsInCombat();
				const bool goes = speaker && LineShare::Reported(inCombat, a_isPlayer, true, creature, creature && !inCombat && HasRecording(recording));  // no lookup for a combat bark
				REX::DEBUG("Conversations: {:08X} says '{}'{}", id, text, goes ? " (reported)" : (creature && !inCombat ? " (a creature's, no recording: not reported)" : ""));
				if (goes && a_connected) {
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
			// A spawn id names our copy of the friend's NPC (Spawns); a plugin's reference is the same here.
			const auto local = Protocol::IsSpawnRef(a_line.speaker) ? Spawns::LocalOf(a_line.speaker) : a_line.speaker;
			const auto actor = local != 0 ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
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

		// The plugin a form id belongs to, by its load index (a light plugin's by its FE index), or null when none loaded
		// here has it.
		const RE::TESFile* FileOfId(std::uint32_t a_formId)
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return nullptr;
			}
			if ((a_formId >> 24) == 0xFE) {
				return handler->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((a_formId >> 12) & 0xFFF));
			}
			return handler->LookupLoadedModByIndex(static_cast<std::uint8_t>(a_formId >> 24));
		}

		// The voice file of a friend's line, resolved by us from what the line names: only the game's
		// own infos and voice types (Official.h), as for everything said or set from a friend's game.
		// The recording is named by the info's id and its plugin, so it is found even when the engine
		// has no form for the info here: a quest's dialogue is in memory as that quest needs it, and the
		// lines of a random encounter only a friend's game runs came as text alone, their files in the
		// archives all the while (the rig, 0.14.0: RESceneLC01's accusor, voice MaleEvenToned). The
		// plugin is then the id's, and official by name as the form would be by its file.
		Resolved Resolve(const Protocol::Line& a_line)
		{
			const auto info = RE::TESForm::GetFormByID<RE::TESTopicInfo>(a_line.info);
			const auto voice = RE::TESForm::GetFormByID<RE::BGSVoiceType>(a_line.voiceType);
			if (!voice || !Official::IsOfficialForm(voice) || (info && !Official::IsOfficialForm(info))) {
				return {};
			}
			if (info) {
				for (const RE::TESTopicInfo* candidate : { info, info->dataInfo }) {
					if (candidate && Official::IsOfficialForm(candidate)) {
						if (auto path = FileOf(candidate, voice, a_line.response); !path.empty() && Exists(path)) {
							return { candidate, std::move(path) };
						}
					}
				}
			}
			const auto  file = FileOfId(a_line.info);
			const char* voiceName = voice->formEditorID.c_str();
			if (!file || !voiceName || !Official::IsOfficialFile(file)) {
				return { info, {} };
			}
			if (auto path = VoicePath::Build(file->GetFilename(), voiceName, VoicePath::LocalId(a_line.info, file->IsLight()), a_line.response); !path.empty() && Exists(path)) {
				return { info, std::move(path) };
			}
			return { info, {} };
		}

		bool HasRecording(const Recording& a_recording)
		{
			return a_recording.info != 0 && !Resolve(Protocol::Line{ 0, 0, "-", a_recording.info, a_recording.response, a_recording.voiceType }).path.empty();
		}

		// The length of a voice file's recording, read from the file (loose or in an archive): only its start, the
		// lip sync and the audio's header and packet table (SubtitleTime::HeaderBytes), not the recording itself.
		std::optional<double> VoiceSecondsOf(const std::string& a_path)
		{
			RE::BSTSmartPointer<RE::BSResource::Stream> stream;
			if (a_path.empty() || RE::BSResource::GetOrCreateStream(a_path.c_str(), stream) != RE::BSResource::ErrorCode::kNone || !stream) {
				return std::nullopt;
			}
			if (stream->DoOpen() != RE::BSResource::ErrorCode::kNone) {
				return std::nullopt;
			}
			const std::uint64_t       size = stream->totalSize;
			std::vector<std::uint8_t> bytes;
			std::uint64_t             have = 0;
			const auto                readTo = [&](std::uint64_t a_want) {
				   a_want = std::min<std::uint64_t>(a_want, MAX_VOICE_READ);
				   if (size != 0) {
					   a_want = std::min(a_want, size);
				   }
				   if (a_want <= have) {
					   return;
				   }
				   bytes.resize(static_cast<std::size_t>(a_want));
				   while (have < a_want) {
					   std::uint64_t read = 0;
					   if (stream->DoRead(bytes.data() + have, a_want - have, read) != RE::BSResource::ErrorCode::kNone || read == 0) {
						   break;
					   }
					   have += read;
				   }
				   bytes.resize(static_cast<std::size_t>(have));
			};
			readTo(SubtitleTime::HEADER_PROBE);
			readTo(SubtitleTime::HeaderBytes(bytes));
			stream->DoClose();
			return SubtitleTime::VoiceSeconds(bytes);
		}

		struct Sound
		{
			RE::BSSoundHandle handle;
			std::string       path;  // the file it plays
		};

		// A sound for a voice file. The archives hold .fuz files (the recording with its lip sync); should
		// the audio manager want one of the other names a recording can go by, it is asked for those too.
		Sound SoundFor(const std::string& a_path, std::uint32_t a_flags, std::uint8_t a_priority)
		{
			Sound      sound;
			const auto audio = RE::BSAudioManager::GetSingleton();
			if (!audio || a_path.size() < 4) {
				return sound;
			}
			const auto stem = a_path.substr(0, a_path.size() - 3);
			for (const auto extension : { "fuz"sv, "wav"sv, "xwm"sv }) {
				sound.handle = {};
				sound.path = stem + std::string(extension);
				audio->GetSoundHandleByFile(sound.handle, RE::BSResource::ID(sound.path.c_str()), a_flags, a_priority);
				if (sound.handle.IsValid()) {
					break;
				}
			}
			return sound;
		}

		// Who said a line, for the log: "player N", or the voiceline dev command.
		std::string WhoSaid(std::uint64_t a_key)
		{
			const auto player = static_cast<std::uint32_t>(a_key >> 32);
			return player == DEV_PLAYER ? std::string("the voiceline command") : std::format("player {}", player);
		}

		// Ends a friend's line's sound (LineSounds): faded out and let go, whether the audio manager still says it
		// plays or not (on a sound that is over it does nothing). One still sounding at its hard end, or crowded out,
		// is logged: its recording ended before that, so the sound would have run on by itself.
		void EndSound(Sounds::Entry& a_entry, Sounds::Why a_why)
		{
			const bool sounding = a_entry.handle.IsPlaying();
			a_entry.handle.FadeOutAndRelease(VOICE_CUT_MS);
			if (!sounding || a_why == Sounds::Why::kNext) {
				return;
			}
			const bool tooLong = a_why == Sounds::Why::kTooLong;
			++(tooLong ? cutLong : cutCrowded);
			const auto now = Clock::now();
			if (cutLoggedAt && now - *cutLoggedAt < CUT_LOG_EVERY) {
				++cutUnlogged;
				return;
			}
			const auto after = std::chrono::duration<double>(now - a_entry.started).count();
			const auto speaker = static_cast<std::uint32_t>(a_entry.key);
			const auto length = a_entry.seconds ? std::format("{:.1f} s", *a_entry.seconds) : std::string("unknown");
			const auto why = tooLong ? std::string("at its end") : std::format("with {} newer ones", Sounds::MAX_PLAYING);
			const auto unlogged = cutUnlogged != 0 ? std::format("; {} more cut since the last such line", cutUnlogged) : std::string();
			REX::INFO("Conversations: the sound of a line from {} (speaker {:08X}) still played {:.1f} s after it started (its recording: {}) {}: ended ({} at the end, {} crowded out so far{})",
				WhoSaid(a_entry.key), speaker, after, length, why, cutLong, cutCrowded, unlogged);
			cutLoggedAt = now;
			cutUnlogged = 0;
		}

		// Plays a voice file at the actor and follows them while it lasts: a sound like any other, so
		// nothing of the dialogue system (no conversation, Say, scene or lip sync) is involved. It is ended
		// a little after its recording's length at the latest (LineSounds::CapFor): a_seconds, a_path's length,
		// or, should another name of the recording be what plays, that file's.
		bool Play(RE::Actor* a_actor, const std::string& a_path, std::uint64_t a_key, std::optional<double> a_seconds, std::uint32_t a_flags = VOICE_FLAGS,
			std::uint8_t a_priority = VOICE_PRIORITY)
		{
			auto sound = SoundFor(a_path, a_flags, a_priority);
			auto& handle = sound.handle;
			if (!handle.IsValid()) {
				return false;
			}
			const auto seconds = sound.path == a_path ? a_seconds : VoiceSecondsOf(sound.path);
			const auto defaults = DefaultObjects();
			if (const auto model = defaults ? defaults->GetDefaultObject<RE::BGSSoundOutput>(RE::DEFAULT_OBJECT::kDialogueOutputModel3D) : nullptr) {
				handle.SetOutputModel(model);  // how far an NPC's own lines carry, and how they fade
			}
			handle.SetPosition(a_actor->data.location);
			if (const auto root = a_actor->Get3D()) {
				handle.SetObjectToFollow(root);
			}
			if (!handle.Play()) {
				handle.Stop();  // made and never played: let go
				return false;
			}
			// A speaker says one line at a time: the one before, if still playing, is cut short. Every sound
			// ends by its hard end at the latest, and only a few play at once (LineSounds).
			lineSounds.Start(a_key, handle, Clock::now(), seconds, EndSound);
			return true;
		}

		// Says a friend's line at our copy of its speaker, if it names a recording we have (a_file: Resolve's;
		// a_seconds: its recording's length).
		void Speak(RE::Actor* a_actor, const Protocol::Line& a_line, std::uint64_t a_key, const Resolved& a_file, std::optional<double> a_seconds)
		{
			if (a_line.info == 0) {
				++unplaced;
				return;
			}
			const auto& file = a_file;
			if (file.path.empty()) {
				++missing;
				REX::DEBUG("Conversations: no voice file for {:08X} response {} voice {:08X}{}", a_line.info, a_line.response, a_line.voiceType,
					RE::TESForm::GetFormByID<RE::TESTopicInfo>(a_line.info) ? "" : " (the info is no form here)");
				return;
			}
			if (!Play(a_actor, file.path, a_key, a_seconds)) {
				++unplayed;
				REX::DEBUG("Conversations: no sound for '{}'", file.path);
				return;
			}
			++voiced;
			REX::DEBUG("Conversations: {:08X} says '{}'", a_actor->GetFormID(), file.path);
		}

		// The game's bGeneralSubtitles (Interface): whether lines said near the player, not to them, are shown.
		RE::Setting* GeneralSubtitles()
		{
			static RE::Setting* const found = [] {
				const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
				const auto inPrefs = prefs ? prefs->GetSetting("bGeneralSubtitles:Interface") : nullptr;
				const auto setting = inPrefs ? inPrefs : RE::GetINISetting("bGeneralSubtitles:Interface");
				if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
					REX::WARN("Conversations: the game's bGeneralSubtitles setting was not found; friends' lines show as HUD notices");
					return static_cast<RE::Setting*>(nullptr);
				}
				return setting;
			}();
			return found;
		}

		// Whether the game would show a friend's line in its subtitle area now: the player's General Subtitles
		// are on and they are not in a conversation of their own (the game then shows only that conversation's
		// lines, and only with Dialogue Subtitles on).
		bool SubtitleAreaOpen()
		{
			const auto setting = GeneralSubtitles();
			const auto topics = Dialogue::TopicManager();
			return RE::SubtitleManager::GetSingleton() && setting && setting->GetBinary() && topics && !topics->menuOpen;
		}

		void AddSubtitle(RE::TESObjectREFR* a_speaker, const RE::BSFixedStringCS& a_text)
		{
			using Fn = void (*)(RE::SubtitleManager*, RE::TESObjectREFR*, const RE::BSFixedStringCS&, RE::TESTopicInfo*, bool);
			static REL::Relocation<Fn> fn{ ADD_SUBTITLE };
			if (const auto manager = RE::SubtitleManager::GetSingleton()) {
				fn(manager, a_speaker, a_text, nullptr, false);
			}
		}

		void RemoveSubtitle(RE::TESObjectREFR* a_speaker)
		{
			using Fn = void (*)(RE::SubtitleManager*, RE::TESObjectREFR*);
			static REL::Relocation<Fn> fn{ REMOVE_SUBTITLE };
			if (const auto manager = RE::SubtitleManager::GetSingleton(); manager && a_speaker) {
				fn(manager, a_speaker);
			}
		}

		// Whether the engine's list holds this line of this speaker.
		bool Listed(const Subtitle& a_line)
		{
			const auto manager = RE::SubtitleManager::GetSingleton();
			if (!manager) {
				return false;
			}
			RE::BSAutoReadLock lock{ RE::SubtitleManager::GetRWLock() };
			for (const auto& entry : manager->subtitlePriorityArray) {
				const char* text = entry.subtitleText.c_str();
				if (entry.speaker == a_line.speaker && text && a_line.text == text) {
					return true;
				}
			}
			return false;
		}

		// Takes our line out of the engine's list, unless the speaker is saying a line of its own here: the game
		// takes all of its lines out when that one ends, and taking them now would cut its own short. At a session's
		// end (a_always) it comes out whatever the speaker is doing: nothing of ours would take it down later, and it
		// stayed on screen until the speaker's next line of its own ended, a minute or two (2026-11-02, the rig).
		void TakeDown(const Subtitle& a_line, bool a_always = false)
		{
			const auto ref = a_line.speaker.get();
			if (!ref) {
				if (a_always) {
					REX::INFO("Conversations: a friend's line left up at the session's end: its speaker is gone");
				}
				return;  // gone: the engine stops showing a line whose speaker it can't find
			}
			const auto actor = ref->As<RE::Actor>();
			if (actor && actor->IsTalking()) {
				if (!a_always) {
					return;
				}
				REX::INFO("Conversations: a friend's line taken down at the session's end while its speaker talks");
			}
			RemoveSubtitle(ref.get());
		}

		// Shows a friend's line in the game's subtitle area for a_for, in place of the speaker's line before it.
		// False when the game wouldn't show it now (the caller shows a HUD notice instead).
		bool ShowSubtitle(RE::Actor* a_actor, const std::string& a_text, std::uint64_t a_key, std::string a_notice, std::chrono::milliseconds a_for)
		{
			if (a_text.empty() || !SubtitleAreaOpen()) {
				return false;
			}
			const auto handle = a_actor->GetHandle();
			if (!handle) {
				return false;
			}
			for (auto it = subtitlesUp.begin(); it != subtitlesUp.end();) {  // a speaker says one line at a time
				if (it->key == a_key || it->speaker == handle) {
					TakeDown(*it);
					it = subtitlesUp.erase(it);
				} else {
					++it;
				}
			}
			const RE::BSFixedStringCS text{ a_text.c_str() };
			AddSubtitle(a_actor, text);
			const auto now = Clock::now();
			subtitlesUp.push_back({ a_key, handle, a_text, std::move(a_notice), now, now + a_for, false });
			return true;
		}

		// Each frame: a line we added and the engine didn't keep becomes a HUD notice (once), and a line whose
		// time is up comes down.
		void SubtitlesFrame()
		{
			if (subtitlesUp.empty()) {
				return;
			}
			const auto now = Clock::now();
			for (auto it = subtitlesUp.begin(); it != subtitlesUp.end();) {
				if (!it->checked && now - it->at >= SUBTITLE_CHECK) {
					it->checked = true;
					if (!Listed(*it)) {
						++dropped;
						REX::DEBUG("Conversations: the game dropped a friend's line from its subtitles; shown as a notice");
						Hud::Notify(it->notice);
						it = subtitlesUp.erase(it);
						continue;
					}
				}
				if (now >= it->until) {
					TakeDown(*it);
					it = subtitlesUp.erase(it);
					continue;
				}
				++it;
			}
		}

		void TakeDownSubtitles()
		{
			for (const auto& line : subtitlesUp) {
				TakeDown(line, true);
			}
			subtitlesUp.clear();
		}

		// A friend's line whose speaker, a spawn of theirs, had no copy here yet as it came (Apply): said now if the copy
		// stands (one line held at most HOLD_FOR: a copy that takes longer is far, or not to be placed), else let go.
		void HeldLinesFrame()
		{
			if (heldLines.empty()) {
				return;
			}
			const auto now = Clock::now();
			std::vector<Protocol::Line> due;
			for (auto it = heldLines.begin(); it != heldLines.end();) {
				const auto copy = CopyOf(it->line.speaker);
				switch (LineHold::Decide(Spawns::Expects(it->line.speaker), copy.placed, copy.loaded, now - it->since)) {
				case LineHold::Step::kDrop:
					++heldDropped;
					REX::DEBUG("Conversations: a line from player {} (speaker {:08X}, info {:08X}) held for its speaker's copy is let go: {}", it->line.playerId, it->line.speaker, it->line.info,
						!Spawns::Expects(it->line.speaker) ? "the spawn is over" : copy.placed ? "the copy's 3D did not come in time" : "no copy was placed in time");
					it = heldLines.erase(it);
					break;
				case LineHold::Step::kSay:
					due.push_back(it->line);
					it = heldLines.erase(it);
					break;
				case LineHold::Step::kHold:
					++it;
					break;
				}
			}
			for (const auto& line : due) {
				--received;  // counted once, as it came
				Apply(line);
			}
		}
	}

	void Frame(bool a_connected)
	{
		Dialogue::Frame();  // a conversation left without its speaker is ended
		SubtitlesFrame();   // friends' lines in the subtitle area: checked once, taken down when their time is up
		lineSounds.EndDue(Clock::now(), EndSound);  // friends' lines' sounds at their hard end
		HeldLinesFrame();   // a line held for a copy about to be placed is said once it stands, or let go

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
		// A friend's spawn whose copy is not standing here yet (one just shared: the copy follows in a moment, and has a
		// cell before its 3D) says its line before anyone is here to say it: held for a little, and said once the copy
		// stands with its 3D (Frame, LineHold).
		if (Protocol::IsSpawnRef(a_line.speaker)) {
			const auto copy = CopyOf(a_line.speaker);
			if (LineHold::Holds(Spawns::Expects(a_line.speaker), copy.placed, copy.loaded, heldLines.size())) {
				heldLines.push_back({ a_line, Clock::now() });
				++held;
				REX::DEBUG("Conversations: a line from player {} (speaker {:08X}, info {:08X}) is held: its speaker's copy is {} yet", a_line.playerId, a_line.speaker, a_line.info,
					copy.placed ? "placed, without its 3D" : "not placed");
				return;
			}
		}
		// Why a line ends unsaid, for the log (DEBUG): a line that reached nobody on the rig left no trace otherwise.
		const auto unsaid = [&](std::string_view a_why) {
			REX::DEBUG("Conversations: a line from player {} (speaker {:08X}, info {:08X}) is not said: {}", a_line.playerId, a_line.speaker, a_line.info, a_why);
		};
		const auto [actor, name] = FindSpeaker(a_line);
		if (!player || !actor) {
			if (a_line.speaker == 0) {
				unsaid("their stand-in is not here");
			} else {
				const auto local = Protocol::IsSpawnRef(a_line.speaker) ? Spawns::LocalOf(a_line.speaker) : a_line.speaker;
				const auto here = local != 0 ? RE::TESForm::GetFormByID<RE::Actor>(local) : nullptr;
				unsaid(!here ? "no copy of the speaker here" : here->IsDead(false) ? "the speaker is dead here" : "the speaker's copy has no 3D");
			}
			return;
		}
		if (!SameSpace(player, actor)) {
			unsaid("the speaker is in another space");
			return;
		}
		if (const float distance = actor->data.location.GetDistance(player->data.location); distance > HEAR_RANGE) {
			unsaid(std::format("the speaker is {:.0f} units away, out of hearing", distance));
			return;
		}
		// Our own copy is saying it too (or we're in this conversation ourselves): we hear it already.
		if (a_line.speaker != 0 && (actor->IsTalking() || NpcSync::TalkingTo() == actor->GetFormID())) {
			unsaid(actor->IsTalking() ? "our copy of the speaker says it itself" : "we are in this conversation ourselves");
			return;
		}
		const auto key = (static_cast<std::uint64_t>(a_line.playerId) << 32) | a_line.speaker;
		const auto now = Clock::now();
		if (Throttled(key, a_line.text, now)) {
			unsaid("the same line again, or too many lately");
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
		const auto file = a_line.info != 0 ? Resolve(a_line) : Resolved{};
		const bool spoken = Config::Get().spokenLines;
		const bool blank = SubtitleTime::Blank(a_line.text);
		const bool subtitleArea = !blank && SubtitleAreaOpen();
		// The recording's length: how long its sound may play at most, and its subtitle stays up.
		const auto seconds = (spoken || subtitleArea) && !file.path.empty() ? VoiceSecondsOf(file.path) : std::nullopt;
		if (spoken) {
			Speak(actor, a_line, key, file, seconds);
		}
		// The text goes where the game shows a nearby NPC's line, its subtitle area, for as long as the
		// recording lasts (it played in the friend's game for that long, whether we play it or not). With the
		// player's General Subtitles off, or in a conversation of their own, it is a HUD notice, as it always was.
		if (blank) {  // nothing to read: the game would drop the subtitle, and a notice would be just a name
			return;
		}
		auto notice = std::format("{}: {}", name, a_line.text);
		if (subtitleArea) {
			if (ShowSubtitle(actor, a_line.text, key, notice, SubtitleTime::ShowFor(seconds, a_line.text))) {
				++subtitled;
				timed += seconds.has_value();
				return;
			}
		}
		++noticed;
		Hud::Notify(notice);
	}

	void Reset()
	{
		TakeDownSubtitles();
		outgoing.clear();
		shown.clear();
		lineSounds.EndAll([](Sounds::Entry& a_entry) { a_entry.handle.FadeOutAndRelease(VOICE_CUT_MS); });
		cutLoggedAt.reset();
		cutUnlogged = 0;
		heldLines.clear();
	}

	std::string Describe()
	{
		return std::format("conversations: heard={} reported={} placed={} received={} shown={} voiced={} unplaced={} missing={} unplayed={} subtitled={} timed={} noticed={} dropped={} up={} sounds={} cutLong={} cutCrowded={} held={}/{} {}",
			heard, reported, placed, received, displayed, voiced, unplaced, missing, unplayed, subtitled, timed, noticed, dropped, subtitlesUp.size(), lineSounds.Kept(), cutLong,
			cutCrowded, held, heldDropped, Dialogue::Describe());
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
		const bool played = Play(a_actor, file.path, (static_cast<std::uint64_t>(DEV_PLAYER) << 32) | a_actor->GetFormID(), VoiceSecondsOf(file.path), a_flags, a_priority);
		return std::format("file='{}' played={}", file.path, played);
	}
}
