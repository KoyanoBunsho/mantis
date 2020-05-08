//
// Created by Fatemeh Almodaresi on 2018-10-13.
//

#ifndef MANTIS_MST_H
#define MANTIS_MST_H

#include <set>
#include <vector>
#include <queue>
#include <cstdint>
#include <mutex>
#include <thread>

// sparsepp should be included before gqf_cpp! ow, we'll get a conflict in MAGIC_NUMBER
#include "sparsepp/spp.h"

#include "mantisconfig.hpp"
#include "gqf_cpp.h"
#include "spdlog/spdlog.h"

#include "canonicalKmer.h"
#include "sdsl/bit_vectors.hpp"
#include "gqf/hashutil.h"

#include "lru/lru.hpp"
#include "mstQuery.h"


using LRUCacheMap =  LRU::Cache<uint64_t, std::vector<uint64_t>>;

//using LRUCacheMap = HPHP::ConcurrentScalableCache<uint64_t , std::vector<uint64_t >>;

using SpinLockT = std::mutex;

using FilterType = CQF<KeyObject>;

typedef uint32_t colorIdType;
typedef uint32_t weightType;
typedef unsigned __int128 uint128_t;

struct Cost {
    uint64_t numSteps{0};
    uint64_t numQueries{0};
};

// undirected edge
struct Edge {
    colorIdType n1;
    colorIdType n2;

    Edge() : n1{static_cast<colorIdType>(-1)}, n2{static_cast<colorIdType>(-1)} {}

    Edge(colorIdType inN1, colorIdType inN2) : n1(inN1), n2(inN2) {
        if (n1 > n2) {
            std::swap(n1, n2);
        }
    }

    bool operator==(const Edge &e) const {
        return n1 == e.n1 && n2 == e.n2;
    }
};
/*
// note: @fatal: careful! The hash highly depends on the length of the edge ID (uint32)
struct edge_hash {
    uint64_t operator()(const Edge &e) const {
        return MurmurHash64A(&e, sizeof(Edge), 2038074743);
        *//*uint64_t res = e.n1;
        return (res << 32) | (uint64_t)e.n2;*//*
    }
};*/

struct workItem {
    dna::canonical_kmer node;
    colorIdType colorId;

    workItem(dna::canonical_kmer n, colorIdType c) : node(n), colorId(c) {}

    // Required to be able to use it as a key in set
    bool operator<(const workItem &item2) const {
        return (*this).node < item2.node;
    }
};

/*

struct DisjointSetNode {
    colorIdType parent{0};
    uint64_t rnk{0}, w{0}, edges{0};

    void setParent(colorIdType p) { parent = p; }

    void mergeWith(DisjointSetNode &n, uint32_t edgeW, colorIdType id) {
        n.setParent(parent);
        w += (n.w + static_cast<uint64_t>(edgeW));
        edges += (n.edges + 1);
        n.edges = 0;
        n.w = 0;
        if (rnk == n.rnk) {
            rnk++;
        }
    }
};
*/

// To represent Disjoint Sets
struct DisjointSets {
    sdsl::int_vector<> els;
//    std::vector<DisjointSetNode> els; // the size of total number of colors
    uint64_t n;

    // Constructor.
    explicit DisjointSets(uint64_t n) {
        // Allocate memory
        this->n = n;
        els = sdsl::int_vector<>(n, 0, ceil(log2(n))+1); // 1 bit which is set if the node IS its own parent
//        els.resize(n);
        // Initially, all vertices are in
        // different sets and have rank 0.
//        for (uint64_t i = 0; i < n; i++) {
//            //every element is parent of itself
//            els[i].setParent(static_cast<colorIdType>(i));
//        }
    }

    bool selfParent(uint64_t idx) {
        return els[idx] & 1ULL;
    }

    void setParent(uint64_t idx, uint64_t parentIdx) {
        bool ownParent = idx == parentIdx;
        els[idx] = (parentIdx << 1ULL) | static_cast<uint64_t>(ownParent);
    }

    uint64_t getParent(uint64_t idx) {
        if (selfParent(idx))
            return idx;
        return els[idx] >> 1ULL;
    }

    uint64_t getRank(uint64_t idx) {
        uint64_t par = find(idx);
        return els[par] >> 1ULL;
    }

    void incrementRank(uint64_t idx) {
        auto parIdx = find(idx);
        uint64_t rank = els[parIdx] >> 1ULL;
        ++rank;
        els[parIdx] = (rank << 1ULL) & 1ULL; // selfParent bit is set
    }
    // Find the parent of a node 'u'
    // Path Compression
    uint64_t find(uint64_t u) {
        /* Make the parent of the nodes in the path
           from u--> parent[u] point to parent[u] */
        if (not selfParent(u)) {
            setParent(u, find(u));
        }
        return getParent(u);
//        if (u != els[u].parent)
//            els[u].parent = find(els[u].parent);
//        return els[u].parent;
    }

    // Union by rank
    void merge(uint64_t x, uint64_t y, uint32_t edgeW) {
        x = find(x), y = find(y);

        /* Make tree with smaller height
           a subtree of the other tree  */
        if (getRank(x) <= getRank(y)) {
            std::swap(x, y);
            if (getRank(x) == getRank(y)) {
                incrementRank(x);
            }
//            els[x].mergeWith(els[y], edgeW, x);

        }
//        else {// If rnk[x] <= rnk[y]
//            els[y].mergeWith(els[x], edgeW, y);
//        }
        setParent(x, y);
    }
};

class MSTMerger {
public:
    MSTMerger(std::string prefix,
            spdlog::logger *logger,
            uint32_t numThreads,
            std::string prefix1,
            std::string prefix2);

    void mergeMSTs();

private:

    std::pair<uint64_t, uint64_t> buildMultiEdgesFromCQFs();

    bool buildEdgeSets();

    bool calculateMSTBasedWeights();

    bool encodeColorClassUsingMST();

    DisjointSets kruskalMSF();

    std::set<workItem> neighbors(CQF<KeyObject> &cqf, workItem n);

    bool exists(CQF<KeyObject> &cqf, dna::canonical_kmer e, uint64_t &eqid);

    uint64_t mstBasedHammingDist(uint64_t eqid1,
                                 uint64_t eqid2,
                                 MSTQuery *mst,
                                 LRUCacheMap &lru_cache,
                                 std::vector<uint64_t> &srcEq,
                                 QueryStats &queryStats,
                                 std::unordered_map<uint64_t, std::vector<uint64_t>> &fixed_cache);

    void buildPairedColorIdEdgesInParallel(uint32_t threadId, CQF<KeyObject> &cqf,
                                           uint64_t &cnt, uint64_t &maxId, uint64_t &numOfKmers);


    void findNeighborEdges(CQF<KeyObject> &cqf, KeyObject &keyobj, std::vector<Edge> &edgeList);

    void calcMSTHammingDistInParallel(uint32_t i,
                                  std::vector<std::pair<colorIdType, weightType>> &edgeList,
                                  std::vector<uint32_t> &srcStarts,
                                  MSTQuery *mst,
                                  std::vector<LRUCacheMap> &lru_cache,
                                  std::vector<QueryStats> &queryStats,
                                  std::unordered_map<uint64_t, std::vector<uint64_t>> &fixed_cache,
                                  uint32_t numSamples);

    void calcDeltasInParallel(uint32_t threadID, uint64_t cbvID1, uint64_t cbvID2,
                              sdsl::int_vector<> &parentbv, sdsl::int_vector<> &deltabv,
                              sdsl::bit_vector::select_1_type &sbbv,
                              bool isMSTBased);

    void buildMSTBasedColor(uint64_t eqid1, MSTQuery *mst1,
                            LRUCacheMap &lru_cache1, std::vector<uint64_t> &eq1,
                            QueryStats &queryStats,
                            std::unordered_map<uint64_t, std::vector<uint64_t>> &fixed_cache);

    std::vector<uint32_t> getMSTBasedDeltaList(uint64_t eqid1, uint64_t eqid2,
                                               MSTQuery * mstPtr,
                                               std::unordered_map<uint64_t, std::vector<uint64_t>>& fixed_cache,
                                               LRUCacheMap &lru_cache,
                                               QueryStats &queryStats);

    void planCaching(MSTQuery *mst,
                     std::vector<std::pair<colorIdType, weightType>> &edges,
                     std::vector<uint32_t> &srcStartIdx,
                     std::vector<colorIdType> &colorsInCache);

    void planRecursively(uint64_t nodeId,
                         std::vector<std::vector<colorIdType>> &children,
                         std::vector<Cost> &mstCost,
                         std::vector<colorIdType> &colorsInCache,
                         uint64_t &cntr);

    std::string prefix;
    uint32_t numSamples = 0;
    uint32_t numOfFirstMantisSamples = 0;
    uint32_t secondMantisSamples = 0;
    uint64_t k;
    uint64_t numCCPerBuffer;
    uint64_t num_edges = 0;
    uint64_t num_colorClasses = 0;
    uint64_t mstTotalWeight = 0;
    colorIdType zero = static_cast<colorIdType>(UINT64_MAX);
    std::vector<LRUCacheMap> lru_cache1;//10000);
    std::vector<LRUCacheMap> lru_cache2;//10000);
    std::unordered_map<uint64_t, std::vector<uint64_t>> fixed_cache1;
    std::unordered_map<uint64_t, std::vector<uint64_t>> fixed_cache2;
    std::vector<QueryStats> queryStats1;
    std::vector<QueryStats> queryStats2;
    std::vector<std::pair<colorIdType, colorIdType >> colorPairs;
    std::string prefix1;
    std::string prefix2;
    std::unique_ptr<MSTQuery> mst1;
    std::unique_ptr<MSTQuery> mst2;
    std::unique_ptr<std::vector<Edge>> edges;
    std::vector<std::unique_ptr<std::vector<Edge>>> weightBuckets;
    std::unique_ptr<std::vector<std::vector<std::pair<colorIdType, uint32_t> >>> mst;
    spdlog::logger *logger{nullptr};
    uint32_t nThreads = 1;
    SpinLockT colorMutex;

    uint64_t numBlocks;

};

#endif //MANTIS_MST_H
