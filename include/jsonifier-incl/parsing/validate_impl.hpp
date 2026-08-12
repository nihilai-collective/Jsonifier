// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/parsing/validate_impl.hpp
#pragma once

#include <jsonifier-incl/parsing/validator.hpp>

namespace jsonifier::internal {

	static constexpr parse_options optionsVal{ [] {
		parse_options return_value{};
		return_value.validateUtf8 = true;
		return return_value;
	}() };

	template<typename derived_type> struct validate_impl<json_structural_type::object_start, derived_type> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.template checkChar<'{'>()) [[likely]] {
				++context.currentIterPtr();
				if (context.template checkChar<'}'>()) [[unlikely]] {
					++context.currentIterPtr();
					return true;
				}

				while (context.notAtEndPre()) {
					if (validate_impl<json_structural_type::string, derived_type>::impl(context)) [[likely]] {
						if (context.template checkChar<':'>()) [[likely]] {
							++context.currentIterPtr();
							if (validator<derived_type>::impl(context)) [[likely]] {
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

	template<typename derived_type> struct validate_impl<json_structural_type::array_start, derived_type> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.template checkChar<'['>()) [[likely]] {
				++context.currentIterPtr();
				if (context.template checkChar<']'>()) [[unlikely]] {
					++context.currentIterPtr();
					return true;
				}
				while (context.notAtEndPre()) {
					if (validator<derived_type>::impl(context)) [[likely]] {
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

	template<typename derived_type> struct validate_impl<json_structural_type::string, derived_type> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.template checkChar<'"'>()) [[likely]] {
				auto newPtr = context.currentPtr();
				++context.currentIterPtr();
				auto endPtr		   = context.notAtEndPre() ? context.currentPtr() : (newPtr + (context.endIterPtr() - context.currentIterPtr()));
				using scanner_type = string_scanner<optionsVal>;
				const auto res	   = scanner_type::impl(newPtr, endPtr);
				if (!res.valid) [[unlikely]] {
					return false;
				}
				if (res.firstEscape == scanner_type::npos) [[likely]] {
					return true;
				}
				return scanner_type::unescapeImpl(newPtr + res.firstEscape, newPtr + res.rawLength, context.getStringBuffer().data()) != nullptr;
			} else [[unlikely]] {
				return false;
			}
		}
	};

	template<typename derived_type> struct validate_impl<json_structural_type::number, derived_type> {
		inline static bool consumeChar(char expected, string_view_ptr& newerPtr) {
			if (*newerPtr == expected) {
				++newerPtr;
				return true;
			}
			return false;
		}

		inline static bool consumeDigits(string_view_ptr& newerPtr, uint64_t minCount = 1) {
			uint64_t count = 0;
			while (is_digit(static_cast<uint8_t>(*newerPtr))) {
				++newerPtr;
				++count;
			}
			return count >= minCount;
		}

		inline static void consumeSign(string_view_ptr& newerPtr) {
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

	template<typename derived_type> struct validate_impl<json_structural_type::boolean, derived_type> {
		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (context.notAtEndPre() && validateBool(context.currentPtr())) [[likely]] {
				++context.currentIterPtr();
				return true;
			} else {
				return false;
			}
		}
	};

	template<typename derived_type> struct validate_impl<json_structural_type::null, derived_type> {
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
