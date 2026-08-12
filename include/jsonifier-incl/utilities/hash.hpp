// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/hash.hpp
#pragma once

namespace jsonifier::internal {

	struct xoshiro256 {
		uint64_t state[4]{};

		constexpr xoshiro256() noexcept {
			constexpr auto x   = 0x9E3779B185EBCA87ull >> 12ull;
			constexpr auto x01 = x ^ x << 25ull;
			constexpr auto x02 = x01 ^ x01 >> 27ull;
			uint64_t s		   = x02 * 0x2545F4914F6CDD1Dull;
			for (uint64_t y = 0; y < 4; ++y) {
				state[y] = splitmix64(s);
			}
		}

		constexpr uint64_t operator()() noexcept {
			const uint64_t result = rotl(state[1ull] * 5ull, 7ull) * 9ull;
			const uint64_t t	  = state[1ull] << 17ull;

			state[2ull] ^= state[0ull];
			state[3ull] ^= state[1ull];
			state[1ull] ^= state[2ull];
			state[0ull] ^= state[3ull];

			state[2ull] ^= t;

			state[3ull] = rotl(state[3ull], 45ull);

			return result;
		}

	  protected:
		constexpr uint64_t rotl(const uint64_t x, const uint64_t k) const noexcept {
			return (x << k) | (x >> (64ull - k));
		}

		constexpr uint64_t splitmix64(uint64_t& seed64) const noexcept {
			uint64_t result = seed64 += 0x9E3779B97F4A7C15ull;
			result			= (result ^ (result >> 30ull)) * 0xBF58476D1CE4E5B9ull;
			result			= (result ^ (result >> 27ull)) * 0x94D049BB133111EBull;
			return result ^ (result >> 31ull);
		}
	};

	alignas(64) static constexpr array<uint64_t, 135ULL> prns{ { 1033321092324544984ull, 2666561049963377653ull, 3901177690447069239ull, 4218182233242110882ull,
		5911765535454950103ull, 6788651254494793497ull, 7100864855074445223ull, 8121427956336305945ull, 9038010914689427860ull, 14840306302415334885ull, 2861875790078914964ull,
		3162274379479658823ull, 4716213344225307449ull, 540950270129450019ull, 6138393194460717092ull, 7344427311844191385ull, 8475133706542525636ull, 9707373313909664576ull,
		13125261184447140558ull, 2935828130652229499ull, 3352961464321085856ull, 4654333323360932970ull, 5071886467123008198ull, 6337413869067417456ull, 7068363609472928302ull,
		8706829452892616150ull, 9383326841165471636ull, 16102866716245881820ull, 2811014628691939071ull, 3268225168635854144ull, 4143407405368768949ull, 5597712091605167573ull,
		6100647393685909969ull, 7810560643861675820ull, 8193567265249468576ull, 9274898615585908930ull, 1186958974127274710ull, 246203706832441443ull, 3668316000003001120ull,
		4918933721064431133ull, 5627034507966762943ull, 6439181573813589114ull, 7007274452014357082ull, 8727797399712164036ull, 9543719554692367837ull, 17847026727775507706ull,
		2455135551339952688ull, 3249111793759010315ull, 4777639692643446085ull, 5509261895474102266ull, 6044529605818700585ull, 7171416927005376731ull, 8039758273674712696ull,
		9590025961231307183ull, 11664492738409977550ull, 2284380607188886295ull, 3813446608469001272ull, 4331825983983119949ull, 5837226587704917004ull, 6635783511790253542ull,
		705765947185415012ull, 8161069307156878177ull, 9569010482089894937ull, 10396430003774290631ull, 2470420036324932164ull, 3824517089721070566ull, 4514057289782578484ull,
		5632633334704453746ull, 6925174598726443993ull, 7516137935779736019ull, 824571755531910559ull, 9361703638678697870ull, 13796235445108347584ull, 2481146946454909733ull,
		3823008181066037442ull, 4754601782272553608ull, 588747081408207180ull, 6322894155156329605ull, 7737051281621502357ull, 8728044800884920985ull, 9923282424466541678ull,
		18161647536849824815ull, 2607456623799892498ull, 3651449820230355939ull, 4624058760756378704ull, 564341449426358799ull, 6732980169420780216ull, 7082954320121844082ull,
		8326246156222992233ull, 9417642353078551282ull, 17539099562248686315ull, 226802774388233589ull, 3258991441457498839ull, 4386515027804287469ull, 5492870100834679754ull,
		6249105792560430415ull, 7289628920991893817ull, 8241072433031030544ull, 9727644451441173921ull, 13586305903807621608ull, 200516020872926547ull, 3730616597292952024ull,
		4256645544584917949ull, 5613544969337462956ull, 6264647669092059269ull, 7960331042009250727ull, 8582958636583556090ull, 9171663339272942914ull, 1619903645133495717ull,
		2840616349619109328ull, 3166096472286566799ull, 4494229804275778550ull, 5497884137148100871ull, 6572487097017879223ull, 738706937289335047ull, 8122825727823277447ull,
		9131968469543030694ull, 14054393997887833558ull, 2874030832643593377ull, 3673904271267944876ull, 4542812785880908260ull, 5621946313585701459ull, 6176632143181793702ull,
		7512972502278041818ull, 8724494295438506783ull, 9277533619161797917ull, 13495127262014153477ull, 2883303557104387784ull, 3039599040070277986ull, 4196273005435491662ull,
		5417879022829474871ull, 6476778602757520149ull, 7959620869796075525ull, 8518936512742009562ull, 9635246566869230345ull } };

	template<typename value_type> constexpr value_type readBitsCt(string_view_ptr ptr) noexcept {
		value_type chunk{};
		for (uint64_t x = 0; x < sizeof(value_type); ++x) {
			chunk |= static_cast<value_type>(static_cast<uint8_t>(ptr[x])) << (x * 8);
		}
		if constexpr (std::endian::native == std::endian::big) {
			chunk = byteswap(chunk);
		}
		return chunk;
	}

	struct ct_key_hasher {
		uint64_t index{};
		uint64_t seed{};

		constexpr ct_key_hasher() noexcept {
			updateSeed();
		}

		constexpr void updateSeed() noexcept {
			seed = prns[index % prns.size()];
			++index;
		}

		constexpr uint64_t hashKeyCt(string_view_ptr value, uint64_t length) const noexcept {
			uint64_t seed64{ seed };
			uint64_t chunk64{};

			while (length >= 8) {
				chunk64 = readBitsCt<uint64_t>(value);
				seed64 ^= chunk64 * 0x9E3779B185EBCA87ull;
				value += 8;
				length -= 8;
			}

			if (length > 0) {
				uint64_t tail{};
				for (uint64_t i = 0; i < length; ++i) {
					tail |= static_cast<uint64_t>(static_cast<uint8_t>(value[i])) << (i * 8);
				}
				if constexpr (std::endian::native == std::endian::big) {
					tail = byteswap(tail);
				}
				seed64 ^= tail * 0x9E3779B185EBCA87ull;
			}

			return seed64 ^ (seed64 >> 32);
		}
	};

	template<typename value_type> inline static consteval value_type constEval(value_type value) {
		return value;
	}

	template<uint64_t seedNew> struct rt_key_hasher {
		static constexpr auto seed{ seedNew };

		JSONIFIER_INLINE static uint64_t hashKeyRt(string_view_ptr value, uint64_t length) noexcept {
			uint64_t seed64{ constEval(seed) };
			uint64_t chunk64{};

			while (length >= 8) {
				std::memcpy(&chunk64, value, 8);
				seed64 ^= chunk64 * 0x9E3779B185EBCA87ull;
				value += 8;
				length -= 8;
			}

			if (length > 0) {
				uint64_t tail{};
				std::memcpy(&tail, value, length);
				seed64 ^= tail * 0x9E3779B185EBCA87ull;
			}

			return seed64 ^ (seed64 >> 32);
		}
	};
}
