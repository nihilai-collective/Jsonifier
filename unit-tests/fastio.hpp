// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/fastio.hpp
#pragma once

#include "common.hpp"

#if !JSONIFIER_PLATFORM_WINDOWS
	#include <fcntl.h>
#endif

namespace fastio_tests {

	class stream_capture {
	  public:
		explicit stream_capture(jsonifier::internal::stream_target target) : target_{ target } {
			jsonifier::internal::out.flushNow();
			jsonifier::internal::err.flushNow();
			tempPath = (std::filesystem::temp_directory_path() / "jsonifier_fastio_capture.tmp").string();
			const bool isStdout = target_ == jsonifier::internal::stream_target::stdout_target;
#if JSONIFIER_PLATFORM_WINDOWS
			savedHandle = GetStdHandle(isStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
			fileHandle	= CreateFileA(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			SetStdHandle(isStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, fileHandle);
#else
			fdNumber  = isStdout ? 1 : 2;
			savedFd	  = dup(fdNumber);
			int newFd = open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(newFd, fdNumber);
			close(newFd);
#endif
		}

		std::string finish() {
			if (target_ == jsonifier::internal::stream_target::stdout_target) {
				jsonifier::internal::out.flushNow();
			} else {
				jsonifier::internal::err.flushNow();
			}
#if JSONIFIER_PLATFORM_WINDOWS
			SetStdHandle(target_ == jsonifier::internal::stream_target::stdout_target ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, savedHandle);
			CloseHandle(fileHandle);
#else
			dup2(savedFd, fdNumber);
			close(savedFd);
#endif
			std::string content = file_handler::get(tempPath);
			std::error_code errorCode{};
			std::filesystem::remove(tempPath, errorCode);
			return content;
		}

	  private:
		jsonifier::internal::stream_target target_;
		std::string tempPath{};
#if JSONIFIER_PLATFORM_WINDOWS
		HANDLE savedHandle{};
		HANDLE fileHandle{};
#else
		int savedFd{};
		int fdNumber{};
#endif
	};

	constexpr jsonifier::internal::array<uint64_t, 22> digitCountBoundaries{ { 0ULL, 1ULL, 9ULL, 10ULL, 99ULL, 100ULL, 999ULL, 1000ULL, 9999ULL, 10000ULL, 99999ULL, 100000ULL,
		999999999ULL, 1000000000ULL, 9999999999999ULL, 10000000000000ULL, 999999999999999999ULL, 1000000000000000000ULL, 9999999999999999999ULL, 10000000000000000000ULL,
		18446744073709551614ULL, 18446744073709551615ULL } };

	inline static void digitCountTests() {
		std::cout << "fastio Digit-Count Tests: " << std::endl;
		for (auto value: digitCountBoundaries) {
			rt_ut::unit_test<"fastio_digit_count_matches_decimal_length", true>::assert_eq(std::to_string(value).size(), [&]() {
				return static_cast<std::size_t>(jsonifier::internal::fastDigitCount(value));
			});
		}
		std::cout << "fastio Digit-Count tests complete." << std::endl;
	}

	template<typename value_type, const auto& values> inline static void checkWriteIntegerMatchesStdToString() {
		for (auto value: values) {
			char buffer[32]{};
			auto len = jsonifier::internal::writeInteger<value_type>(buffer, value);
			std::string result{ buffer, len };
			rt_ut::unit_test<"fastio_write_integer_matches_std_to_string", true>::assert_eq(std::to_string(value), [&]() {
				return result;
			});
		}
	}

	constexpr jsonifier::internal::array<uint64_t, 10> uint64Boundaries{ { 0ULL, 9ULL, 10ULL, 99999ULL, 100000ULL, 9999999999ULL, 10000000000ULL, 999999999999999999ULL,
		1000000000000000000ULL, 18446744073709551615ULL } };

	constexpr jsonifier::internal::array<int64_t, 12> int64Boundaries{ { 0LL, 1LL, -1LL, 99999LL, -99999LL, 100000LL, -100000LL, 999999999999999999LL, -999999999999999999LL,
		9223372036854775807LL, -9223372036854775807LL, std::numeric_limits<int64_t>::min() } };

	constexpr jsonifier::internal::array<uint32_t, 6> uint32Boundaries{ { 0u, 9u, 10u, 999999999u, 4294967294u, 4294967295u } };

	constexpr jsonifier::internal::array<int32_t, 6> int32Boundaries{ { 0, 9, -9, 2147483647, -2147483647, -2147483648 } };

	constexpr jsonifier::internal::array<uint8_t, 4> uint8Boundaries{ { 0, 9, 254, 255 } };

	constexpr jsonifier::internal::array<int8_t, 4> int8Boundaries{ { 0, 9, -9, -128 } };

	inline static void writeIntegerTests() {
		std::cout << "fastio writeInteger Tests: " << std::endl;
		checkWriteIntegerMatchesStdToString<uint8_t, uint8Boundaries>();
		checkWriteIntegerMatchesStdToString<int8_t, int8Boundaries>();
		checkWriteIntegerMatchesStdToString<uint32_t, uint32Boundaries>();
		checkWriteIntegerMatchesStdToString<int32_t, int32Boundaries>();
		checkWriteIntegerMatchesStdToString<uint64_t, uint64Boundaries>();
		checkWriteIntegerMatchesStdToString<int64_t, int64Boundaries>();
		std::cout << "fastio writeInteger tests complete." << std::endl;
	}

	constexpr jsonifier::internal::array<double, 8> floatValues{ { 0.0, -0.0, 1.5, -10.5, 3.14159, 1e20, 1e-20, 1.7976931348623157e+308 } };

	inline static void writeFloatTests() {
		std::cout << "fastio writeFloat Tests: " << std::endl;
		for (auto value: floatValues) {
			char buffer[64]{};
			auto len = jsonifier::internal::writeFloat<double>(buffer, value);
			std::string result{ buffer, len };

			char expectedBuffer[64]{};
			auto expectedResult = std::to_chars(expectedBuffer, expectedBuffer + 64, value);
			std::string expected{ expectedBuffer, static_cast<std::size_t>(expectedResult.ptr - expectedBuffer) };

			rt_ut::unit_test<"fastio_write_float_matches_to_chars", true>::assert_eq(expected, [&]() {
				return result;
			});
		}
		std::cout << "fastio writeFloat tests complete." << std::endl;
	}

	inline static void basicStreamBufferedOutputTest() {
		stream_capture capture{ jsonifier::internal::stream_target::stdout_target };
		{
			jsonifier::internal::basic_stream<8> stream{ jsonifier::internal::stream_target::stdout_target };
			stream << "hello" << ' ' << 42 << ' ' << -7 << ' ' << true << ' ' << false << jsonifier::internal::endl;
			stream << std::string_view{ "this string is definitely longer than the eight byte buffer" } << jsonifier::internal::flush;
			stream.flushNow();
		}
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_basic_stream_small_buffer_preserves_ordering", true>::assert_eq(
			std::string{ "hello 42 -7 true false\nthis string is definitely longer than the eight byte buffer" }, [&]() {
				return captured;
			});
	}

	inline static void basicStreamLargeSingleWriteBypassesBufferTest() {
		stream_capture capture{ jsonifier::internal::stream_target::stdout_target };
		{
			jsonifier::internal::basic_stream<4> stream{ jsonifier::internal::stream_target::stdout_target };
			stream << "ab";
			stream << std::string_view{ "0123456789" };
			stream.flushNow();
		}
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_basic_stream_oversized_write_bypasses_buffer", true>::assert_eq(std::string{ "ab0123456789" }, [&]() {
			return captured;
		});
	}

	inline static void outSingletonWritesToStdoutTest() {
		stream_capture capture{ jsonifier::internal::stream_target::stdout_target };
		jsonifier::internal::out << "out-singleton-check " << 123;
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_out_singleton_writes_to_stdout", true>::assert_eq(std::string{ "out-singleton-check 123" }, [&]() {
			return captured;
		});
	}

	inline static void errSingletonWritesToStderrTest() {
		stream_capture capture{ jsonifier::internal::stream_target::stderr_target };
		jsonifier::internal::err << "err-singleton-check " << 456;
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_err_singleton_writes_to_stderr", true>::assert_eq(std::string{ "err-singleton-check 456" }, [&]() {
			return captured;
		});
	}

	inline static void runTests() {
		digitCountTests();
		writeIntegerTests();
		writeFloatTests();
		basicStreamBufferedOutputTest();
		basicStreamLargeSingleWriteBypassesBufferTest();
		outSingletonWritesToStdoutTest();
		errSingletonWritesToStderrTest();
		std::cout << "fastio validation tests complete." << std::endl;
	}

}
