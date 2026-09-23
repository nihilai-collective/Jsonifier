/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/d_to_str.hpp
 */
#pragma once

#include <jsonifier-incl/containers/allocator.hpp>
#include <jsonifier-incl/utilities/zmij.hpp>

namespace jsonifier::internal {

	template<float_t value_type> struct to_chars<value_type> {
		JSONIFIER_INLINE static write_buffer_ptr impl(write_buffer_ptr buf, value_type val) noexcept {
			return zmij::detail::write(val, buf);
		}
	};
}
