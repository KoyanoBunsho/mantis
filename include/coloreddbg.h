/*
 * ============================================================================
 *
 *         Author:  Prashant Pandey (), ppandey@cs.stonybrook.edu
 *                  Mike Ferdman (), mferdman@cs.stonybrook.edu
 *                  Jamshed Khan (), jamshed@umd.edu
 *   Organization:  Stony Brook University, University of Maryland
 *
 * ============================================================================
 */

#ifndef _COLORED_DBG_H_
#define _COLORED_DBG_H_

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <chrono>

#include <inttypes.h>

#include "sparsepp/spp.h"
#include "tsl/sparse_map.h"
#include "sdsl/bit_vectors.hpp"
#include "gqf_cpp.h"
#include "gqf/hashutil.h"
#include "common_types.h"
#include "mantisconfig.hpp"

#define MANTIS_DBG_IN_MEMORY (0x01)
#define MANTIS_DBG_ON_DISK (0x02)

typedef sdsl::bit_vector BitVector;
typedef sdsl::rrr_vector<63> BitVectorRRR;

constexpr static uint64_t invalid = std::numeric_limits<uint64_t>::max();
constexpr static uint64_t block_kmer_threshold = 100000000;

struct hash128 {
    uint64_t operator()(const __uint128_t &val128) const {
        __uint128_t val = val128;
        // Using the same seed as we use in k-mer hashing.
        return MurmurHash64A((void *) &val, sizeof(__uint128_t),
                             2038074743);
    }
};

template<class key_obj>
struct Iterator {
    QFi qfi;
    typename key_obj::kmer_t kmer;
    uint32_t id;
    bool do_madvice{false};

    Iterator(uint32_t id, const QF *cqf, bool flag) : id(id), do_madvice(flag) {
        if (qf_iterator_from_position(cqf, &qfi, 0) != QFI_INVALID) {
            get_key();
            if (do_madvice)
                qfi_initial_madvise(&qfi);
        }
    }

    bool next() {
        if (do_madvice) {
            if (qfi_next_madvise(&qfi) == QFI_INVALID) return false;
        } else {
            if (qfi_next(&qfi) == QFI_INVALID) return false;
        }
        get_key();
        return true;
    }

    bool end() const {
        return qfi_end(&qfi);
    }

    bool operator>(const Iterator &rhs) const {
        return key() > rhs.key();
    }

    const typename key_obj::kmer_t &key() const { return kmer; }

private:
    void get_key() {
        uint64_t value, count;
        qfi_get_hash(&qfi, &kmer, &value, &count);
    }
};



template<class key_obj>
struct Minheap_PQ {
    void push(const Iterator<key_obj> &obj) {
        c.emplace_back(obj);
        std::push_heap(c.begin(), c.end(), std::greater<Iterator<key_obj>>());
    }

    void pop() {
        std::pop_heap(c.begin(), c.end(), std::greater<Iterator<key_obj>>());
        c.pop_back();
    }

    void replace_top(const Iterator<key_obj> &obj) {
        c.emplace_back(obj);
        pop();
    }

    Iterator<key_obj> &top() { return c.front(); }

    bool empty() const { return c.empty(); }

private:
    std::vector<Iterator<key_obj>> c;
};

template<typename Key, typename Value>
using cdbg_bv_map_t = spp::sparse_hash_map<Key, Value, hash128>;

using default_cdbg_bv_map_t = cdbg_bv_map_t<__uint128_t,
        std::pair<uint64_t, uint64_t>>;

template<class qf_obj, class key_obj>
class ColoredDbg {
public:
    ColoredDbg(std::string &cqf_file, std::vector<std::string> &eqclass_files,
               std::string &sample_file, int flag);

    ColoredDbg(uint64_t qbits, uint64_t key_bits, enum qf_hashmode hashmode,
               uint32_t seed, std::string &prefix, uint64_t nqf, int flag);

    void build_sampleid_map(qf_obj *incqfs);

    default_cdbg_bv_map_t & construct(qf_obj *incqfs, uint64_t num_kmers);

    void set_console(spdlog::logger *c) { console = c; }

    const CQF<key_obj> *get_cqf(void) const { return &dbg; }

    uint64_t get_num_bitvectors(void) const;

    uint64_t get_num_eqclasses(void) const { return eqclass_map.size(); }

    uint64_t get_num_samples(void) const { return num_samples; }

    std::string get_sample(uint32_t id) const;

    uint32_t seed(void) const { return dbg.seed(); }

    uint64_t range(void) const { return dbg.range(); }

    inline uint64_t get_color_class_per_buffer(void) const { return colorClassPerBuffer; }

    std::vector<uint64_t>
    find_samples(const mantis::QuerySet &kmers);

    std::unordered_map<uint64_t, std::vector<uint64_t>>
    find_samples(const std::unordered_map<mantis::KmerHash, uint64_t> &uniqueKmers);

    void serialize();

    void reinit(default_cdbg_bv_map_t &map);

    void set_flush_eqclass_dist(void) { flush_eqclass_dis = true; }



    // Additional public members required for mantii merge.

    // Checks if all the required data for a mantis index exists at directory 'dir'.
    static bool data_exists(std::string &dir, spdlog::logger *console);

    ColoredDbg() {}

    // Required to load the input CdBGs for mantii merge.
    ColoredDbg(std::string &dir, int flag);

    // Required to instantitate the output CdBG for mantii merge.
    ColoredDbg(ColoredDbg<qf_obj, key_obj> &cdbg1, ColoredDbg<qf_obj, key_obj> &cdbg2,
               std::string &prefix, int flag);

    // Returns the vector of names of all the color-class (bitvector) files.
    inline std::vector<std::string> &get_eq_class_files() { return eqClsFiles; }

    // Returns the number of color-class (bitvector) files.
    inline uint64_t get_eq_class_file_count() { return eqClsFiles.size(); }

    // Returns the collection of BitvectorRRR's (compressed color-classes) of this
    // CdBG.
    std::vector<BitVectorRRR> get_eqclasses() { return eqclasses; }

    // Remove the mantis index residing at directory 'dir'.
    static void remove_index(std::string dir, spdlog::logger *console);

    // Move the mantis index at directory 'source' to directory 'destination'.
    static void move_index(std::string source, std::string destination, spdlog::logger *console);

    // Friend class that merges two mantis indices into one.
    template<typename q_obj, typename k_obj> friend
    class CdBG_Merger;

    //////////// blockedCQF
    ColoredDbg(uint64_t nqf);

    void initializeCQFs(std::string &prefixIn, std::vector<uint32_t>& qbits, uint64_t key_bits, qf_hashmode hashmode,
                        uint32_t seed, uint64_t cnt, int flag);

    std::pair<uint64_t, uint64_t> findMinimizer(const typename key_obj::kmer_t &key, uint64_t k);

    std::vector<uint64_t> divideKmersIntoBlocks();

    cdbg_bv_map_t<__uint128_t, std::pair<uint64_t, uint64_t>> &enumerate_minimizers(qf_obj *incqfs);

    void constructBlockedCQF(qf_obj *incqfs);
    void serializeBlockedCQF();


private:
    // returns true if adding this k-mer increased the number of equivalence
    // classes
    // and false otherwise.

    bool add_colorClass(uint64_t &eq_id, const BitVector &vector);

    bool add_kmer(const typename key_obj::kmer_t &hash, const BitVector &vector);

    void add_bitvector(const BitVector &vector, uint64_t eq_id);

    uint64_t get_next_available_id(void);

    void bv_buffer_serialize();

    void reshuffle_bit_vectors(cdbg_bv_map_t<__uint128_t, std::pair<uint64_t,
            uint64_t>> &map);

    std::unordered_map<uint64_t, std::string> sampleid_map;
    // bit_vector --> <eq_class_id, abundance>
    cdbg_bv_map_t<__uint128_t, std::pair<uint64_t, uint64_t>> eqclass_map;
    CQF<key_obj> dbg;
    std::vector<CQF<key_obj>*> dbgs;
    BitVector bv_buffer;
    std::vector<BitVectorRRR> eqclasses;
    std::string prefix;
    uint64_t num_samples;
    uint64_t num_serializations;
    int dbg_alloc_flag;
    bool flush_eqclass_dis{false};
    std::time_t start_time_;
    spdlog::logger *console;

    uint64_t minlen{8};
    std::vector<uint64_t> minimizerCntr;
    // Maximum number of color-class bitvectors that can be present at the bitvector buffer.
    uint64_t colorClassPerBuffer{mantis::NUM_BV_BUFFER};



    // Additional private members required for mantii merge.

    // Color-class bitvector file names for this CdBG.
    std::vector<std::string> eqClsFiles;

    // Returns the sample-id mapping.
    inline std::unordered_map<uint64_t, std::string> &get_sample_id_map() { return sampleid_map; }

    // Concatenates the sample-id mappings of the CdBG's 'cdbg1' and 'cdbg2' into
    // the sample-id list of this CdBG, in order.
    void concat_sample_id_maps(ColoredDbg<qf_obj, key_obj> &cdbg1, ColoredDbg<qf_obj, key_obj> &cdbg2);
};

template<class T>
class SampleObject {
public:
    SampleObject() : obj(), sample_id(), id(0) {}

    SampleObject(T o, std::string &s = std::string(),
                 uint32_t id = 0) : obj(o), sample_id(s), id(id) {}

    SampleObject(const SampleObject &o) : obj(o.obj),
                                          sample_id(o.sample_id), id(o.id) {}

    T obj;
    std::string sample_id;
    uint32_t id;
};

template<class T>
struct compare {
    bool operator()(const SampleObject<T> &lhs, const SampleObject<T> &rhs) {
        return lhs.obj.key > rhs.obj.key;
    }
};

template<class qf_obj, class key_obj>
inline uint64_t ColoredDbg<qf_obj, key_obj>::get_next_available_id(void) {
    return get_num_eqclasses() + 1;
}

template<class qf_obj, class key_obj>
std::string ColoredDbg<qf_obj, key_obj>::get_sample(uint32_t id) const {
    auto it = sampleid_map.find(id);
    if (it == sampleid_map.end())
        return std::string();
    else
        return it->second;
}

template<class qf_obj, class key_obj>
uint64_t ColoredDbg<qf_obj, key_obj>::get_num_bitvectors(void) const {
    uint64_t total = 0;
    for (uint32_t i = 0; i < num_serializations; i++)
        // total += eqclasses[i].size();
        total += eqclasses[i].size() / num_samples;

    // return total / num_samples;
    return total;
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj,
        key_obj>::reshuffle_bit_vectors(cdbg_bv_map_t<__uint128_t,
        std::pair<uint64_t, uint64_t>> &map) {
    //  BitVector new_bv_buffer(mantis::NUM_BV_BUFFER * num_samples);
    BitVector new_bv_buffer(colorClassPerBuffer * num_samples);
    for (auto &it_input : map) {
        auto it_local = eqclass_map.find(it_input.first);
        if (it_local == eqclass_map.end()) {
            console->error("Can't find the vector hash during shuffling");
            exit(1);
        } else {
            //  assert(it_local->second.first <= mantis::NUM_BV_BUFFER &&
            // 				it_input.second.first <= mantis::NUM_BV_BUFFER);
            assert(it_local->second.first <= colorClassPerBuffer &&
                   it_input.second.first <= colorClassPerBuffer);
            uint64_t src_idx = ((it_local->second.first - 1) * num_samples);
            uint64_t dest_idx = ((it_input.second.first - 1) * num_samples);
            for (uint32_t i = 0; i < num_samples; i++, src_idx++, dest_idx++)
                if (bv_buffer[src_idx])
                    new_bv_buffer[dest_idx] = 1;
        }
    }
    bv_buffer = new_bv_buffer;
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::reinit(cdbg_bv_map_t<__uint128_t,
        std::pair<uint64_t, uint64_t>> &map) {
    // dbg.reset();
   /* uint64_t qbits = log2(dbg.numslots());
    uint64_t keybits = dbg.keybits();
    enum qf_hashmode hashmode = dbg.hash_mode();
    uint64_t seed = dbg.seed();
    dbg.delete_file();
    CQF<key_obj> cqf(qbits, keybits, hashmode, seed, prefix + mantis::CQF_FILE);
    dbg = cqf;

    reshuffle_bit_vectors(map);
    // Check if the current bit vector buffer is full and needs to be serialized.
    // This happens when the sampling phase fills up the bv buffer.
    // if (get_num_eqclasses() % mantis::NUM_BV_BUFFER == 0) {
    if (get_num_eqclasses() % colorClassPerBuffer == 0) {
        // The bit vector buffer is full.
        console->info("Serializing bit vector with {} eq classes.",
                      get_num_eqclasses());
        bv_buffer_serialize();
    }*/
    eqclass_map = map;
}

template<class qf_obj, class key_obj>
bool ColoredDbg<qf_obj, key_obj>::add_colorClass(uint64_t &eq_id, const BitVector &vector) {
    __uint128_t vec_hash = MurmurHash128A((void *) vector.data(),
                                          vector.capacity() / 8, 2038074743,
                                          2038074751);

    auto it = eqclass_map.find(vec_hash);
    bool added_eq_class{false};
    // Find if the eqclass of the kmer is already there.
    // If it is there then increment the abundance.
    // Else create a new eq class.
    if (it == eqclass_map.end()) {
        // eq class is seen for the first time.
        eq_id = get_next_available_id();
        eqclass_map.emplace(std::piecewise_construct,
                            std::forward_as_tuple(vec_hash),
                            std::forward_as_tuple(eq_id, 1));
        add_bitvector(vector, eq_id - 1);
        added_eq_class = true;
    } else { // eq class is seen before so increment the abundance.
        eq_id = it->second.first;
        // with standard map
        it->second.second += 1; // update the abundance.
    }
    return added_eq_class;
}

template<class qf_obj, class key_obj>
bool ColoredDbg<qf_obj, key_obj>::add_kmer(const typename key_obj::kmer_t &key, const BitVector &vector) {
    // A kmer (hash) is seen only once during the merge process.
    // So we insert every kmer in the dbg
    uint64_t eq_id{0};
    bool added_eq_class = add_colorClass(eq_id, vector);

    // check: the k-mer should not already be present.
    uint64_t count = dbg.query(KeyObject(key, 0, eq_id), QF_NO_LOCK |
                                                         QF_KEY_IS_HASH);
    if (count > 0) {
        console->error("K-mer was already present. kmer: {} colorID: {}", key, count);
        exit(1);
    }

    // we use the count to store the eqclass ids
    int ret = dbg.insert(KeyObject(key, 0, eq_id), QF_NO_LOCK | QF_KEY_IS_HASH);
    if (ret == QF_NO_SPACE) {
        // This means that auto_resize failed.
        console->error("The CQF is full and auto resize failed. Please rerun build with a bigger size.");
        exit(1);
    }

    return added_eq_class;
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::add_bitvector(const BitVector &vector,
                                                uint64_t eq_id) {
    // uint64_t start_idx = (eq_id  % mantis::NUM_BV_BUFFER) * num_samples;
    uint64_t start_idx = (eq_id % colorClassPerBuffer) * num_samples;
    for (uint32_t i = 0; i < num_samples / 64 * 64; i += 64)
        bv_buffer.set_int(start_idx + i, vector.get_int(i, 64), 64);
    if (num_samples % 64)
        bv_buffer.set_int(start_idx + num_samples / 64 * 64,
                          vector.get_int(num_samples / 64 * 64, num_samples % 64),
                          num_samples % 64);
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::bv_buffer_serialize() {
    BitVector bv_temp(bv_buffer);
    // if (get_num_eqclasses() % mantis::NUM_BV_BUFFER > 0) {
    if (get_num_eqclasses() % colorClassPerBuffer > 0) {
        // bv_temp.resize((get_num_eqclasses() % mantis::NUM_BV_BUFFER) *
        // 							 num_samples);
        bv_temp.resize((get_num_eqclasses() % colorClassPerBuffer) *
                       num_samples);
    }

    BitVectorRRR final_com_bv(bv_temp);
    std::string bv_file(prefix + std::to_string(num_serializations) + "_" +
                        mantis::EQCLASS_FILE);
    sdsl::store_to_file(final_com_bv, bv_file);
    bv_buffer = BitVector(bv_buffer.bit_size());
    num_serializations++;
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::serialize() {
    // serialize the CQF
    if (dbg_alloc_flag == MANTIS_DBG_IN_MEMORY)
        dbg.serialize(prefix + mantis::CQF_FILE);
    else
        dbg.close();

    // serialize the bv buffer last time if needed
    // if (get_num_eqclasses() % mantis::NUM_BV_BUFFER > 0)
    if (get_num_eqclasses() % colorClassPerBuffer > 0)
        bv_buffer_serialize();

    //serialize the eq class id map
    std::ofstream opfile(prefix + mantis::SAMPLEID_FILE);
    for (auto sample : sampleid_map)
        opfile << sample.first << " " << sample.second << std::endl;
    opfile.close();

    if (flush_eqclass_dis) {
        // dump eq class abundance dist for further analysis.
        std::ofstream tmpfile(prefix + "eqclass_dist.lst");
        for (auto sample : eqclass_map)
            tmpfile << sample.second.first << " " << sample.second.second <<
                    std::endl;
        tmpfile.close();
    }
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::serializeBlockedCQF() {
    for (auto i = 0; i < dbgs.size(); i++) {
        // serialize the CQF
        if (dbg_alloc_flag == MANTIS_DBG_IN_MEMORY)
            dbgs[i]->serialize(prefix + mantis::CQF_FILE + std::to_string(i));
        else
            dbgs[i]->close();
    }
    std::ofstream minfile(prefix + mantis::MINIMIZER_BOUNDARY, std::ios::binary);
    minfile.write(reinterpret_cast<char *>(minimizerCntr.data()), minimizerCntr.size()*sizeof(typename decltype(minimizerCntr)::value_type));
    // serialize the bv buffer last time if needed
    // if (get_num_eqclasses() % mantis::NUM_BV_BUFFER > 0)
    if (get_num_eqclasses() % colorClassPerBuffer > 0)
        bv_buffer_serialize();

    //serialize the eq class id map
    std::ofstream opfile(prefix + mantis::SAMPLEID_FILE);
    for (auto sample : sampleid_map)
        opfile << sample.first << " " << sample.second << std::endl;
    opfile.close();

    if (flush_eqclass_dis) {
        // dump eq class abundance dist for further analysis.
        std::ofstream tmpfile(prefix + "eqclass_dist.lst");
        for (auto sample : eqclass_map)
            tmpfile << sample.second.first << " " << sample.second.second <<
                    std::endl;
        tmpfile.close();
    }
}


template<class qf_obj, class key_obj>
std::vector<uint64_t>
ColoredDbg<qf_obj, key_obj>::find_samples(const mantis::QuerySet &kmers) {
    // Find a list of eq classes and the number of kmers that belong those eq
    // classes.
    std::unordered_map<uint64_t, uint64_t> query_eqclass_map;
    for (auto k : kmers) {
        key_obj key(k, 0, 0);
        uint64_t eqclass = dbg.query(key, 0);
        if (eqclass)
            query_eqclass_map[eqclass] += 1;
    }

    std::vector<uint64_t> sample_map(num_samples, 0);
    for (auto it = query_eqclass_map.begin(); it != query_eqclass_map.end();
         ++it) {
        auto eqclass_id = it->first;
        auto count = it->second;
        // counter starts from 1.
        uint64_t start_idx = (eqclass_id - 1);
        // uint64_t bucket_idx = start_idx / mantis::NUM_BV_BUFFER;
        uint64_t bucket_idx = start_idx / colorClassPerBuffer;
        // uint64_t bucket_offset = (start_idx % mantis::NUM_BV_BUFFER) * num_samples;
        uint64_t bucket_offset = (start_idx % colorClassPerBuffer) * num_samples;
        for (uint32_t w = 0; w <= num_samples / 64; w++) {
            uint64_t len = std::min((uint64_t) 64, num_samples - w * 64);
            uint64_t wrd = eqclasses[bucket_idx].get_int(bucket_offset, len);
            for (uint32_t i = 0, sCntr = w * 64; i < len; i++, sCntr++)
                if ((wrd >> i) & 0x01)
                    sample_map[sCntr] += count;
            bucket_offset += len;
        }
    }
    return sample_map;
}

template<class qf_obj, class key_obj>
std::unordered_map<uint64_t, std::vector<uint64_t>>
ColoredDbg<qf_obj, key_obj>::find_samples(const std::unordered_map<mantis::KmerHash, uint64_t> &uniqueKmers) {
    // Find a list of eq classes and the number of kmers that belong those eq
    // classes.
    std::unordered_map<uint64_t, std::vector<uint64_t>> query_eqclass_map;
    for (auto kv : uniqueKmers) {
        key_obj key(kv.first, 0, 0);
        uint64_t eqclass = dbg.query(key, 0);
        if (eqclass) {
            kv.second = eqclass;
            query_eqclass_map[eqclass] = std::vector<uint64_t>();
        }
    }

    std::vector<uint64_t> sample_map(num_samples, 0);
    for (auto it = query_eqclass_map.begin(); it != query_eqclass_map.end();
         ++it) {
        auto eqclass_id = it->first;
        auto &vec = it->second;
        // counter starts from 1.
        uint64_t start_idx = (eqclass_id - 1);
        // uint64_t bucket_idx = start_idx / mantis::NUM_BV_BUFFER;
        uint64_t bucket_idx = start_idx / colorClassPerBuffer;
        // uint64_t bucket_offset = (start_idx % mantis::NUM_BV_BUFFER) * num_samples;
        uint64_t bucket_offset = (start_idx % colorClassPerBuffer) * num_samples;
        for (uint32_t w = 0; w <= num_samples / 64; w++) {
            uint64_t len = std::min((uint64_t) 64, num_samples - w * 64);
            uint64_t wrd = eqclasses[bucket_idx].get_int(bucket_offset, len);
            for (uint32_t i = 0, sCntr = w * 64; i < len; i++, sCntr++)
                if ((wrd >> i) & 0x01)
                    vec.push_back(sCntr);
            bucket_offset += len;
        }
    }
    return query_eqclass_map;
}

template<class qf_obj, class key_obj>
cdbg_bv_map_t<__uint128_t, std::pair<uint64_t, uint64_t>> &
ColoredDbg<qf_obj, key_obj>::construct(qf_obj *incqfs, uint64_t num_kmers) {
    uint64_t counter = 0;
    bool is_sampling = (num_kmers < invalid);

    typename CQF<key_obj>::Iterator walk_behind_iterator;

    Minheap_PQ<key_obj> minheap;

    for (uint32_t i = 0; i < num_samples; i++) {
        Iterator<key_obj> qfi(i, incqfs[i].obj->get_cqf(), true);
        if (qfi.end()) continue;
        minheap.push(qfi);
    }

    while (!minheap.empty()) {
        BitVector eq_class(num_samples);
        KeyObject::kmer_t last_key;
        do {
            Iterator<key_obj> &cur = minheap.top();
            last_key = cur.key();
            eq_class[cur.id] = 1;
            if (cur.next())
                minheap.replace_top(cur);
            else
                minheap.pop();
        } while (!minheap.empty() && last_key == minheap.top().key());
        bool added_eq_class = add_kmer(last_key, eq_class);
        ++counter;

        if (counter == 4096) {
            walk_behind_iterator = dbg.begin(true);
        } else if (counter > 4096) {
            ++walk_behind_iterator;
        }

        // Progress tracker
        static uint64_t last_size = 0;
        if (dbg.dist_elts() % 10000000 == 0 &&
            dbg.dist_elts() != last_size) {
            last_size = dbg.dist_elts();
            console->info("Kmers merged: {}  Num eq classes: {}  Total time: {}",
                          dbg.dist_elts(), get_num_eqclasses(), time(nullptr) -
                                                                start_time_);
        }

        // Check if the bit vector buffer is full and needs to be serialized.
        // if (added_eq_class and (get_num_eqclasses() % mantis::NUM_BV_BUFFER == 0))
        if (added_eq_class and (get_num_eqclasses() % colorClassPerBuffer == 0)) {
            // Check if the process is in the sampling phase.
            if (is_sampling) {
                break;
            } else {
                // The bit vector buffer is full.
                console->info("Serializing bit vector with {} eq classes.",
                              get_num_eqclasses());
                bv_buffer_serialize();
            }
        } else if (counter > num_kmers) {
            // Check if the sampling phase is finished based on the number of k-mers.
            break;
        }

        //while(!minheap.empty() && minheap.top().end()) minheap.pop();
    }
    return eqclass_map;
}

template<class qf_obj, class key_obj>
std::pair<uint64_t, uint64_t> ColoredDbg<qf_obj, key_obj>::findMinimizer(const typename key_obj::kmer_t &key, uint64_t k) {
//    uint64_t k = dbgs[0]->keybits();
    uint64_t j = minlen * 2;
    uint64_t jmask = (1 << j) - 1;
    uint64_t min = invalid;
    uint64_t first{min}, last{min}, second_min{min};
    for (uint64_t s = 0; s <= k - j; s += 2) {
//                auto h = hash_64(key >> s, jmask);
        auto h = (key >> s) & jmask;
        second_min = (h < second_min and h > min) ? h : second_min;
        min = min <= h ? min : h;
        if (s == 0) {
            first = h;
        } else if (s == k - j) {
            last = h;
        }
    }
    if (min != first and min != last) {
        second_min = invalid;
    }
    return std::make_pair(min, second_min);
}

template<class qf_obj, class key_obj>
cdbg_bv_map_t<__uint128_t, std::pair<uint64_t, uint64_t>> &
ColoredDbg<qf_obj, key_obj>::enumerate_minimizers(qf_obj *incqfs) {
    typename CQF<key_obj>::Iterator walk_behind_iterator;

    minimizerCntr.resize(1ULL << (minlen * 2)); // does it also zero out the cells?

    uint64_t counter = 0;
    Minheap_PQ<key_obj> minheap;

    uint64_t k = incqfs[0].obj->get_cqf()->metadata->key_bits;
    for (uint32_t i = 0; i < num_samples; i++) {
        Iterator<key_obj> qfi(i, incqfs[i].obj->get_cqf(), true);
        if (qfi.end()) continue;
        minheap.push(qfi);
    }

    while (!minheap.empty()) {
        BitVector eq_class(num_samples);
        KeyObject::kmer_t last_key;
        // enumerate all the samples containing the kmer and create the color bv
        do {
            Iterator<key_obj> &cur = minheap.top();
            last_key = cur.key();
            eq_class[cur.id] = 1;
            if (cur.next())
                minheap.replace_top(cur);
            else
                minheap.pop();
        } while (!minheap.empty() && last_key == minheap.top().key());
        auto first_second_minimizers = findMinimizer(last_key, k);
        if (first_second_minimizers.first == invalid) {
            console->error("K-mer and therefore the minimizer of the k-mer are invalid.");
            std::exit(3);
        }
        minimizerCntr[first_second_minimizers.first]++;
        if (first_second_minimizers.second != invalid) {
            minimizerCntr[first_second_minimizers.second]++;
        }
        // main functionality --> add colorClass
        uint64_t eq_id{0};
        bool added_eq_class = add_colorClass(eq_id, eq_class);
        ++counter;

        // ?
        if (counter == 4096) {
            walk_behind_iterator = dbg.begin(true);
        } else if (counter > 4096) {
            ++walk_behind_iterator;
        }
        // Progress tracker
        if (counter % 10000000 == 0) {
            console->info("Kmers enumerated: {} Total time: {}",
                          counter, time(nullptr) - start_time_);
        }

        // Check if the bit vector buffer is full and needs to be serialized.
        if (added_eq_class and (get_num_eqclasses() % colorClassPerBuffer == 0)) {
            console->info("Serializing bit vector with {} eq classes.", get_num_eqclasses());
            bv_buffer_serialize();
        }
    }
    return eqclass_map;
}

template<class qf_obj, class key_obj>
std::vector<uint64_t> ColoredDbg<qf_obj, key_obj>::divideKmersIntoBlocks() {
    uint64_t blockCnt{minimizerCntr[0]}, block{0};
    std::vector<uint64_t> blockKmerCount;
    for (auto i = 1; i < minimizerCntr.size(); i++) {
        minimizerCntr[i - 1] = block;
        if ((blockCnt + minimizerCntr[i]) > block_kmer_threshold) {
            block++;
            blockKmerCount.push_back(blockCnt);
            blockCnt = 0;
        }
        blockCnt += minimizerCntr[i];
    }
    blockKmerCount.push_back(blockCnt);
    minimizerCntr[minimizerCntr.size() - 1] = block;
    return blockKmerCount;
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::constructBlockedCQF(qf_obj *incqfs) {
    uint64_t counter = 0;
    Minheap_PQ<key_obj> minheap;
    std::cerr << "# of dbgs: " << dbgs.size() << "\n";
    uint64_t k = incqfs[0].obj->get_cqf()->metadata->key_bits;
    std::cerr << "k: " << k << "\n";
    for (uint32_t i = 0; i < num_samples; i++) {
        Iterator<key_obj> qfi(i, incqfs[i].obj->get_cqf(), true);
        if (qfi.end()) continue;
        minheap.push(qfi);
    }

    while (!minheap.empty()) {
        BitVector eq_class(num_samples);
        KeyObject::kmer_t last_key;
        do {
            Iterator<key_obj> &cur = minheap.top();
            last_key = cur.key();
            eq_class[cur.id] = 1;
            if (cur.next())
                minheap.replace_top(cur);
            else
                minheap.pop();
        } while (!minheap.empty() && last_key == minheap.top().key());

        auto minimizerPair = findMinimizer(last_key, k);
        auto minimizer = minimizerPair.first;
        auto secondMinimizer = minimizerPair.second;
        uint64_t eq_id{0};
        bool added_eq_class = add_colorClass(eq_id, eq_class);
        if (added_eq_class) {
            console->error("Found a new color class in the second round.");
            std::exit(1);
        }
        // check: the k-mer should not already be present.
        uint64_t count = dbgs[minimizerCntr[minimizer]]->query(KeyObject(last_key, 0, eq_id), QF_NO_LOCK |
                                                             QF_KEY_IS_HASH);
        if (count > 0) {
            console->error("K-mer was already present. kmer: {} colorID: {}", last_key, count);
            exit(1);
        }
        // we use the count to store the eqclass ids
        int ret = dbgs[minimizerCntr[minimizer]]->insert(KeyObject(last_key, 0, eq_id), QF_NO_LOCK | QF_KEY_IS_HASH);
        if (ret == QF_NO_SPACE) {
            // This means that auto_resize failed.
            console->error("The CQF is full and auto resize failed. Please rerun build with a bigger size.");
            exit(1);
        }

        if (secondMinimizer != invalid and minimizerCntr[secondMinimizer] != minimizerCntr[minimizer]) {

            uint64_t count = dbgs[minimizerCntr[secondMinimizer]]->query(KeyObject(last_key, 0, eq_id), QF_NO_LOCK |
                                                                                                 QF_KEY_IS_HASH);
            if (count > 0) {
                console->error("K-mer was already present. kmer: {} colorID: {}", last_key, count);
                exit(1);
            }
            // we use the count to store the eqclass ids
            int ret = dbgs[minimizerCntr[secondMinimizer]]->insert(KeyObject(last_key, 0, eq_id), QF_NO_LOCK | QF_KEY_IS_HASH);
            if (ret == QF_NO_SPACE) {
                // This means that auto_resize failed.
                console->error("The CQF is full and auto resize failed. Please rerun build with a bigger size.");
                exit(1);
            }
        }
        ++counter;

        // Progress tracker
        static uint64_t last_size = 0;
        if (counter % 10000000 == 0 && counter != last_size) {
            last_size = counter;
            console->info("Kmers merged: {}  Total time: {}",
                          counter, time(nullptr) - start_time_);
        }
    }
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::build_sampleid_map(qf_obj *incqfs) {
    for (uint32_t i = 0; i < num_samples; i++) {
        std::pair<uint32_t, std::string> pair(incqfs[i].id, incqfs[i].sample_id);
        sampleid_map.insert(pair);
    }
}

template<class qf_obj, class key_obj>
ColoredDbg<qf_obj, key_obj>::ColoredDbg(uint64_t qbits, uint64_t key_bits,
                                        enum qf_hashmode hashmode,
                                        uint32_t seed, std::string &prefix,
                                        uint64_t nqf, int flag) :
// bv_buffer(mantis::NUM_BV_BUFFER * nqf), prefix(prefix), num_samples(nqf),
// num_serializations(0), start_time_(std::time(nullptr)) {
        prefix(prefix), num_samples(nqf), num_serializations(0), start_time_(std::time(nullptr)) {
    colorClassPerBuffer = mantis::BV_BUF_LEN / num_samples;
    bv_buffer = BitVector(colorClassPerBuffer * num_samples);

    if (flag == MANTIS_DBG_IN_MEMORY) {
        CQF<key_obj> cqf(qbits, key_bits, hashmode, seed);
        dbg = cqf;
        dbg_alloc_flag = MANTIS_DBG_IN_MEMORY;
    } else if (flag == MANTIS_DBG_ON_DISK) {
        CQF<key_obj> cqf(qbits, key_bits, hashmode, seed, prefix +
                                                          mantis::CQF_FILE);
        dbg = cqf;
        dbg_alloc_flag = MANTIS_DBG_ON_DISK;
    } else {
        ERROR("Wrong Mantis alloc mode.");
        exit(EXIT_FAILURE);
    }
    dbg.set_auto_resize();
}

template<class qf_obj, class key_obj>
ColoredDbg<qf_obj, key_obj>::ColoredDbg(std::string &cqf_file,
                                        std::vector<std::string> &
                                        eqclass_files, std::string &
sample_file, int flag) : bv_buffer(),
                         start_time_(std::time(nullptr)) {
    num_samples = 0;
    num_serializations = 0;

    if (flag == MANTIS_DBG_IN_MEMORY) {
        CQF<key_obj> cqf(cqf_file, CQF_FREAD);
        dbg = cqf;
        dbg_alloc_flag = MANTIS_DBG_IN_MEMORY;
    } else if (flag == MANTIS_DBG_ON_DISK) {
        CQF<key_obj> cqf(cqf_file, CQF_MMAP);
        dbg = cqf;
        dbg_alloc_flag = MANTIS_DBG_ON_DISK;
    } else {
        ERROR("Wrong Mantis alloc mode.");
        exit(EXIT_FAILURE);
    }

    std::map<int, std::string> sorted_files;
    for (std::string file : eqclass_files) {
        int id = std::stoi(first_part(last_part(file, '/'), '_'));
        sorted_files[id] = file;
    }

    eqclasses.reserve(sorted_files.size());
    BitVectorRRR bv;
    for (auto file : sorted_files) {
        sdsl::load_from_file(bv, file.second);
        eqclasses.push_back(bv);
        num_serializations++;
    }

    std::ifstream sampleid(sample_file.c_str());
    std::string sample;
    uint32_t id;
    while (sampleid >> id >> sample) {
        std::pair<uint32_t, std::string> pair(id, sample);
        sampleid_map.insert(pair);
        num_samples++;
    }

    colorClassPerBuffer = mantis::BV_BUF_LEN / num_samples;

    sampleid.close();
}


template<typename qf_obj, typename key_obj>
ColoredDbg<qf_obj, key_obj>::ColoredDbg(std::string &dir, int flag):
        bv_buffer(),
        prefix(dir),
        num_samples(0),
        num_serializations(0),
        dbg_alloc_flag(flag),
        start_time_(std::time(nullptr)) {
    std::string cqfFile(dir + mantis::CQF_FILE);
    std::string sampleListFile(dir + mantis::SAMPLEID_FILE);
    std::vector<std::string> colorClassFiles = mantis::fs::GetFilesExt(dir.c_str(), mantis::EQCLASS_FILE);


    // Load the CQF
    if (dbg_alloc_flag == MANTIS_DBG_IN_MEMORY) {
        CQF<key_obj> cqf(cqfFile, CQF_FREAD);
        dbg = cqf;
    } else if (dbg_alloc_flag == MANTIS_DBG_ON_DISK) {
        CQF<key_obj> cqf(cqfFile, CQF_MMAP);
        dbg = cqf;
    } else {
        ERROR("Wrong Mantis alloc mode.");
        exit(EXIT_FAILURE);
    }


    // Load the sample / experiment names.
    std::ifstream sampleList(sampleListFile.c_str());
    std::string sampleName;
    uint32_t sampleID;

    while (sampleList >> sampleID >> sampleName) {
        sampleid_map[sampleID] = sampleName;
        num_samples++;
    }

    colorClassPerBuffer = mantis::BV_BUF_LEN / num_samples;

    sampleList.close();


    // Load the color-class bitvector file names only.
    std::map<uint, std::string> sortedFiles;
    for (std::string file : colorClassFiles) {
        uint fileID = std::stoi(first_part(last_part(file, '/'), '_'));
        sortedFiles[fileID] = file;
    }


    // Store the color-class files names in sorted order (based on their sequence).
    eqClsFiles.reserve(colorClassFiles.size());
    for (auto idFilePair : sortedFiles)
        eqClsFiles.push_back(idFilePair.second);

    num_serializations = eqClsFiles.size();
}


template<typename qf_obj, typename key_obj>
ColoredDbg<qf_obj, key_obj>::
ColoredDbg(ColoredDbg<qf_obj, key_obj> &cdbg1, ColoredDbg<qf_obj, key_obj> &cdbg2, std::string &prefix,
           int flag):
        bv_buffer(),
        prefix(prefix),
        num_samples(cdbg1.get_num_samples() + cdbg2.get_num_samples()),
        num_serializations(0),
        dbg_alloc_flag(flag),
        start_time_(std::time(nullptr)) {
    colorClassPerBuffer = mantis::BV_BUF_LEN / num_samples;

    // Construct the sample-id list.
    concat_sample_id_maps(cdbg1, cdbg2);
}

template<class qf_obj, class key_obj>
ColoredDbg<qf_obj, key_obj>::ColoredDbg(uint64_t nqf) :
        num_samples(nqf), num_serializations(0), start_time_(std::time(nullptr)) {
    colorClassPerBuffer = mantis::BV_BUF_LEN / num_samples;
    bv_buffer = BitVector(colorClassPerBuffer * num_samples);
}

template<class qf_obj, class key_obj>
void ColoredDbg<qf_obj, key_obj>::initializeCQFs(std::string &prefixIn, std::vector<uint32_t>& qbits, uint64_t key_bits,
                                                 qf_hashmode hashmode, uint32_t seed,
                                                 uint64_t cnt, int flag) {
    prefix = prefixIn;
    dbgs.resize(cnt);
    for (auto i = 0; i < cnt; i++) {
        if (flag == MANTIS_DBG_IN_MEMORY) {
            dbgs[i] = new CQF<key_obj>(qbits[i], key_bits, hashmode, seed);
            dbg_alloc_flag = MANTIS_DBG_IN_MEMORY;
        } else if (flag == MANTIS_DBG_ON_DISK) {
            std::cerr << "qbits: " << qbits[i] << "\n";
            dbgs[i] = new CQF<key_obj>(qbits[i], key_bits, hashmode, seed, prefix + mantis::CQF_FILE + std::to_string(i));
            dbg_alloc_flag = MANTIS_DBG_ON_DISK;
        } else {
            ERROR("Wrong Mantis alloc mode.");
            exit(EXIT_FAILURE);
        }
        dbgs[i]->set_auto_resize();
    }
}

template<typename qf_obj, typename key_obj>
void ColoredDbg<qf_obj, key_obj>::concat_sample_id_maps(ColoredDbg<qf_obj, key_obj> &cdbg1,
                                                        ColoredDbg<qf_obj, key_obj> &cdbg2) {
    for (auto idSample : cdbg1.get_sample_id_map())
        sampleid_map[idSample.first] = idSample.second;

    for (auto idSample : cdbg2.get_sample_id_map())
        sampleid_map[cdbg1.get_num_samples() + idSample.first] = idSample.second;
}


template<typename qf_obj, typename key_obj>
bool ColoredDbg<qf_obj, key_obj>::data_exists(std::string &dir, spdlog::logger *console) {
    if (!mantis::fs::FileExists((dir + mantis::CQF_FILE).c_str())) {
        console->error("CQF file {} does not exist in input directory {}.", mantis::CQF_FILE, dir);
        return false;
    }

    if (!mantis::fs::FileExists((dir + mantis::SAMPLEID_FILE).c_str())) {
        console->error("Sample-ID list file {} does not exist in input directory {}.",
                       mantis::SAMPLEID_FILE, dir);
        return false;
    }

    /*if(mantis::fs::GetFilesExt(dir.c_str(), mantis::EQCLASS_FILE).empty())
    {
        console -> error("No equivalence-class file with extension {} exists in input directory {}.",
                        mantis::EQCLASS_FILE, dir);
        return false;
    }*/


    return true;
}


template<typename qf_obj, typename key_obj>
void ColoredDbg<qf_obj, key_obj>::remove_index(std::string dir, spdlog::logger *console) {
    if (!mantis::fs::FileExists((dir + mantis::CQF_FILE).c_str()))
        console->error("CQF file {} does not exist in directory {}.", mantis::CQF_FILE, dir);
    else if (remove((dir + mantis::CQF_FILE).c_str()) != 0) {
        console->error("File deletion of {} failed.", dir + mantis::CQF_FILE);
        exit(1);
    } else
        console->info("CQF file {} successfully deleted.", dir + mantis::CQF_FILE);


    if (!mantis::fs::FileExists((dir + mantis::SAMPLEID_FILE).c_str()))
        console->error("Sample-ID list file {} does not exist in directory {}.",
                       mantis::SAMPLEID_FILE, dir);
    else if (remove((dir + mantis::SAMPLEID_FILE).c_str()) != 0) {
        console->error("Sample-ID list file deletion of {} failed.", dir + mantis::SAMPLEID_FILE);
        exit(1);
    } else
        console->info("File {} successfully deleted.", dir + mantis::SAMPLEID_FILE);


    auto eqclassFiles = mantis::fs::GetFilesExt(dir.c_str(), mantis::EQCLASS_FILE);
    if (eqclassFiles.empty())
        console->error("No equivalence-class file with extension {} exists in directory {}.",
                       mantis::EQCLASS_FILE, dir);
    else {
        for (auto p = eqclassFiles.begin(); p != eqclassFiles.end(); ++p)
            if (remove(p->c_str()) != 0) {
                console->error("File deletion of {} failed.", *p);
                exit(1);
            }

        console->info("Color-class bitvector files with extension {} at directory {} successfully deleted.",
                      mantis::EQCLASS_FILE, dir);
    }


    if (mantis::fs::FileExists((dir + mantis::meta_file_name).c_str()) &&
        remove((dir + mantis::meta_file_name).c_str()) != 0) {
        console->error("File deletion of {} failed.",
                       dir + mantis::meta_file_name);
        exit(1);
    }

    if (rmdir(dir.c_str()) == -1) {
        console->error("Cannot remove directory {}.", dir);
        exit(1);
    }
}


template<typename qf_obj, typename key_obj>
void ColoredDbg<qf_obj, key_obj>::move_index(std::string source, std::string destination, spdlog::logger *console) {
    if (destination.back() != '/')
        destination += '/'; // Make sure it is a full directory.

    if (!mantis::fs::DirExists(destination.c_str()))
        mantis::fs::MakeDir(destination.c_str());

    if (!mantis::fs::DirExists(destination.c_str())) {
        console->error("Directory {} could not be created.", destination);
        exit(1);
    }


    console->info("Directory {} created.", destination);

    if (rename((source + mantis::CQF_FILE).c_str(), (destination + mantis::CQF_FILE).c_str()) != 0) {
        console->error("Moving CQF file {} to directory {} failed.", source + mantis::CQF_FILE, destination);
        exit(1);
    }

    if (rename((source + mantis::SAMPLEID_FILE).c_str(), (destination + mantis::SAMPLEID_FILE).c_str()) != 0) {
        console->error("Moving sample-id list file {} to directory {} failed.",
                       source + mantis::SAMPLEID_FILE, destination);
        exit(1);
    }


    // std::vector<std::string> colorClassFiles = mantis::fs::GetFilesExt(source.c_str(), mantis::EQCLASS_FILE);
    // std::map<uint, std::string> sortedFiles;

    // for (std::string file : colorClassFiles)
    // {
    // 	uint fileID = std::stoi(first_part(last_part(file, '/'), '_'));
    // 	sortedFiles[fileID] = file;
    // }

    // colorClassFiles.clear();
    // for(auto idFilePair : sortedFiles)
    // 	colorClassFiles.push_back(idFilePair.second);

    // for(uint i = 0; i < colorClassFiles.size(); ++i)
    // 	if(rename(colorClassFiles[i].c_str(),
    // 				(destination + std::to_string(i) + "_" + mantis::EQCLASS_FILE).c_str()) != 0)
    // 	{
    // 		console -> error("Moving color-class bitvector file {} to directory {} failed.",
    // 						colorClassFiles[i], destination);
    // 		exit(1);
    // 	}

    if (rename((source + mantis::PARENTBV_FILE).c_str(), (destination + mantis::PARENTBV_FILE).c_str()) != 0) {
        console->error("Moving parent-bv file {} to directory {} failed.",
                       source + mantis::PARENTBV_FILE, destination);
        exit(1);
    }


    if (rename((source + mantis::BOUNDARYBV_FILE).c_str(), (destination + mantis::BOUNDARYBV_FILE).c_str()) != 0) {
        console->error("Moving boundary-bv file {} to directory {} failed.",
                       source + mantis::BOUNDARYBV_FILE, destination);
        exit(1);
    }


    if (rename((source + mantis::DELTABV_FILE).c_str(), (destination + mantis::DELTABV_FILE).c_str()) != 0) {
        console->error("Moving delta-bv file {} to directory {} failed.",
                       source + mantis::DELTABV_FILE, destination);
        exit(1);
    }


    if (mantis::fs::FileExists((source + mantis::meta_file_name).c_str()) &&
        remove((source + mantis::meta_file_name).c_str()) != 0) {
        console->error("File deletion of {} failed.",
                       source + mantis::meta_file_name);
        exit(1);
    }

    if (mantis::fs::FileExists((source + "newID2oldIDs").c_str()) &&
        remove((source + "newID2oldIDs").c_str()) != 0) {
        console->error("File deletion of {} failed.",
                       source + "newID2oldIDs");
        exit(1);
    }

    if (rmdir(source.c_str()) == -1) {
        console->error("Cannot remove directory {}.", source);
        exit(1);
    }
}

#endif