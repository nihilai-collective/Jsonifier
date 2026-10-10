/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/jsonifier_core.hpp
 */
#pragma once

#include <jsonifier-incl/core/backend_passes.hpp>
#include <jsonifier-incl/simd/backend_detection.hpp>

namespace jsonifier {

	template<uint64_t initialBufferSize> struct jsonifier_core_storage {
		inline jsonifier_core_storage() noexcept										  = default;
		inline jsonifier_core_storage(jsonifier_core_storage&& other) noexcept			  = default;
		inline jsonifier_core_storage& operator=(jsonifier_core_storage&& other) noexcept = default;
		jsonifier_core_storage(const jsonifier_core_storage& other)						  = delete;
		jsonifier_core_storage& operator=(const jsonifier_core_storage& other)			  = delete;

		string_base<initialBufferSize> stringBuffer{};
		std::vector<internal::error> errors{};
		std::unique_ptr<generic::generic_storage> genericData{};
	};

	template<uint64_t initialBufferSize, bool sharedStorage> struct jsonifier_core_storage_holder {
		inline jsonifier_core_storage_holder() noexcept													= default;
		inline jsonifier_core_storage_holder(jsonifier_core_storage_holder&& other) noexcept			= default;
		inline jsonifier_core_storage_holder& operator=(jsonifier_core_storage_holder&& other) noexcept = default;
		jsonifier_core_storage_holder(const jsonifier_core_storage_holder& other)						= delete;
		jsonifier_core_storage_holder& operator=(const jsonifier_core_storage_holder& other)			= delete;

		jsonifier_core_storage<initialBufferSize> ownedStorage{};
	};

	template<uint64_t initialBufferSize> struct jsonifier_core_storage_holder<initialBufferSize, true> {};

	template<jsonifier_backend backend = default_backend, uint64_t initialBufferSize = 1024 * 1024, bool sharedStorage = false> class jsonifier_core_internal
		: protected jsonifier_core_storage_holder<initialBufferSize, sharedStorage>,
		  public prixon_core,
		  public backend_types<backend>::template json_printer<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>>,
		  public backend_types<backend>::template prettifier<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>>,
		  public backend_types<backend>::template serializer<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>>,
		  public backend_types<backend>::template validator<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>>,
		  public backend_types<backend>::template minifier<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>>,
		  public backend_types<backend>::template parser<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>>,
		  public backend_types<backend>::template generic_iterator<jsonifier_core_internal<backend, initialBufferSize, sharedStorage>> {
		using storage_holder = jsonifier_core_storage_holder<initialBufferSize, sharedStorage>;
		using backend_type	 = backend_types<backend>;

	  public:
		using storage_type = jsonifier_core_storage<initialBufferSize>;
		using shared_core  = jsonifier_core_internal<backend, initialBufferSize, true>;

		static constexpr jsonifier_backend backendType{ backend };

		friend typename backend_type::template json_printer<jsonifier_core_internal>;
		friend typename backend_type::template prettifier<jsonifier_core_internal>;
		friend typename backend_type::template serializer<jsonifier_core_internal>;
		friend typename backend_type::template validator<jsonifier_core_internal>;
		friend typename backend_type::template minifier<jsonifier_core_internal>;
		friend typename backend_type::template parser<jsonifier_core_internal>;
		friend typename backend_type::template generic_iterator<jsonifier_core_internal>;

		inline jsonifier_core_internal() noexcept
			requires(!sharedStorage)
			: stringBuffer{ storage_holder::ownedStorage.stringBuffer }, errors{ storage_holder::ownedStorage.errors }, genericData{ storage_holder::ownedStorage.genericData } {
		}

		inline explicit jsonifier_core_internal(storage_type& storage) noexcept
			requires(sharedStorage)
			: stringBuffer{ storage.stringBuffer }, errors{ storage.errors }, genericData{ storage.genericData } {
		}

		inline jsonifier_core_internal(jsonifier_core_internal&& other) noexcept
			requires(!sharedStorage)
			: storage_holder{ internal::move(static_cast<storage_holder&>(other)) }, podSection{ internal::move(other.podSection) }, section{ internal::move(other.section) },
			  stringBuffer{ storage_holder::ownedStorage.stringBuffer }, errors{ storage_holder::ownedStorage.errors }, genericData{ storage_holder::ownedStorage.genericData } {
		}

		inline jsonifier_core_internal(storage_type& storage, jsonifier_core_internal&& other) noexcept
			requires(sharedStorage)
			: podSection{ internal::move(other.podSection) }, section{ internal::move(other.section) }, stringBuffer{ storage.stringBuffer }, errors{ storage.errors },
			  genericData{ storage.genericData } {
		}

		inline jsonifier_core_internal& operator=(jsonifier_core_internal&& other) noexcept {
			if (this != &other) [[likely]] {
				if constexpr (!sharedStorage) {
					storage_holder::ownedStorage = internal::move(other.ownedStorage);
				}
				podSection = internal::move(other.podSection);
				section	   = internal::move(other.section);
			}
			return *this;
		}

		jsonifier_core_internal& operator=(const jsonifier_core_internal& other) = delete;
		jsonifier_core_internal(const jsonifier_core_internal& other)			 = delete;

		inline const std::vector<internal::error>& getErrors() const noexcept {
			return errors;
		}

		inline std::vector<internal::error>& getErrors() noexcept {
			return errors;
		}

		inline ~jsonifier_core_internal() noexcept = default;

	  protected:
		typename backend_type::template pod_string_reader<backend_type::bytesPerStep> podSection{};
		typename backend_type::template string_reader<initialBufferSize> section{};
		string_base<initialBufferSize>& stringBuffer;
		std::vector<internal::error>& errors;
		std::unique_ptr<generic::generic_storage>& genericData;

		inline generic::generic_storage& genericStorage() noexcept {
			return generic::acquireStorage(genericData);
		}
	};

	template<uint64_t index, typename core_type, typename... core_types> struct core_at_impl {
		using type = typename core_at_impl<index - 1, core_types...>::type;
	};

	template<typename core_type, typename... core_types> struct core_at_impl<0, core_type, core_types...> {
		using type = core_type;
	};

#if JSONIFIER_COMPILER_MSVC
	#pragma warning(push)
	#pragma warning(disable : 4355)
#endif

	template<typename... cores> struct jsonifier_core_collection_impl : public core_at_impl<0, cores...>::type::storage_type, public cores... {
		using storage_type = typename core_at_impl<0, cores...>::type::storage_type;

		static_assert((std::is_same_v<storage_type, typename cores::storage_type> && ...), "Every core in a jsonifier_core_collection must share one buffer size.");

		inline jsonifier_core_collection_impl() noexcept : storage_type{}, cores{ static_cast<storage_type&>(*this) }... {
		}

		inline jsonifier_core_collection_impl(jsonifier_core_collection_impl&& other) noexcept
			: storage_type{ internal::move(static_cast<storage_type&>(other)) }, cores{ static_cast<storage_type&>(*this), internal::move(static_cast<cores&>(other)) }... {
		}

		inline jsonifier_core_collection_impl& operator=(jsonifier_core_collection_impl&& other) noexcept {
			if (this != &other) [[likely]] {
				static_cast<storage_type&>(*this) = internal::move(static_cast<storage_type&>(other));
				((static_cast<cores&>(*this) = internal::move(static_cast<cores&>(other))), ...);
			}
			return *this;
		}

		static inline jsonifier_backend selectBackend() noexcept {
			return internal::selectSupportedBackend<cores::backendType...>();
		}

		jsonifier_core_collection_impl& operator=(const jsonifier_core_collection_impl& other) = delete;
		jsonifier_core_collection_impl(const jsonifier_core_collection_impl& other)			   = delete;

		static inline jsonifier_backend selectedBackend() noexcept {
			static const jsonifier_backend backend{ selectBackend() };
			return backend;
		}

		static inline const char* activeBackendName() noexcept {
			const char* result{ cpu_arch_name };
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = backend_traits<cores::backendType>::name, true)) || ...));
			return result;
		}

		template<parse_options options = parse_options{}, internal::string_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
			return result;
		}

		template<parse_options options = parse_options{}, internal::bool_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
			return result;
		}

		template<parse_options options = parse_options{}, internal::number_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
			return result;
		}

		template<parse_options options = parse_options{}, typename value_type, typename buffer_type> inline bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
			return result;
		}

		template<serialize_options options = serialize_options{}, internal::number_t value_type, internal::buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType &&
								   (result = cores::template serializeJson<options>(internal::forward<value_type>(object), internal::forward<buffer_type>(buffer)), true)) ||
				...));
			return result;
		}

		template<serialize_options options = serialize_options{}, internal::string_t value_type, internal::buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType &&
								   (result = cores::template serializeJson<options>(internal::forward<value_type>(object), internal::forward<buffer_type>(buffer)), true)) ||
				...));
			return result;
		}

		template<serialize_options options = serialize_options{}, internal::bool_t value_type, internal::buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType &&
								   (result = cores::template serializeJson<options>(internal::forward<value_type>(object), internal::forward<buffer_type>(buffer)), true)) ||
				...));
			return result;
		}

		template<serialize_options options = serialize_options{}, typename value_type, internal::buffer_like buffer_type>
		inline bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType &&
								   (result = cores::template serializeJson<options>(internal::forward<value_type>(object), internal::forward<buffer_type>(buffer)), true)) ||
				...));
			return result;
		}

		template<parse_options options = parse_options{}, typename buffer_type> inline auto collectStructurals(buffer_type&& in) noexcept {
			decltype(core_at_impl<0, cores...>::type::template collectStructurals<options>(internal::forward<buffer_type>(in))) result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template collectStructurals<options>(internal::forward<buffer_type>(in)), true)) || ...));
			return result;
		}

		template<parse_options options = parse_options{}, typename buffer_type> inline auto collectStructuralsSingle(buffer_type&& in) noexcept {
			decltype(core_at_impl<0, cores...>::type::template collectStructuralsSingle<options>(internal::forward<buffer_type>(in))) result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template collectStructuralsSingle<options>(internal::forward<buffer_type>(in)), true)) || ...));
			return result;
		}

		template<typename string_type> inline bool validateJson(string_type&& in) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::validateJson(internal::forward<string_type>(in)), true)) || ...));
			return result;
		}

		template<typename input_string_type, typename output_buffer_type> inline bool minifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(
				((backend == cores::backendType && (result = cores::minifyJson(internal::forward<input_string_type>(in), internal::forward<output_buffer_type>(buffer)), true)) ||
					...));
			return result;
		}

		template<prettify_options options = prettify_options{}, typename input_string_type, typename output_buffer_type>
		inline bool prettifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			bool result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(
				((backend == cores::backendType &&
					 (result = cores::template prettifyJson<options>(internal::forward<input_string_type>(in), internal::forward<output_buffer_type>(buffer)), true)) ||
					...));
			return result;
		}

		template<parse_options options = parse_options{}, typename buffer_type> inline generic::document iterate(const buffer_type& in) noexcept {
			generic::document result{ generic::error_code::empty };
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template iterate<options>(in), true)) || ...));
			return result;
		}

		template<parse_options options = parse_options{ .newLineDelimited = true }, typename buffer_type>
		inline generic::document_stream<jsonifier_core_collection_impl, options> iterateMany(const buffer_type& in, uint64_t batchSize = generic::defaultBatchSize) noexcept {
			return generic::document_stream<jsonifier_core_collection_impl, options>{ *this, std::bit_cast<read_buffer_ptr>(in.data()), in.size(), batchSize };
		}

		template<typename value_type, typename stream_type> inline static void printJson(value_type&& value, stream_type& os, uint64_t depth = 0) noexcept {
			core_at_impl<0, cores...>::type::printJson(internal::forward<value_type>(value), os, depth);
		}

		inline const std::vector<internal::error>& getErrors() const noexcept {
			return storage_type::errors;
		}

		inline std::vector<internal::error>& getErrors() noexcept {
			return storage_type::errors;
		}

	  protected:
		template<typename, parse_options> friend class generic::document_stream;
		template<uint64_t> friend class generic::parser;

		template<parse_options options> inline uint64_t indexGenericWindow(read_buffer_ptr rootIter, uint64_t length) noexcept {
			uint64_t result{};
			static const jsonifier_backend backend{ selectedBackend() };
			static_cast<void>(((backend == cores::backendType && (result = cores::template indexGenericWindow<options>(rootIter, length), true)) || ...));
			return result;
		}

		inline generic::generic_storage& genericStorage() noexcept {
			return generic::acquireStorage(storage_type::genericData);
		}
	};

#if JSONIFIER_COMPILER_MSVC
	#pragma warning(pop)
#endif

	template<typename... cores> using jsonifier_core_collection = jsonifier_core_collection_impl<typename cores::shared_core...>;

#if JSONIFIER_CONFIGURED_AVX_TIER >= 3
	template<uint64_t initialBufferSize = 1024 * 1024> using jsonifier_core = jsonifier_core_collection<jsonifier_core_internal<jsonifier_backend::avx512, initialBufferSize>,
		jsonifier_core_internal<jsonifier_backend::avx2, initialBufferSize>, jsonifier_core_internal<jsonifier_backend::avx, initialBufferSize>>;
#elif JSONIFIER_CONFIGURED_AVX_TIER == 2
	template<uint64_t initialBufferSize = 1024 * 1024> using jsonifier_core =
		jsonifier_core_collection<jsonifier_core_internal<jsonifier_backend::avx2, initialBufferSize>, jsonifier_core_internal<jsonifier_backend::avx, initialBufferSize>>;
#else
	template<uint64_t initialBufferSize = 1024 * 1024> using jsonifier_core = jsonifier_core_collection<jsonifier_core_internal<default_backend, initialBufferSize>>;
#endif

	inline const char* activeBackendName() noexcept {
		return jsonifier_core<>::activeBackendName();
	}

}
