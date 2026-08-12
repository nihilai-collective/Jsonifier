// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/fastio.hpp
#pragma once

// MIT License
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/fastio.hpp
#pragma once

// MIT License
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/fastio.hpp
#pragma once

// MIT License
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/fastio.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <charconv>

#if defined(_WIN32)
	#define FASTIO_WINDOWS 1
#else
	#define FASTIO_POSIX 1
#endif

#if defined(FASTIO_WINDOWS)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#else
extern "C" {
long write(int32_t fd, const void* buf, uint64_t count);
}
#endif

namespace fastio {

	namespace detail {

		enum class stream_target { stdout_target, stderr_target };

		inline std::size_t raw_write(stream_target target, const char* data, std::size_t len) {
#if defined(FASTIO_WINDOWS)
			HANDLE handle	  = GetStdHandle(target == stream_target::stdout_target ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
			DWORD written	  = 0;
			std::size_t total = 0;
			while (total < len) {
				DWORD chunk = static_cast<DWORD>(len - total);
				if (!WriteFile(handle, data + total, chunk, &written, nullptr)) {
					break;
				}
				total += written;
				if (written == 0) {
					break;
				}
			}
			return total;
#else
			int32_t fd			  = target == stream_target::stdout_target ? 1 : 2;
			std::size_t total = 0;
			while (total < len) {
				long result = write(fd, data + total, static_cast<uint64_t>(len - total));
				if (result <= 0) {
					break;
				}
				total += static_cast<std::size_t>(result);
			}
			return total;
#endif
		}

		inline std::uint32_t digit_count(std::uint64_t value) {
			if (value < 10ull)
				return 1;
			if (value < 100ull)
				return 2;
			if (value < 1000ull)
				return 3;
			if (value < 10000ull)
				return 4;
			if (value < 100000ull)
				return 5;
			if (value < 1000000ull)
				return 6;
			if (value < 10000000ull)
				return 7;
			if (value < 100000000ull)
				return 8;
			if (value < 1000000000ull)
				return 9;
			if (value < 10000000000ull)
				return 10;
			if (value < 100000000000ull)
				return 11;
			if (value < 1000000000000ull)
				return 12;
			if (value < 10000000000000ull)
				return 13;
			if (value < 100000000000000ull)
				return 14;
			if (value < 1000000000000000ull)
				return 15;
			if (value < 10000000000000000ull)
				return 16;
			if (value < 100000000000000000ull)
				return 17;
			if (value < 1000000000000000000ull)
				return 18;
			if (value < 10000000000000000000ull)
				return 19;
			return 20;
		}

		inline char* write_unsigned(char* buffer_end, std::uint64_t value) {
			char* pos = buffer_end;
			while (value >= 100) {
				auto pair = value % 100;
				value /= 100;
				static constexpr char digit_pairs[201] = "0001020304050607080910111213141516171819"
														 "2021222324252627282930313233343536373839"
														 "4041424344454647484950515253545556575859"
														 "6061626364656667686970717273747576777879"
														 "8081828384858687888990919293949596979899";
				pos -= 2;
				pos[0] = digit_pairs[pair * 2];
				pos[1] = digit_pairs[pair * 2 + 1];
			}
			if (value < 10) {
				*--pos = static_cast<char>('0' + value);
			} else {
				static constexpr char digit_pairs[201] = "0001020304050607080910111213141516171819"
														 "2021222324252627282930313233343536373839"
														 "4041424344454647484950515253545556575859"
														 "6061626364656667686970717273747576777879"
														 "8081828384858687888990919293949596979899";
				pos -= 2;
				pos[0] = digit_pairs[value * 2];
				pos[1] = digit_pairs[value * 2 + 1];
			}
			return pos;
		}

		template<typename value_type> std::size_t write_integer(char* dest, value_type value) {
			if constexpr (std::is_signed_v<value_type>) {
				std::uint64_t magnitude;
				char* out = dest;
				if (value < 0) {
					*out++	  = '-';
					magnitude = ~static_cast<std::uint64_t>(value) + 1;
				} else {
					magnitude = static_cast<std::uint64_t>(value);
				}
				auto count = digit_count(magnitude);
				char* end  = out + count;
				write_unsigned(end, magnitude);
				return static_cast<std::size_t>(end - dest);
			} else {
				std::uint64_t magnitude = static_cast<std::uint64_t>(value);
				auto count				= digit_count(magnitude);
				char* end				= dest + count;
				write_unsigned(end, magnitude);
				return count;
			}
		}

		template<typename value_type> std::size_t write_float(char* dest, value_type value) {
			auto result = std::to_chars(dest, dest + 64, value);
			return static_cast<std::size_t>(result.ptr - dest);
		}

	}

	struct endl_t {};
	inline constexpr endl_t endl{};

	struct flush_t {};
	inline constexpr flush_t flush{};

	template<std::size_t buffer_size = 8192> class basic_stream {
	  public:
		explicit basic_stream(detail::stream_target target) : target_(target), len_(0) {
		}

		~basic_stream() {
			do_flush();
		}

		basic_stream(const basic_stream&)			 = delete;
		basic_stream& operator=(const basic_stream&) = delete;

		basic_stream& operator<<(std::string_view value) {
			write_raw(value.data(), value.size());
			return *this;
		}

		basic_stream& operator<<(const char* value) {
			return (*this) << std::string_view(value);
		}

		basic_stream& operator<<(char value) {
			ensure_space(1);
			buffer_[len_++] = value;
			return *this;
		}

		template<typename integer_type>
		std::enable_if_t<std::is_integral_v<integer_type> && !std::is_same_v<integer_type, char> && !std::is_same_v<integer_type, bool>, basic_stream&> operator<<(
			integer_type value) {
			ensure_space(21);
			len_ += detail::write_integer(buffer_ + len_, value);
			return *this;
		}

		basic_stream& operator<<(bool value) {
			return (*this) << std::string_view(value ? "true" : "false");
		}

		template<typename float_type> std::enable_if_t<std::is_floating_point_v<float_type>, basic_stream&> operator<<(float_type value) {
			ensure_space(64);
			len_ += detail::write_float(buffer_ + len_, value);
			return *this;
		}

		basic_stream& operator<<(endl_t) {
			(*this) << '\n';
			do_flush();
			return *this;
		}

		basic_stream& operator<<(flush_t) {
			do_flush();
			return *this;
		}

		void flush_now() {
			do_flush();
		}

	  private:
		void ensure_space(std::size_t needed) {
			if (len_ + needed > buffer_size) {
				do_flush();
			}
		}

		void write_raw(const char* data, std::size_t size) {
			if (size >= buffer_size) {
				do_flush();
				detail::raw_write(target_, data, size);
				return;
			}
			ensure_space(size);
			std::memcpy(buffer_ + len_, data, size);
			len_ += size;
		}

		void do_flush() {
			if (len_ > 0) {
				detail::raw_write(target_, buffer_, len_);
				len_ = 0;
			}
		}

		detail::stream_target target_;
		char buffer_[buffer_size];
		std::size_t len_;
	};

	inline basic_stream<>& out() {
		static basic_stream<> instance(detail::stream_target::stdout_target);
		return instance;
	}

	inline basic_stream<>& err() {
		static basic_stream<> instance(detail::stream_target::stderr_target);
		return instance;
	}

}
