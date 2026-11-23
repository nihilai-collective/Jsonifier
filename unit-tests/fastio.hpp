/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/fastio.hpp
 */
#pragma once

#if !JSONIFIER_PLATFORM_WINDOWS
	#include <fcntl.h>
#endif

#include "common.hpp"

namespace fastio_tests {

	struct stream_capture {
		explicit stream_capture(jsonifier::internal::stream_targets target = jsonifier::internal::stream_targets::stdout_target) : target_{ target } {
			jsonifier::internal::cout.doFlush();
			jsonifier::internal::err.doFlush();
			tempPath			= (std::filesystem::temp_directory_path() / "jsonifier_fastio_capture.tmp").string();
			const bool isStdout = target_ == jsonifier::internal::stream_targets::stdout_target;
#if JSONIFIER_PLATFORM_WINDOWS
			savedHandle = GetStdHandle(isStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
			fileHandle	= CreateFileA(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			SetStdHandle(isStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, fileHandle);
#else
			fdNumber	  = isStdout ? 1 : 2;
			savedFd		  = dup(fdNumber);
			int32_t newFd = open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(newFd, fdNumber);
			close(newFd);
#endif
		}

		std::string finish() {
			if (target_ == jsonifier::internal::stream_targets::stdout_target) {
				jsonifier::internal::cout.doFlush();
			} else {
				jsonifier::internal::err.doFlush();
			}
#if JSONIFIER_PLATFORM_WINDOWS
			SetStdHandle(target_ == jsonifier::internal::stream_targets::stdout_target ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, savedHandle);
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

		jsonifier::internal::stream_targets target_;
		std::string tempPath{};
#if JSONIFIER_PLATFORM_WINDOWS
		HANDLE savedHandle{};
		HANDLE fileHandle{};
#else
		int32_t fdNumber{};
		int32_t savedFd{};
#endif
	};

	inline constexpr jsonifier::internal::array<uint64_t, 10> uint64Boundaries{ { 0ULL, 9ULL, 10ULL, 99999ULL, 100000ULL, 9999999999ULL, 10000000000ULL, 999999999999999999ULL,
		1000000000000000000ULL, 18446744073709551615ULL } };

	inline constexpr jsonifier::internal::array<int64_t, 12> int64Boundaries{ { 0LL, 1LL, -1LL, 99999LL, -99999LL, 100000LL, -100000LL, 999999999999999999LL, -999999999999999999LL,
		9223372036854775807LL, -9223372036854775807LL, std::numeric_limits<int64_t>::min() } };

	inline constexpr jsonifier::internal::array<uint32_t, 6> uint32Boundaries{ { 0u, 9u, 10u, 999999999u, 4294967294u, 4294967295u } };

	inline constexpr jsonifier::internal::array<int32_t, 6> int32Boundaries{ { 0, 9, -9, 2147483647, -2147483647, -2147483648 } };

	inline constexpr jsonifier::internal::array<uint8_t, 4> uint8Boundaries{ { 0, 9, 254, 255 } };

	inline constexpr jsonifier::internal::array<int8_t, 4> int8Boundaries{ { 0, 9, -9, -128 } };

	inline constexpr jsonifier::internal::array<double, 8> floatValues{ { 0.0, -0.0, 1.5, -10.5, 3.14159, 1e20, 1e-20, 1.7976931348623157e+308 } };

	inline void basicStreamLargeSingleWriteBypassesBufferTest() {
		stream_capture capture{};
		{
			jsonifier::internal::basic_stream<4> stream{};
			stream << "ab";
			stream << std::string_view{ "0123456789" };
			stream.doFlush();
		}
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_basic_stream_oversized_write_bypasses_buffer", true>::assert_eq(std::string{ "ab0123456789" }, [&]() {
			return captured;
		});
	}

	inline void outSingletonWritesToStdoutTest() {
		stream_capture capture{};
		jsonifier::internal::cout << "out-singleton-check " << 123;
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_out_singleton_writes_to_stdout", true>::assert_eq(std::string{ "out-singleton-check 123" }, [&]() {
			return captured;
		});
	}

	inline void errSingletonWritesToStderrTest() {
		stream_capture capture{ jsonifier::internal::stream_targets::stderr_target };
		jsonifier::internal::err << "err-singleton-check " << 456;
		std::string captured = capture.finish();
		rt_ut::unit_test<"fastio_err_singleton_writes_to_stderr", true>::assert_eq(std::string{ "err-singleton-check 456" }, [&]() {
			return captured;
		});
	}

	inline void runTests() {
		basicStreamLargeSingleWriteBypassesBufferTest();
		outSingletonWritesToStdoutTest();
		errSingletonWritesToStderrTest();
		std::cout << "fastio validation tests complete." << std::endl;
	}

}
