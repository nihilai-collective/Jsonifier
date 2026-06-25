/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier

#pragma once

#include <jsonifier-incl/utilities/get_enum_name.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	enum class status_classes : uint8_t {
		unset,
		parsing,
		serializing,
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

	inline void printBytes(std::ostringstream& stream, char b) {
		switch (b) {
			case '\a':
				stream << "\\a";
				return;
			case '\b':
				stream << "\\b";
				return;
			case '\f':
				stream << "\\f";
				return;
			case '\n':
				stream << "\\n";
				return;
			case '\r':
				stream << "\\r";
				return;
			case '\t':
				stream << "\\t";
				return;
			case '\v':
				stream << "\\v";
				return;
			case '\\':
				stream << "\\\\";
				return;
			case '\'':
				stream << "\\'";
				return;
			case '\"':
				stream << "\\\"";
				return;
			case '\0':
				stream << "\\0";
				return;
			default:
				if (std::isprint(static_cast<unsigned char>(b))) {
					stream << b;
					return;
				} else {
					stream << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(b)) << std::dec;
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
			return errorType == rhs.errorType && rootIter == rhs.rootIter && errorPos == rhs.errorPos && endIter == rhs.endIter;
		}

		inline string reportError() const noexcept {
			uint64_t errorIndex = static_cast<uint64_t>(errorPos - rootIter);
			uint64_t line{ 0 };
			uint64_t localIndex{ 0 };
			string context{};
			if (errorPos && endIter && errorPos < endIter) {
				uint64_t errorLength = std::min(static_cast<uint64_t>(16ULL), static_cast<uint64_t>(endIter - errorPos));
				int64_t reportIndex	 = 0;
				string_view view{ errorPos, errorLength };
				using V				   = std::decay_t<decltype(view[0])>;
				const auto start = std::begin(view) + reportIndex;
				line				   = static_cast<uint64_t>(std::count(rootIter, errorPos, '\n') + 1);
				const auto rstart	   = std::rbegin(view) + static_cast<int64_t>(view.size()) - reportIndex - 1ll;
				const auto prevNewLine = std::find(std::min(rstart + 1, std::rend(view)), std::rend(view), static_cast<V>('\n'));
				localIndex			   = static_cast<uint64_t>(std::distance(rstart, prevNewLine) - 1ll);
				auto endIndex{ std::end(view) - start >= 64 ? 64 : std::end(view) - start };
				context = string{ start, static_cast<uint64_t>(endIndex) };
				for (auto& c: context) {
					if (c == '\t') {
						c = ' ';
					}
				}
			}
			std::ostringstream stream{};
			stream << "Error of Class: " << errorClass << ", of Type: " << errorType << ", at global index: " << errorIndex << ", on line: " << line
				   << ", at local index: " << localIndex;
			if (!context.empty()) {
				stream << "\nHere's some of the string's values: ";
				collectValues(stream, context);
			}
			stream << "\nIn file: " << sourceLocation.file_name() << ", at line/column: " << sourceLocation.line() << ":" << sourceLocation.column() << std::endl;
			return stream.str();
		}

	  protected:
		std::source_location sourceLocation{};
		status_classes errorClass{};
		string_view_ptr rootIter{};
		string_view_ptr errorPos{};
		string_view_ptr endIter{};
		uint64_t errorType{};

		static inline void collectValues(std::ostringstream& stream, const string& inputValues) {
			for (uint64_t i = 0; i < 32 && i < inputValues.size(); ++i) {
				stream << "'";
				printBytes(stream, inputValues[i]);
				stream << "' ";
			}
			return;
		}
	};

	inline static std::ostream& operator<<(std::ostream& os, const error& errorNew) noexcept {
		os << errorNew.reportError();
		return os;
	}

}
