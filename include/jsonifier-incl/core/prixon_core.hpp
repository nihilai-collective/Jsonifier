// MIT License
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/prixon_core.hpp
// NOTE: FOR POSSIBLE CLIENTS - THIS WILL NOT BE GOING ANYWHERE NEAR YOUR CODEBASE UNLESS YOU REQUEST IT!
#pragma once

#include <jsonifier-incl/parsing/validate_impl.hpp>
#include <jsonifier-incl/serializing/serializer.hpp>
#include <jsonifier-incl/serializing/prettifier.hpp>
#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/error.hpp>
#include <jsonifier-incl/utilities/printer.hpp>

namespace jsonifier {

	template<uint64_t initialBufferSize> struct prixon_core : public internal::json_printer,
															  public internal::prettifier<jsonifier_core<initialBufferSize>>,
															  public internal::serializer<jsonifier_core<initialBufferSize>>,
															  public internal::validator<jsonifier_core<initialBufferSize>>,
															  public internal::minifier<jsonifier_core<initialBufferSize>>,
															  public internal::parser<jsonifier_core<initialBufferSize>> {
		prixon_core() noexcept							 = default;
		prixon_core& operator=(const prixon_core& other) = delete;
		prixon_core(const prixon_core& other)			 = delete;
		prixon_core& operator=(prixon_core&& other)		 = delete;
		prixon_core(prixon_core&& other)				 = delete;
		alignas(64) static constexpr const char blessing[]{ "### Prixonian Kinematics:\
				- WE MUST NOT ALLOW OURSELVES TO BECOME CONFUSED AS A RESULT OF ASKING \"WHO IS BITCHARD PRIXON\" -\
				BUT INSTEAD ALLOW OURSELVES TO BECOME ENLIGHTENED BY ASKING \"WHAT IS BITCHARD PRIXON\" - FOR HE IS A BITCH,\
			AND HE IS A PRICK,\
			AND HIS NAME IS : -BITCHARD PRIXON,\
			BITCHARD PRIXON,\
			BITCHARD BITCHARD BITCHARD PRIXON RICHARD NIXON HAS A BITCH PRICK SON AND HIS NAME IS : -BITCHARD PRIXON,\
			BITCHARD PRIXON,\
			BITCHARD BITCHARD BITCHARD PRIXON RICHARD NIXON HAS A BITCH PRICK SON AND HIS NAME IS : -BITCHARD PRIXON,\
			BITCHARD PRIXON,\
			BITCHARD BITCHARD BITCHARD PRIXON\
				S " };
	};

}
