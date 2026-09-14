/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/prixon_core.hpp
 * NOTE: FOR POSSIBLE CLIENTS - THIS WILL NOT BE GOING ANYWHERE NEAR YOUR CODEBASE UNLESS YOU REQUEST IT!
 */
#pragma once

#include <jsonifier-incl/parsing/validate_impl.hpp>
#include <jsonifier-incl/serializing/serializer.hpp>
#include <jsonifier-incl/serializing/prettifier.hpp>
#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/error.hpp>
#include <jsonifier-incl/utilities/printer.hpp>

namespace jsonifier {

	struct prixon_core {
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
