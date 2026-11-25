#include "JoinProof.h"

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#endif

namespace JoinProof
{
#ifdef _WIN32
	namespace
	{
		// An open HMAC-SHA256 provider, closed at the end of its scope.
		class Provider
		{
		public:
			Provider() { ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)); }
			~Provider()
			{
				if (ok) {
					BCryptCloseAlgorithmProvider(handle, 0);
				}
			}
			Provider(const Provider&) = delete;
			Provider& operator=(const Provider&) = delete;

			[[nodiscard]] bool Ok() const { return ok; }
			[[nodiscard]] BCRYPT_ALG_HANDLE Get() const { return handle; }

		private:
			BCRYPT_ALG_HANDLE handle = nullptr;
			bool              ok = false;
		};
	}

	bool Random(std::span<std::uint8_t> a_out)
	{
		return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, a_out.data(), static_cast<ULONG>(a_out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
	}

	std::optional<std::array<std::uint8_t, 32>> Hmac(std::span<const std::uint8_t> a_key, std::span<const std::uint8_t> a_message)
	{
		Provider provider;
		if (!provider.Ok()) {
			return std::nullopt;
		}
		std::array<std::uint8_t, 32> out{};
		// An empty span has a null data(); CNG takes a pointer and a length.
		static constexpr std::uint8_t none = 0;
		const auto                    key = a_key.empty() ? &none : a_key.data();
		const auto                    message = a_message.empty() ? &none : a_message.data();
		if (!BCRYPT_SUCCESS(BCryptHash(provider.Get(), const_cast<PUCHAR>(key), static_cast<ULONG>(a_key.size()), const_cast<PUCHAR>(message), static_cast<ULONG>(a_message.size()), out.data(), static_cast<ULONG>(out.size())))) {
			return std::nullopt;
		}
		return out;
	}

	bool Pbkdf2(std::string_view a_password, std::span<const std::uint8_t> a_salt, std::uint32_t a_iterations, std::span<std::uint8_t> a_out)
	{
		Provider provider;
		if (!provider.Ok()) {
			return false;
		}
		static constexpr std::uint8_t none = 0;
		const auto                    password = a_password.empty() ? &none : reinterpret_cast<const std::uint8_t*>(a_password.data());
		const auto                    salt = a_salt.empty() ? &none : a_salt.data();
		return BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(provider.Get(), const_cast<PUCHAR>(password), static_cast<ULONG>(a_password.size()), const_cast<PUCHAR>(salt), static_cast<ULONG>(a_salt.size()), a_iterations, a_out.data(), static_cast<ULONG>(a_out.size()), 0));
	}
#else
	bool Random(std::span<std::uint8_t>) { return false; }
	std::optional<std::array<std::uint8_t, 32>> Hmac(std::span<const std::uint8_t>, std::span<const std::uint8_t>) { return std::nullopt; }
	bool Pbkdf2(std::string_view, std::span<const std::uint8_t>, std::uint32_t, std::span<std::uint8_t>) { return false; }
#endif

	std::optional<Key> DeriveKey(std::string_view a_password, std::span<const std::uint8_t> a_salt)
	{
		Key key{};
		if (!Pbkdf2(a_password, a_salt, ITERATIONS, key)) {
			return std::nullopt;
		}
		return key;
	}

	std::optional<Proof> Answer(const Key& a_key, std::span<const std::uint8_t> a_nonce)
	{
		static constexpr std::string_view label = "F4MP join v1";
		std::vector<std::uint8_t>         message(label.begin(), label.end());
		message.insert(message.end(), a_nonce.begin(), a_nonce.end());
		return Hmac(a_key, message);
	}

	bool Equal(std::span<const std::uint8_t> a_a, std::span<const std::uint8_t> a_b)
	{
		if (a_a.size() != a_b.size()) {
			return false;
		}
		std::uint8_t difference = 0;
		for (std::size_t i = 0; i < a_a.size(); ++i) {
			difference |= static_cast<std::uint8_t>(a_a[i] ^ a_b[i]);
		}
		return difference == 0;
	}
}
