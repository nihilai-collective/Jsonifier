// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/fastio.hpp
#pragma once

namespace jsonifier::internal {

	enum class stream_target { stdout_target, stderr_target };

	inline size_t rawWrite(stream_target target, const char* data, size_t len) {
#if JSONIFIER_PLATFORM_WINDOWS
		HANDLE handle	  = GetStdHandle(target == stream_target::stdout_target ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
		DWORD written	  = 0;
		size_t total = 0;
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
		int fd			  = target == stream_target::stdout_target ? 1 : 2;
		size_t total = 0;
		while (total < len) {
			ssize_t result = write(fd, data + total, len - total);
			if (result <= 0) {
				break;
			}
			total += static_cast<size_t>(result);
		}
		return total;
#endif
	}	

	static constexpr uint8_t digitCounts[]{ 19, 19, 19, 19, 18, 18, 18, 17, 17, 17, 16, 16, 16, 16, 15, 15, 15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 11, 11, 11, 10, 10, 10, 10,
		9, 9, 9, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 1, 1, 1 };

	static constexpr uint64_t digitCountThresholds[]{ 0ULL, 9ULL, 99ULL, 999ULL, 9999ULL, 99999ULL, 999999ULL, 9999999ULL, 99999999ULL, 999999999ULL, 9999999999ULL, 99999999999ULL,
		999999999999ULL, 9999999999999ULL, 99999999999999ULL, 999999999999999ULL, 9999999999999999ULL, 99999999999999999ULL, 999999999999999999ULL, 9999999999999999999ULL };

	inline static uint64_t fastDigitCount(const uint64_t inputValue) {
		const uint64_t originalDigitCount{ digitCounts[std::countl_zero(inputValue)] };
		return originalDigitCount + static_cast<uint64_t>(inputValue > digitCountThresholds[originalDigitCount]);
	}

	inline char* writeUnsigned(char* buffer_end, uint64_t value) {
		char* pos							   = buffer_end;
		static constexpr char digit_pairs[201] = "0001020304050607080910111213141516171819"
												 "2021222324252627282930313233343536373839"
												 "4041424344454647484950515253545556575859"
												 "6061626364656667686970717273747576777879"
												 "8081828384858687888990919293949596979899";
		while (value >= 100) {
			auto pair = value % 100;
			value /= 100;
			pos -= 2;
			pos[0] = digit_pairs[pair * 2];
			pos[1] = digit_pairs[pair * 2 + 1];
		}
		if (value < 10) {
			*--pos = static_cast<char>('0' + value);
		} else {
			pos -= 2;
			pos[0] = digit_pairs[value * 2];
			pos[1] = digit_pairs[value * 2 + 1];
		}
		return pos;
	}

	template<typename value_type> size_t writeInteger(char* dest, value_type value) {
		if constexpr (std::is_signed_v<value_type>) {
			uint64_t magnitude;
			char* out_local = dest;
			if (value < 0) {
				*out_local++ = '-';
				magnitude = ~static_cast<uint64_t>(value) + 1;
			} else {
				magnitude = static_cast<uint64_t>(value);
			}
			auto count = fastDigitCount(magnitude);
			char* end  = out_local + count;
			writeUnsigned(end, magnitude);
			return static_cast<size_t>(end - dest);
		} else {
			uint64_t magnitude = static_cast<uint64_t>(value);
			auto count				= fastDigitCount(magnitude);
			char* end				= dest + count;
			writeUnsigned(end, magnitude);
			return count;
		}
	}

	template<typename value_type> size_t writeFloat(char* dest, value_type value) {
		auto result = std::to_chars(dest, dest + 64, value);
		return static_cast<size_t>(result.ptr - dest);
	}

	struct endl_t {};
	inline constexpr endl_t endl{};

	struct flush_t {};
	inline constexpr flush_t flush{};

	template<size_t buffer_size = 8192> class basic_stream {
	  public:
		explicit basic_stream(stream_target target) : target_(target), len_(0) {
		}

		~basic_stream() {
			doFlush();
		}

		basic_stream(const basic_stream&)			 = delete;
		basic_stream& operator=(const basic_stream&) = delete;

		basic_stream& operator<<(std::string_view value) {
			writeRaw(value.data(), value.size());
			return *this;
		}

		basic_stream& operator<<(const char* value) {
			return (*this) << std::string_view(value);
		}

		basic_stream& operator<<(char value) {
			ensureSpace(1);
			buffer_[len_++] = value;
			return *this;
		}

		template<typename integer_type>
		std::enable_if_t<std::is_integral_v<integer_type> && !std::is_same_v<integer_type, char> && !std::is_same_v<integer_type, bool>, basic_stream&> operator<<(
			integer_type value) {
			ensureSpace(21);
			len_ += writeInteger(buffer_ + len_, value);
			return *this;
		}

		basic_stream& operator<<(bool value) {
			return (*this) << std::string_view(value ? "true" : "false");
		}

		template<typename float_type> std::enable_if_t<std::is_floating_point_v<float_type>, basic_stream&> operator<<(float_type value) {
			ensureSpace(64);
			len_ += writeFloat(buffer_ + len_, value);
			return *this;
		}

		basic_stream& operator<<(endl_t) {
			(*this) << '\n';
			doFlush();
			return *this;
		}

		basic_stream& operator<<(flush_t) {
			doFlush();
			return *this;
		}

		void flushNow() {
			doFlush();
		}

	  protected:
		void ensureSpace(size_t needed) {
			if (len_ + needed > buffer_size) {
				doFlush();
			}
		}

		void writeRaw(const char* data, size_t size) {
			if (size >= buffer_size) {
				doFlush();
				rawWrite(target_, data, size);
				return;
			}
			ensureSpace(size);
			std::memcpy(buffer_ + len_, data, size);
			len_ += size;
		}

		void doFlush() {
			if (len_ > 0) {
				rawWrite(target_, buffer_, len_);
				len_ = 0;
			}
		}

		stream_target target_;
		char buffer_[buffer_size];
		size_t len_;
	};

#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
	#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

	inline static basic_stream<> out{ stream_target::stdout_target };

	inline static basic_stream<> err{ stream_target::stderr_target };

#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic pop
#endif

}
