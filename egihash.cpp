// Copyright (c) 2017 Ryan Lucchese
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "egihash.h"
extern "C"
{
#include "keccak-tiny.h"
}

#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>
#include <type_traits>
#include <iostream> // TODO: remove me (debugging)

namespace
{
	namespace constants
	{
		constexpr char DAG_MAGIC_BYTES[] = "EGIHASH_DAG";
		constexpr uint32_t DAG_FILE_HEADER_SIZE = sizeof(DAG_MAGIC_BYTES) + (5 * sizeof(uint64_t)) + (3 * sizeof(uint32_t)) + 2;
		constexpr uint32_t CALLBACK_FREQUENCY = 1u;
		constexpr uint32_t MAJOR_VERSION = 1u;
		constexpr uint32_t REVISION = 23u;
		constexpr uint32_t MINOR_VERSION = 0u;
		constexpr uint32_t WORD_BYTES = 4u;                       // bytes in word
		constexpr uint32_t DATASET_BYTES_INIT = 1u << 30u;        // bytes in dataset at genesis
		constexpr uint32_t DATASET_BYTES_GROWTH = 1u << 23u;      // dataset growth per epoch
		constexpr uint32_t CACHE_BYTES_INIT = 1u << 24u;          // bytes in cache at genesis
		constexpr uint32_t CACHE_BYTES_GROWTH = 1u << 17u;        // cache growth per epoch
		constexpr uint32_t CACHE_MULTIPLIER=1024u;                // Size of the DAG relative to the cache
		constexpr uint32_t EPOCH_LENGTH = 30000u;                 // blocks per epoch
		constexpr uint32_t MIX_BYTES = 128u;                      // width of mix
		constexpr uint32_t HASH_BYTES = 64u;                      // hash length in bytes
		constexpr uint32_t DATASET_PARENTS = 256u;                // number of parents of each dataset element
		constexpr uint32_t CACHE_ROUNDS = 3u;                     // number of rounds in cache production
		constexpr uint32_t ACCESSES = 64u;                        // number of accesses in hashimoto loop

		constexpr EGIHASH_NAMESPACE(h256_t) empty_h256 = {{0}};
		constexpr EGIHASH_NAMESPACE(result_t) empty_result = {{{0}}, {{0}}};
	}

	inline int32_t decode_int(uint8_t const * data, uint8_t const * dataEnd) noexcept
	{
		if (!data || (dataEnd < (data + 3)))
			return 0;

		return static_cast<int32_t>(
			(static_cast<int32_t>(data[0]) << 24) |
			(static_cast<int32_t>(data[1]) << 16) |
			(static_cast<int32_t>(data[2]) << 8) |
			(static_cast<int32_t>(data[3]))
		);
	}

	inline ::std::string zpad(::std::string const & str, size_t const length)
	{
		return str + ::std::string(::std::max(length - str.length(), static_cast<::std::string::size_type>(0)), 0);
	}

	template <typename IntegralType >
	typename ::std::enable_if<::std::is_integral<IntegralType>::value, ::std::string>::type
	/*::std::string*/ encode_int(IntegralType x)
	{
		using namespace std;

		if (x == 0) return string();

		// TODO: fast hex conversion
		stringstream ss;
		ss << hex << x;
		string hex_str = ss.str();
		string encoded(hex_str.length() % 2, '0');
		encoded += hex_str;

		string ret;
		ss.str(string());
		for (size_t i = 0; i < encoded.size(); i += 2)
		{
			ret += static_cast<char>(stoi(encoded.substr(i, 2), 0, 16));
		}

		return ret;
	}

	template <typename IntegralType >
	typename ::std::enable_if<::std::is_integral<IntegralType>::value, bool>::type
	/*bool*/ is_prime(IntegralType x) noexcept
	{
		for (auto i = IntegralType(2); i <= ::std::sqrt(x); i++)
		{
			if ((x % i) == 0) return false;
		}
		return true;
	}

	inline uint32_t fnv(uint32_t v1, uint32_t v2) noexcept
	{
		constexpr uint32_t FNV_PRIME = 0x01000193ull;             // prime number used for FNV hash function
		constexpr uint64_t FNV_MODULUS = 1ull << 32ull;           // modulus used for FNV hash function

		return ((v1 * FNV_PRIME) ^ v2) % FNV_MODULUS;
	}

	class hash_exception : public ::std::runtime_error
	{
	public:
		hash_exception(std::string const & what_arg) noexcept
		: runtime_error(what_arg)
		{

		}

		hash_exception(char const * what_arg) noexcept
		: runtime_error(what_arg)
		{

		}
	};

	template <size_t HashSize, int (*HashFunction)(uint8_t *, size_t, uint8_t const * in, size_t)>
	struct sha3_base
	{
		using deserialized_hash_t = ::std::vector<int32_t>;

		static constexpr size_t hash_size = HashSize;
		uint8_t data[hash_size];

		sha3_base(sha3_base const &) = default;
		sha3_base(sha3_base &&) = default;
		sha3_base & operator=(sha3_base const &) = default;
		sha3_base & operator=(sha3_base &&) = default;
		~sha3_base() = default;

		sha3_base()
		: data{0}
		{

		}

		sha3_base(::std::string const & input)
		: data{0}
		{
			compute_hash(input.c_str(), input.size());
		}

		sha3_base(void const * input, size_t const input_size)
		: data{0}
		{
			compute_hash(input, input_size);
		}

		void compute_hash(void const * input, size_t const input_size)
		{
			if (HashFunction(data, hash_size, reinterpret_cast<uint8_t const *>(input), input_size) != 0)
			{
				throw hash_exception("Unable to compute hash"); // TODO: better message?
			}
		}

		deserialized_hash_t deserialize() const
		{
			deserialized_hash_t out(hash_size / 4, 0);
			for (size_t i = 0, j = 0; i < hash_size; i += constants::WORD_BYTES, j++)
			{
				out[j] = decode_int(&data[i], &data[hash_size - 1]);
			}
			return out;
		}

		static ::std::string serialize(deserialized_hash_t const & h)
		{
			::std::string ret;
			for (auto const i : h)
			{
				ret += zpad(encode_int(i), 4);
			}
			return ret;
		}

		operator ::std::string() const
		{
			// TODO: fast hex conversion
			::std::stringstream ss;
			ss << ::std::hex;
			for (auto const i : data)
			{
				ss << ::std::setw(2) << ::std::setfill('0') << static_cast<uint32_t>(i);
			}
			return ss.str();
		}
	};

	struct sha3_256_t : public sha3_base<32, ::sha3_256>
	{
		using deserialized_hash_t = ::std::vector<int32_t>;

		sha3_256_t(::std::string const & input)
		: sha3_base(input)
		{

		}

		sha3_256_t(void const * input, size_t const input_size)
		: sha3_base(input, input_size)
		{

		}

		sha3_256_t(EGIHASH_NAMESPACE(h256_t) const & h256)
		: sha3_base()
		{
			::std::memcpy(&data[0], &h256.b[0], hash_size);
		}
	};

	struct sha3_512_t : public sha3_base<64, ::sha3_512>
	{
		using deserialized_hash_t = ::std::vector<int32_t>;

		sha3_512_t(::std::string const & input)
		: sha3_base(input)
		{

		}

		sha3_512_t(void const * input, size_t const input_size)
		: sha3_base(input, input_size)
		{

		}
	};

	// TODO: unit tests / validation
	template <typename HashType>
	typename HashType::deserialized_hash_t hash_words(::std::string const & data)
	{
		auto const hash = HashType(data);
		return hash.deserialize();
	}

	// TODO: unit tests / validation
	template <typename HashType>
	typename HashType::deserialized_hash_t hash_words(typename HashType::deserialized_hash_t const & deserialized)
	{
		auto const serialized = HashType::serialize(deserialized);
		return hash_words<HashType>(serialized);
	}
}

namespace egihash
{
	h256_t::h256_t()
	: b{0}
	{

	}

	// TODO: unit tests / validation
	template <typename T>
	sha3_512_t::deserialized_hash_t sha3_512(T const & data)
	{
		return hash_words<sha3_512_t>(data);
	}

	// TODO: unit tests / validation
	template <typename T>
	sha3_256_t::deserialized_hash_t sha3_256(T const & data)
	{
		return hash_words<sha3_256_t>(data);
	}

	// TODO: unit tests / validation
	template <typename T>
	::std::string serialize_cache(T const & cache_data)
	{
		::std::string ret;
		for (auto const & i : cache_data)
		{
			ret += serialize_hash(cache_data);
		}
	}

	// TODO: unit tests / validation
	template <typename T>
	::std::string serialize_dataset(T const & dataset)
	{
		return serialize_cache(dataset);
	}

	// TODO: unit tests / validation
	::std::string get_seedhash(uint64_t const block_number)
	{
		::std::string s(epoch0_seedhash);
		for (size_t i = 0; i < (block_number / constants::EPOCH_LENGTH); i++)
		{
			s = sha3_256_t::serialize(sha3_256(s));
		}
		return s;
	}

	struct cache_t::impl_t
	{
		using size_type = cache_t::size_type;
		using data_type = ::std::vector<::std::vector<int32_t>>;

		impl_t(uint64_t const block_number, ::std::string const & seed, progress_callback_type callback)
		: epoch(block_number / constants::EPOCH_LENGTH)
		, size(get_cache_size(block_number))
		, data()
		{
			mkcache(seed, callback);
		}

		void mkcache(::std::string const & seed, progress_callback_type callback)
		{
			size_t n = size / constants::HASH_BYTES;

			data.reserve(n);
			data.push_back(sha3_512(seed));
			for (size_t i = 1; i < n; i++)
			{
				data.push_back(sha3_512(data.back()));
				if (((i % constants::CALLBACK_FREQUENCY) == 0) && !callback(i, n, cache_seeding))
				{
					throw hash_exception("Cache creation cancelled.");
				}
			}

			size_t progress_counter = 0;
			for (size_t i = 0; i < constants::CACHE_ROUNDS; i++)
			{
				for (size_t j = 0; j < n; j++)
				{
					auto v = data[j][0] % n;
					auto & u = data[(j-1+n)%n];
					size_t count = 0;
					for (auto & k : u)
					{
						k = k ^ data[v][count++];
					}
					data[i] = sha3_512(u);
					if (((i % constants::CALLBACK_FREQUENCY) == 0) && !callback(progress_counter++, n * constants::CACHE_ROUNDS, cache_generation))
					{
						throw hash_exception("Cache creation cancelled.");
					}
				}
			}
		}

		void load(::std::function<bool(void *, size_type)> read, progress_callback_type callback)
		{
			size_type const cache_hash_count = size / constants::HASH_BYTES;

			data.resize(cache_hash_count);
			size_t count = 0;
			for (auto i : data)
			{
				i.resize(constants::HASH_BYTES);
				read(&i[0], constants::HASH_BYTES);
				if (((++count % constants::CALLBACK_FREQUENCY) == 0) && !callback(count, cache_hash_count, cache_loading))
				{
					throw hash_exception("Cache loading cancelled.");
				}
			}
		}

		static size_type get_cache_size(uint64_t block_number) noexcept
		{
			using namespace constants;

			size_type cache_size = (CACHE_BYTES_INIT + (CACHE_BYTES_GROWTH * (block_number / EPOCH_LENGTH))) - HASH_BYTES;
			while (!is_prime(cache_size / HASH_BYTES))
			{
				cache_size -= (2 * HASH_BYTES);
			}
			return cache_size;
		}

		uint64_t epoch;
		size_type size;
		data_type data;
	};

	cache_t::cache_t(uint64_t const block_number, ::std::string const & seed, progress_callback_type callback)
	: impl(new impl_t(block_number, seed, callback))
	{
	}

	uint64_t cache_t::epoch() const
	{
		return impl->epoch;
	}

	cache_t::size_type cache_t::size() const
	{
		return impl->size;
	}

	cache_t::data_type const & cache_t::data() const
	{
		return impl->data;
	}

	void cache_t::load(::std::function<bool(void *, size_type)> read, progress_callback_type callback)
	{
		impl->load(read, callback);
	}

	cache_t::size_type cache_t::get_cache_size(uint64_t const block_number) noexcept
	{
		return impl_t::get_cache_size(block_number);
	}

	struct dag::impl_t
	{
		using size_type = dag::size_type;
		using data_type = ::std::vector<::std::vector<int32_t>>;
		using dag_cache_map = ::std::map<uint64_t /* epoch */, ::std::shared_ptr<impl_t>>;
		static constexpr uint64_t max_epoch = ::std::numeric_limits<uint64_t>::max();

		impl_t(uint64_t block_number, progress_callback_type callback)
		: epoch(block_number / constants::EPOCH_LENGTH)
		, size(get_full_size(block_number))
		, cache(block_number, get_seedhash(block_number), callback)
		, data()
		{
			generate(callback);
		}

		impl_t(::std::string const & file_path, progress_callback_type callback)
		: epoch(max_epoch)
		, size(0)
		, cache(max_epoch, "") // TODO: fixme
		, data()
		{
			load(file_path, callback);
		}

		void save(::std::string const & file_path, progress_callback_type callback) const
		{
			using namespace std;
			ofstream fs;
			fs.open(file_path, ios::out | ios::binary);

			static constexpr uint64_t zero = 0;
			uint64_t cache_begin = constants::DAG_FILE_HEADER_SIZE;
			uint64_t cache_end = cache_begin + cache.size();
			uint64_t dag_begin = cache_end;
			uint64_t dag_end = dag_begin + size;

			auto write = [&fs](void const * data, size_type count) -> bool
			{
				// TODO: write all value in little endian
				fs.write(reinterpret_cast<char const *>(data), count);
				return fs.good();
			};

			fs << constants::DAG_MAGIC_BYTES;
			write(&zero, 1);
			write(&constants::MAJOR_VERSION, sizeof(constants::MAJOR_VERSION));
			write(&constants::REVISION, sizeof(constants::REVISION));
			write(&constants::MINOR_VERSION, sizeof(constants::MINOR_VERSION));
			write(&epoch, sizeof(epoch));
			write(&cache_begin, sizeof(cache_begin));
			write(&cache_end, sizeof(cache_end));
			write(&dag_begin, sizeof(dag_begin));
			write(&dag_end, sizeof(dag_end));
			write(&zero, 1);

			size_t max_count = cache.size() + data.size();
			size_t count = 0;
			for (auto const i : cache.data())
			{
				for (auto const j : i)
				{
					write(&j, sizeof(j));
				}
				if (((++count % constants::CALLBACK_FREQUENCY) == 0) && !callback(count, max_count, dag_saving))
				{
					throw hash_exception("DAG save cancelled.");
				}
			}

			for (auto const i : data)
			{
				for (auto const j : i)
				{
					write(&j, sizeof(j));
				}
				if (((++count % constants::CALLBACK_FREQUENCY) == 0) && !callback(count, max_count, dag_saving))
				{
					throw hash_exception("DAG save cancelled.");
				}
			}

			fs.close();
		}

		void load(::std::string const & file_path, progress_callback_type callback)
		{
			using namespace std;
			ifstream fs;
			fs.open(file_path, ios::in | ios::binary);
			uint64_t unused = 0;

			auto read = [&fs](void * dst, size_type count) -> bool
			{
				// TODO: read values in as little endian
				fs.read(reinterpret_cast<char *>(dst), count);
				return fs.good();
			};

			fs.seekg(0, ios::end);
			auto filesize = fs.tellg();
			fs.seekg(0, ios::beg);

			// check minimum dag size
			if (filesize < 1090516865) // TODO: make a variable for the header size
			{
				throw hash_exception("DAG is corrupt");
			}

			static constexpr size_t magic_size = sizeof(constants::DAG_MAGIC_BYTES);
			char magic[magic_size] = {0};
			read(magic, magic_size - 1);
			read(&unused, 1);

			if (std::string(magic) != constants::DAG_MAGIC_BYTES)
			{
				throw hash_exception("Not a DAG file");
			}

			uint32_t major = 0, revision = 0, minor = 0;
			read(&major, sizeof(major));
			read(&revision, sizeof(revision));
			read(&minor, sizeof(minor));
			if ((major != constants::MAJOR_VERSION) || (revision != constants::REVISION))
			{
				throw hash_exception("DAG version is invalid");
			}

			uint64_t cache_begin = 0, cache_end = 0, dag_begin = 0, dag_end = 0;
			read(&epoch, sizeof(epoch));
			read(&cache_begin, sizeof(cache_begin));
			read(&cache_end, sizeof(cache_end));
			read(&dag_begin, sizeof(dag_begin));
			read(&dag_end, sizeof(dag_end));
			read(&unused, 1);

			// validate size of cache
			cache_t::size_type cache_size = cache_t::get_cache_size((epoch * constants::EPOCH_LENGTH) + 1);
			if ((cache_end <= cache_begin) || (cache_size != (cache_end - cache_begin)) || (cache_end >= static_cast<size_type>(filesize)))
			{
				throw hash_exception("DAG cache is corrupt");
			}

			// validate size of DAG
			size = get_full_size((epoch * constants::EPOCH_LENGTH) + 1); // get the correct dag size
			if ((dag_end <= dag_begin) || (size != (dag_end - dag_begin)) || (dag_end > static_cast<size_type>(filesize)))
			{
				throw hash_exception("DAG is corrupt");
			}

			cache.impl->epoch = epoch;
			cache.impl->size = cache_size;

			// load the cache
			cache.load(read, callback);

			// load the DAG
			size_type dag_hash_count = size / constants::HASH_BYTES;
			data.resize(dag_hash_count);
			size_t count = 0;
			for (auto i : data)
			{
				i.resize(constants::HASH_BYTES / constants::WORD_BYTES);
				read(&i[0], constants::HASH_BYTES);
				if (((++count % constants::CALLBACK_FREQUENCY) == 0) && !callback(count, data.size(), dag_loading))
				{
					throw hash_exception("DAG loading cancelled.");
				}
			}
		}

		void generate(progress_callback_type callback)
		{
			size_type const n = size / constants::HASH_BYTES;
			data.reserve(n);
			for (size_type i = 0; i < n; i++)
			{
				data.push_back(calc_dataset_item(cache.data(), i));
				if ((i % constants::CALLBACK_FREQUENCY) == 0 && !callback(i, n, dag_generation))
				{
					throw hash_exception("DAG creation cancelled.");
				}
			}
		}

		static data_type::value_type calc_dataset_item(::std::vector<sha3_512_t::deserialized_hash_t> const & cache, size_type const i)
		{
			size_type const n = cache.size();
			constexpr size_type r = constants::HASH_BYTES / constants::WORD_BYTES;
			sha3_512_t::deserialized_hash_t mix(cache[i%n]);
			mix[0] ^= i;
			mix = sha3_512(mix);
			for (size_type j = 0; j < constants::DATASET_PARENTS; j++)
			{
				size_type const cache_index = fnv(i ^ j, mix[j % r]);
				auto l = cache[cache_index % n].begin();
				auto lEnd = cache[cache_index % n].end();
				for (auto k = mix.begin(), kEnd = mix.end();
					((k != kEnd) && (l != lEnd)); k++, l++)
				{
					*k = fnv(*k, *l);
				}

			}
			return sha3_512(mix);
		}

		cache_t get_cache() const
		{
			return cache;
		}

		static size_type get_full_size(uint64_t const block_number) noexcept
		{
			using namespace constants;

			uint64_t full_size = (DATASET_BYTES_INIT + (DATASET_BYTES_GROWTH * (block_number / EPOCH_LENGTH))) - MIX_BYTES;
			while (!is_prime(full_size / MIX_BYTES))
			{
				full_size -= (2 * MIX_BYTES);
			}
			return full_size;
		}

		uint64_t epoch;
		size_type size;
		cache_t cache;
		data_type data;
	};

	static dag::impl_t::dag_cache_map dag_cache;
	static ::std::mutex dag_cache_mutex;

	::std::shared_ptr<dag::impl_t> get_dag(uint64_t block_number, progress_callback_type callback)
	{
		using namespace std;
		uint64_t epoch_number = block_number / constants::EPOCH_LENGTH;

		// if we have the correct DAG already loaded, return it from the cache
		{
			lock_guard<mutex> lock(dag_cache_mutex);
			auto const dag_cache_iterator = dag_cache.find(epoch_number);
			if (dag_cache_iterator != dag_cache.end())
			{
				return dag_cache_iterator->second;
			}
		}

		// otherwise create the dag and add it to the cache
		// this is not locked as it can be a lengthy process and we don't want to block access to the dag cache
		shared_ptr<dag::impl_t> impl(new dag::impl_t(block_number, callback));

		lock_guard<mutex> lock(dag_cache_mutex);
		auto insert_pair = dag_cache.insert(make_pair(epoch_number, impl));

		// if insert succeded, return the dag
		if (insert_pair.second)
		{
			return insert_pair.first->second;
		}

		// if insert failed, it's probably already been inserted
		auto const dag_cache_iterator = dag_cache.find(epoch_number);
		if (dag_cache_iterator != dag_cache.end())
		{
			return dag_cache_iterator->second;
		}

		// we couldn't insert it and it's not in the cache
		throw hash_exception("Could not get DAG");
	}

	::std::shared_ptr<dag::impl_t> get_dag(::std::string const & file_path, progress_callback_type callback)
	{
		using namespace std;
		shared_ptr<dag::impl_t> impl;

		// TODO: implement me
		(void)file_path;
		(void)callback;

		return impl;
	}

	dag::dag(uint64_t block_number, progress_callback_type callback)
	: impl(get_dag(block_number, callback))
	{
	}

	dag::dag(::std::string const & file_path, progress_callback_type callback)
	: impl(get_dag(file_path, callback))
	{

	}

	uint64_t dag::epoch() const
	{
		return impl->epoch;
	}

	dag::size_type dag::size() const
	{
		return impl->size;
	}

	dag::data_type const & dag::data() const
	{
		return impl->data;
	}

	void dag::save(::std::string const & file_path, progress_callback_type callback) const
	{
		impl->save(file_path, callback);
	}

	cache_t dag::get_cache() const
	{
		return impl->get_cache();
	}

	dag::size_type dag::get_full_size(uint64_t const block_number) noexcept
	{
		return impl_t::get_full_size(block_number);
	}

	// TODO: unit tests / validation
	result_t hashimoto(sha3_256_t::deserialized_hash_t const & header, uint64_t const nonce, size_t const full_size, ::std::function<sha3_512_t::deserialized_hash_t (size_t const)> lookup)
	{
		auto const n = full_size / constants::HASH_BYTES;
		auto const w = constants::MIX_BYTES / constants::WORD_BYTES;
		auto const mixhashes = constants::MIX_BYTES / constants::HASH_BYTES;

		sha3_256_t::deserialized_hash_t header_seed(header);
		for (size_t i = 0; i < 8; i++)
		{
			// TODO: nonce is big endian, this converts to little endian (do something sensible for big endian)
			header_seed.push_back(reinterpret_cast<uint8_t const *>(&nonce)[7 - i]);
		}
		auto s = sha3_512(header_seed);
		decltype(s) mix;
		for (size_t i = 0; i < (constants::MIX_BYTES / constants::HASH_BYTES); i++)
		{
			mix.insert(mix.end(), s.begin(), s.end());
		}

		for (size_t i = 0; i < constants::ACCESSES; i++)
		{
			auto p = fnv(i ^ s[0], mix[i % w]) % (n / mixhashes) * mixhashes;
			decltype(s) newdata;
			for (size_t j = 0; j < (constants::MIX_BYTES / constants::HASH_BYTES); j++)
			{
				auto const & h = lookup(p + j);
				newdata.insert(newdata.end(), h.begin(), h.end());
			}
			for (auto j = mix.begin(), jEnd = mix.end(), k = newdata.begin(), kEnd = newdata.end(); j != jEnd && k != kEnd; j++, k++)
			{
				*j = fnv(*j, *k);
			}
		}

		decltype(s) cmix;
		for (size_t i = 0; i < mix.size(); i += 4)
		{
			cmix.push_back(fnv(fnv(fnv(mix[i], mix[i+1]), mix[i+2]), mix[i+3]));
		}

		auto v = sha3_256(s);
		result_t out;
		::std::memcpy(&out.value.b[0], &v[0], ::std::min(sizeof(out.value.b), v.size()));
		::std::memcpy(&out.mixhash.b[0], &cmix[0], ::std::min(sizeof(out.mixhash.b), cmix.size()));
		return out;

		//::std::shared_ptr<decltype(s)> shared_mix(::std::make_shared<decltype(s)>(std::move(cmix)));
		//::std::map<::std::string, decltype(shared_mix)> out;
		//out.insert(decltype(out)::value_type(::std::string("mix digest"), shared_mix));
		//s.insert(s.end(), shared_mix->begin(), shared_mix->end());
		//out.insert(decltype(out)::value_type(::std::string("result"), ::std::make_shared<decltype(s)>(sha3_256(s))));
		//return out;
	}

	// TODO: unit tests / validation
	result_t hashimoto_light(size_t const full_size, cache_t const & c, sha3_256_t::deserialized_hash_t const & header, uint64_t const nonce)
	{
		return hashimoto(header, nonce, full_size, [c](size_t const x){return dag::impl_t::calc_dataset_item(c.data(), x);});
	}

	// TODO: unit tests / validation
	result_t hashimoto_full(size_t const full_size, dag const & dataset, sha3_256_t::deserialized_hash_t const & header, uint64_t const nonce)
	{
		return hashimoto(header, nonce, full_size, [dataset](size_t const x){return dataset.data()[x];});
	}

	bool test_function()
	{
		using namespace std;

		string base_str("this is some test data to be hashed. ");
		string input_str(base_str);
		vector<unique_ptr<sha3_512_t>> sixtyfour_results;
		vector<unique_ptr<sha3_256_t>> thirtytwo_results;

		for (size_t i = 0; i < 10; i++)
		{
			input_str += base_str;
			// TODO: make_unique requires c++14
			//sixtyfour_results.push_back(make_unique<sha3_512_t>(input_str));
			//thirtytwo_results.push_back(make_unique<sha3_256_t>(input_str));
		}

		// compare to known values
		static vector<string> const sixtyfour_expected = {
			"24f586494157502950fdd5097f77f7c7e9246744a155f75cfa6a80f23a1819e57eccdba39955869a8fb3a30a3536b5f9602b40c1660c446749a8b56f2649142c",
			"a8d1f26010dd21fb82f1ba96e04dd6d31ecd67cb8f1a2154a39372b3a195a91ee01006f723da488dc12e49c499d63828d1ff9f5f8bfe64084191865151616eaa",
			"dbe6ead2b1a7ddd74e5de9898e9fa1daad9d754cdb407b9a5682d2a9dffe4cd3fa9c86426f2d76b8f8ba176e5b1cc260ebca4ce4d9bd50e9d547a322de58c3ec",
			"b8c51bc171966c32bd9f322f2aefdd133bae9b5e562628861f04ddb52461c217ff2bd14dcd40a83e319316b2cae3388116234d195bf77bf19abd5422e2e47d80",
			"e0e85917f2c04543a302454a66bca5ce3520c313d6a8c88d6aec7e5720a7552a083c035cca96063dd67af9c4288a9e27c4ee0a519a17adec5ba83234a0bc059c",
			"5631d064b0f51bcfbea77eed49961776f13ca8e0ef42795ad66fc6928c59b25975b40fee8fc058a8ddd11152632b0047dcd9d322bd025b03566205bacde57e26",
			"2308c94619f0cfb0cbe1023f117b39eecbdc00ab4a5d6bc45bde5790b24760afb7962714db71b82539ffe35438419bac0a47daeb12adf4bde061503a080f4786",
			"5d25cd6f9cfd479e806d14012c139ee3f10cb671d909be5b0b17ba95669b298bda865fb343930a694d1010cfeefd07cd3a20f84ed376640a3f77bba77d95bce0",
			"4fa2d31d30a1e2ccb964833be9e7ef678597bebb199a76d99af4d8388a6297d7b77a9e110fceaf8d38c293db9c11ee24912bcef45f947690cf7b1c25aa5bc5f4",
			"0902efb9bb8ba40318beb87fe61a43aab979ccd55bfab832645d9f694527ba47df9c993860fa52a91315827632b42149911e7e5e5d1d927ce071880a10de2d83"
		};

		static vector<string> const thirtytwo_expected = {
			"c238de32a98915279c67528e48e18a96d2fffd7cf889e22ca9054cbcf5d47573",
			"1d28e738c30bca86842d914443590411f32eedd6e21abb0d35c78b570d340396",
			"8bd6726d9a5a9e43b477bc0de67d3d72269dada45385487f2654db94d30714d6",
			"08e2ce62b2949983c8be8e93b01786cbc96ba57cd2c2c1edb4b087c9cfb2f41f",
			"a3190d6ede39a5d157e71881b02ead3e9780b35b0d9effdaf8cc591d29698030",
			"da852b5c560592902bf9a415f57c592ef1464cba02749b2bd0a5f1bff5fb0534",
			"0048b940c000685737a2b6c951680f2afc712d8da11669a741f33692154e373d",
			"60a9b6752eeeee83801d398a95c1509b1dbb8d058cb772614f0bc217f4942590",
			"17c8116db267a04a2bce6462ad8ecabe519686f1b6ad16a5b4554dfde780b609",
			"8fa5343466f7796341d97ff3108eb979858b97fbac73d9bc251257e71854b31f"
		};

		bool success = true;
		#if 0 // don't do anything yet
		for (size_t i = 0; i < sixtyfour_results.size(); i++)
		{
			if (string(*sixtyfour_results[i]) != sixtyfour_expected[i])
			{
				cerr << "sha3_512 tests failed" << endl;
				success = false;
				break;
			}
		}

		for (size_t i = 0; i < thirtytwo_results.size(); i++)
		{
			if (string(*thirtytwo_results[i]) != thirtytwo_expected[i])
			{
				cerr << "sha3_256 tests failed" << endl;
				success = false;
				break;
			}
		}

		cout << "hash is " << std::string(*sixtyfour_results[0]) << endl;
		auto h = sixtyfour_results[0]->deserialize();
		cout << "deserialized is ";
		for (auto i = h.begin(), iEnd = h.end(); i != iEnd; i++)
		{
			cout << ::std::hex << ::std::setw(2) << ::std::setfill('0') << *i;
		}
		cout << endl;

		cout << "encode_int(41) == " << encode_int(41) << endl;
		vector<int32_t> v = {0x41, 0x42};
		cout << "serialize_hash({41, 42}) == " << sha3_512_t::serialize(v) << endl;
		if (success)
		{
			cout << dec << "all tests passed" << endl;
		}
		#endif

		auto progress = [](::std::size_t step, ::std::size_t max, int phase) -> bool
		{
			switch(phase)
			{
				case cache_seeding:
					cout << "\rSeeding cache...";
					break;
				case cache_generation:
					cout << "\rGenerating cache...";
					break;
				case cache_saving:
					cout << "\rSaving cache...";
					break;
				case cache_loading:
					cout << "\rLoading cache...";
					break;
				case dag_generation:
					cout << "\rGenerating DAG...";
					break;
				case dag_saving:
					cout << "\rSaving DAG...";
					break;
				case dag_loading:
					cout << "\rLoading DAG...";
					break;
				default:
					break;
			}
			cout << fixed << setprecision(2)
			<< static_cast<double>(step) / static_cast<double>(max) * 100.0 << "%"
			<< setfill(' ') << setw(80) << flush;

			return true;
		};

		dag d(0, progress);
		cout << endl << "Saving DAG..." << endl;
		d.save("epoch0.dag");

		return success;
	}
}

extern "C"
{
	#if 0 // TODO: FIXME
	struct EGIHASH_NAMESPACE(light)
	{
		unsigned int block_number;
		::egihash::cache_t cache;

		EGIHASH_NAMESPACE(light)(unsigned int block_number)
		: block_number(block_number)
		, cache(block_number, ::egihash::get_seedhash(block_number))
		{

		}

		EGIHASH_NAMESPACE(result_t) compute(EGIHASH_NAMESPACE(h256_t) header_hash, uint64_t nonce)
		{
			// TODO: copy-free version
			EGIHASH_NAMESPACE(result_t) result;
			auto ret = ::egihash::hashimoto_light(get_full_size(block_number), cache.data(), sha3_256_t(header_hash).deserialize(), nonce);
			auto const & val = ret["result"];
			auto const & mix = ret["mix hash"];
			::std::memcpy(result.value.b, &(*val)[0], sizeof(result.value.b));
			::std::memcpy(result.mixhash.b, &(*mix)[0], sizeof(result.mixhash.b));
			return result;
		}
	};

	struct EGIHASH_NAMESPACE(full)
	{
		EGIHASH_NAMESPACE(light_t) light;
		::std::vector<sha3_512_t::deserialized_hash_t> dataset;

		EGIHASH_NAMESPACE(full)(EGIHASH_NAMESPACE(light_t) light, EGIHASH_NAMESPACE(callback) callback)
		: light(light)
		, dataset()//TODO::egihash::calc_dataset(light->cache, get_full_size(light->block_number), callback))
		{
			(void)callback;//TODO
		}

		EGIHASH_NAMESPACE(result_t) compute(EGIHASH_NAMESPACE(h256_t) header_hash, uint64_t nonce)
		{
			// TODO: copy free version
			// TODO: validate memset sizes i.e. min(sizeof(dest), sizeof(src))
			EGIHASH_NAMESPACE(result_t) result;
			auto ret = ::egihash::hashimoto_full(get_full_size(light->block_number), dataset, sha3_256_t(header_hash).deserialize(), nonce);
			auto const & val = ret["result"];
			auto const & mix = ret["mix hash"];
			::std::memcpy(result.value.b, &(*val)[0], sizeof(result.value.b));
			::std::memcpy(result.mixhash.b, &(*mix)[0], sizeof(result.mixhash.b));
			return result;
		}
	};

	EGIHASH_NAMESPACE(light_t) EGIHASH_NAMESPACE(light_new)(unsigned int block_number)
	{
		try
		{
			return new EGIHASH_NAMESPACE(light)(block_number);
		}
		catch (...)
		{
			return 0; // nullptr return indicates error
		}
	}

	EGIHASH_NAMESPACE(result_t) EGIHASH_NAMESPACE(light_compute)(EGIHASH_NAMESPACE(light_t) light, EGIHASH_NAMESPACE(h256_t) header_hash, uint64_t nonce)
	{
		try
		{
			return light->compute(header_hash, nonce);
		}
		catch (...)
		{
			return constants::empty_result; // empty result indicates error
		}
	}

	void EGIHASH_NAMESPACE(light_delete)(EGIHASH_NAMESPACE(light_t) light)
	{
		try
		{
			delete light;
		}
		catch (...)
		{
			// no way to indicate error
		}
	}

	EGIHASH_NAMESPACE(full_t) EGIHASH_NAMESPACE(full_new)(EGIHASH_NAMESPACE(light_t) light, EGIHASH_NAMESPACE(callback) callback)
	{
		try
		{
			return new EGIHASH_NAMESPACE(full)(light, callback);
		}
		catch (...)
		{
			return 0; // nullptr indicates error
		}
	}

	uint64_t EGIHASH_NAMESPACE(full_dag_size)(EGIHASH_NAMESPACE(full_t) full)
	{
		try
		{
			return get_full_size(full->light->block_number);
		}
		catch (...)
		{
			return 0; // zero result indicates error
		}
	}

	void const * EGIHASH_NAMESPACE(full_dag)(EGIHASH_NAMESPACE(full_t) full)
	{
		try
		{
			return &full->dataset[0];
		}
		catch (...)
		{
			return 0; // nullptr indicates error
		}
	}

	EGIHASH_NAMESPACE(result_t) EGIHASH_NAMESPACE(full_compute)(EGIHASH_NAMESPACE(full_t) full, EGIHASH_NAMESPACE(h256_t) header_hash, uint64_t nonce)
	{
		try
		{
			return full->compute(header_hash, nonce);
		}
		catch (...)
		{
			return constants::empty_result; // empty result indicates error
		}
	}

	void EGIHASH_NAMESPACE(full_delete)(EGIHASH_NAMESPACE(full_t) full)
	{
		try
		{
			delete full;
		}
		catch (...)
		{
			// no way to indicate error
		}
	}

	void egihash_h256_compute(EGIHASH_NAMESPACE(h256_t) * output_hash, void * input_data, uint64_t input_size)
	{
		try
		{
			sha3_256_t hash(input_data, input_size);
			::std::memcpy(output_hash->b, hash.data, hash.hash_size);
		}
		catch (...)
		{
			// zero hash data indicates error
			::std::memset(output_hash->b, 0, 32);
		}
	}
	#endif
}
