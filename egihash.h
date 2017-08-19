// Copyright (c) 2017 Ryan Lucchese
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <stdint.h>

#ifdef __cplusplus

#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

namespace egihash
{
	bool test_function();

	/** \brief epoch0_seedhash is is the seed hash for the genesis block and first epoch of the DAG.
	*			All seed hashes for subsequent epochs will be generated from this seedhash.
	*
	*	The epoch0_seedhash should be set to a randomized set of 32 bytes for a given crypto currency.
	*	This represents a keccak-256 hash that will be used as input for building the DAG/cache.
	*/
	// TODO: randomized seedhash not zero seedhash
	static constexpr char epoch0_seedhash[] = "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
	static_assert(sizeof(epoch0_seedhash) == 33, "Invalid seedhash");

	/** \brief get_seedhash(uint64_t) will compute the seedhash for a given block number.
	*
	*	\param block_number An unsigned 64-bit integer representing the block number for which to compute the seed hash.
	*	\return A hexidecimal encoded keccak-256 seed hash for the given block number.
	*/
	::std::string get_seedhash(uint64_t const block_number);

	/** \brief h256_t represents a the result of a keccak-256 hash.
	*/
	struct h256_t
	{
		h256_t();
		// TODO: copy/move ctors/operators & operator bool()
		uint8_t b[32];
	};

	/** \brief result_t represents the result of an egihash.
	*/
	struct result_t
	{
		result_t() = default;
		// TODO: copy/move ctors/operators & operator bool()
		h256_t value;
		h256_t mixhash;
	};

	/** \brief progress_callback_phase values represent different stages at which a progress callback may be called.
	*/
	enum progress_callback_phase
	{
		cache_seeding,		/**< cache_seeding means fill the cache with hashes of seed hashes: { cache_0 = seedhash; cache_n = keccak512(cache_n-1) } */
		cache_generation,	/**< cache_generation is performed with a memory hard hashing function of a seeded cache */
		cache_saving,		/**< cache_saving is saving the cache to disk */
		cache_loading,		/**< cache_loading is loading the cache from disk */
		dag_generation,		/**< dag_generation is computing the DAG for a given epoch (block_number) */
		dag_saving,			/**< dag_saving is saving the DAG to disk */
		dag_loading			/**< dag_loading is loading the DAG from disk */
	};

	/** \brief progress_callback_type is a function which may be passed to any phase of DAG/cache or generation to receive progress updates.
	*
	*	\param step is the count of the step just compeleted before this call of the callback.
	*	\param max is the maximum number of steps that will be performed for this progress_callback_phase.
	*	\param phase the progress_callback_phase indicating what is being computed at the time of the callback.
	*	\return false to cancel whatever action is being performed, true to continue.
	*/
	using progress_callback_type = ::std::function<bool (::std::size_t step, ::std::size_t max, progress_callback_phase phase)>;

	/** \brief cache_t is the cache used to compute a DAG for a given epoch.
	*
	* Each DAG owns a cache_t and the size of the cache grows linearly in time.
	*/
	struct cache_t
	{
		/** \brief size_type represents sizes used by a cache.
		*/
		using size_type = ::std::size_t;

		/** \brief data_type is the underlying data store which stores a cache.
		*/
		using data_type = ::std::vector<::std::vector<int32_t>>;

		/** \brief default copy constructor.
		*/
		cache_t(const cache_t &) = default;

		/** \brief default copy assignment operator.
		*/
		cache_t & operator=(cache_t const &) = default;

		/** \brief default move constructor.
		*/
		cache_t(cache_t &&) = default;

		/** \brief default move assignment operator.
		*/
		cache_t & operator=(cache_t &&) = default;

		/** \brief default destructor.
		*/
		~cache_t() = default;

		/** \brief explicitly deleted default constructor.
		*/
		cache_t() = delete;

		/** \brief Construct a cache_t given a block number, a seed hash, and a progress callback function.
		*
		*	\param block_number is the block number for which this cache_t is to be constructed.
		*	\param seed is the seed hash (i.e. get_seedhash(block_number)) for which this cache_t is to be constructed.
		*	\param callback (optional) may be used to monitor the progress of cache generation. Return false to cancel, true to continue.
		*/
		cache_t(uint64_t block_number, ::std::string const & seed, progress_callback_type callback = [](size_type, size_type, int){ return true; });

		/** \brief Get the epoch number for which this cache is valid.
		*
		*	\returns uint64_t representing the epoch number (block_number / constants::EPOCH_LENGTH)
		*/
		uint64_t epoch() const;

		/** \brief Get the size of the cache data in bytes.
		*
		*	\returns size_type representing the size of the cache data in bytes.
		*/
		size_type size() const;

		/** \brief Get the data the cache contains.
		*
		*	\returns data_type const & to the actual cache data.
		*/
		data_type const & data() const;

		/** \brief Load a cache from disk.
		*
		*	\param read A function which will read cache data from disk.
		*	\param callback (optional) may be used to monitor the progress of cache loading. Return false to cancel, true to continue.
		*/
		void load(::std::function<bool(void *, size_type)> read, progress_callback_type callback = [](size_type, size_type, int){ return true; });

		/** \brief Get the size of the cache data in bytes.
		*
		*	\param block_number is the block number for which cache size to compute.
		*	\returns size_type representing the size of the cache data in bytes.
		*/
		static size_type get_cache_size(uint64_t const block_number) noexcept;

		/** \brief cache_t private implementation.
		*/
		struct impl_t;

		/** \brief shared_ptr to impl allows default moving/copying of cache. Internally, only one cache_t::impl_t per epoch will exist.
		*/
		::std::shared_ptr<impl_t> impl;
	};

	/** \brief dag_t is the DAG which is used by full nodes and miners to compute egihashes.
	*
	*	The DAG gives egihash it's ASIC resistance, ensuring that this hashing function is memory bound not compute bound.
	*	The DAG must be updated once per constants::EPOCH_LENGTH block numbers.
	*	The DAG for epoch 0 is 1073739904 bytes in size and will grow linearly with each following epoch.
	*	The DAG can take a long time to generate. It is recommended to save the DAG to disk to avoid having to regenerate it each time.
	*	It makes sense to pre-compute the DAG for the next epoch, so generating it does not interrupt mining / operation of a full node.
	*	Whether by generation or by loading, the DAG will own a cache_t which corresponds to cache for the same epoch.
	*/
	struct dag_t
	{
		/** \brief size_type represents sizes used by a cache.
		*/
		using size_type = ::std::size_t;

		/** \brief data_type is the underlying data store which stores a cache.
		*/
		using data_type = ::std::vector<::std::vector<int32_t>>;

		/** \brief default copy constructor.
		*/
		dag_t(dag_t const &) = default;

		/** \brief default copy assignment operator.
		*/
		dag_t & operator=(dag_t const &) = default;

		/** \brief default move constructor.
		*/
		dag_t(dag_t &&) = default;

		/** \brief default move assignment operator.
		*/
		dag_t & operator=(dag_t &&) = default;

		/** \brief default destructor.
		*/
		~dag_t() = default;

		/** \brief explicitly deleted default constructor.
		*/
		dag_t() = delete;

		/** \brief generate a DAG for a given block_number.
		*
		*	DAG's are cached in a singleton per epoch. If this DAG is already loaded in memory it will be returned quickly.
		*	If this DAG is not yet loaded, this will take a long time.
		*	\param block_number is the block number for which to generate a DAG.
		*	\param callback (optional) may be used to monitor the progress of DAG generation. Return false to cancel, true to continue.
		*/
		dag_t(uint64_t const block_number, progress_callback_type = [](size_type, size_type, int){ return true; });

		/** \brief load a DAG from a file.
		*
		*	DAG's are cached in a singleton per epoch. If this DAG is already loaded in memory it will be returned quickly.
		*	\param file_path is the path to the file the DAG should be loaded from.
		*	\param callback (optional) may be used to monitor the progress of DAG loading. Return false to cancel, true to continue.
		*/
		dag_t(::std::string const & file_path, progress_callback_type = [](size_type, size_type, int){ return true; });

		/** \brief Get the epoch number for which this DAG is valid.
		*
		*	\returns uint64_t representing the epoch number (block_number / constants::EPOCH_LENGTH)
		*/
		uint64_t epoch() const;

		/** \brief Get the size of the DAG data in bytes.
		*
		*	\returns size_type representing the size of the DAG data in bytes.
		*/
		size_type size() const;

		/** \brief Get the data the DAG contains.
		*
		*	\returns data_type const & to the actual DAG data.
		*/
		data_type const & data() const;

		/** \brief Save the DAG to a file fur future loading.
		*
		*	\param file_path is the path to the file the DAG should be saved to.
		*	\param callback (optional) may be used to monitor the progress of DAG saving. Return false to cancel, true to continue.
		*/
		void save(::std::string const & file_path, progress_callback_type callback = [](size_type, size_type, int){ return true; }) const;

		/** \brief Get the cache for this DAG.
		*
		*	\return cache_t of the cache for this DAG.
		*/
		cache_t get_cache() const;

		/** \brief Get the size of the DAG data in bytes.
		*
		*	\param block_number is the block number for which DAG size to compute.
		*	\return size_type representing the size of the DAG data in bytes.
		*/
		static size_type get_full_size(uint64_t const block_number) noexcept;

		/** \brief dag_t private implementation.
		*/
		struct impl_t;

		/** \brief shared_ptr to impl allows default moving/copying of cache. Internally, only one dag_t::impl_t per epoch will exist.
		*
		*	Since DAGs consume a large amount of memory, it is important that they are cached.
		*/
		::std::shared_ptr<impl_t> impl;
	};
}

extern "C"
{
#endif // __cplusplus

#define EGIHASH_NAMESPACE_PREFIX egihash
#define EGIHASH_CONCAT(x, y) EGIHASH_CONCAT_(x, y)
#define EGIHASH_CONCAT_(x, y) x ## y
#define EGIHASH_NAMESPACE(name) EGIHASH_NAMESPACE_(_ ## name)
#define EGIHASH_NAMESPACE_(name) EGIHASH_CONCAT(EGIHASH_NAMESPACE_PREFIX, name)

typedef int (* EGIHASH_NAMESPACE(callback))(unsigned int);
typedef struct EGIHASH_NAMESPACE(light) * EGIHASH_NAMESPACE(light_t);
typedef struct EGIHASH_NAMESPACE(full) * EGIHASH_NAMESPACE(full_t);
typedef struct EGIHASH_NAMESPACE(h256) { uint8_t b[32]; } EGIHASH_NAMESPACE(h256_t);
typedef struct EGIHASH_NAMESPACE(result) { EGIHASH_NAMESPACE(h256_t) value; EGIHASH_NAMESPACE(h256_t) mixhash; } EGIHASH_NAMESPACE(result_t);

#if 0 // TODO: FIXME
EGIHASH_NAMESPACE(light_t) EGIHASH_NAMESPACE(light_new)(unsigned int block_number);
EGIHASH_NAMESPACE(result_t) EGIHASH_NAMESPACE(light_compute)(EGIHASH_NAMESPACE(light_t) light, EGIHASH_NAMESPACE(h256_t) header_hash, uint64_t nonce);
void EGIHASH_NAMESPACE(light_delete)(EGIHASH_NAMESPACE(light_t) light);

EGIHASH_NAMESPACE(full_t) EGIHASH_NAMESPACE(full_new)(EGIHASH_NAMESPACE(light_t) light, EGIHASH_NAMESPACE(callback) callback);
uint64_t EGIHASH_NAMESPACE(full_dag_size)(EGIHASH_NAMESPACE(full_t) full);
void const * EGIHASH_NAMESPACE(full_dag)(EGIHASH_NAMESPACE(full_t) full);
EGIHASH_NAMESPACE(result_t) EGIHASH_NAMESPACE(full_compute)(EGIHASH_NAMESPACE(full_t) full, EGIHASH_NAMESPACE(h256_t) header_hash, uint64_t nonce);
void EGIHASH_NAMESPACE(full_delete)(EGIHASH_NAMESPACE(full_t) full);

void egihash_h256_compute(EGIHASH_NAMESPACE(h256_t) * output_hash, void * input_data, uint64_t input_size);
#endif

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus
