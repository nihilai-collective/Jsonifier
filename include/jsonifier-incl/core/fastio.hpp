/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/fastio.hpp
 */
#pragma once

#include <jsonifier-incl/containers/aligned_allocator.hpp>
#include <jsonifier-incl/simd/bit_ops.hpp>
#include <jsonifier-incl/utilities/string_view.hpp>

namespace jsonifier::internal {

	struct byte_alloc_buffer : public aligned_allocator<std::byte> {
		using allocator = aligned_allocator<std::byte>;

		inline byte_alloc_buffer(uint64_t capacityNew) noexcept {
			if (capacityNew <= allocator::maxSize() && capacityNew > 0) {
				values = allocator::allocate(capacityNew);
				if (values) {
					capacity = capacityNew;
				}
			}
		}

		byte_alloc_buffer& operator=(byte_alloc_buffer&&) noexcept		= delete;
		byte_alloc_buffer(byte_alloc_buffer&&) noexcept					= delete;
		byte_alloc_buffer& operator=(const byte_alloc_buffer&) noexcept = delete;
		byte_alloc_buffer(const byte_alloc_buffer&) noexcept			= delete;

		inline operator bool() const noexcept {
			return capacity > 0 && values;
		}

		inline ~byte_alloc_buffer() noexcept {
			if (values && capacity) {
				allocator::deallocate(values, capacity);
			}
		}

		uint64_t capacity{};
		std::byte* values{};
	};

	enum class stream_targets : uint8_t { stdout_target, stderr_target };

	template<serialize_options options> struct get_size;
	template<serialize_options options> struct serialize;

	template<typename> inline constexpr serialize_options default_serialize_options{};

	struct endl_t {};
	inline constexpr endl_t endl{};

	template<uint64_t bufferSize = 8192, stream_targets stream_target = stream_targets::stdout_target> struct basic_stream {
		inline explicit basic_stream() noexcept : bufferLength{ 0 } {
		}

		basic_stream& operator=(const basic_stream&) = delete;
		basic_stream(const basic_stream&)			 = delete;

		template<typename value_type> inline basic_stream& operator<<(value_type&& value) noexcept {
			static constexpr serialize_options options{ default_serialize_options<value_type> };
			uint64_t indent{};
			uint64_t requiredSize{ get_size<options>::impl(value, indent) + 64 };
			if (requiredSize > bufferSize) {
				if (doFlush()) {
					byte_alloc_buffer allocBuffer{ requiredSize };
					if (allocBuffer) {
						write_buffer_ptr scratchStart{ std::bit_cast<write_buffer_ptr>(allocBuffer.values) };
						rawWrite(allocBuffer.values,
							static_cast<uint64_t>(serialize<options>::impl(value, std::bit_cast<write_buffer_ptr>(allocBuffer.values), indent) - scratchStart));
					}
				}
				return *this;
			}
			if (!ensureSpace(requiredSize)) {
				return *this;
			}
			write_buffer_ptr start{ std::bit_cast<write_buffer_ptr>(streamBuffer + bufferLength) };
			bufferLength += static_cast<uint64_t>(serialize<options>::impl(value, std::bit_cast<write_buffer_ptr>(streamBuffer + bufferLength), indent) - start);
			return *this;
		}

		template<error_reportable_types value_type> inline basic_stream& operator<<(value_type&& value) noexcept {
			const auto report{ value.reportError() };
			write(report.data(), report.size());
			return *this;
		}

		template<string_types value_type> inline basic_stream& operator<<(value_type&& value) noexcept {
			write(value.data(), value.size());
			return *this;
		}

		template<typename char_type> inline uint64_t rawWrite(const char_type* data, uint64_t len) noexcept {
			if (len == 0 || data == nullptr) {
				return 0;
			}
#if JSONIFIER_PLATFORM_WINDOWS
			static constexpr DWORD handle_ids[2]{ STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
			static constexpr uint64_t max_chunk{ 1ull << 30 };
			HANDLE handle{ GetStdHandle(handle_ids[static_cast<uint64_t>(stream_target)]) };
			if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
				return 0;
			}
			uint64_t total{ 0 };
			while (total < len) {
				uint64_t remaining{ len - total };
				DWORD chunk{ static_cast<DWORD>(remaining < max_chunk ? remaining : max_chunk) };
				DWORD written{ 0 };
				if (!WriteFile(handle, static_cast<const void*>(data + total), chunk, &written, nullptr) || written == 0) {
					break;
				}
				total += written;
			}
			return total;
#else
			static constexpr int32_t stream_types[2]{ 1, 2 };
			int32_t fd{ stream_types[static_cast<uint64_t>(stream_target)] };
			uint64_t total{ 0 };
			while (total < len) {
				ssize_t result{ ::write(fd, static_cast<const void*>(data + total), len - total) };
				if (result < 0) {
					if (errno == EINTR) {
						continue;
					}
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						pollfd pfd{ fd, POLLOUT, 0 };
						int32_t ready{ poll(&pfd, 1, -1) };
						if (ready < 0) {
							if (errno == EINTR) {
								continue;
							}
							break;
						}
						if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 || (pfd.revents & POLLOUT) == 0) {
							break;
						}
						continue;
					}
					break;
				}
				if (result == 0) {
					break;
				}
				total += static_cast<uint64_t>(result);
			}
			return total;
#endif
		}

		template<uint64_t size> inline basic_stream& operator<<(const char (&value)[size]) noexcept {
			write(value, size - 1);
			return *this;
		}

		template<char_types value_type> inline basic_stream& operator<<(value_type&& value) noexcept {
			if (ensureSpace(1)) {
				streamBuffer[bufferLength++] = static_cast<uint8_t>(value);
			}
			return *this;
		}

		inline void write(const char* data, uint64_t size) noexcept {
			if (size >= bufferSize) {
				if (doFlush()) {
					rawWrite(data, size);
				}
				return;
			}
			if (ensureSpace(size)) {
				jsonifierMemcpy(streamBuffer + bufferLength, data, size);
				bufferLength += size;
			}
		}

		inline bool doFlush() noexcept {
			if (bufferLength == 0) {
				return true;
			}
			uint64_t written{ rawWrite(streamBuffer, bufferLength) };
			if (written < bufferLength) {
				memmoveWrapper(+streamBuffer, streamBuffer + written, bufferLength - written);
				bufferLength -= written;
				return false;
			}
			bufferLength = 0;
			return true;
		}

		inline bool ensureSpace(uint64_t needed) noexcept {
			if (bufferLength + needed <= bufferSize) {
				return true;
			}
			doFlush();
			return bufferLength + needed <= bufferSize;
		}

		inline basic_stream& operator<<(endl_t) noexcept {
			(*this) << '\n';
			doFlush();
			return *this;
		}

		inline string_view view() const noexcept {
			return { std::bit_cast<const char*>(+streamBuffer), bufferLength };
		}

		inline void discard() noexcept {
			bufferLength = 0;
		}

		inline ~basic_stream() noexcept {
			doFlush();
		}

		uint64_t bufferLength;
		uint8_t streamBuffer[bufferSize];
	};

#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
	#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

	inline basic_stream<8192, stream_targets::stdout_target> cout{};

	inline basic_stream<8192, stream_targets::stderr_target> err{};

#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic pop
#endif

	template<basic_stream_types basic_stream_type> JSONIFIER_INLINE basic_stream_type& operator<<(basic_stream_type& os, const string_view& input) noexcept {
		os.write(input.data(), input.size());
		return os;
	}

	template<basic_stream_types basic_stream_type, uint64_t size> inline basic_stream_type& operator<<(basic_stream_type& os, const string_literal<size>& input) noexcept {
		os.write(input.data(), input.size());
		return os;
	}

}
