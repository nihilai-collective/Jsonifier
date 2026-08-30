// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/error.hpp
#pragma once

#include <jsonifier-incl/utilities/get_enum_name.hpp>
#include <jsonifier-incl/utilities/i_to_str.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	enum class status_classes : uint8_t {
		unset,
		parsing,
		minifying,
		prettifying,
		validating,
		count,
	};

	enum class parse_statuses : uint8_t {
		success,
		missing_key_start,
		missing_object_start,
		missing_object_end,
		missing_array_start,
		missing_array_end,
		invalid_string_characters,
		missing_colon,
		missing_comma,
		invalid_number_value,
		invalid_null_value,
		invalid_bool_value,
		no_input,
		unfinished_input,
		unexpected_string_end,
		unexpected_end_of_input,
		exceeded_max_depth,
		unexpected_token,
		illegal_control_character,
		count,
	};

	enum class minify_statuses : uint8_t {
		success,
		no_input,
		invalid_string_length,
		invalid_number_value,
		incorrect_structural_index,
		count,
	};

	enum class prettify_statuses : uint8_t {
		success,
		no_input,
		exceeded_max_depth,
		incorrect_structural_index,
		count,
	};

	enum class validate_statuses : uint8_t {
		success,
		missing_object_start,
		missing_object_end,
		missing_array_start,
		missing_array_end,
		invalid_string_characters,
		missing_colon,
		missing_comma,
		invalid_number_value,
		invalid_null_value,
		invalid_bool_value,
		invalid_escape_characters,
		missing_comma_or_closing_brace,
		no_input,
		count,
	};

	inline void appendUnsigned(string& stream, uint64_t value) {
		char buffer[24];
		auto* end = to_chars<uint64_t>::impl(buffer, value);
		stream.append(buffer, static_cast<size_t>(end - buffer));
	}

	inline void appendErrorType(status_classes status_class, string& stream, uint64_t value) {
		switch (static_cast<uint64_t>(status_class)) {
			case static_cast<uint64_t>(status_classes::parsing): {
				stream += getName(static_cast<parse_statuses>(value));
				return;
			}
			case static_cast<uint64_t>(status_classes::minifying): {
				stream += getName(static_cast<minify_statuses>(value));
				return;
			}
			case static_cast<uint64_t>(status_classes::prettifying): {
				stream += getName(static_cast<prettify_statuses>(value));
				return;
			}
			case static_cast<uint64_t>(status_classes::validating): {
				stream += getName(static_cast<validate_statuses>(value));
				return;
			}
			default: {
				return;
			}
		}
	}

	inline void appendEscapedByte(string& stream, char b) {
		switch (b) {
			case '\a':
				stream += "\\a";
				return;
			case '\b':
				stream += "\\b";
				return;
			case '\f':
				stream += "\\f";
				return;
			case '\n':
				stream += "\\n";
				return;
			case '\r':
				stream += "\\r";
				return;
			case '\t':
				stream += "\\t";
				return;
			case '\v':
				stream += "\\v";
				return;
			case '\\':
				stream += "\\\\";
				return;
			case '\'':
				stream += "\\'";
				return;
			case '\"':
				stream += "\\\"";
				return;
			case '\0':
				stream += "\\0";
				return;
			default:
				if (std::isprint(static_cast<uint8_t>(b))) {
					stream += b;
					return;
				} else {
					static constexpr char hexDigits[]{ "0123456789abcdef" };
					stream += "\\x";
					stream += hexDigits[(static_cast<uint8_t>(b) >> 4U) & 0xFU];
					stream += hexDigits[static_cast<uint8_t>(b) & 0xFU];
					return;
				}
		}
	}

	enum class json_structural_type : int8_t {
		error		 = -1,
		unset		 = 0,
		string		 = '"',
		comma		 = ',',
		number		 = '-',
		colon		 = ':',
		array_start	 = '[',
		array_end	 = ']',
		null		 = 'n',
		boolean		 = 't',
		object_start = '{',
		object_end	 = '}',
	};

	alignas(64) static constexpr array<bool, 256> boolTable{ []() constexpr {
		array<bool, 256> returnValues{};
		returnValues['t'] = true;
		returnValues['f'] = true;
		return returnValues;
	}() };

	class error {
	  public:
		template<parse_options parseOpts, typename iterator_type, typename string_buffer_type> friend struct json_iterator;

		inline error() noexcept = default;

		template<typename error_class> inline error(std::source_location sourceLocationNew, status_classes errorClassNew, string_view_ptr rootIterNew, string_view_ptr errorPosNew,
			string_view_ptr endIterNew, error_class typeNew) noexcept
			: sourceLocation{ sourceLocationNew }, errorClass{ errorClassNew }, rootIter{ rootIterNew }, errorPos{ errorPosNew }, endIter{ endIterNew },
			  errorType{ static_cast<uint64_t>(typeNew) } {
			formatReport();
		}

		template<status_classes errorClassNew, typename error_class> static inline error constructError(error_class typeNew, string_view_ptr rootIter, string_view_ptr errorPos,
			string_view_ptr endIter, const std::source_location& sourceLocation = std::source_location::current()) noexcept {
			return { sourceLocation, errorClassNew, rootIter, errorPos, endIter, typeNew };
		}

		template<status_classes errorClassNew, auto typeNew> static inline error constructError(string_view_ptr rootIter, string_view_ptr errorPos, string_view_ptr endIter,
			const std::source_location& sourceLocation = std::source_location::current()) noexcept {
			return { sourceLocation, errorClassNew, rootIter, errorPos, endIter, typeNew };
		}

		template<typename error_class> inline operator error_class() const noexcept {
			return static_cast<error_class>(errorType);
		}

		inline bool operator==(const error& rhs) const noexcept {
			return errorClass == rhs.errorClass && errorType == rhs.errorType && errorIndex == rhs.errorIndex && line == rhs.line && localIndex == rhs.localIndex;
		}

		inline const string& reportError() const noexcept {
			return reportString;
		}

	  protected:
		std::source_location sourceLocation{};
		status_classes errorClass{};
		string_view_ptr rootIter{};
		string_view_ptr errorPos{};
		string_view_ptr endIter{};
		uint64_t localIndex{};
		string reportString{};
		uint64_t errorIndex{};
		uint64_t errorType{};
		uint64_t line{};

		inline void formatReport() noexcept {
			line	   = 0;
			localIndex = 0;
			errorIndex = 0;
			string context{};
			if (errorPos && endIter && rootIter && errorPos >= rootIter && errorPos <= endIter) {
				errorIndex = static_cast<uint64_t>(errorPos - rootIter);

				line = static_cast<uint64_t>(std::count(rootIter, errorPos, '\n') + 1);

				string_view_ptr scan		   = errorPos;
				uint64_t distanceFromLineStart = 0;
				while (scan > rootIter && *(scan - 1) != '\n') {
					--scan;
					++distanceFromLineStart;
				}
				localIndex = distanceFromLineStart;

				if (errorPos < endIter) {
					uint64_t errorLength = std::min(static_cast<uint64_t>(16ULL), static_cast<uint64_t>(endIter - errorPos));
					string_view view{ errorPos, errorLength };
					context = string{ std::begin(view), static_cast<uint64_t>(view.size()) };
					for (auto& c: context) {
						if (c == '\t') {
							c = ' ';
						}
					}
				}
			}
			string stream{};
			stream += "Error of Class: ";
			stream += getName(errorClass);
			stream += ", of Type: ";
			appendErrorType(errorClass, stream, errorType);
			stream += ", at global index: ";
			appendUnsigned(stream, errorIndex);
			stream += ", on line: ";
			appendUnsigned(stream, line);
			stream += ", at local index: ";
			appendUnsigned(stream, localIndex);
			if (!context.empty()) {
				stream += "\nHere's some of the string's values: ";
				collectValues(stream, context);
			}
			stream += "\nIn file: ";
			stream += sourceLocation.file_name();
			stream += ", at line/column: ";
			appendUnsigned(stream, static_cast<uint64_t>(sourceLocation.line()));
			stream += ":";
			appendUnsigned(stream, static_cast<uint64_t>(sourceLocation.column()));
			stream += "\n";
			reportString = stream;
		}

		static inline void collectValues(string& stream, const string& inputValues) {
			for (uint64_t i = 0; i < 32 && i < inputValues.size(); ++i) {
				stream += "'";
				appendEscapedByte(stream, inputValues[i]);
				stream += "' ";
			}
			return;
		}
	};

	inline static std::ostream& operator<<(std::ostream& os, const error& errorNew) noexcept {
		os << errorNew.reportError();
		return os;
	}

}
