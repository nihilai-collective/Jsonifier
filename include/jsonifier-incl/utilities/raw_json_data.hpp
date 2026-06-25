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

#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/utilities/string.hpp>

namespace jsonifier {

	struct json_number {
		inline json_number() noexcept = default;

		inline json_number(const string& stringNew) noexcept : rawJson{ stringNew } {
		}

		inline uint64_t getUint() const {
			return strToUint64(rawJson);
		}

		inline int64_t getInt() const {
			return strToInt64(rawJson);
		}

		inline double getDouble() const {
			return strToDouble(rawJson);
		}

		inline friend bool operator==(const json_number& lhs, const json_number& rhs) {
			return lhs.rawJson == rhs.rawJson;
		}

		string rawJson{};
	};

	class raw_json_data {
	  public:
		using object_type = std::unordered_map<string, raw_json_data>;
		using string_type = string;
		using array_type  = std::vector<raw_json_data>;
		using number_type = json_number;
		using bool_type	  = bool;
		using null_type	  = std::nullptr_t;
		using error_type  = std::monostate;
		using value_type  = std::variant<object_type, array_type, string_type, number_type, bool_type, null_type, error_type>;

		inline raw_json_data() noexcept {
			value = null_type{};
		}

		template<typename iterator_type> inline raw_json_data(iterator_type& iterator, const string& jsonDataNew) noexcept {
			internal::json_iterator<parse_options{}, string_view_ptr, string_base<1024 * 1024>> localIterator{ &iterator.getStringBuffer(), &iterator.getErrors(),
				jsonDataNew.data(), jsonDataNew.data() + jsonDataNew.size() };
			value	 = constructValueFromRawJsonData(localIterator, jsonDataNew);
			jsonData = jsonDataNew;
		}

		inline json_type getType() const noexcept {
			if (std::holds_alternative<object_type>(value)) {
				return json_type::object;
			} else if (std::holds_alternative<array_type>(value)) {
				return json_type::array;
			} else if (std::holds_alternative<string_type>(value)) {
				return json_type::string;
			} else if (std::holds_alternative<number_type>(value)) {
				return json_type::number;
			} else if (std::holds_alternative<bool_type>(value)) {
				return json_type::boolean;
			} else if (std::holds_alternative<null_type>(value)) {
				return json_type::null;
			} else {
				return json_type::unset;
			}
		}

		inline const object_type& getObject() const noexcept {
			return std::get<object_type>(value);
		}

		inline object_type& getObject() noexcept {
			return std::get<object_type>(value);
		}

		inline const array_type& getArray() const noexcept {
			return std::get<array_type>(value);
		}

		inline array_type& getArray() noexcept {
			return std::get<array_type>(value);
		}

		inline const string_type& getString() const noexcept {
			return std::get<string_type>(value);
		}

		inline string_type& getString() noexcept {
			return std::get<string_type>(value);
		}

		inline const number_type& getNumber() const noexcept {
			return std::get<number_type>(value);
		}

		inline number_type& getNumber() noexcept {
			return std::get<number_type>(value);
		}

		inline double getDouble() const noexcept {
			return std::get<number_type>(value).getDouble();
		}

		inline int64_t getInt() const noexcept {
			return std::get<number_type>(value).getInt();
		}

		inline uint64_t getUint() const noexcept {
			return std::get<number_type>(value).getUint();
		}

		inline const bool_type& getBool() const noexcept {
			return std::get<bool_type>(value);
		}

		inline bool_type& getBool() noexcept {
			return std::get<bool_type>(value);
		}

		template<concepts::uint_types index_type> inline raw_json_data& operator[](index_type&& index) noexcept {
			return (std::get<array_type>(value))[index];
		}

		template<concepts::uint_types index_type> inline const raw_json_data& operator[](index_type&& index) const noexcept {
			return (std::get<array_type>(value))[index];
		}

		template<std::convertible_to<string_view> key_type> inline raw_json_data& operator[](key_type&& key) noexcept {
			if (std::holds_alternative<null_type>(value)) {
				value = object_type{};
			}
			auto& object = std::get<object_type>(value);
			using key_type_local = typename concepts::base_t<decltype(object)>::key_type;
			return object[static_cast<key_type_local>(key)];
		}

		template<std::convertible_to<const string_view> key_type> inline const raw_json_data& operator[](key_type&& key) const noexcept {
			const auto& object	 = std::get<object_type>(value);
			using key_type_local = typename concepts::base_t<decltype(object)>::key_type;
			return object.at(static_cast<key_type_local>(key));
		}

		template<std::convertible_to<string_view> key_type> inline bool contains(key_type&& key) const noexcept {
			if (!std::holds_alternative<object_type>(value)) {
				return false;
			}
			const auto& object	 = std::get<object_type>(value);
			using key_type_local = typename concepts::base_t<decltype(object)>::key_type;
			return object.contains(static_cast<key_type_local>(key));
		}

		inline uint64_t size() const noexcept {
			if (std::holds_alternative<object_type>(value)) {
				return std::get<object_type>(value).size();
			} else if (std::holds_alternative<array_type>(value)) {
				return std::get<array_type>(value).size();
			} else if (std::holds_alternative<string_type>(value)) {
				return std::get<string_type>(value).size();
			} else {
				return 0;
			}
		}

		inline string_view rawJson() const noexcept {
			return jsonData;
		}

		inline bool operator==(const raw_json_data& other) const noexcept {
			return jsonData == other.jsonData && value == other.value;
		}

	  protected:
		value_type value{};
		string jsonData{};

		template<typename json_iterator_type> inline value_type constructValueFromRawJsonData(json_iterator_type& iterator, const string& jsonDataNew) noexcept {
			static constexpr parse_options optionsNew{};
			if (jsonDataNew.size() > 0) {
				switch (jsonDataNew[0]) {
					case '{': {
						typename raw_json_data::object_type results{};
						internal::parse_impl<typename raw_json_data::object_type, json_iterator_type, optionsNew>::impl(results, iterator);
						if (iterator.getErrors().size() == 0) {
							return value_type{ results };
						} else {
							return value_type{ null_type{} };
						}
					}
					case '[': {
						typename raw_json_data::array_type results{};
						internal::parse_impl<typename raw_json_data::array_type, json_iterator_type, optionsNew>::impl(results, iterator);
						if (iterator.getErrors().size() == 0) {
							return value_type{ results };
						} else {
							return value_type{ null_type{} };
						}
					}
					case '"': {
						typename raw_json_data::string_type results{};
						internal::parse_impl<typename raw_json_data::string_type, json_iterator_type, optionsNew>::impl(results, iterator);
						if (iterator.getErrors().size() == 0) {
							return value_type{ results };
						} else {
							return value_type{ null_type{} };
						}
					}
					case 't':
						return value_type{ bool_type{ true } };
					case 'f': {
						return value_type{ bool_type{ false } };
					}
					case 'n': {
						return value_type{ null_type{} };
					}
					case '0':
						[[fallthrough]];
					case '1':
						[[fallthrough]];
					case '2':
						[[fallthrough]];
					case '3':
						[[fallthrough]];
					case '4':
						[[fallthrough]];
					case '5':
						[[fallthrough]];
					case '6':
						[[fallthrough]];
					case '7':
						[[fallthrough]];
					case '8':
						[[fallthrough]];
					case '9':
						[[fallthrough]];
					case '-': {
						return value_type{ number_type{ jsonDataNew } };
					}
					default: {
						return value_type{ null_type{} };
					}
				}
			} else {
				return value_type{ null_type{} };
			}
		}
	};

	inline static std::ostream& operator<<(std::ostream& os, const raw_json_data& jsonValue) noexcept {
		os << jsonValue.rawJson();
		return os;
	}
}
