// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/raw_json_data.hpp
#pragma once

#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/utilities/string.hpp>

namespace jsonifier {

	class json_number {
	  public:
		enum class number_types : uint8_t { uint64, int64, double64 };

		JSONIFIER_INLINE json_number() noexcept : uint_val(0), number_type(number_types::uint64) {
		}

		JSONIFIER_INLINE json_number(string_view sv) noexcept {
			const char* first = sv.data();
			const char* last  = sv.data() + sv.size();

			if (sv.empty()) {
				uint_val  = 0;
				number_type = number_types::uint64;
				return;
			}

			if (sv[0] == '-') {
				int64_t ival   = 0;
				auto ptr	 = internal::integer_parser<int64_t>::parseInt(ival, first, last);
				if (ptr == last) {
					int_val		= ival;
					number_type = number_types::int64;
					return;
				}
			} else {
				uint64_t uval  = 0;
				auto ptr	  = internal::integer_parser<uint64_t>::parseInt(uval, first, last);
				if (ptr == last) {
					uint_val	= uval;
					number_type = number_types::uint64;
					return;
				}
			}

			double dval	   = 0.0;
			auto ptr	= internal::float_parser<double>::parseFloat(dval, first, last);
			if (ptr == last) {
				double_val  = dval;
				number_type = number_types::double64;
			} else {
				uint_val  = 0;
				number_type = number_types::uint64;
			}
		}

		JSONIFIER_INLINE uint64_t getUint() const noexcept {
			return number_type == number_types::uint64 ? uint_val : 0;
		}

		JSONIFIER_INLINE int64_t getInt() const noexcept {
			switch (static_cast<uint64_t>(number_type)) {
				case static_cast<uint64_t>(number_types::uint64): {
					return static_cast<int64_t>(uint_val);
				}
				case static_cast<uint64_t>(number_types::int64): {
					return int_val;
				}
				case static_cast<uint64_t>(number_types::double64): {
					return static_cast<int64_t>(double_val);
				}
				default: {
					return 0;
				}
			}
		}

		JSONIFIER_INLINE double getDouble() const noexcept {
			return number_type == number_types::double64 ? double_val : 0.0;
		}

		JSONIFIER_INLINE bool friend operator==(const json_number& lhs, const json_number& rhs) {
			if (lhs.number_type == rhs.number_type) {
				switch (static_cast<uint64_t>(lhs.number_type)) {
					case static_cast<uint64_t>(number_types::uint64): {
						return lhs.getUint() == rhs.getUint();
					}
					case static_cast<uint64_t>(number_types::int64): {
						return lhs.getInt() == rhs.getInt();
					}
					case static_cast<uint64_t>(number_types::double64): {
						return std::bit_cast<uint64_t>(lhs.getDouble()) == std::bit_cast<uint64_t>(rhs.getDouble());
					}
					default: {
						return false;
					}
				}
			} else {
				return false;
			}
		}

	  private:
		union {
			uint64_t uint_val;
			double double_val;
			int64_t int_val;
		};
		number_types number_type;
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
			value.emplace<null_type>();
		}

		template<typename iterator_type> inline raw_json_data(iterator_type& iterator, const string& jsonDataNew) noexcept {
			internal::json_iterator<parse_options{}, string_view_ptr, string_base<1024 * 1024>> localIterator{ &iterator.getStringBuffer(), &iterator.getErrors(),
				jsonDataNew.data(), jsonDataNew.data() + jsonDataNew.size() };
			constructValueFromRawJsonData(localIterator, jsonDataNew);
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

		template<internal::uint_types index_type> inline raw_json_data& operator[](index_type&& index) noexcept {
			return (std::get<array_type>(value))[index];
		}

		template<internal::uint_types index_type> inline const raw_json_data& operator[](index_type&& index) const noexcept {
			return (std::get<array_type>(value))[index];
		}

		template<std::convertible_to<string_view> key_type> inline raw_json_data& operator[](key_type&& key) noexcept {
			if (std::holds_alternative<null_type>(value)) {
				value = object_type{};
			}
			auto& object = std::get<object_type>(value);
			using key_type_local = typename internal::base_t<decltype(object)>::key_type;
			return object[static_cast<key_type_local>(key)];
		}

		template<std::convertible_to<const string_view> key_type> inline const raw_json_data& operator[](key_type&& key) const noexcept {
			const auto& object	 = std::get<object_type>(value);
			using key_type_local = typename internal::base_t<decltype(object)>::key_type;
			return object.at(static_cast<key_type_local>(key));
		}

		template<std::convertible_to<string_view> key_type> inline bool contains(key_type&& key) const noexcept {
			if (!std::holds_alternative<object_type>(value)) {
				return false;
			}
			const auto& object	 = std::get<object_type>(value);
			using key_type_local = typename internal::base_t<decltype(object)>::key_type;
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

		inline bool operator==(const raw_json_data& other) const noexcept {
			return value == other.value;
		}

	  protected:
		value_type value{};

		template<typename json_iterator_type> inline void constructValueFromRawJsonData(json_iterator_type& iterator, const string& jsonDataNew) noexcept {
			static constexpr parse_options optionsNew{};
			if (jsonDataNew.size() > 0) {
				switch (jsonDataNew[0]) {
					case '{': {
						internal::parse_impl<object_type, json_iterator_type, optionsNew>::impl(value.emplace<object_type>(), iterator);
						if (iterator.getErrors().size() != 0) {
							value.emplace<null_type>();
						}
						return;
					}
					case '[': {
						internal::parse_impl<array_type, json_iterator_type, optionsNew>::impl(value.emplace<array_type>(), iterator);
						if (iterator.getErrors().size() != 0) {
							value.emplace<null_type>();
						}
						return;
					}
					case '"': {
						internal::parse_impl<string_type, json_iterator_type, optionsNew>::impl(value.emplace<string_type>(), iterator);
						if (iterator.getErrors().size() != 0) {
							value.emplace<null_type>();
						}
						return;
					}
					case 't':
						value.emplace<bool_type>(true);
						return;
					case 'f': {
						value.emplace<bool_type>(false);
						return;
					}
					case 'n': {
						value.emplace<null_type>();
						return;
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
						value.emplace<number_type>(jsonDataNew);
						return;
					}
					default: {
						value.emplace<null_type>();
						return;
					}
				}
			} else {
				value.emplace<null_type>();
				return;
			}
		}
	};

}
