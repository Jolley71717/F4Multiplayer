#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The password challenge for a join over a connection that isn't private (UDP): the password never travels. The server
// sends a salt and a fresh random nonce; the joiner answers with HMAC-SHA256, under a key made from the password
// (PBKDF2-HMAC-SHA256 with that salt), of a fixed label and the nonce; the server compares in constant time. Only
// Windows CNG (BCrypt) does the hashing: nothing here is a hash or cipher of ours. Off Windows the functions fail (the
// game, and so the real server, are Windows only; the coverage build on Linux has no use for them).
// Nothing here logs.
namespace JoinProof
{
	inline constexpr std::size_t NONCE_SIZE = 32;
	inline constexpr std::size_t SALT_SIZE = 16;
	inline constexpr std::size_t KEY_SIZE = 32;
	inline constexpr std::size_t PROOF_SIZE = 32;
	// Slow enough that a captured answer can't be tried against a word list cheaply; the server derives its key once per
	// password, a joiner once per join (a few milliseconds).
	inline constexpr std::uint32_t ITERATIONS = 10000;

	using Nonce = std::array<std::uint8_t, NONCE_SIZE>;
	using Salt = std::array<std::uint8_t, SALT_SIZE>;
	using Key = std::array<std::uint8_t, KEY_SIZE>;
	using Proof = std::array<std::uint8_t, PROOF_SIZE>;

	// Random bytes from the system's generator; false when it fails.
	[[nodiscard]] bool Random(std::span<std::uint8_t> a_out);

	// HMAC-SHA256 (RFC 2104 / 4231).
	[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> Hmac(std::span<const std::uint8_t> a_key, std::span<const std::uint8_t> a_message);

	// PBKDF2-HMAC-SHA256 (RFC 8018), a_out.size() bytes.
	[[nodiscard]] bool Pbkdf2(std::string_view a_password, std::span<const std::uint8_t> a_salt, std::uint32_t a_iterations, std::span<std::uint8_t> a_out);

	// The key a password makes under a salt, with ITERATIONS.
	[[nodiscard]] std::optional<Key> DeriveKey(std::string_view a_password, std::span<const std::uint8_t> a_salt);

	// The answer to a nonce: HMAC-SHA256(a_key, label + nonce).
	[[nodiscard]] std::optional<Proof> Answer(const Key& a_key, std::span<const std::uint8_t> a_nonce);

	// Whether two byte strings are the same, in a time that doesn't depend on where they differ (their sizes are not secret).
	[[nodiscard]] bool Equal(std::span<const std::uint8_t> a_a, std::span<const std::uint8_t> a_b);
}
