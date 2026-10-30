/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_document.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/json_iterator.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::generic {

	inline constexpr uint64_t defaultBatchSize{ 1024 * 1024 };

	enum class error_code : uint8_t {
		success,
		empty,
		tape_error,
		unclosed_container,
		trailing_content,
		incorrect_type,
		no_such_field,
		index_out_of_bounds,
		number_error,
		string_error,
		t_atom_error,
		f_atom_error,
		n_atom_error,
		invalid_json_pointer,
		capacity,
	};

	template<typename value_type> class alloc_buffer : internal::alloc_wrapper<value_type> {
	  public:
		using allocator = internal::alloc_wrapper<value_type>;

		JSONIFIER_INLINE alloc_buffer& operator=(alloc_buffer&& other) noexcept {
			if (this != &other) [[likely]] {
				release();
				ptr		 = std::exchange(other.ptr, nullptr);
				capacity = std::exchange(other.capacity, uint64_t{});
			}
			return *this;
		}

		void reserve(uint64_t capacityNew) noexcept {
			if (capacityNew > capacity) {
				release();
				ptr		 = allocator::allocate(capacityNew);
				capacity = capacityNew;
			}
		}

		JSONIFIER_INLINE alloc_buffer(alloc_buffer&& other) noexcept : capacity{ std::exchange(other.capacity, uint64_t{}) }, ptr{ std::exchange(other.ptr, nullptr) } {
		}

		JSONIFIER_INLINE explicit alloc_buffer(uint64_t capacityNew) noexcept {
			reserve(capacityNew);
		}

		JSONIFIER_INLINE uint64_t size() const noexcept {
			return capacity;
		}

		JSONIFIER_INLINE value_type* data() const noexcept {
			return ptr;
		}

		JSONIFIER_INLINE ~alloc_buffer() noexcept {
			release();
		}

		alloc_buffer& operator=(const alloc_buffer&) = delete;
		JSONIFIER_INLINE alloc_buffer() noexcept	 = default;
		alloc_buffer(const alloc_buffer&)			 = delete;

	  protected:
		JSONIFIER_INLINE void release() noexcept {
			if (ptr) {
				allocator::deallocate(ptr, capacity);
				ptr		 = nullptr;
				capacity = 0;
			}
		}

		uint64_t capacity{};
		value_type* ptr{};
	};

	struct string_arena {
		JSONIFIER_INLINE void reset(uint64_t documentLength) noexcept {
			buffer.reserve(documentLength * 2 + 128);
			slots	   = buffer.data();
			scratchPtr = buffer.data() + documentLength + 64;
		}

		JSONIFIER_INLINE char* slot(uint64_t offset) const noexcept {
			return slots + offset;
		}

		JSONIFIER_INLINE char* scratch() const noexcept {
			return scratchPtr;
		}

		JSONIFIER_INLINE string_arena& operator=(string_arena&& other) noexcept {
			if (this != &other) [[likely]] {
				buffer	   = internal::move(other.buffer);
				scratchPtr = std::exchange(other.scratchPtr, nullptr);
				slots	   = std::exchange(other.slots, nullptr);
			}
			return *this;
		}

		JSONIFIER_INLINE string_arena(string_arena&& other) noexcept
			: buffer{ internal::move(other.buffer) }, scratchPtr{ std::exchange(other.scratchPtr, nullptr) }, slots{ std::exchange(other.slots, nullptr) } {
		}

		string_arena& operator=(const string_arena&) = delete;
		string_arena(const string_arena&)			 = delete;
		string_arena() noexcept						 = default;

	  protected:
		alloc_buffer<char> buffer{};
		char* scratchPtr{};
		char* slots{};
	};

	struct field_index_group {
#if JSONIFIER_NEON || JSONIFIER_SVE2
		static constexpr uint64_t laneShift{ 2 };
		static constexpr uint64_t laneMask{ 0x8888888888888888ull };
#else
		static constexpr uint64_t laneShift{ 0 };
		static constexpr uint64_t laneMask{ 0xFFFFFFFFFFFFFFFFull };
#endif

		JSONIFIER_INLINE static uint64_t match(const int8_t* controls, int8_t control) noexcept {
			const jsonifier_simd_int_128 group	 = internal::simd::gatherValuesU<jsonifier_simd_int_128>(controls);
			const jsonifier_simd_int_128 needles = internal::simd::gatherValue<jsonifier_simd_int_128>(static_cast<char>(control));
			return static_cast<uint64_t>(internal::simd::opBitMaskRaw(internal::simd::opCmpEqRaw(group, needles))) & laneMask;
		}

		JSONIFIER_INLINE static uint64_t firstLane(uint64_t mask) noexcept {
			return static_cast<uint64_t>(std::countr_zero(mask)) >> laneShift;
		}

		JSONIFIER_INLINE static uint64_t clearLane(uint64_t mask) noexcept {
			return internal::simd::blsr(mask);
		}
	};

	class field_index_map {
	  public:
		static constexpr uint32_t npos{ std::numeric_limits<uint32_t>::max() };

		JSONIFIER_INLINE static uint64_t hashKey(read_buffer_ptr keyData, uint64_t keyLength, uint32_t objectIndex) noexcept {
			uint64_t low{};
			uint64_t high{};
			if (keyLength > 16) {
				return mix64(longKeyHash(keyData, keyLength) ^ (static_cast<uint64_t>(objectIndex) << 32), mixMultiplier);
			}
			if (keyLength >= 8) {
				low	 = loadWord<uint64_t>(keyData);
				high = loadWord<uint64_t>(keyData + keyLength - 8);
			} else if (keyLength >= 4) {
				low	 = loadWord<uint32_t>(keyData);
				high = loadWord<uint32_t>(keyData + keyLength - 4);
			} else if (keyLength > 0) {
				low = static_cast<uint64_t>(static_cast<uint8_t>(keyData[0])) | (static_cast<uint64_t>(static_cast<uint8_t>(keyData[keyLength >> 1])) << 8) |
					(static_cast<uint64_t>(static_cast<uint8_t>(keyData[keyLength - 1])) << 16);
			}
			const uint64_t lengthAndObject = (static_cast<uint64_t>(objectIndex) << 32) | keyLength;
			return mix64(low ^ hashSeed ^ lengthAndObject, mixMultiplier) ^ mix64(high + mixMultiplier, hashSeed ^ lengthAndObject);
		}

		template<typename matcher_type, typename state_type, typename key_type>
		JSONIFIER_INLINE uint32_t find(uint64_t hash, uint32_t objectIndex, state_type&& state, key_type&& key) const noexcept {
			const int8_t control = static_cast<int8_t>(hash & 0x7F);
			uint64_t group		 = (hash >> 7) & groupMask;
			for (uint64_t probe = 0; probe <= groupMask; ++probe) {
				const int8_t* groupControls = controls.data() + group * groupWidth;
				for (uint64_t mask = field_index_group::match(groupControls, control); mask != 0;) {
					const uint64_t lane	   = field_index_group::firstLane(mask);
					const entry& candidate = entries.data()[group * groupWidth + lane];
					if (candidate.hash == hash && candidate.object == objectIndex && matcher_type::impl(state, key, candidate.key)) {
						return candidate.key;
					}
					mask = field_index_group::clearLane(mask);
				}
				if (field_index_group::match(groupControls, emptyControl) != 0) {
					return npos;
				}
				group = (group + 1) & groupMask;
			}
			return npos;
		}

		JSONIFIER_INLINE void insert(uint64_t hash, uint32_t objectIndex, uint32_t keyIndex) noexcept {
			if ((count + 1) * 8 > uint64_t{ slotCount } * 7) [[unlikely]] {
				grow();
			}
			place(entry{ hash, objectIndex, keyIndex });
			++count;
		}

		JSONIFIER_INLINE void reset() noexcept {
			if (count != 0) {
				std::memset(controls.data(), static_cast<uint8_t>(emptyControl), slotCount);
				count = 0;
			}
		}

		JSONIFIER_INLINE uint64_t size() const noexcept {
			return count;
		}

		JSONIFIER_INLINE field_index_map& operator=(field_index_map&& other) noexcept {
			if (this != &other) [[likely]] {
				controls  = internal::move(other.controls);
				entries	  = internal::move(other.entries);
				slotCount = std::exchange(other.slotCount, uint32_t{});
				groupMask = std::exchange(other.groupMask, uint64_t{});
				count	  = std::exchange(other.count, uint64_t{});
			}
			return *this;
		}

		JSONIFIER_INLINE field_index_map(field_index_map&& other) noexcept
			: controls{ internal::move(other.controls) }, entries{ internal::move(other.entries) }, slotCount{ std::exchange(other.slotCount, uint32_t{}) },
			  groupMask{ std::exchange(other.groupMask, uint64_t{}) }, count{ std::exchange(other.count, uint64_t{}) } {
		}

		field_index_map& operator=(const field_index_map&) = delete;
		JSONIFIER_INLINE field_index_map() noexcept		   = default;
		field_index_map(const field_index_map&)			   = delete;

	  protected:
		struct entry {
			uint64_t hash;
			uint32_t object;
			uint32_t key;
		};

		static constexpr uint64_t groupWidth{ 16 };
		static constexpr uint64_t initialSlots{ 128 };
		static constexpr int8_t emptyControl{ static_cast<int8_t>(-128) };
		static constexpr uint64_t hashSeed{ 14695981039346656037ull };
		static constexpr uint64_t mixMultiplier{ 0x9DDFEA08EB382D69ull };

		JSONIFIER_INLINE static uint64_t longKeyHash(read_buffer_ptr keyData, uint64_t keyLength) noexcept {
			static constexpr uint64_t c1{ 0x87c37b91114253d5ull };
			static constexpr uint64_t c2{ 0x4cf5ad432745937full };
			uint64_t h1			   = hashSeed;
			uint64_t h2			   = hashSeed;
			const uint64_t nblocks = keyLength / 16;
			for (uint64_t block = 0; block < nblocks; ++block) {
				uint64_t k1 = loadWord<uint64_t>(keyData + block * 16);
				uint64_t k2 = loadWord<uint64_t>(keyData + block * 16 + 8);
				k1 *= c1;
				k1 = rotl64(k1, 31);
				k1 *= c2;
				h1 ^= k1;
				h1 = rotl64(h1, 27);
				h1 += h2;
				h1 = h1 * 5 + 0x52dce729;
				k2 *= c2;
				k2 = rotl64(k2, 33);
				k2 *= c1;
				h2 ^= k2;
				h2 = rotl64(h2, 31);
				h2 += h1;
				h2 = h2 * 5 + 0x38495ab5;
			}
			const uint64_t tailLength = keyLength & 15;
			if (tailLength != 0) {
				read_buffer_ptr tail = keyData + keyLength - 16;
				uint64_t k1			 = loadWord<uint64_t>(tail);
				uint64_t k2			 = loadWord<uint64_t>(tail + 8);
				k1 *= c1;
				k1 = rotl64(k1, 31);
				k1 *= c2;
				h1 ^= k1;
				k2 *= c2;
				k2 = rotl64(k2, 33);
				k2 *= c1;
				h2 ^= k2;
			}
			h1 ^= keyLength;
			h2 ^= keyLength;
			h1 += h2;
			h2 += h1;
			h1 = fmix64(h1);
			h2 = fmix64(h2);
			return h1 + h2;
		}

		void grow() noexcept {
			const uint64_t newSlotCount = slotCount == 0 ? initialSlots : uint64_t{ slotCount } * 2;
			alloc_buffer<int8_t> oldControls{ internal::move(controls) };
			alloc_buffer<entry> oldEntries{ internal::move(entries) };
			const uint64_t oldSlotCount = slotCount;
			controls.reserve(newSlotCount);
			entries.reserve(newSlotCount);
			std::memset(controls.data(), static_cast<uint8_t>(emptyControl), newSlotCount);
			slotCount = static_cast<uint32_t>(newSlotCount);
			groupMask = newSlotCount / groupWidth - 1;
			for (uint64_t slot = 0; slot < oldSlotCount; ++slot) {
				if (oldControls.data()[slot] >= 0) {
					place(oldEntries.data()[slot]);
				}
			}
		}

		JSONIFIER_INLINE static uint64_t mix64(uint64_t value, uint64_t multiplier) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			const __uint128_t product = static_cast<__uint128_t>(value) * static_cast<__uint128_t>(multiplier);
			return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
#elif JSONIFIER_COMPILER_MSVC
			uint64_t highPart;
			const uint64_t lowPart = _umul128(value, multiplier, &highPart);
			return lowPart ^ highPart;
#else
			const uint64_t lowPart = value * multiplier;
			return lowPart ^ ((value >> 32) * (multiplier >> 32));
#endif
		}

		JSONIFIER_INLINE void place(const entry& newEntry) noexcept {
			uint64_t group = (newEntry.hash >> 7) & groupMask;
			while (true) {
				int8_t* groupControls = controls.data() + group * groupWidth;
				if (const uint64_t empties = field_index_group::match(groupControls, emptyControl); empties != 0) [[likely]] {
					const uint64_t lane						  = field_index_group::firstLane(empties);
					groupControls[lane]						  = static_cast<int8_t>(newEntry.hash & 0x7F);
					entries.data()[group * groupWidth + lane] = newEntry;
					return;
				}
				group = (group + 1) & groupMask;
			}
		}

		JSONIFIER_INLINE static uint64_t fmix64(uint64_t value) noexcept {
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdull;
			value ^= value >> 33;
			value *= 0xc4ceb9fe1a85ec53ull;
			value ^= value >> 33;
			return value;
		}

		template<typename word_type> JSONIFIER_INLINE static uint64_t loadWord(read_buffer_ptr source) noexcept {
			word_type word;
			pow2MemcpyWrapper<sizeof(word_type)>(&word, source);
			return static_cast<uint64_t>(word);
		}

		JSONIFIER_INLINE static uint64_t rotl64(uint64_t value, int32_t shift) noexcept {
			return (value << shift) | (value >> (64 - shift));
		}

		alloc_buffer<int8_t> controls{};
		alloc_buffer<entry> entries{};
		uint32_t slotCount{};
		uint64_t groupMask{};
		uint64_t count{};
	};

	// Sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
	consteval jsonifier::internal::array<int8_t, 256> genDepthDeltaTable() {
		jsonifier::internal::array<int8_t, 256> returnValues{};
		for (auto& entry: returnValues) {
			entry = 0;
		}
		returnValues[static_cast<uint64_t>('{')] = 1;
		returnValues[static_cast<uint64_t>('[')] = 1;
		returnValues[static_cast<uint64_t>('}')] = -1;
		returnValues[static_cast<uint64_t>(']')] = -1;
		return returnValues;
	}

	alignas(64) inline constexpr auto depthDeltaTableRaw{ genDepthDeltaTable() };

	inline constexpr const int8_t* __restrict depthDeltaTable{ depthDeltaTableRaw.data() };

	template<bool minified> struct document_state {
		JSONIFIER_INLINE bool keyHasEscape(read_structural_index_ptr iter) const noexcept {
			read_buffer_ptr keyStart = root + *iter + 1;
			read_buffer_ptr keyLimit = root + *(iter + 1);
			return keyLimit > keyStart && internal::char_comparison<'\\', uint8_t>::memchar(keyStart, static_cast<uint64_t>(keyLimit - keyStart));
		}

		JSONIFIER_INLINE bool unescapedKeyMatches(read_structural_index_ptr iter, string_view key) const noexcept {
			read_buffer_ptr keyStart = root + *iter + 1;
			char* unescaped			 = arena->scratch();
			const auto scanned = internal::string_scanner<parse_options{}>::impl(keyStart, end, std::bit_cast<write_buffer_ptr>(unescaped));
			return scanned.outLength == key.size() && internal::comparison::compare(unescaped, key.data(), key.size());
		}

		JSONIFIER_INLINE error_code advanceChild(read_structural_index_ptr childValue, uint64_t containerDepth, char closer, read_structural_index_ptr& next) noexcept {
			const read_buffer_ptr base		   = root;
			read_structural_index_ptr limit	   = tapeEnd;
			read_structural_index_ptr position = cursor;
			uint64_t currentDepth			   = depth;
			if (currentDepth == containerDepth) {
				if (position <= childValue) {
					position = skipValue(base, childValue, limit);
				}
			} else if (currentDepth > containerDepth) {
				position = finishToDepth(base, position, limit, currentDepth, containerDepth);
			}
			error_code result{ error_code::tape_error };
			if (position >= limit) [[unlikely]] {
				result = error_code::unclosed_container;
			} else if (const uint8_t separator = base[*position]; separator == ',') [[likely]] {
				++position;
				next   = position;
				result = position < limit ? error_code::success : error_code::unclosed_container;
			} else if (separator == closer) [[likely]] {
				++position;
				--currentDepth;
				next   = nullptr;
				result = error_code::success;
			}
			cursor = position;
			depth  = currentDepth;
			return result;
		}

		JSONIFIER_INLINE static internal::unrestrict_t<read_structural_index_ptr> skipValue(read_buffer_ptr base, read_structural_index_ptr iter,
			read_structural_index_ptr limit) noexcept {
			if (!isOpener(base[*iter])) [[likely]] {
				return iter + 1;
			}
			int64_t openCount{ 1 };
			++iter;
			while (iter < limit) {
				openCount += depthDeltaTable[base[*iter]];
				++iter;
				if (openCount == 0) {
					return iter;
				}
			}
			return limit;
		}

		JSONIFIER_INLINE internal::unrestrict_t<read_structural_index_ptr> skipValue(read_structural_index_ptr iter) const noexcept {
			return skipValue(root, iter, tapeEnd);
		}

		JSONIFIER_INLINE bool keyMatches(read_structural_index_ptr iter, string_view key) const noexcept {
			const uint64_t keySize	 = key.size();
			const uint64_t span		 = static_cast<uint64_t>(*(iter + 1) - *iter);
			read_buffer_ptr keyStart = root + *iter + 1;
			if (span == keySize + 2) [[likely]] {
				return internal::comparison::compare(keyStart, key.data(), keySize);
			}
			if constexpr (minified) {
				return false;
			} else {
				return span > keySize + 2 && keyStart[keySize] == '"' && internal::comparison::compare(keyStart, key.data(), keySize);
			}
		}

		JSONIFIER_INLINE static internal::unrestrict_t<read_structural_index_ptr> finishToDepth(read_buffer_ptr base, read_structural_index_ptr position,
			read_structural_index_ptr limit, uint64_t& currentDepth, uint64_t targetDepth) noexcept {
			int64_t localDepth{ static_cast<int64_t>(currentDepth) };
			const int64_t target{ static_cast<int64_t>(targetDepth) };
			if (localDepth > target) {
				while (position < limit) {
					localDepth += depthDeltaTable[base[*position]];
					++position;
					if (localDepth <= target) {
						break;
					}
				}
			}
			currentDepth = static_cast<uint64_t>(localDepth);
			return position;
		}

		JSONIFIER_INLINE void finishToDepth(uint64_t targetDepth) noexcept {
			uint64_t currentDepth{ depth };
			cursor = finishToDepth(root, cursor, tapeEnd, currentDepth, targetDepth);
			depth  = currentDepth;
		}

		template<typename word_type> JSONIFIER_INLINE static word_type loadWord(read_buffer_ptr source) noexcept {
			word_type word;
			pow2MemcpyWrapper<sizeof(word_type)>(&word, source);
			return word;
		}

		template<char value> JSONIFIER_INLINE bool charAt(read_structural_index_ptr iter) const noexcept {
			return root[*iter] == value;
		}

		JSONIFIER_INLINE uint8_t charAt(read_structural_index_ptr iter) const noexcept {
			return root[*iter];
		}

		JSONIFIER_INLINE static bool isOpener(uint8_t c) noexcept {
			return (c | 0x20) == '{';
		}

		JSONIFIER_INLINE static bool isCloser(uint8_t c) noexcept {
			return (c | 0x20) == '}';
		}

		JSONIFIER_INLINE bool reverseMode() const noexcept {
			return reverseScore >= reverseEngageScore;
		}

		JSONIFIER_INLINE void noteBackwardHit() noexcept {
			reverseScore += static_cast<uint8_t>(reverseScore < reverseMaxScore);
		}

		JSONIFIER_INLINE void noteForwardHit() noexcept {
			reverseScore -= static_cast<uint8_t>(reverseScore > 0);
		}

		static constexpr uint8_t reverseEngageScore{ 4 };
		static constexpr uint8_t reverseMaxScore{ 8 };

		read_structural_index_ptr tapeEnd{};
		read_structural_index_ptr cursor{};
		read_structural_index_ptr tape{};
		field_index_map* fieldIndex{};
		uint8_t reverseScore{};
		read_buffer_ptr root{};
		read_buffer_ptr end{};
		string_arena* arena{};
		uint64_t depth{};
	};

	using array_type_table = internal::array<json_type, 256>;

	consteval internal::array<bool, 256ULL> genUtf8StringStopTable() {
		internal::array<bool, 256ULL> returnValues{};
		for (uint64_t x = 0; x < 0x20; ++x) {
			returnValues[x] = true;
		}
		returnValues[static_cast<uint64_t>('"')]  = true;
		returnValues[static_cast<uint64_t>('\\')] = true;
		return returnValues;
	}

	alignas(64) inline constexpr auto utf8StringStopTableRaw{ genUtf8StringStopTable() };

	inline constexpr const bool* __restrict utf8StringStopTable{ utf8StringStopTableRaw.data() };

	consteval internal::array<bool, 256ULL> genAsciiStringStopTable() {
		internal::array<bool, 256ULL> returnValues{};
		for (uint64_t x = 0; x < 0x20; ++x) {
			returnValues[x] = true;
		}
		for (uint64_t x = 0x80; x < 0x100; ++x) {
			returnValues[x] = true;
		}
		returnValues[static_cast<uint64_t>('"')]  = true;
		returnValues[static_cast<uint64_t>('\\')] = true;
		return returnValues;
	}

	alignas(64) inline constexpr auto asciiStringStopTableRaw{ genAsciiStringStopTable() };

	inline constexpr const bool* __restrict asciiStringStopTable{ asciiStringStopTableRaw.data() };

	consteval array_type_table genTypeTable() {
		array_type_table returnValues{};
		for (auto& entry: returnValues) {
			entry = json_type::unset;
		}
		returnValues[static_cast<uint64_t>('{')] = json_type::object;
		returnValues[static_cast<uint64_t>('[')] = json_type::array;
		returnValues[static_cast<uint64_t>('"')] = json_type::string;
		returnValues[static_cast<uint64_t>('t')] = json_type::boolean;
		returnValues[static_cast<uint64_t>('f')] = json_type::boolean;
		returnValues[static_cast<uint64_t>('n')] = json_type::null;
		returnValues[static_cast<uint64_t>('-')] = json_type::number;
		for (uint64_t x = static_cast<uint64_t>('0'); x <= static_cast<uint64_t>('9'); ++x) {
			returnValues[x] = json_type::number;
		}
		return returnValues;
	}

	alignas(64) inline constexpr auto typeTableRaw{ genTypeTable() };

	inline constexpr const json_type* __restrict typeTable{ typeTableRaw.data() };

	consteval jsonifier::internal::array<bool, 256> genSeperatorByteTable() {
		jsonifier::internal::array<bool, 256> returnValues{};
		for (auto& entry: returnValues) {
			entry = false;
		}
		returnValues[static_cast<uint64_t>(' ')]  = true;
		returnValues[static_cast<uint64_t>('\n')] = true;
		returnValues[static_cast<uint64_t>('\r')] = true;
		returnValues[static_cast<uint64_t>('\t')] = true;
		returnValues[static_cast<uint64_t>(',')]  = true;
		return returnValues;
	}

	alignas(64) inline constexpr auto seperatorByteTableRaw{ genSeperatorByteTable() };

	inline constexpr const bool* __restrict seperatorByteTable{ seperatorByteTableRaw.data() };

	struct utf8_span_step {
		template<uint64_t index> JSONIFIER_INLINE static internal::unrestrict_t<read_buffer_ptr> impl(internal::unrestrict_t<read_buffer_ptr>& current, read_buffer_ptr end,
			internal::utf8_validation_state& state) noexcept {
			using simd_list_local					 = internal::type_list_element_t<index, internal::simd::avx_integer_list>;
			static constexpr auto simdType			 = simd_list_local::type::simdType;
			using simd_type_local					 = typename internal::simd_type_wrapper<simdType>::type;
			static constexpr uint64_t bytesProcessed = simd_list_local::bytesProcessed;
			internal::utf8_register_validator<internal::simd_type_wrapper<simdType>> validator{ state };
			while (static_cast<uint64_t>(end - current) >= bytesProcessed) {
				validator.checkRegister(internal::simd::gatherValuesU<simd_type_local>(current));
				current += bytesProcessed;
			}
			if constexpr (index == internal::listSize - 1) {
				validator.checkPartial(current, static_cast<uint64_t>(end - current));
				current = end;
				return validator.errors() ? nullptr : current;
			} else {
				validator.flush();
				return current;
			}
		}
	};

	template<bool minified> class basic_object;
	template<bool minified> class basic_array;
	template<bool minified> class basic_field;

	struct plain_string_copy_context {
		JSONIFIER_INLINE uint64_t operator()(char* __restrict ptrNew, uint64_t length) const noexcept {
			jsonifier::internal::jsonifierMemcpy(ptrNew, source, length);
			return length;
		}

		plain_string_copy_context& operator=(const plain_string_copy_context&) noexcept = delete;

		JSONIFIER_INLINE plain_string_copy_context(read_buffer_ptr sourceNew) noexcept : source{ sourceNew } {
		}

		plain_string_copy_context& operator=(plain_string_copy_context&&) noexcept = delete;
		plain_string_copy_context(const plain_string_copy_context&) noexcept	   = delete;
		plain_string_copy_context(plain_string_copy_context&&) noexcept			   = delete;
		plain_string_copy_context() noexcept									   = delete;

		read_buffer_ptr source{};
	};

	struct escaped_string_copy_context {
		JSONIFIER_INLINE uint64_t operator()(char* __restrict ptrNew, uint64_t) const noexcept {
			outLength = internal::string_scanner<parse_options{}>::impl(source, sourceEnd, ptrNew).outLength;
			return outLength == std::numeric_limits<uint64_t>::max() ? uint64_t{} : outLength;
		}

		JSONIFIER_INLINE escaped_string_copy_context(read_buffer_ptr sourceNew, read_buffer_ptr sourceEndNew, uint64_t& outLengthNew) noexcept
			: sourceEnd{ sourceEndNew }, source{ sourceNew }, outLength{ outLengthNew } {
		}

		escaped_string_copy_context& operator=(const escaped_string_copy_context&) noexcept = delete;
		escaped_string_copy_context& operator=(escaped_string_copy_context&&) noexcept		= delete;
		escaped_string_copy_context(const escaped_string_copy_context&) noexcept			= delete;
		escaped_string_copy_context(escaped_string_copy_context&&) noexcept					= delete;
		escaped_string_copy_context() noexcept												= delete;

		read_buffer_ptr sourceEnd{};
		read_buffer_ptr source{};
		uint64_t& outLength;
	};

	template<bool minified> class basic_value {
	  public:
		using document_state = generic::document_state<minified>;
		using object		 = basic_object<minified>;
		using array			 = basic_array<minified>;
		using value			 = basic_value;

		template<internal::buffer_like_types string_type> [[nodiscard]] JSONIFIER_INLINE error_code getString(string_type& out) const {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			if (!doc->template charAt<'"'>(iter)) [[unlikely]] {
				return error_code::incorrect_type;
			}
			consume();
			read_buffer_ptr stringStart = doc->root + *iter + 1;
			if (const uint64_t directLength = zeroCopyLength<false>(stringStart); directLength != noZeroCopy) [[likely]] {
				if constexpr (internal::resize_and_overwriteable_types<string_type>) {
					out.resize_and_overwrite(directLength, plain_string_copy_context{ stringStart });
				} else {
					out.resize(directLength);
					plain_string_copy_context{ stringStart }(out.data(), directLength);
				}
				return error_code::success;
			}
			const uint64_t upperBound = static_cast<uint64_t>(doc->root + *(iter + 1) - stringStart) + 64;
			uint64_t outLength{};
			if constexpr (internal::resize_and_overwriteable_types<string_type>) {
				out.resize_and_overwrite(upperBound, escaped_string_copy_context{ stringStart, doc->end, outLength });
			} else {
				if (out.size() < upperBound) [[unlikely]] {
					out.resize(upperBound);
				}
				out.resize(escaped_string_copy_context{ stringStart, doc->end, outLength }(out.data(), upperBound));
			}
			return outLength == std::numeric_limits<uint64_t>::max() ? error_code::string_error : error_code::success;
		}

		[[nodiscard]] JSONIFIER_INLINE error_code rawJson(string_view& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			if (document_state::isOpener(*valueStart)) {
				read_structural_index_ptr afterClose = doc->skipValue(iter);
				if (afterClose == doc->tapeEnd && !document_state::isCloser(doc->charAt(afterClose - 1))) [[unlikely]] {
					return error_code::unclosed_container;
				}
				out = string_view{ std::bit_cast<const char*>(valueStart), static_cast<uint64_t>(doc->root + *(afterClose - 1) + 1 - valueStart) };
				return error_code::success;
			}
			read_buffer_ptr valueEnd = doc->root + *(iter + 1);
			while (valueEnd > valueStart && isWhitespace(valueEnd[-1])) {
				--valueEnd;
			}
			out = string_view{ std::bit_cast<const char*>(valueStart), static_cast<uint64_t>(valueEnd - valueStart) };
			return error_code::success;
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getBool(bool& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			switch (doc->charAt(iter)) {
				case 't': {
					consume();
					out = true;
					return trueMatches() ? error_code::success : error_code::t_atom_error;
				}
				case 'f': {
					consume();
					out = false;
					return falseMatches() ? error_code::success : error_code::f_atom_error;
				}
				default: {
					return error_code::incorrect_type;
				}
			}
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getUint64(uint64_t& out) const noexcept {
			if (!isNumberStart()) [[unlikely]] {
				return typeError();
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			read_buffer_ptr valueEnd   = doc->root + *(iter + 1);
			read_buffer_ptr parsedEnd  = internal::integer_parser<uint64_t>::parseInt(out, valueStart, valueEnd);
			consume();
			return finishNumber(parsedEnd, valueEnd);
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getInt64(int64_t& out) const noexcept {
			if (!isNumberStart()) [[unlikely]] {
				return typeError();
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			read_buffer_ptr valueEnd   = doc->root + *(iter + 1);
			read_buffer_ptr parsedEnd  = internal::integer_parser<int64_t>::parseInt(out, valueStart, valueEnd);
			consume();
			return finishNumber(parsedEnd, valueEnd);
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getDouble(double& out) const noexcept {
			if (!isNumberStart()) [[unlikely]] {
				return typeError();
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			read_buffer_ptr valueEnd   = doc->root + *(iter + 1);
			read_buffer_ptr parsedEnd  = internal::float_parser<double>::parseFloat(out, valueStart, valueEnd);
			consume();
			return finishNumber(parsedEnd, valueEnd);
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getString(std::string_view& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			if (!doc->template charAt<'"'>(iter)) [[unlikely]] {
				return error_code::incorrect_type;
			}
			consume();
			return unescape(iter, out);
		}

		[[nodiscard]] JSONIFIER_INLINE bool isNull() const noexcept {
			if (err == error_code::success && doc->template charAt<'n'>(iter) && nullMatches()) {
				consume();
				return true;
			}
			return false;
		}

		JSONIFIER_INLINE json_type type() const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return json_type::unset;
			}
			return typeTable[doc->charAt(iter)];
		}

		JSONIFIER_INLINE basic_value(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew) noexcept : iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
		}

		template<jsonifier::internal::string_view_convertible_types key_type> JSONIFIER_INLINE value operator[](key_type&& key) const noexcept;
		template<typename value_type> [[nodiscard]] JSONIFIER_INLINE error_code get(value_type& out) const;
		JSONIFIER_INLINE value findFieldUnordered(string_view key) const noexcept;

		JSONIFIER_INLINE basic_value(error_code errNew) noexcept : err{ errNew } {
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE value findField(string_view key) const noexcept;
		JSONIFIER_INLINE value operator[](uint64_t index) const noexcept;
		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept;
		JSONIFIER_INLINE object getObject() const noexcept;
		JSONIFIER_INLINE array getArray() const noexcept;
		JSONIFIER_INLINE basic_value() noexcept = default;

	  protected:
		static constexpr uint64_t noZeroCopy{ std::numeric_limits<uint64_t>::max() };
#if JSONIFIER_NEON
		static constexpr uint64_t stopMaskLaneShift{ 2 };
#else
		static constexpr uint64_t stopMaskLaneShift{ 0 };
#endif

		struct string_stop_masks {
			uint64_t nonAscii{};
			uint64_t stops{};
		};

		friend class basic_object<minified>;
		friend class basic_array<minified>;
		friend class basic_field<minified>;

		template<bool allowNonAscii> JSONIFIER_INLINE uint64_t zeroCopyLength(read_buffer_ptr stringStart) const noexcept {
			read_buffer_ptr current = stringStart;
			bool sawNonAscii{};
			if (static_cast<uint64_t>(doc->end - current) >= simdBytesPerRegister) [[likely]] {
				if (const string_stop_masks masks = selectStops<allowNonAscii>(stringStopMasks(current)); masks.stops != 0) [[likely]] {
					return resolveMaskedStop(stringStart, current, masks, false);
				} else {
					sawNonAscii = masks.nonAscii != 0;
				}
				current += simdBytesPerRegister;
				while (static_cast<uint64_t>(doc->end - current) >= simdBytesPerRegister) {
					if (const string_stop_masks masks = selectStops<allowNonAscii>(stringStopMasks(current)); masks.stops != 0) {
						return resolveMaskedStop(stringStart, current, masks, sawNonAscii);
					} else {
						sawNonAscii = sawNonAscii || masks.nonAscii != 0;
					}
					current += simdBytesPerRegister;
				}
			}
			uint8_t accumulated{ static_cast<uint8_t>(sawNonAscii ? 0x80 : 0x00) };
			for (; current < doc->end; ++current) {
				const uint8_t currentChar = static_cast<uint8_t>(*current);
				if ((allowNonAscii ? utf8StringStopTable : asciiStringStopTable)[currentChar]) {
					return resolveStringStop(stringStart, current, (accumulated & 0x80) != 0);
				}
				accumulated |= currentChar;
			}
			return noZeroCopy;
		}

		JSONIFIER_INLINE static bool validUtf8(read_buffer_ptr source, uint64_t length) noexcept {
			internal::utf8_validation_state state{};
			internal::unrestrict_t<read_buffer_ptr> current = source;
			return internal::string_parse_executor<utf8_span_step, internal::make_ascending_range<internal::startIndex, internal::listSize>>::impl(current, source + length, state);
		}

		JSONIFIER_INLINE static string_stop_masks stringStopMasks(read_buffer_ptr current) noexcept {
			const jsonifier_simd_int_t values		  = internal::simd::gatherValuesU<jsonifier_simd_int_t>(current);
			const jsonifier_simd_int_t backslashes	  = internal::simd::gatherValue<jsonifier_simd_int_t>('\\');
			const jsonifier_simd_int_t quotes		  = internal::simd::gatherValue<jsonifier_simd_int_t>('"');
			const jsonifier_simd_int_t controlCeiling = internal::simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x20));
#if JSONIFIER_AVX || JSONIFIER_AVX2 || JSONIFIER_AVX512
			using mask_type			= std::make_unsigned_t<decltype(internal::simd::opBitMask(jsonifier_simd_int_t{}))>;
			const uint64_t nonAscii = static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(values)));
			const auto stopsAndHigh = internal::simd::opOr(internal::simd::opOr(internal::simd::opCmpEqRaw(values, backslashes), internal::simd::opCmpEqRaw(values, quotes)),
				internal::simd::opCmpLtSignedRaw(values, controlCeiling));
			return { nonAscii, static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(stopsAndHigh))) & ~nonAscii };
#else
			const jsonifier_simd_int_t asciiCeiling = internal::simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x7F));
			const auto stops = internal::simd::opOr(internal::simd::opOr(internal::simd::opCmpEqRaw(values, backslashes), internal::simd::opCmpEqRaw(values, quotes)),
				internal::simd::opCmpLtRaw(values, controlCeiling));
	#if JSONIFIER_NEON
			return { internal::simd::opBitMaskRaw(internal::simd::opCmpLtRaw(asciiCeiling, values)), internal::simd::opBitMaskRaw(stops) };
	#else
			using mask_type = std::make_unsigned_t<decltype(internal::simd::opBitMask(jsonifier_simd_int_t{}))>;
			return { static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(internal::simd::opCmpLtRaw(asciiCeiling, values)))),
				static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(stops))) };
	#endif
#endif
		}

		template<jsonifier::internal::string_types value_type> JSONIFIER_INLINE error_code unescape(read_structural_index_ptr stringIter, value_type& result) const noexcept {
			read_buffer_ptr stringStart = doc->root + *stringIter + 1;
			if (const uint64_t directLength = zeroCopyLength<true>(stringStart); directLength != noZeroCopy) [[likely]] {
				result = value_type{ std::bit_cast<const char*>(stringStart), directLength };
				return error_code::success;
			}
			char* scratch	   = doc->arena->scratch();
			const auto scanned = internal::string_scanner<parse_options{}>::impl(stringStart, doc->end, std::bit_cast<write_buffer_ptr>(scratch));
			if (scanned.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return error_code::string_error;
			}
			if (scanned.outLength == scanned.rawLength) {
				result = value_type{ std::bit_cast<const char*>(stringStart), scanned.rawLength };
				return error_code::success;
			}
			char* slot = doc->arena->slot(static_cast<uint64_t>(stringStart - doc->root));
			jsonifier::internal::jsonifierMemcpy(slot, scratch, scanned.outLength);
			result = value_type{ slot, scanned.outLength };
			return error_code::success;
		}

		template<uint32_t leadingWord, uint64_t atomSize> JSONIFIER_INLINE bool atomMatches() const noexcept {
			read_buffer_ptr valueStart = doc->root + *iter;
			if (static_cast<uint64_t>(doc->end - valueStart) < atomSize) [[unlikely]] {
				return false;
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, valueStart);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			if constexpr (atomSize == 5) {
				if (comparison != leadingWord || valueStart[4] != 'e') [[unlikely]] {
					return false;
				}
			} else {
				if (comparison != leadingWord) [[unlikely]] {
					return false;
				}
			}
			return valueStart + atomSize == doc->end || internal::validPostPrimitiveTable[static_cast<uint64_t>(valueStart[atomSize])];
		}

		JSONIFIER_INLINE bool trueMatches() const noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			return atomMatches<trueVal, 4>();
		}

		JSONIFIER_INLINE bool falseMatches() const noexcept {
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			return atomMatches<falseVal, 5>();
		}

		JSONIFIER_INLINE bool nullMatches() const noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			return atomMatches<nullVal, 4>();
		}

		JSONIFIER_INLINE static uint64_t resolveMaskedStop(read_buffer_ptr stringStart, read_buffer_ptr current, string_stop_masks masks, bool sawNonAscii) noexcept {
			const uint64_t beforeStop = (masks.stops & (0 - masks.stops)) - 1;
			return resolveStringStop(stringStart, current + (std::countr_zero(masks.stops) >> stopMaskLaneShift), sawNonAscii || (masks.nonAscii & beforeStop) != 0);
		}

		JSONIFIER_INLINE static uint64_t resolveStringStop(read_buffer_ptr stringStart, read_buffer_ptr stop, bool sawNonAscii) noexcept {
			if (*stop != '"') [[unlikely]] {
				return noZeroCopy;
			}
			const uint64_t length = static_cast<uint64_t>(stop - stringStart);
			return !sawNonAscii || validUtf8(stringStart, length) ? length : noZeroCopy;
		}

		JSONIFIER_INLINE static error_code finishNumber(read_buffer_ptr parsedEnd, read_buffer_ptr valueEnd) noexcept {
			if (parsedEnd && (parsedEnd == valueEnd || internal::validPostPrimitiveTable[static_cast<uint64_t>(*parsedEnd)])) [[likely]] {
				return error_code::success;
			}
			return error_code::number_error;
		}

		template<bool allowNonAscii> JSONIFIER_INLINE static string_stop_masks selectStops(string_stop_masks masks) noexcept {
			if constexpr (!allowNonAscii) {
				masks.stops |= masks.nonAscii;
				masks.nonAscii = 0;
			}
			return masks;
		}

		JSONIFIER_INLINE bool isNumberStart() const noexcept {
			return err == error_code::success && typeTable[doc->charAt(iter)] == json_type::number;
		}

		JSONIFIER_INLINE static bool isWhitespace(uint8_t c) noexcept {
			return jsonifier::internal::whitespaceTable[c];
		}

		JSONIFIER_INLINE error_code typeError() const noexcept {
			return err != error_code::success ? err : error_code::incorrect_type;
		}

		JSONIFIER_INLINE void consume() const noexcept {
			if (doc->cursor == iter) [[likely]] {
				doc->cursor = iter + 1;
			}
		}

		JSONIFIER_INLINE static bool unescapePointerToken(string_view& token, string& unescapedToken) noexcept;
		JSONIFIER_INLINE value pointerStep(string_view token, string& unescapedToken) const noexcept;
		JSONIFIER_INLINE static error_code parsePointerIndex(string_view token, uint64_t& index) noexcept;

		error_code err{ error_code::success };
		read_structural_index_ptr iter{};
		document_state* doc{};
		uint64_t depth{};
	};

	template<bool minified> class basic_field {
	  public:
		using document_state = generic::document_state<minified>;

		JSONIFIER_INLINE string_view key() const noexcept {
			read_buffer_ptr keyStart = doc->root + *iter + 1;
			read_buffer_ptr keyEnd	 = doc->root + *(iter + 1);
			while (keyEnd > keyStart && *--keyEnd != '"') {
			}
			return string_view{ std::bit_cast<const char*>(keyStart), static_cast<uint64_t>(keyEnd - keyStart) };
		}

		JSONIFIER_INLINE basic_field(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew, error_code errNew = error_code::success) noexcept
			: err{ errNew }, iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_code unescapedKey(std::string_view& out) const noexcept {
			return basic_value<minified>{ doc, iter, depth }.unescape(iter, out);
		}

		JSONIFIER_INLINE basic_value<minified> value() const noexcept {
			return err == error_code::success ? basic_value<minified>{ doc, iter + 2, depth } : basic_value<minified>{ err };
		}

		JSONIFIER_INLINE bool keyEquals(string_view keyNew) const noexcept {
			return doc->keyMatches(iter, keyNew);
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE basic_field() noexcept = default;

	  protected:
		error_code err{ error_code::success };
		read_structural_index_ptr iter{};
		document_state* doc{};
		uint64_t depth{};
	};

	struct key_matcher {
		template<typename state_type, typename key_type> JSONIFIER_INLINE static bool impl(state_type&& state, key_type&& key, uint32_t keyIndex) {
			return state->keyMatches(state->tape + keyIndex, key);
		}
	};

	template<bool minified> class basic_object {
	  public:
		using document_state = generic::document_state<minified>;
		using field			 = basic_field<minified>;
		using value			 = basic_value<minified>;

		class iterator {
		  public:
			using iterator_category = std::forward_iterator_tag;
			using value_type		= field;
			using difference_type	= ptrdiff_t;

			JSONIFIER_INLINE iterator& operator++() noexcept {
				if (err != error_code::success) [[unlikely]] {
					iter = nullptr;
					return *this;
				}
				err = doc->advanceChild(iter + 2, depth, '}', iter);
				validateFieldStart();
				return *this;
			}

			JSONIFIER_INLINE iterator(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew) noexcept : iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
				validateFieldStart();
			}

			JSONIFIER_INLINE bool operator==(const iterator& other) const noexcept {
				return iter == other.iter;
			}

			JSONIFIER_INLINE field operator*() const noexcept {
				return field{ doc, iter, depth, err };
			}

			JSONIFIER_INLINE iterator() noexcept = default;

		  protected:
			JSONIFIER_INLINE void validateFieldStart() noexcept {
				if (err == error_code::success && iter && (!doc->template charAt<'"'>(iter) || iter + 1 >= doc->tapeEnd || !doc->template charAt<':'>(iter + 1))) [[unlikely]] {
					err = error_code::tape_error;
				}
			}

			error_code err{ error_code::success };
			read_structural_index_ptr iter{};
			document_state* doc{};
			uint64_t depth{};
		};

		[[nodiscard]] JSONIFIER_INLINE error_code countFields(uint64_t& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			out = 0;
			for (auto iter = begin(); iter != end(); ++iter) {
				if (const error_code childError = (*iter).error(); childError != error_code::success) [[unlikely]] {
					return childError;
				}
				++out;
			}
			return error_code::success;
		}

		JSONIFIER_INLINE iterator begin() const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return iterator{};
			}
			document_state* const state		   = doc;
			read_structural_index_ptr position = open + 1;
			if (position < state->tapeEnd && state->template charAt<'}'>(position)) {
				state->cursor = position + 1;
				state->depth  = depth - 1;
				return iterator{};
			}
			state->cursor = position;
			state->depth  = depth;
			if (position >= state->tapeEnd) [[unlikely]] {
				return iterator{};
			}
			return iterator{ state, position, depth };
		}

		JSONIFIER_INLINE value findField(string_view key) const noexcept {
			value found{ err };
			if (err == error_code::success) [[likely]] {
				settle();
				found = scanFields<false>(key, doc->tapeEnd);
				if (found.err == error_code::no_such_field) [[unlikely]] {
					found = escapeAwareRetry(key, doc->cursor);
				}
			}
			return found;
		}

		JSONIFIER_INLINE value findFieldUnordered(string_view key) const noexcept {
			value found{ err };
			if (err == error_code::success) [[likely]] {
				settle();
				found = expectedField(key);
				if (found.err == error_code::no_such_field) [[unlikely]] {
					found = indexedField(key);
				}
			}
			return found;
		}

		JSONIFIER_INLINE basic_object(document_state* docNew, read_structural_index_ptr openNew, uint64_t depthNew) noexcept : open{ openNew }, doc{ docNew }, depth{ depthNew } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_code rawJson(string_view& out) const noexcept {
			return value{ doc, open, depth - 1 }.rawJson(out);
		}

		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept {
			return value{ doc, open, depth - 1 }.atPointer(pointer);
		}

		JSONIFIER_INLINE value operator[](string_view key) const noexcept {
			return findFieldUnordered(key);
		}

		JSONIFIER_INLINE iterator end() const noexcept {
			return iterator{};
		}

		JSONIFIER_INLINE basic_object(error_code errNew) noexcept : err{ errNew } {
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE basic_object() noexcept = default;

#if JSONIFIER_PLATFORM_MAC && JSONIFIER_COMPILER_CLANG
		static constexpr uint32_t indexThreshold{ 16 };
#else
		static constexpr uint32_t indexThreshold{ 8 };
#endif
		static constexpr uint32_t wrapThreshold{ 2 };
		static constexpr uint32_t trailCapacity{ 16 };

	  protected:
		JSONIFIER_INLINE value indexedField(string_view key) const noexcept {
			document_state* const state			  = doc;
			read_structural_index_ptr resumePoint = state->cursor;
			value found{ error_code::no_such_field };
			if (!indexed) {
				if (!indexEarned) {
					const bool resumed = resumePoint != open + 1;
					if (!trailOverflow) {
						if (trailCount == 0 && !trailComplete && !(resumed && state->reverseMode())) {
							found = scanFields<false>(key, state->tapeEnd);
							if (found.err != error_code::no_such_field) {
								if (found.err == error_code::success && resumed) {
									state->noteForwardHit();
								}
								return found;
							}
						}
						found = trailLookup(key, resumePoint, resumed);
						if (!trailOverflow) {
							return found;
						}
						state->cursor = resumePoint;
					}
					if (resumed && state->reverseMode()) {
						found = scanBeforeResume(key, resumePoint);
						if (found.err == error_code::no_such_field) {
							state->cursor = resumePoint;
							found		  = scanFields<false>(key, state->tapeEnd);
							if (found.err == error_code::success) {
								state->noteForwardHit();
							}
						}
					} else {
						found = scanFields<false>(key, state->tapeEnd);
						if (found.err == error_code::success) {
							if (resumed) {
								state->noteForwardHit();
							}
						} else if (found.err == error_code::no_such_field && resumed) {
							found = scanBeforeResume(key, resumePoint);
						}
					}
					if (found.err == error_code::no_such_field) {
						found = escapeAwareRetry(key, resumePoint);
					}
					return found;
				}
				indexAllFields();
				indexed = true;
			}

			const uint32_t object = objectIndex();
			const uint64_t hash	  = field_index_map::hashKey(std::bit_cast<read_buffer_ptr>(key.data()), key.size(), object);
			const uint32_t hit	  = state->fieldIndex->template find<key_matcher>(hash, object, state, key);
			if (hit != field_index_map::npos) {
				state->cursor = state->tape + hit + 2;
				found		  = value{ state, state->cursor, depth };
			}
			if (found.err == error_code::no_such_field) {
				found = escapeAwareRetry(key, resumePoint);
			}
			return found;
		}

		JSONIFIER_INLINE value scanBeforeResume(string_view key, read_structural_index_ptr resumePoint) const noexcept {
			doc->cursor = open + 1;
			value found{ scanFields<false>(key, resumePoint) };
			indexEarned = ++wrapCount >= wrapThreshold && skippedKeys >= indexThreshold;
			if (found.err == error_code::success) {
				doc->noteBackwardHit();
			}
			return found;
		}

		JSONIFIER_INLINE value extendTrail(string_view key, read_structural_index_ptr resumePoint, bool resumed) const noexcept {
			document_state* const state		  = doc;
			read_structural_index_ptr tapeEnd = state->tapeEnd;
			read_structural_index_ptr cursor  = trailCount == 0 ? open + 1 : state->skipValue(state->tape + trailEntry(trailCount - 1) + 2);
			while (cursor < tapeEnd) {
				const uint8_t current = state->charAt(cursor);
				if (current == '}') {
					trailComplete = true;
					return value{ error_code::no_such_field };
				}
				if (current == ',') {
					++cursor;
					continue;
				}
				if (current == '"' && cursor + 1 < tapeEnd && state->template charAt<':'>(cursor + 1)) [[likely]] {
					if (trailCount == trailCapacity) {
						trailOverflow = true;
						return value{ error_code::no_such_field };
					}
					const uint32_t keyOffset = static_cast<uint32_t>(cursor - state->tape);
					pow2MemcpyWrapper<sizeof(uint32_t)>(trailBytes + trailCount * sizeof(uint32_t), &keyOffset);
					++trailCount;
					if (state->keyMatches(cursor, key)) {
						return trailHit(cursor, resumePoint, resumed);
					}
					cursor += 2;
				}
				cursor = state->skipValue(cursor);
			}
			trailOverflow = true;
			return value{ error_code::no_such_field };
		}

		JSONIFIER_INLINE uint32_t trailEntry(uint32_t index) const noexcept {
			uint32_t keyOffset;
			pow2MemcpyWrapper<sizeof(uint32_t)>(&keyOffset, trailBytes + index * sizeof(uint32_t));
			return keyOffset;
		}

		JSONIFIER_INLINE value trailHit(read_structural_index_ptr keyIter, read_structural_index_ptr resumePoint, bool resumed) const noexcept {
			if (resumed) {
				if (keyIter < resumePoint) {
					doc->noteBackwardHit();
				} else {
					doc->noteForwardHit();
				}
			}
			doc->cursor = keyIter + 2;
			return value{ doc, keyIter + 2, depth };
		}

		JSONIFIER_INLINE value trailLookup(string_view key, read_structural_index_ptr resumePoint, bool resumed) const noexcept {
			document_state* const state = doc;
			for (uint32_t x = 0; x < trailCount; ++x) {
				read_structural_index_ptr keyIter = state->tape + trailEntry(x);
				if (state->keyMatches(keyIter, key)) {
					return trailHit(keyIter, resumePoint, resumed);
				}
			}
			if (!trailComplete) {
				value found{ extendTrail(key, resumePoint, resumed) };
				if (found.err == error_code::success || trailOverflow) {
					return found;
				}
			}
			if (!escapeFree) {
				bool sawEscape{};
				for (uint32_t x = 0; x < trailCount; ++x) {
					read_structural_index_ptr keyIter = state->tape + trailEntry(x);
					if (state->keyHasEscape(keyIter)) {
						sawEscape = true;
						if (state->unescapedKeyMatches(keyIter, key)) {
							return trailHit(keyIter, resumePoint, resumed);
						}
					}
				}
				escapeFree = !sawEscape;
			}
			state->cursor = resumePoint;
			return value{ error_code::no_such_field };
		}

		JSONIFIER_INLINE void indexAllFields() const noexcept {
			document_state* const state		  = doc;
			read_structural_index_ptr cursor  = open + 1;
			read_structural_index_ptr tapeEnd = state->tapeEnd;
			while (cursor < tapeEnd) {
				const uint8_t current = state->charAt(cursor);
				if (current == '}') {
					return;
				}
				if (current == ',') {
					++cursor;
					continue;
				}
				if (current == '"' && cursor + 1 < tapeEnd && state->template charAt<':'>(cursor + 1)) [[likely]] {
					recordField(cursor);
					cursor += 2;
				}
				cursor = state->skipValue(cursor);
			}
		}

		template<bool escapeAware> JSONIFIER_INLINE value scanFields(string_view key, read_structural_index_ptr stop) const noexcept {
			document_state* const state		  = doc;
			read_structural_index_ptr cursor  = state->cursor;
			read_structural_index_ptr tapeEnd = state->tapeEnd;
			uint32_t skipped{ skippedKeys };
			bool expectSeparator{};
			while (cursor < stop) {
				const uint8_t current = state->charAt(cursor);
				if (current == ',') {
					++cursor;
					expectSeparator = false;
					continue;
				}
				if (current == '}') {
					state->cursor = cursor;
					skippedKeys   = skipped;
					return value{ error_code::no_such_field };
				}
				if (expectSeparator) [[unlikely]] {
					state->cursor = cursor;
					skippedKeys   = skipped;
					return value{ error_code::tape_error };
				}
				if (current == '"' && cursor + 1 < tapeEnd && state->template charAt<':'>(cursor + 1)) [[likely]] {
					read_structural_index_ptr valueIter = cursor + 2;
					bool matched{};
					if constexpr (escapeAware) {
						const bool hasEscape = state->keyHasEscape(cursor);
						escapedKeySeen |= hasEscape;
						matched = hasEscape && state->unescapedKeyMatches(cursor, key);
					} else {
						matched = state->keyMatches(cursor, key);
					}
					if (matched) {
						state->cursor = valueIter;
						skippedKeys   = skipped;
						return value{ state, valueIter, depth };
					}
					++skipped;
					cursor = valueIter;
				}
				cursor			= state->skipValue(cursor);
				expectSeparator = true;
			}
			state->cursor = cursor;
			skippedKeys   = skipped;
			return value{ cursor >= tapeEnd ? error_code::unclosed_container : error_code::no_such_field };
		}

		JSONIFIER_INLINE value expectedField(string_view key) const noexcept {
			document_state* const state			= doc;
			read_structural_index_ptr candidate = state->cursor;
			if (candidate < state->tapeEnd && state->template charAt<','>(candidate)) {
				++candidate;
			}
			if (candidate + 1 < state->tapeEnd && state->template charAt<'"'>(candidate) && state->template charAt<':'>(candidate + 1) && state->keyMatches(candidate, key))
				[[likely]] {
				state->cursor = candidate + 2;
				return value{ state, candidate + 2, depth };
			}
			return value{ error_code::no_such_field };
		}

		JSONIFIER_INLINE void recordField(read_structural_index_ptr keyIter) const noexcept {
			document_state* const state = doc;
			read_buffer_ptr keyStart	= state->root + *keyIter + 1;
			read_buffer_ptr keyEnd		= state->root + *(keyIter + 1);
			while (keyEnd > keyStart && *--keyEnd != '"') {
			}
			const uint32_t object = objectIndex();
			state->fieldIndex->insert(field_index_map::hashKey(keyStart, static_cast<uint64_t>(keyEnd - keyStart), object), object, static_cast<uint32_t>(keyIter - state->tape));
		}

		JSONIFIER_INLINE value escapeAwareRetry(string_view key, read_structural_index_ptr restorePoint) const noexcept {
			if (escapeFree) {
				doc->cursor = restorePoint;
				return value{ error_code::no_such_field };
			}
			doc->cursor	   = open + 1;
			escapedKeySeen = false;
			value found{ scanFields<true>(key, doc->tapeEnd) };
			if (found.err == error_code::no_such_field) {
				escapeFree	= !escapedKeySeen;
				doc->cursor = restorePoint;
			}
			return found;
		}

		JSONIFIER_INLINE void settle() const noexcept {
			if (doc->depth != depth) [[unlikely]] {
				if (doc->depth > depth) {
					doc->finishToDepth(depth);
				}
				if (doc->depth != depth) {
					rewind();
				}
			}
		}

		JSONIFIER_INLINE uint32_t objectIndex() const noexcept {
			return static_cast<uint32_t>(open - doc->tape);
		}

		JSONIFIER_INLINE void rewind() const noexcept {
			doc->cursor = open + 1;
			doc->depth	= depth;
		}

		mutable uint8_t trailBytes[trailCapacity * sizeof(uint32_t)]{};
		error_code err{ error_code::success };
		read_structural_index_ptr open{};
		mutable uint32_t skippedKeys{};
		mutable uint32_t trailCount{};
		mutable bool escapedKeySeen{};
		mutable bool trailComplete{};
		mutable uint32_t wrapCount{};
		mutable bool trailOverflow{};
		mutable bool indexEarned{};
		mutable bool escapeFree{};
		mutable bool indexed{};
		document_state* doc{};
		uint64_t depth{};
	};

	template<bool minified> class basic_array {
	  public:
		using document_state = generic::document_state<minified>;
		using value			 = basic_value<minified>;

		class iterator {
		  public:
			using iterator_category = std::forward_iterator_tag;
			using value_type		= value;
			using difference_type	= ptrdiff_t;

			JSONIFIER_INLINE iterator& operator++() noexcept {
				if (err != error_code::success) [[unlikely]] {
					iter = nullptr;
					return *this;
				}
				err = doc->advanceChild(iter, depth, ']', iter);
				validateElementStart();
				return *this;
			}

			JSONIFIER_INLINE iterator(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew) noexcept : iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
				validateElementStart();
			}

			JSONIFIER_INLINE value operator*() const noexcept {
				return err == error_code::success ? value{ doc, iter, depth } : value{ err };
			}

			JSONIFIER_INLINE bool operator==(const iterator& other) const noexcept {
				return iter == other.iter;
			}

			JSONIFIER_INLINE iterator() noexcept = default;

		  protected:
			JSONIFIER_INLINE void validateElementStart() noexcept {
				if (err == error_code::success && iter && document_state::isCloser(doc->charAt(iter))) [[unlikely]] {
					err = error_code::tape_error;
				}
			}

			error_code err{ error_code::success };
			read_structural_index_ptr iter{};
			document_state* doc{};
			uint64_t depth{};
		};

		JSONIFIER_INLINE iterator begin() const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return iterator{};
			}
			document_state* const state		   = doc;
			read_structural_index_ptr position = open + 1;
			if (position < state->tapeEnd && state->template charAt<']'>(position)) {
				state->cursor = position + 1;
				state->depth  = depth - 1;
				return iterator{};
			}
			state->cursor = position;
			state->depth  = depth;
			if (position >= state->tapeEnd) [[unlikely]] {
				return iterator{};
			}
			return iterator{ state, position, depth };
		}

		[[nodiscard]] JSONIFIER_INLINE error_code countElements(uint64_t& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			out = 0;
			for (auto iter = begin(); iter != end(); ++iter) {
				if (const error_code childError = (*iter).error(); childError != error_code::success) [[unlikely]] {
					return childError;
				}
				++out;
			}
			return error_code::success;
		}

		JSONIFIER_INLINE value at(uint64_t index) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return value{ err };
			}
			for (auto iter = begin(); iter != end(); ++iter) {
				if (index-- == 0) {
					return *iter;
				}
			}
			return value{ error_code::index_out_of_bounds };
		}

		JSONIFIER_INLINE basic_array(document_state* docNew, read_structural_index_ptr openNew, uint64_t depthNew) noexcept : open{ openNew }, doc{ docNew }, depth{ depthNew } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_code rawJson(string_view& out) const noexcept {
			return value{ doc, open, depth - 1 }.rawJson(out);
		}

		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept {
			return value{ doc, open, depth - 1 }.atPointer(pointer);
		}

		JSONIFIER_INLINE value operator[](uint64_t index) const noexcept {
			return at(index);
		}

		JSONIFIER_INLINE iterator end() const noexcept {
			return iterator{};
		}

		JSONIFIER_INLINE basic_array(error_code errNew) noexcept : err{ errNew } {
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE basic_array() noexcept = default;

	  protected:
		error_code err{ error_code::success };
		read_structural_index_ptr open{};
		document_state* doc{};
		uint64_t depth{};
	};

	template<bool minified> JSONIFIER_INLINE basic_object<minified> basic_value<minified>::getObject() const noexcept {
		if (err != error_code::success) [[unlikely]] {
			return object{ err };
		}
		if (!doc->template charAt<'{'>(iter)) [[unlikely]] {
			return object{ error_code::incorrect_type };
		}
		doc->cursor = iter + 1;
		doc->depth	= depth + 1;
		return object{ doc, iter, depth + 1 };
	}

	template<bool minified> JSONIFIER_INLINE basic_array<minified> basic_value<minified>::getArray() const noexcept {
		if (err != error_code::success) [[unlikely]] {
			return array{ err };
		}
		if (!doc->template charAt<'['>(iter)) [[unlikely]] {
			return array{ error_code::incorrect_type };
		}
		doc->cursor = iter + 1;
		doc->depth	= depth + 1;
		return array{ doc, iter, depth + 1 };
	}

	template<bool minified> template<jsonifier::internal::string_view_convertible_types key_type> JSONIFIER_INLINE basic_value<minified> basic_value<minified>::operator[](key_type&& key) const noexcept {
		return getObject().findFieldUnordered(static_cast<string_view>(key));
	}

	template<bool minified> JSONIFIER_INLINE basic_value<minified> basic_value<minified>::operator[](uint64_t index) const noexcept {
		return getArray().at(index);
	}

	template<bool minified> JSONIFIER_INLINE basic_value<minified> basic_value<minified>::findField(string_view key) const noexcept {
		return getObject().findField(key);
	}

	template<bool minified> JSONIFIER_INLINE basic_value<minified> basic_value<minified>::findFieldUnordered(string_view key) const noexcept {
		return getObject().findFieldUnordered(key);
	}

	template<bool minified> JSONIFIER_INLINE basic_value<minified> basic_value<minified>::atPointer(string_view pointer) const noexcept {
		value current{ *this };
		string unescapedToken{};
		const char* position   = pointer.data();
		const char* pointerEnd = position + pointer.size();
		while (current.err == error_code::success && position < pointerEnd) {
			if (*position != '/') [[unlikely]] {
				current = value{ error_code::invalid_json_pointer };
			} else {
				++position;
				const char* slash	 = jsonifier::internal::char_comparison<'/', char>::memchar(position, static_cast<uint64_t>(pointerEnd - position));
				const char* tokenEnd = slash ? slash : pointerEnd;
				current				 = current.pointerStep(string_view{ position, static_cast<uint64_t>(tokenEnd - position) }, unescapedToken);
				position			 = tokenEnd;
			}
		}
		return current;
	}

	template<bool minified> JSONIFIER_INLINE basic_value<minified> basic_value<minified>::pointerStep(string_view token, string& unescapedToken) const noexcept {
		switch (doc->charAt(iter)) {
			case '{': {
				return unescapePointerToken(token, unescapedToken) ? findField(token) : value{ error_code::invalid_json_pointer };
			}
			case '[': {
				uint64_t index{};
				const error_code indexError = parsePointerIndex(token, index);
				return indexError == error_code::success ? getArray().at(index) : value{ indexError };
			}
			default: {
				return value{ error_code::invalid_json_pointer };
			}
		}
	}

	template<bool minified> JSONIFIER_INLINE bool basic_value<minified>::unescapePointerToken(string_view& token, string& unescapedToken) noexcept {
		if (!jsonifier::internal::char_comparison<'~', char>::memchar(token.data(), token.size())) {
			return true;
		}
		unescapedToken.clear();
		for (uint64_t x = 0; x < token.size(); ++x) {
			if (token[x] != '~') {
				unescapedToken.emplace_back(token[x]);
			} else if (x + 1 < token.size() && (token[x + 1] == '0' || token[x + 1] == '1')) {
				unescapedToken.emplace_back(token[x + 1] == '0' ? '~' : '/');
				++x;
			} else {
				return false;
			}
		}
		token = unescapedToken;
		return true;
	}

	template<bool minified> JSONIFIER_INLINE error_code basic_value<minified>::parsePointerIndex(string_view token, uint64_t& index) noexcept {
		if (token.empty() || (token.size() > 1 && token[0] == '0')) [[unlikely]] {
			return error_code::invalid_json_pointer;
		}
		index = 0;
		for (const char digit: token) {
			if (digit < '0' || digit > '9') [[unlikely]] {
				return error_code::invalid_json_pointer;
			}
			const uint64_t digitValue = static_cast<uint64_t>(digit - '0');
			if (index > (std::numeric_limits<uint64_t>::max() - digitValue) / 10) [[unlikely]] {
				return error_code::index_out_of_bounds;
			}
			index = index * 10 + digitValue;
		}
		return error_code::success;
	}

	template<bool minified> template<typename value_type> JSONIFIER_INLINE error_code basic_value<minified>::get(value_type& out) const {
		if constexpr (std::is_same_v<value_type, double>) {
			return getDouble(out);
		} else if constexpr (std::is_same_v<value_type, int64_t>) {
			return getInt64(out);
		} else if constexpr (std::is_same_v<value_type, uint64_t>) {
			return getUint64(out);
		} else if constexpr (std::is_same_v<value_type, bool>) {
			return getBool(out);
		} else if constexpr (std::is_same_v<value_type, std::string_view> || internal::buffer_like_types<value_type>) {
			return getString(out);
		} else if constexpr (std::is_same_v<value_type, object>) {
			out = getObject();
			return out.error();
		} else if constexpr (std::is_same_v<value_type, array>) {
			out = getArray();
			return out.error();
		} else {
			static_assert(std::is_same_v<value_type, value>, "Unsupported generic::value::get type.");
			out = *this;
			return err;
		}
	}

	template<bool minified> class basic_document : public basic_value<minified> {
	  public:
		using basic_value<minified>::basic_value;

		JSONIFIER_INLINE bool atEnd() const noexcept {
			return this->err == error_code::success && this->doc->depth == 0 && this->doc->cursor == this->doc->tapeEnd;
		}
	};

	using document = basic_document<false>;
	using object   = basic_object<false>;
	using array	   = basic_array<false>;
	using field	   = basic_field<false>;
	using value	   = basic_value<false>;

	struct generic_storage {
		generic_storage& operator=(generic_storage&& other) noexcept {
			if (this != &other) [[likely]] {
				fieldIndex			= internal::move(other.fieldIndex);
				arena				= internal::move(other.arena);
				state				= document_state<false>{};
				minifiedState		= document_state<true>{};
				other.state			= document_state<false>{};
				other.minifiedState = document_state<true>{};
			}
			return *this;
		}

		generic_storage(generic_storage&& other) noexcept : fieldIndex{ internal::move(other.fieldIndex) }, arena{ internal::move(other.arena) } {
			other.state			= document_state<false>{};
			other.minifiedState = document_state<true>{};
		}

		template<bool minified> JSONIFIER_INLINE document_state<minified>& stateFor() noexcept {
			if constexpr (minified) {
				return minifiedState;
			} else {
				return state;
			}
		}

		generic_storage& operator=(const generic_storage&) = delete;
		generic_storage(const generic_storage&)			   = delete;
		generic_storage() noexcept						   = default;
		document_state<true> minifiedState{};
		field_index_map fieldIndex{};
		document_state<false> state{};
		string_arena arena{};
	};

	template<typename parser_type, parse_options options> class document_stream {
	  public:
		using document_state = generic::document_state<options.minified>;
		using document		 = basic_document<options.minified>;

		class iterator {
		  public:
			using iterator_category = std::input_iterator_tag;
			using value_type		= document;
			using difference_type	= std::ptrdiff_t;

			JSONIFIER_INLINE bool operator==(const iterator& other) const noexcept {
				return exhausted() == other.exhausted();
			}

			JSONIFIER_INLINE explicit iterator(document_stream* streamNew) noexcept : stream{ streamNew } {
			}

			JSONIFIER_INLINE uint64_t currentIndex() const noexcept {
				return stream->currentIndex();
			}

			JSONIFIER_INLINE iterator& operator++() {
				stream->advance();
				return *this;
			}

			JSONIFIER_INLINE string_view source() const {
				return stream->source();
			}

			JSONIFIER_INLINE document operator*() const {
				return stream->current();
			}

			JSONIFIER_INLINE void operator++(int32_t) {
				stream->advance();
			}

			JSONIFIER_INLINE iterator() noexcept = default;

		  protected:
			JSONIFIER_INLINE bool exhausted() const noexcept {
				return !stream || stream->finished;
			}

			document_stream* stream{};
		};

		JSONIFIER_INLINE document_stream(parser_type& parserNew, read_buffer_ptr inputNew, uint64_t inputLengthNew, uint64_t batchSizeNew) noexcept
			: input{ inputNew }, inputLength{ inputNew ? inputLengthNew : 0 }, parser{ &parserNew },
			  batchSize{ std::clamp<uint64_t>(batchSizeNew, minimumBatchSize, maximumBatchSize) } {
		}

		JSONIFIER_INLINE iterator begin() {
			if (!started) {
				started = true;
				if (inputLength == 0) {
					finished = true;
				} else {
					loadWindow(0);
					locate(windowTape);
				}
			}
			return iterator{ this };
		}

		JSONIFIER_INLINE iterator end() noexcept {
			return iterator{};
		}

		JSONIFIER_INLINE uint64_t truncatedBytes() const noexcept {
			return truncated;
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		document_stream& operator=(const document_stream&) = delete;
		document_stream(const document_stream&)			   = delete;

	  protected:
		static constexpr uint64_t minimumBatchSize{ 64 };
		static constexpr uint64_t maximumBatchSize{ std::numeric_limits<uint32_t>::max() - 64 };

		JSONIFIER_INLINE void locate(read_structural_index_ptr next) {
			if constexpr (!options.newLineDelimited) {
				locateDelimited(next);
			} else {
				locateWhitespace(next);
			}
		}

		JSONIFIER_INLINE void locateWhitespace(read_structural_index_ptr next) {
			while (true) {
				if (next == completeEnd) {
					if (finalWindow) {
						finished = true;
						return;
					}
					if (completeEnd == windowTape) [[unlikely]] {
						err = error_code::capacity;
						return;
					}
					loadWindow(windowStart + (completeEnd == windowTapeEnd ? windowLength : *completeEnd));
					next = windowTape;
					continue;
				}
				if (!startsDocument(docState().charAt(next))) [[unlikely]] {
					err = error_code::tape_error;
					return;
				}
				bindDocument(next, completeEnd);
				return;
			}
		}

		JSONIFIER_INLINE void locateDelimited(read_structural_index_ptr next) {
			while (true) {
				if (next == windowTapeEnd) {
					if (finalWindow) {
						if (pendingSeparator) [[unlikely]] {
							err = error_code::trailing_content;
						} else {
							finished = true;
						}
						return;
					}
					loadWindow(windowStart + windowLength);
					next = windowTape;
					continue;
				}
				if (docBegin && !pendingSeparator && docState().template charAt<','>(next)) {
					++next;
					pendingSeparator = true;
					continue;
				}
				if (read_structural_index_ptr documentEnd = findDocumentEnd(next); documentEnd) [[likely]] {
					bindDocument(next, documentEnd);
					return;
				}
				if (err != error_code::success) [[unlikely]] {
					return;
				}
				if (finalWindow) {
					truncated = windowLength - *next;
					finished  = true;
					return;
				}
				if (next == windowTape) [[unlikely]] {
					err = error_code::capacity;
					return;
				}
				loadWindow(windowStart + *next);
				next = windowTape;
			}
		}

		JSONIFIER_INLINE static bool startsDocument(uint8_t c) noexcept {
			return !document_state::isCloser(c) && c != ',' && c != ':';
		}

		JSONIFIER_INLINE static bool endsValue(uint8_t c) noexcept {
			return !document_state::isOpener(c) && c != ',' && c != ':';
		}

		JSONIFIER_INLINE internal::unrestrict_t<read_structural_index_ptr> lastDocumentStart() const {
			const document_state& state = docState();
			for (read_structural_index_ptr iter = windowTapeEnd - 1; iter > windowTape; --iter) {
				if (startsDocument(state.charAt(iter)) && endsValue(state.charAt(iter - 1))) {
					return iter;
				}
			}
			return windowTape;
		}

		JSONIFIER_INLINE void findCompleteEnd() {
			if (windowTape == windowTapeEnd) {
				completeEnd = windowTapeEnd;
				return;
			}
			read_structural_index_ptr lastStart = lastDocumentStart();
			if (lastStart != windowTape && !finalWindow) {
				completeEnd = lastStart;
				return;
			}
			if (!startsDocument(docState().charAt(lastStart)) || findDocumentEnd(lastStart)) {
				completeEnd = windowTapeEnd;
				return;
			}
			completeEnd = lastStart;
			if (finalWindow) {
				truncated = windowLength - *lastStart;
			}
		}

		JSONIFIER_INLINE internal::unrestrict_t<read_structural_index_ptr> consumedEnd() {
			document_state& state = docState();
			if (state.depth > 0) {
				state.finishToDepth(0);
				return state.cursor;
			}
			if (state.cursor > docBegin) {
				return state.cursor;
			}
			return state.skipValue(docBegin);
		}

		JSONIFIER_INLINE internal::unrestrict_t<read_structural_index_ptr> findDocumentEnd(read_structural_index_ptr start) {
			const document_state& state = docState();
			const uint8_t first			= state.charAt(start);
			if (document_state::isOpener(first)) {
				int64_t openCount{ 1 };
				for (read_structural_index_ptr iter = start + 1; iter < windowTapeEnd; ++iter) {
					openCount += depthDeltaTable[state.charAt(iter)];
					if (openCount == 0) {
						return iter + 1;
					}
				}
				return nullptr;
			}
			if (document_state::isCloser(first) || first == ',' || first == ':') [[unlikely]] {
				err = error_code::tape_error;
				return nullptr;
			}
			if (start + 1 == windowTapeEnd && !finalWindow) {
				return nullptr;
			}
			return start + 1;
		}

		JSONIFIER_INLINE generic_storage& storage() const {
			return parser->genericStorage();
		}

		JSONIFIER_INLINE document_state& docState() const {
			return storage().template stateFor<options.minified>();
		}

		JSONIFIER_INLINE void loadWindow(uint64_t start) {
			windowStart				   = start;
			windowLength			   = std::min(batchSize, inputLength - start);
			finalWindow				   = start + windowLength == inputLength;
			read_buffer_ptr windowRoot = input + start;
			parser->template indexGenericWindow<options>(windowRoot, windowLength);
			generic_storage& data  = storage();
			document_state& state  = data.template stateFor<options.minified>();
			windowTape			   = state.tape;
			windowTapeEnd		   = state.tapeEnd;
			state.root			   = windowRoot;
			state.end			   = windowRoot + windowLength;
			data.arena.reset(windowLength);
			state.arena			   = &data.arena;
			state.fieldIndex	   = &data.fieldIndex;
			if constexpr (options.newLineDelimited) {
				findCompleteEnd();
			}
		}

		JSONIFIER_INLINE string_view source() const {
			if (err != error_code::success) [[unlikely]] {
				return {};
			}
			read_buffer_ptr base = input + windowStart;
			const uint64_t start = *docBegin;
			uint64_t stop{};
			if constexpr (!options.newLineDelimited) {
				stop = *docEnd;
			} else {
				stop = *docState().skipValue(docBegin);
			}
			while (stop > start && isSeparatorByte(base[stop - 1])) {
				--stop;
			}
			return string_view{ std::bit_cast<const char*>(base + start), stop - start };
		}

		JSONIFIER_INLINE void bindDocument(read_structural_index_ptr begin, read_structural_index_ptr end) {
			docBegin			  = begin;
			docEnd				  = end;
			pendingSeparator	  = false;
			document_state& state = docState();
			state.tape			  = begin;
			state.tapeEnd		  = end;
			state.cursor		  = begin;
			state.depth			  = 0;
			storage().fieldIndex.reset();
		}

		JSONIFIER_INLINE document current() const {
			if (err != error_code::success) [[unlikely]] {
				return document{ err };
			}
			return document{ &docState(), docBegin, 0 };
		}

		JSONIFIER_INLINE void advance() {
			if (err != error_code::success) [[unlikely]] {
				finished = true;
				return;
			}
			if constexpr (!options.newLineDelimited) {
				locate(docEnd);
			} else {
				locate(consumedEnd());
			}
		}

		JSONIFIER_INLINE uint64_t currentIndex() const noexcept {
			return err == error_code::success ? windowStart + *docBegin : windowStart;
		}

		JSONIFIER_INLINE static bool isSeparatorByte(uint8_t c) noexcept {
			return seperatorByteTable[c];
		}

		read_structural_index_ptr windowTapeEnd{};
		read_structural_index_ptr completeEnd{};
		read_structural_index_ptr windowTape{};
		error_code err{ error_code::success };
		read_structural_index_ptr docBegin{};
		read_structural_index_ptr docEnd{};
		read_buffer_ptr input{};
		uint64_t windowLength{};
		bool pendingSeparator{};
		uint64_t inputLength{};
		uint64_t windowStart{};
		parser_type* parser{};
		uint64_t batchSize{};
		uint64_t truncated{};
		bool finalWindow{};
		bool finished{};
		bool started{};
	};

}
