// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/utf8_validation.hpp
#pragma once

#include "common.hpp"

#if defined(__unix__) || defined(__APPLE__)
	#include <sys/mman.h>
	#include <unistd.h>
	#define JSONIFIER_HAS_MMAP 1
#else
	#define JSONIFIER_HAS_MMAP 0
#endif

namespace utf8_validation_tests {

	struct utf8_test_case {
		std::vector<uint8_t> bytes;
		bool expected;
	};

	inline std::vector<uint8_t> repeated(uint8_t value, size_t count) {
		return std::vector<uint8_t>(count, value);
	}

	inline std::vector<uint8_t> repeatedSeq(std::vector<uint8_t> seq, size_t times) {
		std::vector<uint8_t> v;
		v.reserve(seq.size() * times);
		for (size_t i = 0; i < times; ++i) {
			v.insert(v.end(), seq.begin(), seq.end());
		}
		return v;
	}

	inline std::vector<uint8_t> concatSeqs(std::vector<std::vector<uint8_t>> seqs) {
		std::vector<uint8_t> v;
		for (const auto& seq: seqs) {
			v.insert(v.end(), seq.begin(), seq.end());
		}
		return v;
	}

	inline std::vector<uint8_t> paddedWithTrailing(size_t leadingAsciiCount, std::vector<uint8_t> trailer) {
		std::vector<uint8_t> v(leadingAsciiCount, 0x41);
		v.insert(v.end(), trailer.begin(), trailer.end());
		return v;
	}

	inline std::vector<uint8_t> asciiThenSeq(size_t asciiCount, std::vector<uint8_t> seq) {
		std::vector<uint8_t> v(asciiCount, 0x41);
		v.insert(v.end(), seq.begin(), seq.end());
		return v;
	}

	inline std::vector<uint8_t> asciiWrapped(size_t leadCount, std::vector<uint8_t> seq, size_t trailCount) {
		std::vector<uint8_t> v(leadCount, 0x41);
		v.insert(v.end(), seq.begin(), seq.end());
		v.insert(v.end(), trailCount, 0x41);
		return v;
	}

	inline std::vector<uint8_t> asciiWithSeqAt(size_t totalLength, size_t seqOffset, std::vector<uint8_t> seq) {
		std::vector<uint8_t> v(totalLength, 0x41);
		for (size_t i = 0; i < seq.size(); ++i) {
			v[seqOffset + i] = seq[i];
		}
		return v;
	}

	class reusable_ascii_buffer {
	  public:
		void resetTo(size_t totalLength) {
			static constexpr uint8_t asciiFill = 0x41;
			if (buffer.size() < totalLength) {
				buffer.assign(totalLength, asciiFill);
			} else {
				buffer.resize(totalLength);
				std::fill(buffer.begin(), buffer.end(), asciiFill);
			}
		}

		void placeSeqAt(size_t seqOffset, const std::vector<uint8_t>& seq) {
			for (size_t i = 0; i < seq.size(); ++i) {
				buffer[seqOffset + i] = seq[i];
			}
		}

		void clearSeqAt(size_t seqOffset, size_t seqLength) {
			for (size_t i = 0; i < seqLength; ++i) {
				buffer[seqOffset + i] = 0x41;
			}
		}

		const std::vector<uint8_t>& data() const {
			return buffer;
		}

	  private:
		std::vector<uint8_t> buffer;
	};

	template<rt_ut::string_literal testNameNew> inline static void utf8ValidationTest(std::vector<uint8_t> bytes, bool expected) {
		static constexpr rt_ut::string_literal testName{ testNameNew };
		rt_ut::unit_test<testName, true>::assert_eq(expected, jsonifier::validateUtf8, bytes.data(), bytes.size());
	}

	static constexpr jsonifier::parse_options utf8ValidatedOpts{ [] {
		jsonifier::parse_options opts{};
		opts.validateUtf8 = true;
		return opts;
	}() };

	inline static bool runValidatedStringParseWithScratch(const std::vector<uint8_t>& contentBytes, std::vector<uint8_t>& sourceScratch, std::vector<char>& destScratch) {
		sourceScratch.clear();
		sourceScratch.reserve(contentBytes.size() + 1);
		sourceScratch.insert(sourceScratch.end(), contentBytes.begin(), contentBytes.end());
		sourceScratch.push_back(static_cast<uint8_t>('"'));
		const size_t neededDestSize = sourceScratch.size() + 256;
		if (destScratch.size() < neededDestSize) {
			destScratch.assign(neededDestSize, 0);
		}
		jsonifier::string_view_ptr string1Start = std::bit_cast<jsonifier::string_view_ptr>(sourceScratch.data());
		jsonifier::string_buffer_ptr string2	= destScratch.data();
		using scanner_type						= jsonifier::internal::string_scanner<utf8ValidatedOpts>;
		const auto res							= scanner_type::impl(string1Start, string1Start + sourceScratch.size());
		if (!res.valid) {
			return false;
		}
		if (res.firstEscape == scanner_type::npos) {
			std::memcpy(string2, string1Start, res.rawLength);
			return true;
		}
		std::memcpy(string2, string1Start, res.firstEscape);
		return scanner_type::unescapeImpl(string1Start + res.firstEscape, string1Start + res.rawLength, string2 + res.firstEscape) != nullptr;
	}

	inline static bool runValidatedStringParse(const std::vector<uint8_t>& contentBytes) {
#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
		static thread_local std::vector<uint8_t> sourceScratch;
		static thread_local std::vector<char> destScratch;
#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic pop
#endif
		return runValidatedStringParseWithScratch(contentBytes, sourceScratch, destScratch);
	}

	template<rt_ut::string_literal testNameNew> inline static void utf8StringParseTest(std::vector<uint8_t> bytes, bool expected) {
		static constexpr rt_ut::string_literal testName{ testNameNew };
		rt_ut::unit_test<testName, true>::assert_eq(expected, runValidatedStringParse, bytes);
	}

	inline static void runUtf8WidthTransitionSweep() {
		static constexpr size_t sweepLengthMin	= 24;
		static constexpr size_t sweepLengthMax	= 224;
		static constexpr size_t asciiTailLength = 20;

		const std::vector<std::vector<uint8_t>> completeSequences{
			{ 0xC3, 0xA9 },
			{ 0xE0, 0xA0, 0x80 },
			{ 0xF0, 0x90, 0x80, 0x80 },
		};

		const std::vector<std::vector<uint8_t>> danglingSequences{
			{ 0xC2 },
			{ 0xE0 },
			{ 0xE0, 0xA0 },
			{ 0xF0 },
			{ 0xF0, 0x90 },
			{ 0xF0, 0x90, 0x80 },
		};

		uint64_t acceptCases = 0;
		uint64_t acceptFails = 0;
		uint64_t rejectCases = 0;
		uint64_t rejectFails = 0;

		reusable_ascii_buffer contentBuffer;
		std::vector<uint8_t> sourceScratch;
		std::vector<char> destScratch;

		for (size_t bodyLength = sweepLengthMin; bodyLength <= sweepLengthMax; ++bodyLength) {
			const size_t totalLength = bodyLength + asciiTailLength;

			for (const auto& seq: completeSequences) {
				if (seq.size() > bodyLength) {
					continue;
				}
				contentBuffer.resetTo(totalLength);
				for (size_t offset = 0; offset + seq.size() <= bodyLength; ++offset) {
					contentBuffer.placeSeqAt(offset, seq);
					++acceptCases;
					if (!runValidatedStringParseWithScratch(contentBuffer.data(), sourceScratch, destScratch)) {
						++acceptFails;
						if (acceptFails <= 8) {
							std::cout << "FALSE POSITIVE: valid " << seq.size() << "-byte sequence rejected at offset " << offset << " of body length " << bodyLength << " (total "
									  << totalLength << ")" << std::endl;
						}
					}
					contentBuffer.clearSeqAt(offset, seq.size());
				}
			}

			for (const auto& seq: danglingSequences) {
				if (seq.size() > bodyLength) {
					continue;
				}
				contentBuffer.resetTo(totalLength);
				for (size_t offset = 0; offset + seq.size() <= bodyLength; ++offset) {
					contentBuffer.placeSeqAt(offset, seq);
					++rejectCases;
					if (runValidatedStringParseWithScratch(contentBuffer.data(), sourceScratch, destScratch)) {
						++rejectFails;
						if (rejectFails <= 8) {
							std::cout << "FALSE NEGATIVE: dangling " << (seq.size() == 1 ? "lead" : "partial sequence") << " (first byte 0x" << std::hex
									  << static_cast<uint32_t>(seq[0]) << std::dec << ") accepted at offset " << offset << " of body length " << bodyLength << " (total "
									  << totalLength << ")" << std::endl;
						}
					}
					contentBuffer.clearSeqAt(offset, seq.size());
				}
			}
		}

		std::cout << "Width-transition accept sweep: " << (acceptCases - acceptFails) << "/" << acceptCases << " passed" << std::endl;
		std::cout << "Width-transition reject sweep: " << (rejectCases - rejectFails) << "/" << rejectCases << " passed" << std::endl;
		static constexpr rt_ut::string_literal acceptSweepName{ "width_transition_accept_sweep" };
		static constexpr rt_ut::string_literal rejectSweepName{ "width_transition_reject_sweep" };
		rt_ut::unit_test<acceptSweepName, true>::assert_eq(
			uint64_t{ 0 },
			[](uint64_t fails) {
				return fails;
			},
			acceptFails);
		rt_ut::unit_test<rejectSweepName, true>::assert_eq(
			uint64_t{ 0 },
			[](uint64_t fails) {
				return fails;
			},
			rejectFails);
	}

	inline static void runUnalignedPointerSweep() {
		static constexpr jsonifier::internal::array<uint8_t, 11> validSeq{ { 0x41, 0xC3, 0xA9, 0xE0, 0xA0, 0x80, 0xF0, 0x90, 0x80, 0x80, 0x41 } };
		static constexpr size_t maxAlignOffset = 64;

		uint64_t sweepCases = 0;
		uint64_t sweepFails = 0;

		std::vector<uint8_t> paddedBuffer(validSeq.size() + maxAlignOffset + 32, 0x41);

		for (size_t alignOffset = 0; alignOffset < maxAlignOffset; ++alignOffset) {
			std::copy(validSeq.begin(), validSeq.end(), paddedBuffer.begin() + static_cast<std::ptrdiff_t>(alignOffset));
			const uint8_t* slicePtr = paddedBuffer.data() + alignOffset;
			++sweepCases;
			if (!jsonifier::validateUtf8(slicePtr, validSeq.size())) {
				++sweepFails;
				if (sweepFails <= 8) {
					std::cout << "UNALIGNED SWEEP FAILURE: valid sequence rejected at pointer alignment offset " << alignOffset
							  << " (ptr mod 32 = " << (std::bit_cast<uintptr_t>(slicePtr) % 32) << ")" << std::endl;
				}
			}
			std::fill(paddedBuffer.begin() + static_cast<std::ptrdiff_t>(alignOffset),
				paddedBuffer.begin() + static_cast<std::ptrdiff_t>(alignOffset) + static_cast<std::ptrdiff_t>(validSeq.size()), static_cast<uint8_t>(0x41));
		}

		std::cout << "Unaligned pointer sweep: " << (sweepCases - sweepFails) << "/" << sweepCases << " passed" << std::endl;
		static constexpr rt_ut::string_literal unalignedSweepName{ "unaligned_pointer_sweep" };
		rt_ut::unit_test<unalignedSweepName, true>::assert_eq(
			uint64_t{ 0 },
			[](uint64_t fails) {
				return fails;
			},
			sweepFails);
	}

	inline static void runUnalignedInvalidSequenceSweep() {
		struct named_invalid_seq {
			jsonifier::string_view_ptr label;
			std::vector<uint8_t> bytes;
		};

		const std::vector<named_invalid_seq> invalidSeqs{
			{ "overlong_c0_80", { 0x41, 0xC0, 0x80, 0x41, 0x41, 0x41, 0x41, 0x41 } },
			{ "surrogate_ed_a0_80", { 0x41, 0xED, 0xA0, 0x80, 0x41, 0x41, 0x41, 0x41 } },
			{ "bad_cont_c2_41", { 0x41, 0xC2, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41 } },
			{ "dangling_e0_a0", { 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0xE0, 0xA0 } },
			{ "overlong_2byte_0x7f", { 0x41, 0xC1, 0xBF, 0x41, 0x41, 0x41, 0x41, 0x41 } },
			{ "overlong_4byte_min", { 0x41, 0xF0, 0x80, 0x80, 0x80, 0x41, 0x41, 0x41 } },
			{ "invalid_5byte_lead", { 0x41, 0xF8, 0x88, 0x80, 0x80, 0x80, 0x41, 0x41 } },
			{ "valid_then_lone_cont", { 0xC2, 0x80, 0x80, 0x41, 0x41, 0x41, 0x41, 0x41 } },
		};

		static constexpr size_t maxAlignOffset = 96;

		uint64_t sweepCases		  = 0;
		uint64_t falseAcceptFails = 0;

		for (const auto& namedSeq: invalidSeqs) {
			std::vector<uint8_t> paddedBuffer(namedSeq.bytes.size() + maxAlignOffset + 32, 0x41);

			for (size_t alignOffset = 0; alignOffset < maxAlignOffset; ++alignOffset) {
				std::copy(namedSeq.bytes.begin(), namedSeq.bytes.end(), paddedBuffer.begin() + static_cast<std::ptrdiff_t>(alignOffset));
				const uint8_t* slicePtr = paddedBuffer.data() + alignOffset;
				++sweepCases;
				if (jsonifier::validateUtf8(slicePtr, namedSeq.bytes.size())) {
					++falseAcceptFails;
					if (falseAcceptFails <= 8) {
						std::cout << "UNALIGNED INVALID-SEQUENCE FAILURE: invalid sequence '" << namedSeq.label << "' incorrectly accepted at pointer alignment offset "
								  << alignOffset << " (ptr mod 32 = " << (std::bit_cast<uintptr_t>(slicePtr) % 32) << ")" << std::endl;
					}
				}
				std::fill(paddedBuffer.begin() + static_cast<std::ptrdiff_t>(alignOffset),
					paddedBuffer.begin() + static_cast<std::ptrdiff_t>(alignOffset) + static_cast<std::ptrdiff_t>(namedSeq.bytes.size()), static_cast<uint8_t>(0x41));
			}
		}

		std::cout << "Unaligned invalid-sequence sweep: " << (sweepCases - falseAcceptFails) << "/" << sweepCases << " passed" << std::endl;
		static constexpr rt_ut::string_literal unalignedInvalidSweepName{ "unaligned_invalid_sequence_sweep" };
		rt_ut::unit_test<unalignedInvalidSweepName, true>::assert_eq(
			uint64_t{ 0 },
			[](uint64_t fails) {
				return fails;
			},
			falseAcceptFails);
	}

#if JSONIFIER_HAS_MMAP
	inline static void runPageBoundaryFaultTest() {
		const long systemPageSize = sysconf(_SC_PAGESIZE);
		const size_t pageSize	  = systemPageSize > 0 ? static_cast<size_t>(systemPageSize) : 4096;

		void* region = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		static constexpr rt_ut::string_literal pageBoundaryName{ "page_boundary_fault_check" };
		if (region == MAP_FAILED) {
			std::cout << "SKIPPED page_boundary_fault_check: mmap failed" << std::endl;
			rt_ut::unit_test<pageBoundaryName, true>::assert_eq(true, [] {
				return true;
			});
			return;
		}

		if (mprotect(static_cast<uint8_t*>(region) + pageSize, pageSize, PROT_NONE) != 0) {
			std::cout << "SKIPPED page_boundary_fault_check: mprotect failed" << std::endl;
			munmap(region, pageSize * 2);
			rt_ut::unit_test<pageBoundaryName, true>::assert_eq(true, [] {
				return true;
			});
			return;
		}

		uint8_t* stringPtr = static_cast<uint8_t*>(region) + pageSize - 10;
		std::memset(stringPtr, 0x41, 8);
		stringPtr[8] = 0xC3;
		stringPtr[9] = 0xA9;

		std::cout << "Running page-boundary fault check (valid 10-byte sequence ending exactly at unmapped page edge)" << std::endl;
		const bool result = jsonifier::validateUtf8(stringPtr, 10);
		std::cout << "Page-boundary fault check completed without segfault, result: " << (result ? "true" : "false") << std::endl;

		munmap(region, pageSize * 2);

		rt_ut::unit_test<pageBoundaryName, true>::assert_eq(
			true,
			[](bool r) {
				return r;
			},
			result);
	}
#endif

	inline static void runTests() {
		std::cout << "UTF8 Validation Tests" << std::endl;
		utf8ValidationTest<"empty">({}, true);
		utf8ValidationTest<"single_ascii">({ 0x41 }, true);
		utf8ValidationTest<"ascii_x16">(repeated(0x41, 16), true);
		utf8ValidationTest<"ascii_x64">(repeated(0x41, 64), true);
		utf8ValidationTest<"ascii_x256">(repeated(0x41, 256), true);
		utf8ValidationTest<"two_byte_U0080">({ 0xC2, 0x80 }, true);
		utf8ValidationTest<"two_byte_U07FF">({ 0xDF, 0xBF }, true);
		utf8ValidationTest<"three_byte_U0800">({ 0xE0, 0xA0, 0x80 }, true);
		utf8ValidationTest<"three_byte_UFFFF">({ 0xEF, 0xBF, 0xBF }, true);
		utf8ValidationTest<"four_byte_U10000">({ 0xF0, 0x90, 0x80, 0x80 }, true);
		utf8ValidationTest<"four_byte_U10FFFF">({ 0xF4, 0x8F, 0xBF, 0xBF }, true);
		utf8ValidationTest<"two_byte_x128">(repeatedSeq({ 0xC2, 0x80 }, 128), true);
		utf8ValidationTest<"three_byte_x64">(repeatedSeq({ 0xE0, 0xA0, 0x80 }, 64), true);
		utf8ValidationTest<"four_byte_x64">(repeatedSeq({ 0xF0, 0x90, 0x80, 0x80 }, 64), true);
		utf8ValidationTest<"two_byte_cross_chunk">(asciiWrapped(15, { 0xC2, 0x80 }, 15), true);
		utf8ValidationTest<"three_byte_cross_chunk">(asciiWrapped(14, { 0xE0, 0xA0, 0x80 }, 14), true);
		utf8ValidationTest<"four_byte_cross_chunk">(asciiWrapped(13, { 0xF0, 0x90, 0x80, 0x80 }, 14), true);
		utf8ValidationTest<"four_byte_cross_block_boundary">(asciiThenSeq(61, { 0xF0, 0x90, 0x80, 0x80 }), true);
		utf8ValidationTest<"mixed_ascii_plus_multibyte">({ 0x41, 0xC2, 0x80, 0x41, 0xE0, 0xA0, 0x80, 0x41, 0xF0, 0x90, 0x80, 0x80, 0x41, 0x41, 0x41, 0x41 }, true);
		utf8ValidationTest<"ascii_x64_then_two_byte">(asciiThenSeq(64, { 0xC2, 0x80 }), true);
		utf8ValidationTest<"ascii_x256_then_two_byte">(asciiThenSeq(256, { 0xC2, 0x80 }), true);
		utf8ValidationTest<"dangling_two_byte_lead">({ 0xC2 }, false);
		utf8ValidationTest<"dangling_three_byte_lead">({ 0xE0 }, false);
		utf8ValidationTest<"dangling_three_byte_lead_plus_1">({ 0xE0, 0xA0 }, false);
		utf8ValidationTest<"dangling_four_byte_lead">({ 0xF0 }, false);
		utf8ValidationTest<"dangling_four_byte_lead_plus_1">({ 0xF0, 0x90 }, false);
		utf8ValidationTest<"dangling_four_byte_lead_plus_2">({ 0xF0, 0x90, 0x80 }, false);
		utf8ValidationTest<"bad_cont_ascii_after_two_lead">({ 0xC2, 0x41 }, false);
		utf8ValidationTest<"bad_cont_ascii_after_three_lead">({ 0xE0, 0xA0, 0x41 }, false);
		utf8ValidationTest<"bad_cont_lead_after_lead">({ 0xC2, 0xC2, 0x80 }, false);
		utf8ValidationTest<"lone_continuation">({ 0x80 }, false);
		utf8ValidationTest<"lone_continuation_x4">({ 0x80, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"surrogate_UD800">({ 0xED, 0xA0, 0x80 }, false);
		utf8ValidationTest<"surrogate_UDFFF">({ 0xED, 0xBF, 0xBF }, false);
		utf8ValidationTest<"overlong_two_byte_U0000">({ 0xC0, 0x80 }, false);
		utf8ValidationTest<"overlong_two_byte_U007F">({ 0xC1, 0xBF }, false);
		utf8ValidationTest<"overlong_three_byte">({ 0xE0, 0x80, 0x80 }, false);
		utf8ValidationTest<"overlong_four_byte">({ 0xF0, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"too_large_U110000">({ 0xF4, 0x90, 0x80, 0x80 }, false);
		utf8ValidationTest<"too_large_0xF5">({ 0xF5, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"0xFF_byte">({ 0xFF }, false);
		utf8ValidationTest<"0xFE_byte">({ 0xFE }, false);
		utf8ValidationTest<"bad_cont_cross_chunk">(asciiWrapped(15, { 0xC2, 0x41 }, 15), false);
		utf8ValidationTest<"surrogate_cross_chunk">(asciiWrapped(14, { 0xED, 0xA0, 0x80 }, 14), false);
		utf8ValidationTest<"dangling_lead_at_block_end">(paddedWithTrailing(63, { 0xC2 }), false);
		utf8ValidationTest<"dangling_lead_at_step_end">(paddedWithTrailing(255, { 0xC2 }), false);
		utf8ValidationTest<"valid_then_lone_cont">({ 0xC2, 0x80, 0x80 }, false);
		std::cout << "UTF8 Markus Kuhn Stress Test Cases" << std::endl;
		utf8ValidationTest<"kuhn_1_correct_kosme">({ 0xCE, 0xBA, 0xE1, 0xBD, 0xB9, 0xCF, 0x83, 0xCE, 0xBC, 0xCE, 0xB5 }, true);
		utf8ValidationTest<"kuhn_2_1_1_first_1byte_U0000">({ 0x00 }, true);
		utf8ValidationTest<"kuhn_2_1_2_first_2byte_U0080">({ 0xC2, 0x80 }, true);
		utf8ValidationTest<"kuhn_2_1_3_first_3byte_U0800">({ 0xE0, 0xA0, 0x80 }, true);
		utf8ValidationTest<"kuhn_2_1_4_first_4byte_U10000">({ 0xF0, 0x90, 0x80, 0x80 }, true);
		utf8ValidationTest<"kuhn_2_1_5_first_5byte_illegal">({ 0xF8, 0x88, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_2_1_6_first_6byte_illegal">({ 0xFC, 0x84, 0x80, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_2_2_1_last_1byte_U007F">({ 0x7F }, true);
		utf8ValidationTest<"kuhn_2_2_2_last_2byte_U07FF">({ 0xDF, 0xBF }, true);
		utf8ValidationTest<"kuhn_2_2_3_last_3byte_UFFFF">({ 0xEF, 0xBF, 0xBF }, true);
		utf8ValidationTest<"kuhn_2_2_4_last_4byte_illegal">({ 0xF7, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_2_2_5_last_5byte_illegal">({ 0xFB, 0xBF, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_2_2_6_last_6byte_illegal">({ 0xFD, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_2_3_1_UD7FF">({ 0xED, 0x9F, 0xBF }, true);
		utf8ValidationTest<"kuhn_2_3_2_UE000">({ 0xEE, 0x80, 0x80 }, true);
		utf8ValidationTest<"kuhn_2_3_3_UFFFD">({ 0xEF, 0xBF, 0xBD }, true);
		utf8ValidationTest<"kuhn_2_3_4_U10FFFF">({ 0xF4, 0x8F, 0xBF, 0xBF }, true);
		utf8ValidationTest<"kuhn_2_3_5_U110000">({ 0xF4, 0x90, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_1_1_cont_0x80">({ 0x80 }, false);
		utf8ValidationTest<"kuhn_3_1_2_cont_0xbf">({ 0xBF }, false);
		utf8ValidationTest<"kuhn_3_1_3_cont_x2">({ 0x80, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_1_4_cont_x3">({ 0x80, 0xBF, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_1_5_cont_x4">({ 0x80, 0xBF, 0x80, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_1_6_cont_x5">({ 0x80, 0xBF, 0x80, 0xBF, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_1_7_cont_x6">({ 0x80, 0xBF, 0x80, 0xBF, 0x80, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_1_8_cont_x7">({ 0x80, 0xBF, 0x80, 0xBF, 0x80, 0xBF, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_1_9_all_64_cont_bytes">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t b = 0x80; b <= 0xBF; ++b) {
					v.push_back(static_cast<uint8_t>(b));
				}
				return v;
			}(),
			false);
		utf8ValidationTest<"kuhn_3_2_1_lonely_2byte_leads">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t b = 0xC0; b <= 0xDF; ++b) {
					v.push_back(static_cast<uint8_t>(b));
					v.push_back(0x20);
				}
				return v;
			}(),
			false);
		utf8ValidationTest<"kuhn_3_2_2_lonely_3byte_leads">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t b = 0xE0; b <= 0xEF; ++b) {
					v.push_back(static_cast<uint8_t>(b));
					v.push_back(0x20);
				}
				return v;
			}(),
			false);
		utf8ValidationTest<"kuhn_3_2_3_lonely_4byte_leads">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t b = 0xF0; b <= 0xF7; ++b) {
					v.push_back(static_cast<uint8_t>(b));
					v.push_back(0x20);
				}
				return v;
			}(),
			false);
		utf8ValidationTest<"kuhn_3_2_4_lonely_5byte_leads">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t b = 0xF8; b <= 0xFB; ++b) {
					v.push_back(static_cast<uint8_t>(b));
					v.push_back(0x20);
				}
				return v;
			}(),
			false);
		utf8ValidationTest<"kuhn_3_2_5_lonely_6byte_leads">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t b = 0xFC; b <= 0xFD; ++b) {
					v.push_back(static_cast<uint8_t>(b));
					v.push_back(0x20);
				}
				return v;
			}(),
			false);
		utf8ValidationTest<"kuhn_3_3_1_2byte_missing_last">({ 0xC0 }, false);
		utf8ValidationTest<"kuhn_3_3_2_3byte_missing_last">({ 0xE0, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_3_3_4byte_missing_last">({ 0xF0, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_3_4_5byte_missing_last">({ 0xF8, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_3_5_6byte_missing_last">({ 0xFC, 0x80, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_3_3_6_2byte_missing_last_max">({ 0xDF }, false);
		utf8ValidationTest<"kuhn_3_3_7_3byte_missing_last_max">({ 0xEF, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_3_8_4byte_missing_last_max">({ 0xF7, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_3_9_5byte_missing_last_max">({ 0xFB, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_3_10_6byte_missing_last_max">({ 0xFD, 0xBF, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_3_4_concat_incomplete">(concatSeqs({
															 { 0xC0 },
															 { 0xE0, 0x80 },
															 { 0xF0, 0x80, 0x80 },
															 { 0xF8, 0x80, 0x80, 0x80 },
															 { 0xFC, 0x80, 0x80, 0x80, 0x80 },
															 { 0xDF },
															 { 0xEF, 0xBF },
															 { 0xF7, 0xBF, 0xBF },
															 { 0xFB, 0xBF, 0xBF, 0xBF },
															 { 0xFD, 0xBF, 0xBF, 0xBF, 0xBF },
														 }),
			false);
		utf8ValidationTest<"kuhn_3_5_1_impossible_0xfe">({ 0xFE }, false);
		utf8ValidationTest<"kuhn_3_5_2_impossible_0xff">({ 0xFF }, false);
		utf8ValidationTest<"kuhn_3_5_3_impossible_fefeffff">({ 0xFE, 0xFE, 0xFF, 0xFF }, false);
		utf8ValidationTest<"kuhn_4_1_1_overlong_slash_2byte">({ 0xC0, 0xAF }, false);
		utf8ValidationTest<"kuhn_4_1_2_overlong_slash_3byte">({ 0xE0, 0x80, 0xAF }, false);
		utf8ValidationTest<"kuhn_4_1_3_overlong_slash_4byte">({ 0xF0, 0x80, 0x80, 0xAF }, false);
		utf8ValidationTest<"kuhn_4_1_4_overlong_slash_5byte">({ 0xF8, 0x80, 0x80, 0x80, 0xAF }, false);
		utf8ValidationTest<"kuhn_4_1_5_overlong_slash_6byte">({ 0xFC, 0x80, 0x80, 0x80, 0x80, 0xAF }, false);
		utf8ValidationTest<"kuhn_4_2_1_max_overlong_2byte">({ 0xC1, 0xBF }, false);
		utf8ValidationTest<"kuhn_4_2_2_max_overlong_3byte">({ 0xE0, 0x9F, 0xBF }, false);
		utf8ValidationTest<"kuhn_4_2_3_max_overlong_4byte">({ 0xF0, 0x8F, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_4_2_4_max_overlong_5byte">({ 0xF8, 0x87, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_4_2_5_max_overlong_6byte">({ 0xFC, 0x83, 0xBF, 0xBF, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_4_3_1_overlong_nul_2byte">({ 0xC0, 0x80 }, false);
		utf8ValidationTest<"kuhn_4_3_2_overlong_nul_3byte">({ 0xE0, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_4_3_3_overlong_nul_4byte">({ 0xF0, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_4_3_4_overlong_nul_5byte">({ 0xF8, 0x80, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_4_3_5_overlong_nul_6byte">({ 0xFC, 0x80, 0x80, 0x80, 0x80, 0x80 }, false);
		utf8ValidationTest<"kuhn_5_1_1_surrogate_UD800">({ 0xED, 0xA0, 0x80 }, false);
		utf8ValidationTest<"kuhn_5_1_2_surrogate_UDB7F">({ 0xED, 0xAD, 0xBF }, false);
		utf8ValidationTest<"kuhn_5_1_3_surrogate_UDB80">({ 0xED, 0xAE, 0x80 }, false);
		utf8ValidationTest<"kuhn_5_1_4_surrogate_UDBFF">({ 0xED, 0xAF, 0xBF }, false);
		utf8ValidationTest<"kuhn_5_1_5_surrogate_UDC00">({ 0xED, 0xB0, 0x80 }, false);
		utf8ValidationTest<"kuhn_5_1_6_surrogate_UDF80">({ 0xED, 0xBE, 0x80 }, false);
		utf8ValidationTest<"kuhn_5_1_7_surrogate_UDFFF">({ 0xED, 0xBF, 0xBF }, false);
		utf8ValidationTest<"kuhn_5_2_1_paired_surrogate_a">(concatSeqs({ { 0xED, 0xA0, 0x80 }, { 0xED, 0xB0, 0x80 } }), false);
		utf8ValidationTest<"kuhn_5_2_2_paired_surrogate_b">(concatSeqs({ { 0xED, 0xA0, 0x80 }, { 0xED, 0xBF, 0xBF } }), false);
		utf8ValidationTest<"kuhn_5_2_3_paired_surrogate_c">(concatSeqs({ { 0xED, 0xAD, 0xBF }, { 0xED, 0xB0, 0x80 } }), false);
		utf8ValidationTest<"kuhn_5_2_4_paired_surrogate_d">(concatSeqs({ { 0xED, 0xAD, 0xBF }, { 0xED, 0xBF, 0xBF } }), false);
		utf8ValidationTest<"kuhn_5_2_5_paired_surrogate_e">(concatSeqs({ { 0xED, 0xAE, 0x80 }, { 0xED, 0xB0, 0x80 } }), false);
		utf8ValidationTest<"kuhn_5_2_6_paired_surrogate_f">(concatSeqs({ { 0xED, 0xAE, 0x80 }, { 0xED, 0xBF, 0xBF } }), false);
		utf8ValidationTest<"kuhn_5_2_7_paired_surrogate_g">(concatSeqs({ { 0xED, 0xAF, 0xBF }, { 0xED, 0xB0, 0x80 } }), false);
		utf8ValidationTest<"kuhn_5_2_8_paired_surrogate_h">(concatSeqs({ { 0xED, 0xAF, 0xBF }, { 0xED, 0xBF, 0xBF } }), false);
		utf8ValidationTest<"kuhn_5_3_1_noncharacter_UFFFE">({ 0xEF, 0xBF, 0xBE }, true);
		utf8ValidationTest<"kuhn_5_3_2_noncharacter_UFFFF">({ 0xEF, 0xBF, 0xBF }, true);
		utf8ValidationTest<"kuhn_5_3_3_noncharacter_range_UFDD0_UFDEF">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t cp = 0xFDD0; cp <= 0xFDEF; ++cp) {
					v.push_back(static_cast<uint8_t>(0xE0 | (cp >> 12)));
					v.push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
					v.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
				}
				return v;
			}(),
			true);
		utf8ValidationTest<"kuhn_5_3_4_noncharacter_plane_pairs">(
			[] {
				std::vector<uint8_t> v;
				for (uint32_t plane = 1; plane <= 10; ++plane) {
					for (uint32_t suffix = 0xFFFE; suffix <= 0xFFFF; ++suffix) {
						const uint32_t cp = (plane << 16) | suffix;
						v.push_back(static_cast<uint8_t>(0xF0 | (cp >> 18)));
						v.push_back(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
						v.push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
						v.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
					}
				}
				return v;
			}(),
			true);
		std::cout << "UTF8 Second-Byte Boundary Tests" << std::endl;
		utf8ValidationTest<"boundary_e0_second_byte_9f_overlong">({ 0xE0, 0x9F, 0xBF }, false);
		utf8ValidationTest<"boundary_e0_second_byte_a0_valid">({ 0xE0, 0xA0, 0x80 }, true);
		utf8ValidationTest<"boundary_e0_second_byte_9f_max_cont">({ 0xE0, 0x9F, 0x80 }, false);
		utf8ValidationTest<"boundary_e0_second_byte_a0_min_cont">({ 0xE0, 0xA0, 0x80 }, true);
		utf8ValidationTest<"boundary_ed_second_byte_9f_valid">({ 0xED, 0x9F, 0xBF }, true);
		utf8ValidationTest<"boundary_ed_second_byte_a0_surrogate">({ 0xED, 0xA0, 0x80 }, false);
		utf8ValidationTest<"boundary_ed_second_byte_9f_max_cont">({ 0xED, 0x9F, 0xBF }, true);
		utf8ValidationTest<"boundary_ed_second_byte_a0_min_cont">({ 0xED, 0xA0, 0x80 }, false);
		utf8ValidationTest<"boundary_f0_second_byte_8f_overlong">({ 0xF0, 0x8F, 0xBF, 0xBF }, false);
		utf8ValidationTest<"boundary_f0_second_byte_90_valid">({ 0xF0, 0x90, 0x80, 0x80 }, true);
		utf8ValidationTest<"boundary_f0_second_byte_8f_max_cont">({ 0xF0, 0x8F, 0x80, 0x80 }, false);
		utf8ValidationTest<"boundary_f0_second_byte_90_min_cont">({ 0xF0, 0x90, 0x80, 0x80 }, true);
		utf8ValidationTest<"boundary_f4_second_byte_8f_valid">({ 0xF4, 0x8F, 0xBF, 0xBF }, true);
		utf8ValidationTest<"boundary_f4_second_byte_90_too_large">({ 0xF4, 0x90, 0x80, 0x80 }, false);
		utf8ValidationTest<"boundary_f4_second_byte_8f_min_cont">({ 0xF4, 0x8F, 0x80, 0x80 }, true);
		utf8ValidationTest<"boundary_f4_second_byte_90_min_cont">({ 0xF4, 0x90, 0x80, 0x80 }, false);
		utf8ValidationTest<"boundary_e0_second_byte_9f_cross_chunk">(asciiWrapped(29, { 0xE0, 0x9F, 0xBF }, 29), false);
		utf8ValidationTest<"boundary_ed_second_byte_a0_cross_chunk">(asciiWrapped(29, { 0xED, 0xA0, 0x80 }, 29), false);
		utf8ValidationTest<"boundary_f0_second_byte_8f_cross_chunk">(asciiWrapped(29, { 0xF0, 0x8F, 0xBF, 0xBF }, 29), false);
		utf8ValidationTest<"boundary_f4_second_byte_90_cross_chunk">(asciiWrapped(29, { 0xF4, 0x90, 0x80, 0x80 }, 29), false);
		std::cout << "UTF8 String-Parser Fused Validation Tests" << std::endl;
		utf8StringParseTest<"sp_ascii_short">(repeated(0x41, 8), true);
		utf8StringParseTest<"sp_ascii_x64">(repeated(0x41, 64), true);
		utf8StringParseTest<"sp_ascii_x200">(repeated(0x41, 200), true);
		utf8StringParseTest<"sp_two_byte_simple">(asciiWrapped(30, { 0xC3, 0xA9 }, 30), true);
		utf8StringParseTest<"sp_bad_cont_mid">(asciiWrapped(30, { 0xC2, 0x41 }, 30), false);
		utf8StringParseTest<"sp_surrogate_mid">(asciiWrapped(30, { 0xED, 0xA0, 0x80 }, 30), false);
		utf8StringParseTest<"sp_overlong_mid">(asciiWrapped(30, { 0xC0, 0x80 }, 30), false);
		utf8StringParseTest<"sp_lone_cont_mid">(asciiWrapped(30, { 0x80 }, 30), false);
		utf8StringParseTest<"sp_valid_lead_at_m3_of_32">(asciiWithSeqAt(52, 29, { 0xC3, 0xA9 }), true);
		utf8StringParseTest<"sp_valid_lead_at_m3_of_64">(asciiWithSeqAt(84, 61, { 0xC3, 0xA9 }), true);
		utf8StringParseTest<"sp_valid_lead_at_m2_of_32">(asciiWithSeqAt(52, 30, { 0xC3, 0xA9 }), true);
		utf8StringParseTest<"sp_valid_three_at_m3_of_32">(asciiWithSeqAt(52, 29, { 0xE0, 0xA0, 0x80 }), true);
		utf8StringParseTest<"sp_valid_four_at_m4_of_64">(asciiWithSeqAt(84, 60, { 0xF0, 0x90, 0x80, 0x80 }), true);
		utf8StringParseTest<"sp_valid_four_at_m3_of_64">(asciiWithSeqAt(84, 61, { 0xF0, 0x90, 0x80, 0x80 }), true);
		utf8StringParseTest<"sp_dangling_c2_at_m1_of_32">(asciiWithSeqAt(52, 31, { 0xC2 }), false);
		utf8StringParseTest<"sp_dangling_c2_at_m1_of_64">(asciiWithSeqAt(84, 63, { 0xC2 }), false);
		utf8StringParseTest<"sp_dangling_e0_at_m2_of_32">(asciiWithSeqAt(52, 30, { 0xE0, 0xA0 }), false);
		utf8StringParseTest<"sp_dangling_f0_at_m3_of_32">(asciiWithSeqAt(52, 29, { 0xF0, 0x90, 0x80 }), false);
		utf8StringParseTest<"sp_escape_then_multibyte_tail">(
			[] {
				std::vector<uint8_t> v(40, 0x41);
				v[10] = '\\';
				v[11] = 'n';
				v.insert(v.end(), { 0xC3, 0xA9 });
				v.insert(v.end(), 20, 0x41);
				return v;
			}(),
			true);
		utf8StringParseTest<"sp_escape_then_dangling_tail">(
			[] {
				std::vector<uint8_t> v(40, 0x41);
				v[10] = '\\';
				v[11] = 'n';
				v.push_back(0xC2);
				v.insert(v.end(), 20, 0x41);
				return v;
			}(),
			false);
		std::cout << "UTF8 Markus Kuhn Stress Test Cases in String-Parser" << std::endl;
		utf8StringParseTest<"sp_kuhn_overlong_slash_2byte">(asciiWrapped(20, { 0xC0, 0xAF }, 20), false);
		utf8StringParseTest<"sp_kuhn_overlong_nul_3byte">(asciiWrapped(20, { 0xE0, 0x80, 0x80 }, 20), false);
		utf8StringParseTest<"sp_kuhn_surrogate_UD800">(asciiWrapped(20, { 0xED, 0xA0, 0x80 }, 20), false);
		utf8StringParseTest<"sp_kuhn_impossible_0xfe">(asciiWrapped(20, { 0xFE }, 20), false);
		utf8StringParseTest<"sp_kuhn_impossible_0xff">(asciiWrapped(20, { 0xFF }, 20), false);
		utf8StringParseTest<"sp_kuhn_noncharacter_UFFFF">(asciiWrapped(20, { 0xEF, 0xBF, 0xBF }, 20), true);
		utf8StringParseTest<"sp_kuhn_lonely_2byte_lead">(asciiWrapped(20, { 0xC2, 0x20 }, 20), false);
		utf8StringParseTest<"sp_kuhn_max_overlong_4byte">(asciiWrapped(20, { 0xF0, 0x8F, 0xBF, 0xBF }, 20), false);
		std::cout << "UTF8 Fused String-Parser Security Edge Cases" << std::endl;
		utf8StringParseTest<"sp_backslash_then_cont_byte">(
			[] {
				std::vector<uint8_t> v(20, 0x41);
				v.push_back('\\');
				v.push_back(0x80);
				v.insert(v.end(), 20, 0x41);
				return v;
			}(),
			false);
		utf8StringParseTest<"sp_two_byte_seq_with_0x22_as_cont_byte_is_impossible">(asciiWrapped(20, { 0xC2, 0x22 }, 20), false);
		utf8StringParseTest<"sp_two_byte_seq_with_0x5c_as_cont_byte_is_impossible">(asciiWrapped(20, { 0xC2, 0x5C }, 20), false);
		utf8StringParseTest<"sp_valid_multibyte_immediately_followed_by_quote">(
			[] {
				std::vector<uint8_t> v(20, 0x41);
				v.insert(v.end(), { 0xC3, 0xA9 });
				return v;
			}(),
			true);
		utf8StringParseTest<"sp_valid_multibyte_immediately_followed_by_escaped_backslash">(
			[] {
				std::vector<uint8_t> v(20, 0x41);
				v.insert(v.end(), { 0xC3, 0xA9 });
				v.push_back('\\');
				v.push_back('\\');
				v.insert(v.end(), 20, 0x41);
				return v;
			}(),
			true);
		utf8StringParseTest<"sp_embedded_null_mid_ascii">(
			[] {
				std::vector<uint8_t> v(20, 0x41);
				v[10] = 0x00;
				return v;
			}(),
			false);
		utf8StringParseTest<"sp_embedded_null_before_multibyte">(
			[] {
				std::vector<uint8_t> v(20, 0x41);
				v[10] = 0x00;
				v.insert(v.end(), { 0xC3, 0xA9 });
				v.insert(v.end(), 20, 0x41);
				return v;
			}(),
			false);
		utf8StringParseTest<"sp_embedded_null_mid_multibyte_lead">(
			[] {
				std::vector<uint8_t> v(20, 0x41);
				v.push_back(0xC2);
				v.push_back(0x00);
				v.insert(v.end(), 20, 0x41);
				return v;
			}(),
			false);
		utf8StringParseTest<"sp_dangling_c2_at_32chunk_end_then_immediate_quote">(
			[] {
				std::vector<uint8_t> v(31, 0x41);
				v.push_back(0xC2);
				return v;
			}(),
			false);
		utf8StringParseTest<"sp_dangling_c2_at_64chunk_end_then_immediate_quote">(
			[] {
				std::vector<uint8_t> v(63, 0x41);
				v.push_back(0xC2);
				return v;
			}(),
			false);
		utf8StringParseTest<"sp_unescaped_control_0x01">(asciiWrapped(10, { 0x01 }, 10), false);
		utf8StringParseTest<"sp_unescaped_control_0x1f">(asciiWrapped(10, { 0x1F }, 10), false);
		utf8StringParseTest<"sp_unescaped_control_0x09_tab">(asciiWrapped(10, { 0x09 }, 10), false);
		utf8StringParseTest<"sp_unescaped_control_0x0a_newline">(asciiWrapped(10, { 0x0A }, 10), false);
		utf8StringParseTest<"sp_escaped_unicode_sequence_then_utf8">(asciiWrapped(10, { '\\', 'u', '0', '0', '2', '0', 0xC3, 0xA9 }, 10), true);
		utf8StringParseTest<"sp_escaped_unicode_sequence_then_dangling_utf8">(asciiWrapped(10, { '\\', 'u', '0', '0', '2', '0', 0xC2 }, 10), false);
		utf8StringParseTest<"sp_utf8_then_escaped_unicode_sequence">(asciiWrapped(10, { 0xC3, 0xA9, '\\', 'u', '0', '0', '2', '0' }, 10), true);
		utf8StringParseTest<"sp_utf8_then_escaped_backslash_then_utf8">(asciiWrapped(10, { 0xC3, 0xA9, '\\', '\\', 0xC3, 0xA9 }, 10), true);
		utf8StringParseTest<"sp_escaped_high_surrogate_alone">(asciiWrapped(10, { '\\', 'u', 'd', '8', '0', '0' }, 10), false);
		utf8StringParseTest<"sp_escaped_low_surrogate_alone">(asciiWrapped(10, { '\\', 'u', 'd', 'c', '0', '0' }, 10), false);
		utf8StringParseTest<"sp_escaped_surrogate_pair_valid">(asciiWrapped(10, { '\\', 'u', 'd', '8', '0', '0', '\\', 'u', 'd', 'c', '0', '0' }, 10), true);
		utf8StringParseTest<"sp_escaped_high_surrogate_then_utf8_not_low">(asciiWrapped(10, { '\\', 'u', 'd', '8', '0', '0', 0xC3, 0xA9 }, 10), false);
		utf8StringParseTest<"sp_escape_to_valid_then_dangling">(asciiWrapped(10, { '\\', 'u', '0', '0', 'E', '9', 0xC2 }, 10), false);
		runUnalignedPointerSweep();
		runUnalignedInvalidSequenceSweep();
#if JSONIFIER_HAS_MMAP
		runPageBoundaryFaultTest();
#endif
		runUtf8WidthTransitionSweep();
		std::cout << "Utf8-validation validation tests complete." << std::endl;
	}

}
