/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/validate_impl.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/validator.hpp>

namespace jsonifier::internal {

	static constexpr parse_options optionsVal{};

	template<typename derived_type_new> struct validate_impl<json_structural_type::object_start, derived_type_new> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.template checkChar<'{'>()) [[likely]] {
				++context.currentIterPtr();
				if (context.template checkChar<'}'>()) [[unlikely]] {
					++context.currentIterPtr();
					return true;
				}

				while (context.notAtEndPre()) {
					if (validate_impl<json_structural_type::string, derived_type_new>::impl(context)) [[likely]] {
						if (context.template checkChar<':'>()) [[likely]] {
							++context.currentIterPtr();
							if (validator<derived_type_new>::impl(context)) [[likely]] {
								if (context.template checkChar<','>()) [[likely]] {
									++context.currentIterPtr();
								} else {
									if (context.template checkChar<'}'>()) {
										++context.currentIterPtr();
										return true;
									} else {
										return false;
									}
								}
							} else {
								return false;
							}
						} else {
							return false;
						}
					} else {
						return false;
					}
				}
				return false;
			} else {
				return false;
			}
		}
	};

	template<typename derived_type_new> struct validate_impl<json_structural_type::array_start, derived_type_new> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.template checkChar<'['>()) [[likely]] {
				++context.currentIterPtr();
				if (context.template checkChar<']'>()) [[unlikely]] {
					++context.currentIterPtr();
					return true;
				}
				while (context.notAtEndPre()) {
					if (validator<derived_type_new>::impl(context)) [[likely]] {
						if (context.template checkChar<','>()) [[likely]] {
							++context.currentIterPtr();
						} else {
							if (context.template checkChar<']'>()) {
								++context.currentIterPtr();
								return true;
							} else {
								return false;
							}
						}
					} else {
						return false;
					}
				}
				return false;
			} else {
				return false;
			}
		}
	};

	template<typename derived_type_new> struct validate_impl<json_structural_type::string, derived_type_new> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.template checkChar<'"'>()) [[likely]] {
				auto newPtr = context.currentPtr();
				++context.currentIterPtr();
				auto endPtr		   = context.notAtEndPre() ? context.currentPtr() : (newPtr + (context.endIterPtr() - context.currentIterPtr()));
				using scanner_type = string_scanner<optionsVal>;
				auto& scratch	   = context.getStringBuffer();
				const auto needed  = static_cast<uint64_t>(endPtr - newPtr) + simdBytesPerStep;
				if (scratch.size() < needed) [[unlikely]] {
					scratch.resize(needed);
				}
				return scanner_type::impl(newPtr, endPtr, scratch.data()).outLength != std::numeric_limits<uint64_t>::max();
			} else [[unlikely]] {
				return false;
			}
		}
	};

	template<typename derived_type_new> struct validate_impl<json_structural_type::number, derived_type_new> {
		inline static bool consumeChar(char expected, read_buffer_ptr& newerPtr) {
			if (*newerPtr == expected) {
				++newerPtr;
				return true;
			}
			return false;
		}

		inline static bool consumeDigits(read_buffer_ptr& newerPtr, uint64_t minCount = 1) {
			uint64_t count = 0;
			while (is_digit(static_cast<uint8_t>(*newerPtr))) {
				++newerPtr;
				++count;
			}
			return count >= minCount;
		}

		inline static void consumeSign(read_buffer_ptr& newerPtr) {
			if (*newerPtr == '-' || *newerPtr == '+') {
				++newerPtr;
			}
			return;
		}

		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			auto newPtr = context.currentPtr();
			++context.currentIterPtr();
			if (context.notAtEndPre() && (*newPtr != 0x30u || !numberTable[static_cast<uint64_t>(*(newPtr + 1))])) [[likely]] {
				consumeSign(newPtr);
				consumeDigits(newPtr);
				if (consumeChar(0x2Eu, newPtr)) {
					if (!context.notAtEndPre() || !consumeDigits(newPtr)) {
						return false;
					}
				}
				if (consumeChar(0x65u, newPtr) || consumeChar(0x45u, newPtr)) {
					consumeSign(newPtr);
				}
				return true;
			} else {
				return false;
			}
		}
	};

	template<typename derived_type_new> struct validate_impl<json_structural_type::boolean, derived_type_new> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.notAtEndPre() && validateBool(context.currentPtr())) [[likely]] {
				++context.currentIterPtr();
				return true;
			} else {
				return false;
			}
		}
	};

	template<typename derived_type_new> struct validate_impl<json_structural_type::null, derived_type_new> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.notAtEndPre() && validateNull(context.currentPtr())) [[likely]] {
				++context.currentIterPtr();
				return true;
			} else {
				return false;
			}
		}
	};

}
