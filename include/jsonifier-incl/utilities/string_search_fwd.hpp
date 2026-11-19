/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/string_search_fwd.hpp
 */
#pragma once

#include <jsonifier-incl/core/config.hpp>

namespace jsonifier::internal {

	struct string_search {
		template<typename... arg_types> JSONIFIER_INLINE static constexpr uint64_t find(const char* text, uint64_t length, arg_types&&... args) noexcept;

		template<typename... arg_types> JSONIFIER_INLINE static constexpr uint64_t rfind(const char* text, uint64_t length, arg_types&&... args) noexcept;

		template<typename... arg_types> JSONIFIER_INLINE static constexpr uint64_t findFirstOf(const char* text, uint64_t length, arg_types&&... args) noexcept;

		template<typename... arg_types> JSONIFIER_INLINE static constexpr uint64_t findFirstNotOf(const char* text, uint64_t length, arg_types&&... args) noexcept;

		template<typename... arg_types> JSONIFIER_INLINE static constexpr uint64_t findLastOf(const char* text, uint64_t length, arg_types&&... args) noexcept;

		template<typename... arg_types> JSONIFIER_INLINE static constexpr uint64_t findLastNotOf(const char* text, uint64_t length, arg_types&&... args) noexcept;
	};

}
