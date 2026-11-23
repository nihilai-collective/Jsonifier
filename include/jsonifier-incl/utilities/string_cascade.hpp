/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/string_cascade.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/string_utils.hpp>

namespace jsonifier::internal {

	template<uint64_t width> struct cascade_register_type {
		using type = simd_register_t<width>;
	};

	template<uint64_t width> using cascade_validator = utf8_register_validator<cascade_register_type<width>>;

	inline constexpr uint64_t cascadeWidestBytes{ simdBytesPerRegister };
	inline constexpr uint64_t cascadeMidBytes{ simdBytesPerRegister < 32 ? simdBytesPerRegister : 32 };
	inline constexpr uint64_t cascadeSwarHighs{ 0x8080808080808080ull };
	inline constexpr uint64_t cascadeSwarLows{ 0x0101010101010101ull };
	inline constexpr uint64_t cascadeNoStop{ std::numeric_limits<uint64_t>::max() };
	inline constexpr uint64_t cascadeSwarBytes{ 8 };

	enum class cascade_status : uint8_t {
		proceed,
		done,
		failed,
	};

	struct cascade_scan_result {
		uint64_t outLength{ std::numeric_limits<uint64_t>::max() };
		uint64_t rawLength{};
	};

	template<uint64_t width> struct cascade_lanes {
		using simd_type = simd_register_t<width>;

		JSONIFIER_INLINE static simd_type load(const uint8_t* src) noexcept {
			return simd::gatherValuesU<simd_type>(src);
		}

		JSONIFIER_INLINE static simd_type splat(const uint8_t value) noexcept {
			return simd::gatherValue<simd_type>(static_cast<char>(value));
		}

		JSONIFIER_INLINE static uint64_t bits(const simd_type values) noexcept {
			if constexpr (width == 64) {
				return static_cast<uint64_t>(simd::opBitMask(values));
			} else {
				return static_cast<uint64_t>(static_cast<uint32_t>(simd::opBitMask(values)));
			}
		}

		JSONIFIER_INLINE static uint64_t escapes(const simd_type values) noexcept {
			return bits(simd::opOr(simd::opCmpEqRaw(values, splat(uint_c_v<'\\'>)), simd::opCmpLtRaw(values, splat(0x20))));
		}

		JSONIFIER_INLINE static uint64_t stops(const simd_type values) noexcept {
			return escapes(values) | bits(simd::opCmpEqRaw(values, splat(uint_c_v<'"'>)));
		}

		JSONIFIER_INLINE static uint64_t below(const uint64_t limit) noexcept {
			return limit < width ? (uint64_t{ 1 } << limit) - 1 : std::numeric_limits<uint64_t>::max();
		}

		JSONIFIER_INLINE static simd_type keepThrough(const simd_type values, const uint64_t lastLane) noexcept {
			const simd_type spaces{ splat(0x20) };
			const simd_type keep{ simd::opCmpLtRaw(simd::gatherValues<simd_type>(std::bit_cast<const simd_type*>(+tail_block_builder::laneIndices)),
				splat(static_cast<uint8_t>(lastLane + 1))) };
			return simd::opXor(spaces, simd::opAnd(simd::opXor(values, spaces), keep));
		}
	};

	template<uint64_t width> struct cascade_guarded_load {
		JSONIFIER_NOINLINE static simd_register_t<width> copyLoad(const uint8_t* src, const uint8_t* docEnd) noexcept {
			alignas(width) uint8_t buffer[width]{};
			const uint64_t available{ docEnd > src ? static_cast<uint64_t>(docEnd - src) : 0 };
			memcpyWrapper(buffer, src, available < width ? available : width);
			return simd::gatherValues<simd_register_t<width>>(std::bit_cast<const simd_register_t<width>*>(+buffer));
		}

		JSONIFIER_INLINE static simd_register_t<width> impl(const uint8_t* src, const uint8_t* docEnd) noexcept {
			if (static_cast<uint64_t>(docEnd - src) >= width) [[likely]] {
				return cascade_lanes<width>::load(src);
			}
			return copyLoad(src, docEnd);
		}
	};

	struct cascade_swar {
		JSONIFIER_NOINLINE static uint64_t copyLoad(const uint8_t* src, const uint8_t* docEnd) noexcept {
			uint64_t value{};
			const uint64_t available{ docEnd > src ? static_cast<uint64_t>(docEnd - src) : 0 };
			memcpyWrapper(&value, src, available < sizeof(value) ? available : sizeof(value));
			return value;
		}

		JSONIFIER_INLINE static uint64_t load(const uint8_t* src, const uint8_t* docEnd) noexcept {
			if (static_cast<uint64_t>(docEnd - src) >= sizeof(uint64_t)) [[likely]] {
				uint64_t value;
				pow2MemcpyWrapper<sizeof(value)>(&value, src);
				return value;
			}
			return copyLoad(src, docEnd);
		}

		JSONIFIER_INLINE static void store(const uint64_t value, uint8_t* dst) noexcept {
			pow2MemcpyWrapper<sizeof(value)>(dst, &value);
		}

		JSONIFIER_INLINE static uint64_t hasByte(const uint64_t value, const uint8_t target) noexcept {
			const uint64_t matched{ value ^ (cascadeSwarLows * target) };
			return (matched - cascadeSwarLows) & ~matched & cascadeSwarHighs;
		}

		JSONIFIER_INLINE static uint64_t hasLess(const uint64_t value, const uint8_t bound) noexcept {
			return (value - cascadeSwarLows * bound) & ~value & cascadeSwarHighs;
		}

		JSONIFIER_INLINE static uint64_t escapes(const uint64_t value) noexcept {
			return hasByte(value, uint_c_v<'\\'>) | hasLess(value, 0x20);
		}

		JSONIFIER_INLINE static uint64_t stops(const uint64_t value) noexcept {
			return escapes(value) | hasByte(value, uint_c_v<'"'>);
		}

		JSONIFIER_INLINE static uint64_t below(const uint64_t limit) noexcept {
			return (uint64_t{ 1 } << (limit * 8)) - 1;
		}

		JSONIFIER_INLINE static uint64_t through(const uint64_t limit) noexcept {
			return (uint64_t{ 2 } << (limit * 8 + 7)) - 1;
		}

		JSONIFIER_INLINE static uint64_t first(const uint64_t mask) noexcept {
			return mask != 0 ? static_cast<uint64_t>(std::countr_zero(mask)) >> 3 : cascadeNoStop;
		}
	};

	struct cascade_unescaper {
		JSONIFIER_INLINE static bool impl(const uint8_t*& src, const uint8_t* srcEnd, uint8_t*& dst) noexcept {
			if (*src != uint_c_v<'\\'> || srcEnd - src < 2) [[unlikely]] {
				return false;
			}
			if (src[1] == uint_c_v<'u'>) {
				return handleUnicodeCodePoint(src, dst, srcEnd);
			}
			const uint8_t decoded{ static_cast<uint8_t>(escapeMap[src[1]]) };
			*dst = decoded;
			++dst;
			src += 2;
			return decoded != 0;
		}
	};

	struct cascade_tail_copy {
		template<uint64_t bytes> JSONIFIER_INLINE static void move(const uint8_t* src, uint8_t* dst) noexcept {
			pow2MemcpyWrapper<bytes>(dst, src);
		}

		JSONIFIER_INLINE static void impl(const uint8_t* src, uint8_t* dst, const uint64_t count) noexcept {
			switch (std::bit_width(count)) {
				case 4:
					move<8>(src, dst);
					move<8>(src + count - 8, dst + count - 8);
					return;
				case 3:
					move<4>(src, dst);
					move<4>(src + count - 4, dst + count - 4);
					return;
				case 2:
					move<2>(src, dst);
					move<2>(src + count - 2, dst + count - 2);
					return;
				case 1:
					*dst = *src;
					return;
				default:
					return;
			}
		}
	};

	struct cascade_bounded_loader {
		template<uint64_t width> JSONIFIER_INLINE static simd_register_t<width> vector(const uint8_t* src, const uint8_t* docEnd) noexcept {
			return cascade_guarded_load<width>::impl(src, docEnd);
		}

		JSONIFIER_INLINE static uint64_t swar(const uint8_t* src, const uint8_t* docEnd) noexcept {
			return cascade_swar::load(src, docEnd);
		}
	};

	struct cascade_page_safe_loader {
		JSONIFIER_INLINE static bool direct(const uint8_t* src, const uint8_t* docEnd, const uint64_t width) noexcept {
			const uint64_t available{ static_cast<uint64_t>(docEnd - src) };
			return available >= width || tail_block_builder::readStaysInPage(src, available, width);
		}

		template<uint64_t width> JSONIFIER_INLINE static simd_register_t<width> vector(const uint8_t* src, const uint8_t* docEnd) noexcept {
			if (direct(src, docEnd, width)) [[likely]] {
				return cascade_lanes<width>::load(src);
			}
			return cascade_guarded_load<width>::copyLoad(src, docEnd);
		}

		JSONIFIER_INLINE static uint64_t swar(const uint8_t* src, const uint8_t* docEnd) noexcept {
			if (direct(src, docEnd, cascadeSwarBytes)) [[likely]] {
				uint64_t value;
				pow2MemcpyWrapper<sizeof(value)>(&value, src);
				return value;
			}
			return cascade_swar::copyLoad(src, docEnd);
		}
	};

	template<bool repeat> struct cascade_repeat {
		JSONIFIER_INLINE static bool impl(const uint8_t* src, const uint8_t* srcEnd) noexcept {
			return src < srcEnd;
		}
	};

	template<> struct cascade_repeat<false> {
		JSONIFIER_INLINE static bool impl(const uint8_t*, const uint8_t*) noexcept {
			return false;
		}
	};

	template<uint64_t width> struct cascade_exact_loop {
		JSONIFIER_INLINE static bool impl(const uint8_t*& src, const uint8_t* contentEnd, uint8_t*& dst, utf8_validation_state& state) noexcept {
			using lane = cascade_lanes<width>;
			cascade_validator<width> validator{ state };
			while (static_cast<uint64_t>(contentEnd - src) >= width) {
				const simd_register_t<width> values{ lane::load(src) };
				validator.checkRegister(values);
				simd::storeU(values, dst);
				const uint64_t stopMask{ lane::stops(values) };
				if (stopMask != 0) {
					const uint64_t offset{ static_cast<uint64_t>(std::countr_zero(stopMask)) };
					src += offset;
					dst += offset;
					if (validator.hardErrors() || !cascade_unescaper::impl(src, contentEnd, dst)) [[unlikely]] {
						return false;
					}
					validator.reset();
					continue;
				}
				src += width;
				dst += width;
			}
			validator.flush();
			return !state.error;
		}
	};

	template<typename loader> struct cascade_exact_tail {
		JSONIFIER_INLINE static uint64_t vectorOffset(const uint8_t* src, const uint8_t* docEnd, utf8_validation_state& state, const uint64_t remaining, bool& valid) noexcept {
			using lane = cascade_lanes<16>;
			cascade_validator<16> validator{ state };
			const simd_register_t<16> values{ loader::template vector<16>(src, docEnd) };
			validator.checkRegister(lane::keepThrough(values, remaining));
			valid					= !validator.hardErrors();
			const uint64_t stopMask{ lane::stops(values) & lane::below(remaining) };
			return stopMask != 0 ? static_cast<uint64_t>(std::countr_zero(stopMask)) : cascadeNoStop;
		}

		JSONIFIER_INLINE static uint64_t offset(const uint8_t* src, const uint8_t* docEnd, utf8_validation_state& state, const uint64_t remaining, bool& valid) noexcept {
			if (remaining < cascadeSwarBytes) {
				const uint64_t word{ loader::swar(src, docEnd) };
				if ((word & cascadeSwarHighs & cascade_swar::through(remaining)) == 0) {
					valid = !state.prevIncomplete && !state.error;
					return cascade_swar::first(cascade_swar::stops(word) & cascade_swar::below(remaining));
				}
			}
			return vectorOffset(src, docEnd, state, remaining, valid);
		}

		JSONIFIER_INLINE static uint8_t* impl(const uint8_t* src, const uint8_t* contentEnd, const uint8_t* docEnd, uint8_t* dst, utf8_validation_state& state) noexcept {
			while (true) {
				const uint64_t remaining{ static_cast<uint64_t>(contentEnd - src) };
				bool valid{};
				const uint64_t stop{ offset(src, docEnd, state, remaining, valid) };
				if (!valid) [[unlikely]] {
					return nullptr;
				}
				cascade_tail_copy::impl(src, dst, stop != cascadeNoStop ? stop : remaining);
				if (stop == cascadeNoStop) {
					return dst + remaining;
				}
				src += stop;
				dst += stop;
				if (!cascade_unescaper::impl(src, contentEnd, dst)) [[unlikely]] {
					return nullptr;
				}
				state.reset();
			}
		}
	};

	template<uint64_t width, bool repeat> struct cascade_open_scan {
		JSONIFIER_INLINE static cascade_status impl(const uint8_t*& src, const uint8_t* docEnd, uint8_t*& dst, utf8_validation_state& state) noexcept {
			using lane = cascade_lanes<width>;
			cascade_validator<width> validator{ state };
			do {
				const simd_register_t<width> values{ cascade_guarded_load<width>::impl(src, docEnd) };
				simd::storeU(values, dst);
				const uint64_t stopMask{ lane::stops(values) };
				if (stopMask != 0) {
					const uint64_t offset{ static_cast<uint64_t>(std::countr_zero(stopMask)) };
					validator.checkRegister(lane::keepThrough(values, offset));
					src += offset;
					dst += offset;
					if (validator.hardErrors() || src >= docEnd) [[unlikely]] {
						return cascade_status::failed;
					}
					if (*src == uint_c_v<'"'>) {
						return cascade_status::done;
					}
					if (!cascade_unescaper::impl(src, docEnd, dst)) [[unlikely]] {
						return cascade_status::failed;
					}
					validator.reset();
					continue;
				}
				validator.checkRegister(values);
				src += width;
				dst += width;
			} while (cascade_repeat<repeat>::impl(src, docEnd));
			validator.flush();
			return state.error ? cascade_status::failed : cascade_status::proceed;
		}
	};

	struct cascade_open_swar {
		JSONIFIER_INLINE static cascade_status impl(const uint8_t*& src, const uint8_t* docEnd, uint8_t*& dst) noexcept {
			const uint64_t word{ cascade_swar::load(src, docEnd) };
			const uint64_t stopMask{ cascade_swar::stops(word) };
			const uint64_t lanesUsed{ stopMask != 0 ? cascade_swar::through(static_cast<uint64_t>(std::countr_zero(stopMask)) >> 3) : std::numeric_limits<uint64_t>::max() };
			if ((word & cascadeSwarHighs & lanesUsed) != 0) {
				return cascade_status::proceed;
			}
			cascade_swar::store(word, dst);
			if (stopMask == 0) {
				src += cascadeSwarBytes;
				dst += cascadeSwarBytes;
				return cascade_status::proceed;
			}
			const uint64_t offset{ static_cast<uint64_t>(std::countr_zero(stopMask)) >> 3 };
			src += offset;
			dst += offset;
			if (src >= docEnd) [[unlikely]] {
				return cascade_status::failed;
			}
			if (*src == uint_c_v<'"'>) {
				return cascade_status::done;
			}
			return cascade_unescaper::impl(src, docEnd, dst) ? cascade_status::proceed : cascade_status::failed;
		}
	};

	struct string_cascade {
		template<typename loader = cascade_bounded_loader> JSONIFIER_INLINE static uint8_t* exactImpl(const uint8_t* src, const uint8_t* contentEnd, const uint8_t* docEnd, uint8_t* dst) noexcept {
			utf8_validation_state state{};
			if (!cascade_exact_loop<cascadeWidestBytes>::impl(src, contentEnd, dst, state)) [[unlikely]] {
				return nullptr;
			}
			if constexpr (cascadeMidBytes < cascadeWidestBytes) {
				if (!cascade_exact_loop<cascadeMidBytes>::impl(src, contentEnd, dst, state)) [[unlikely]] {
					return nullptr;
				}
			}
			if constexpr (16 < cascadeMidBytes) {
				if (!cascade_exact_loop<16>::impl(src, contentEnd, dst, state)) [[unlikely]] {
					return nullptr;
				}
			}
			return cascade_exact_tail<loader>::impl(src, contentEnd, docEnd, dst, state);
		}

		JSONIFIER_INLINE static uint8_t* openImpl(const uint8_t*& src, const uint8_t* docEnd, uint8_t* dst) noexcept {
			utf8_validation_state state{};
			while (src < docEnd) {
				cascade_status status{ cascade_open_swar::impl(src, docEnd, dst) };
				if (status == cascade_status::proceed) {
					status = cascade_open_scan<16, false>::impl(src, docEnd, dst, state);
				}
				if constexpr (16 < cascadeMidBytes) {
					if (status == cascade_status::proceed) {
						status = cascade_open_scan<cascadeMidBytes, false>::impl(src, docEnd, dst, state);
					}
				}
				if (status == cascade_status::proceed) {
					status = cascade_open_scan<cascadeWidestBytes, true>::impl(src, docEnd, dst, state);
				}
				if (status == cascade_status::done) {
					return dst;
				}
				if (status == cascade_status::failed) [[unlikely]] {
					return nullptr;
				}
				state.reset();
			}
			return nullptr;
		}

		template<typename loader = cascade_bounded_loader, typename char_type>
		JSONIFIER_INLINE static char_type* exact(const uint8_t* src, const uint8_t* contentEnd, const uint8_t* docEnd, char_type* dst) noexcept {
			uint8_t* const begin{ std::bit_cast<uint8_t*>(dst) };
			uint8_t* const finish{ exactImpl<loader>(src, contentEnd, docEnd, begin) };
			return finish != nullptr ? dst + (finish - begin) : nullptr;
		}

		template<typename char_type> JSONIFIER_INLINE static cascade_scan_result open(const uint8_t* src, const uint8_t* docEnd, char_type* dst) noexcept {
			const uint8_t* close{ src };
			uint8_t* const begin{ std::bit_cast<uint8_t*>(dst) };
			uint8_t* const finish{ openImpl(close, docEnd, begin) };
			if (finish == nullptr) [[unlikely]] {
				return {};
			}
			return { static_cast<uint64_t>(finish - begin), static_cast<uint64_t>(close - src) };
		}

		JSONIFIER_INLINE static const uint8_t* findClosingQuote(const uint8_t* openQuote, const uint8_t* bound) noexcept {
			while (static_cast<uint64_t>(bound - openQuote) > 16) {
				const uint64_t quotes{ cascade_lanes<16>::bits(simd::opCmpEqRaw(cascade_lanes<16>::load(bound - 16), cascade_lanes<16>::splat(uint_c_v<'"'>))) };
				if (quotes != 0) {
					return bound - 16 + (std::bit_width(quotes) - 1);
				}
				bound -= 16;
			}
			while (*--bound != uint_c_v<'"'>) {
			}
			return bound;
		}
	};

	template<typename string_type, typename loader> struct cascade_exact_context {
		cascade_exact_context() noexcept = delete;

		JSONIFIER_INLINE cascade_exact_context(const uint8_t* srcNew, const uint8_t* contentEndNew, const uint8_t* docEndNew, bool& successNew) noexcept
			: contentEnd{ contentEndNew }, docEnd{ docEndNew }, src{ srcNew }, success{ successNew } {
		}

		cascade_exact_context& operator=(cascade_exact_context&&) noexcept		= delete;
		cascade_exact_context(cascade_exact_context&&) noexcept					= delete;
		cascade_exact_context& operator=(const cascade_exact_context&) noexcept = delete;
		cascade_exact_context(const cascade_exact_context&) noexcept			= delete;

		JSONIFIER_INLINE_EXCEPT_GCC uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			const auto finish{ string_cascade::exact<loader>(src, contentEnd, docEnd, ptrNew) };
			success = finish != nullptr;
			return success ? static_cast<uint64_t>(finish - ptrNew) : uint64_t{};
		}

		const uint8_t* contentEnd{};
		const uint8_t* docEnd{};
		const uint8_t* src{};
		bool& success;
	};

	template<typename loader = cascade_bounded_loader, typename string_type>
	JSONIFIER_INLINE bool assignExactString(string_type& value, const uint8_t* src, const uint8_t* contentEnd, const uint8_t* docEnd) {
		const uint64_t rawLength{ static_cast<uint64_t>(contentEnd - src) };
		if constexpr (resize_and_overwriteable_types<string_type>) {
			bool success{};
			value.resize_and_overwrite(rawLength, cascade_exact_context<string_type, loader>{ src, contentEnd, docEnd, success });
			return success;
		} else {
			if constexpr (resizeable_types<string_type>) {
				value.resize(rawLength);
			}
			const auto finish{ string_cascade::exact<loader>(src, contentEnd, docEnd, value.data()) };
			if (finish == nullptr) [[unlikely]] {
				return false;
			}
			if constexpr (resizeable_types<string_type>) {
				value.resize(static_cast<uint64_t>(finish - value.data()));
			}
			return true;
		}
	}

	template<bool minified, typename string_type>
	JSONIFIER_INLINE bool assignBoundedString(string_type& value, const uint8_t* src, const uint8_t* bound, const uint8_t* docEnd, uint64_t& rawLength) {
		const uint8_t* close;
		if constexpr (minified) {
			close = bound - 1;
		} else {
			close = string_cascade::findClosingQuote(src - 1, bound);
		}
		if (close < src || *close != uint_c_v<'"'>) [[unlikely]] {
			return false;
		}
		rawLength = static_cast<uint64_t>(close - src);
		return assignExactString(value, src, close, docEnd);
	}

	template<uint64_t width> struct cascade_quote_scan {
		JSONIFIER_INLINE static bool impl(const uint8_t* src, const uint8_t* docEnd, uint64_t& rawLength, bool& nonAscii, bool& hasEscapes) noexcept {
			using lane = cascade_lanes<width>;
			const uint8_t* const start{ src };
			simd_register_t<width> highs{};
			uint64_t carryLocal{};
			while (src < docEnd) {
				const simd_register_t<width> values{ cascade_guarded_load<width>::impl(src, docEnd) };
				uint64_t stopMask{ lane::stops(values) & ~carryLocal };
				carryLocal = 0;
				if (stopMask != 0) {
					const uint64_t quotes{ lane::bits(simd::opCmpEqRaw(values, lane::splat(uint_c_v<'"'>))) };
					const uint64_t slashes{ lane::bits(simd::opCmpEqRaw(values, lane::splat(uint_c_v<'\\'>))) };
					do {
						const uint64_t offset{ static_cast<uint64_t>(std::countr_zero(stopMask)) };
						const uint64_t bit{ uint64_t{ 1 } << offset };
						if ((quotes & bit) != 0) {
							nonAscii  = lane::bits(simd::opOr(highs, lane::keepThrough(values, offset))) != 0;
							rawLength = static_cast<uint64_t>(src - start) + offset;
							return src + offset < docEnd;
						}
						if ((slashes & bit) == 0) [[unlikely]] {
							return false;
						}
						hasEscapes = true;
						carryLocal = static_cast<uint64_t>(offset + 1 == width);
						stopMask &= ~(bit | (bit << 1));
					} while (stopMask != 0);
				}
				highs = simd::opOr(highs, values);
				src += width;
			}
			return false;
		}
	};

	template<uint64_t width> struct cascade_span_validator {
		JSONIFIER_INLINE static bool impl(const uint8_t* src, const uint8_t* srcEnd, const uint8_t* docEnd) noexcept {
			using lane = cascade_lanes<width>;
			utf8_validation_state state{};
			cascade_validator<width> validator{ state };
			while (static_cast<uint64_t>(srcEnd - src) >= width) {
				validator.checkRegister(lane::load(src));
				src += width;
			}
			const uint64_t remaining{ static_cast<uint64_t>(srcEnd - src) };
			if (remaining != 0) {
				validator.checkRegister(lane::keepThrough(cascade_guarded_load<width>::impl(src, docEnd), remaining - 1));
			}
			validator.flush();
			return !state.error && !state.prevIncomplete;
		}
	};

	template<typename string_type> struct cascade_copy_context {
		JSONIFIER_INLINE_EXCEPT_GCC uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			jsonifierMemcpy(ptrNew, src, length);
			return length;
		}

		const uint8_t* src{};
		uint64_t length{};
	};

	template<typename string_type> JSONIFIER_INLINE bool assignOpenString(string_type& value, const uint8_t* src, const uint8_t* docEnd, uint64_t& rawLength) {
		bool hasEscapes{};
		bool nonAscii{};
		if (!cascade_quote_scan<cascadeWidestBytes>::impl(src, docEnd, rawLength, nonAscii, hasEscapes)) [[unlikely]] {
			return false;
		}
		if (hasEscapes) [[unlikely]] {
			return assignExactString(value, src, src + rawLength, docEnd);
		}
		if (nonAscii && !cascade_span_validator<cascadeWidestBytes>::impl(src, src + rawLength, docEnd)) [[unlikely]] {
			return false;
		}
		if constexpr (resize_and_overwriteable_types<string_type>) {
			value.resize_and_overwrite(rawLength, cascade_copy_context<string_type>{ src, rawLength });
		} else {
			if constexpr (resizeable_types<string_type>) {
				value.resize(rawLength);
			}
			jsonifierMemcpy(value.data(), src, rawLength);
		}
		return true;
	}

}
