#include "game/Voice.h"

#include "Config.h"
#include "game/ClickWatch.h"
#include "game/Hotkeys.h"
#include "game/RemotePlayers.h"
#include "game/VoiceGate.h"
#include "game/VoiceRamp.h"
#include "steam/SteamApi.h"

// winmm waveOut (windows.h clashes with CommonLibF4's names).
extern "C" {
#pragma pack(push, 1)
struct F4MP_WAVEFORMATEX
{
	std::uint16_t formatTag;
	std::uint16_t channels;
	std::uint32_t samplesPerSec;
	std::uint32_t avgBytesPerSec;
	std::uint16_t blockAlign;
	std::uint16_t bitsPerSample;
	std::uint16_t size;
};
#pragma pack(pop)

struct F4MP_WAVEHDR
{
	char*          data;
	unsigned long  bufferLength;
	unsigned long  bytesRecorded;
	std::uintptr_t user;
	unsigned long  flags;
	unsigned long  loops;
	F4MP_WAVEHDR*  next;
	std::uintptr_t reserved;
};

__declspec(dllimport) unsigned int __stdcall waveOutOpen(void** a_device, unsigned int a_deviceId, const F4MP_WAVEFORMATEX* a_format, std::uintptr_t a_callback, std::uintptr_t a_instance, unsigned long a_flags);
__declspec(dllimport) unsigned int __stdcall waveOutPrepareHeader(void* a_device, F4MP_WAVEHDR* a_header, unsigned int a_size);
__declspec(dllimport) unsigned int __stdcall waveOutUnprepareHeader(void* a_device, F4MP_WAVEHDR* a_header, unsigned int a_size);
__declspec(dllimport) unsigned int __stdcall waveOutWrite(void* a_device, F4MP_WAVEHDR* a_header, unsigned int a_size);
__declspec(dllimport) unsigned int __stdcall waveOutReset(void* a_device);
__declspec(dllimport) unsigned int __stdcall waveOutClose(void* a_device);
}

namespace Voice
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using VoiceGate::Mic;

		constexpr std::uint32_t SAMPLE_RATE = 24000;  // Steam resamples to whatever we ask for
		constexpr unsigned int  WAVE_MAPPER = 0xFFFFFFFF;
		constexpr unsigned long WHDR_DONE = 0x1;
		constexpr auto          JITTER_BUFFER = 80ms;  // silence played before a speaker starts
		constexpr std::size_t   LEAD_SAMPLES = static_cast<std::size_t>(SAMPLE_RATE * JITTER_BUFFER / 1s);
		constexpr std::size_t   MAX_QUEUED = 64;       // buffers per speaker; more means we're far behind
		constexpr std::size_t   MAX_SPARE = 32;        // played buffers kept for the next pieces
		constexpr float         FULL_VOLUME_METERS = 8.0f;
		constexpr float         UNITS_TO_METERS = 0.0142875f;
		constexpr std::uint32_t LOOPBACK_ID = 0xFFFFFFFF;

		struct Buffer
		{
			F4MP_WAVEHDR              header{};
			std::vector<std::int16_t> samples;
		};

		struct Speaker
		{
			void*                               device = nullptr;
			std::deque<std::unique_ptr<Buffer>> queued;
			Clock::time_point                   lastHeard{};
		};

		Mic                                    mic = Mic::kOff;
		Clock::time_point                      stoppedAt{};  // when the tail began
		Clock::time_point                      askedAt{};    // when Steam was last asked for our voice
		bool                                   forceTalk = false;
		bool                                   loopback = false;
		std::map<std::uint32_t, Speaker>       speakers;
		std::vector<std::unique_ptr<Buffer>>   spare;  // a friend talking sends a piece every frame or two
		std::vector<std::vector<std::uint8_t>> outgoing;
		std::vector<std::uint8_t>              captureBuffer(8192);
		std::vector<std::int16_t>              pcmBuffer(SAMPLE_RATE);  // one second
		const std::vector<std::int16_t>        silence(LEAD_SAMPLES);
		const std::vector<std::int16_t>        longSilence(LEAD_SAMPLES * 2);
		std::uint32_t                          underruns = 0;  // a friend's voice that ran out mid-speech (VoiceRamp::Underrun)

		std::uint64_t bytesSent = 0;
		std::uint64_t bytesHeard = 0;
		std::uint32_t buffersPlayed = 0;
		std::uint32_t tooFar = 0;
		std::uint32_t decodeErrors = 0;
		int           lastResult = -1;

		const SteamApi::Api* VoiceApi()
		{
			const auto api = SteamApi::Get();
			return api && api->StartVoiceRecording ? api : nullptr;
		}

		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::Actor* a_actor)
		{
			const auto cell = a_actor ? a_actor->GetParentCell() : nullptr;
			if (!cell) {
				return { 0, 0 };
			}
			if (cell->IsInterior()) {
				return { cell->GetFormID(), 0 };
			}
			return { 0, cell->worldSpace ? cell->worldSpace->GetFormID() : 0 };
		}

		// 1 up close, fading to 0 at the range; 0 when they're somewhere else.
		float VolumeFor(std::uint32_t a_id)
		{
			const float range = Config::Get().voiceRange;
			if (a_id == LOOPBACK_ID || range <= 0.0f) {
				return 1.0f;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetParentCell()) {
				return 0.0f;
			}
			const auto state = RemotePlayers::LastStateOf(a_id);
			if (!state || SpaceOf(player) != std::pair{ state->cell, state->worldspace }) {
				return 0.0f;
			}
			const RE::NiPoint3 position{ state->x, state->y, state->z };
			const float        meters = player->data.location.GetDistance(position) * UNITS_TO_METERS;
			if (meters <= FULL_VOLUME_METERS) {
				return 1.0f;
			}
			return std::clamp(1.0f - (meters - FULL_VOLUME_METERS) / (std::max)(range - FULL_VOLUME_METERS, 1.0f), 0.0f, 1.0f);
		}

		void Close(Speaker& a_speaker)
		{
			if (!a_speaker.device) {
				return;
			}
			waveOutReset(a_speaker.device);
			for (auto& buffer : a_speaker.queued) {
				waveOutUnprepareHeader(a_speaker.device, &buffer->header, sizeof(F4MP_WAVEHDR));
			}
			a_speaker.queued.clear();
			waveOutClose(a_speaker.device);
			a_speaker.device = nullptr;
		}

		// Frees buffers the device has finished playing.
		void Recycle(Speaker& a_speaker)
		{
			while (!a_speaker.queued.empty() && (a_speaker.queued.front()->header.flags & WHDR_DONE) != 0) {
				waveOutUnprepareHeader(a_speaker.device, &a_speaker.queued.front()->header, sizeof(F4MP_WAVEHDR));
				if (spare.size() < MAX_SPARE) {
					spare.push_back(std::move(a_speaker.queued.front()));
				}
				a_speaker.queued.pop_front();
				++buffersPlayed;
			}
		}

		bool Queue(Speaker& a_speaker, std::span<const std::int16_t> a_samples)
		{
			std::unique_ptr<Buffer> buffer;
			if (spare.empty()) {
				buffer = std::make_unique<Buffer>();
			} else {
				buffer = std::move(spare.back());
				spare.pop_back();
				buffer->header = {};
			}
			buffer->samples.assign(a_samples.begin(), a_samples.end());
			buffer->header.data = reinterpret_cast<char*>(buffer->samples.data());
			buffer->header.bufferLength = static_cast<unsigned long>(buffer->samples.size() * sizeof(std::int16_t));
			if (waveOutPrepareHeader(a_speaker.device, &buffer->header, sizeof(F4MP_WAVEHDR)) != 0) {
				return false;
			}
			if (waveOutWrite(a_speaker.device, &buffer->header, sizeof(F4MP_WAVEHDR)) != 0) {
				waveOutUnprepareHeader(a_speaker.device, &buffer->header, sizeof(F4MP_WAVEHDR));
				return false;
			}
			a_speaker.queued.push_back(std::move(buffer));
			return true;
		}

		void Play(std::uint32_t a_id, std::span<const std::uint8_t> a_compressed)
		{
			const auto api = VoiceApi();
			if (!api) {
				return;
			}
			const float volume = VolumeFor(a_id);
			if (volume < 0.02f) {
				++tooFar;
				return;
			}

			std::uint32_t written = 0;
			const int     result = api->DecompressVoice(api->user, a_compressed.data(), static_cast<std::uint32_t>(a_compressed.size()),
					pcmBuffer.data(), static_cast<std::uint32_t>(pcmBuffer.size() * sizeof(std::int16_t)), &written, SAMPLE_RATE);
			if (result != SteamApi::VOICE_OK || written < sizeof(std::int16_t)) {
				++decodeErrors;
				return;
			}
			const std::span samples{ pcmBuffer.data(), written / sizeof(std::int16_t) };
			if (volume < 1.0f) {
				for (auto& sample : samples) {
					sample = static_cast<std::int16_t>(static_cast<float>(sample) * volume);
				}
			}

			auto&      speaker = speakers[a_id];
			const auto now = Clock::now();
			const bool heardBefore = speaker.lastHeard != Clock::time_point{};
			const auto sinceLast = now - speaker.lastHeard;
			speaker.lastHeard = now;
			if (!speaker.device) {
				F4MP_WAVEFORMATEX format{};
				format.formatTag = 1;  // PCM
				format.channels = 1;
				format.samplesPerSec = SAMPLE_RATE;
				format.bitsPerSample = 16;
				format.blockAlign = 2;
				format.avgBytesPerSec = SAMPLE_RATE * 2;
				if (waveOutOpen(&speaker.device, WAVE_MAPPER, &format, 0, 0, 0) != 0) {
					speaker.device = nullptr;
					REX::WARN("Voice: could not open an audio output");
					return;
				}
			}
			Recycle(speaker);
			if (speaker.queued.size() >= MAX_QUEUED) {
				return;  // far behind (the game was paused); drop until it catches up
			}
			// Starting from silence: a little lead so the next pieces arrive before this one ends, and the piece faded in
			// so it doesn't start with a click (VoiceRamp). After an underrun (the last piece ran out before this one
			// came) the lead is twice as long, so a late connection doesn't starve the device piece after piece.
			if (speaker.queued.empty()) {
				const bool underrun = VoiceRamp::Underrun(true, heardBefore, sinceLast);
				underruns += underrun;
				ClickWatch::Note(ClickRates::Source::kVoiceStart);
				if (underrun) {
					ClickWatch::Note(ClickRates::Source::kVoiceUnderrun);
				}
				Queue(speaker, underrun ? longSilence : silence);
				VoiceRamp::FadeIn(samples);
			}
			Queue(speaker, samples);
		}

		bool TalkKeyHeld()
		{
			const auto& settings = Config::Get();
			return forceTalk || settings.voiceOpenMic || Hotkeys::Held(settings.keyVoice);
		}

		void Capture(const SteamApi::Api& a_api, bool a_mayTalk, bool a_inSession)
		{
			const bool talk = a_mayTalk && TalkKeyHeld();
			const auto now = Clock::now();
			switch (VoiceGate::Next(mic, talk)) {
			case VoiceGate::Change::kStart:
				a_api.StartVoiceRecording(a_api.user);
				a_api.SetInGameVoiceSpeaking(a_api.friends, a_api.GetSteamID(a_api.user), true);
				mic = Mic::kOn;
				break;
			case VoiceGate::Change::kStop:
				// Steam still hands out the last bit of speech after this.
				a_api.StopVoiceRecording(a_api.user);
				a_api.SetInGameVoiceSpeaking(a_api.friends, a_api.GetSteamID(a_api.user), false);
				mic = Mic::kTail;
				stoppedAt = now;
				break;
			case VoiceGate::Change::kNone:
				break;
			}
			if (!VoiceGate::Ask(mic, now - askedAt)) {
				return;
			}
			askedAt = now;

			std::uint32_t available = 0;
			lastResult = a_api.GetAvailableVoice(a_api.user, &available, nullptr, 0);
			if (mic == Mic::kTail && VoiceGate::TailOver(lastResult, now - stoppedAt)) {
				mic = Mic::kOff;
			}
			if (lastResult != SteamApi::VOICE_OK || available == 0) {
				return;
			}
			// After a stall (a loading screen) Steam can hold more than the buffer; a short buffer
			// would make GetVoice fail every frame without taking anything.
			if (available > captureBuffer.size()) {
				captureBuffer.resize((std::min)(static_cast<std::size_t>(available), std::size_t{ 1 } << 20));
			}
			std::uint32_t written = 0;
			lastResult = a_api.GetVoice(a_api.user, true, captureBuffer.data(), static_cast<std::uint32_t>(captureBuffer.size()), &written, false, nullptr, 0, nullptr, 0);
			if (lastResult != SteamApi::VOICE_OK || written == 0 || written > Protocol::MAX_VOICE_BYTES) {
				return;
			}
			const std::span<const std::uint8_t> piece{ captureBuffer.data(), written };
			if (a_inSession) {
				outgoing.push_back(Protocol::Encode(Protocol::VoiceData{ 0, { piece.begin(), piece.end() } }, Protocol::MessageType::kVoice));
				bytesSent += written;
			}
			if (loopback) {
				Play(LOOPBACK_ID, piece);
			}
		}
	}

	void Frame(bool a_inSession)
	{
		const bool mayTalk = VoiceGate::MayTalk(Config::Get().voiceChat, a_inSession, RemotePlayers::Any(), loopback);
		const bool outputsOpen = std::ranges::any_of(speakers, [](const auto& a_entry) { return a_entry.second.device != nullptr; });
		if (VoiceGate::Idle(mayTalk, mic, outputsOpen)) {
			return;
		}
		const auto api = VoiceApi();
		if (!api) {
			return;
		}
		if (mayTalk || mic != Mic::kOff) {
			Capture(*api, mayTalk, a_inSession);
		}

		// Close devices of players who stopped talking a while ago.
		if (outputsOpen) {
			const auto now = Clock::now();
			for (auto& [id, speaker] : speakers) {
				Recycle(speaker);
				if (speaker.device && speaker.queued.empty() && now - speaker.lastHeard > 30s) {
					Close(speaker);
				}
			}
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Apply(const Protocol::VoiceData& a_voice)
	{
		if (!Config::Get().voiceChat) {
			return;
		}
		bytesHeard += a_voice.data.size();
		Play(a_voice.playerId, a_voice.data);
	}

	void OnPlayerLeft(std::uint32_t a_id)
	{
		if (const auto it = speakers.find(a_id); it != speakers.end()) {
			Close(it->second);
			speakers.erase(it);
		}
	}

	void Reset()
	{
		if (const auto api = VoiceApi(); api && mic == Mic::kOn) {
			api->StopVoiceRecording(api->user);
			api->SetInGameVoiceSpeaking(api->friends, api->GetSteamID(api->user), false);
			// Frame still takes the tail Steam records on, out of session so it goes nowhere, rather than
			// leave it to come out at the start of the next thing we say.
			mic = Mic::kTail;
			stoppedAt = Clock::now();
		}
		for (auto& [id, speaker] : speakers) {
			Close(speaker);
		}
		speakers.clear();
		spare.clear();
		outgoing.clear();
	}

	void SetForceTalk(bool a_on)
	{
		forceTalk = a_on;
	}

	void SetLoopback(bool a_on)
	{
		loopback = a_on;
	}

	std::string Describe()
	{
		std::size_t open = 0;
		for (const auto& [id, speaker] : speakers) {
			open += speaker.device != nullptr;
		}
		return std::format("voice: available={} recording={} tail={} loopback={} sent={} heard={} played={} tooFar={} decodeErrors={} underruns={} devices={} last={}",
			VoiceApi() != nullptr, mic == Mic::kOn, mic == Mic::kTail, loopback, bytesSent, bytesHeard, buffersPlayed, tooFar, decodeErrors, underruns, open, lastResult);
	}
}
