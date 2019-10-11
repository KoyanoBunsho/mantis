#include <utility>

//
// Created by Fatemeh Almodaresi.
//
#include <string>
#include <sstream>
#include <cstdio>
#include <stdio.h>

#include "MantisFS.h"
#include "mst.h"
#include "ProgOpts.h"

#define MAX_ALLOWED_TMP_EDGES 31250000

MST::MST(std::string prefixIn, std::shared_ptr<spdlog::logger> loggerIn, uint32_t numThreads) :
        prefix(std::move(prefixIn)), lru_cache(10000), nThreads(numThreads) {
    logger = loggerIn.get();

    // Make sure the prefix is a full folder
    if (prefix.back() != '/') {
        prefix.push_back('/');
    }
    // make the output directory if it doesn't exist
    if (!mantis::fs::DirExists(prefix.c_str())) {
        logger->error("Index parent directory {} does not exist", prefix);
        std::exit(1);
    }

    eqclass_files =
            mantis::fs::GetFilesExt(prefix.c_str(), mantis::EQCLASS_FILE);

    // sort eqclass_files
    // note to @robP: It terribly statically relies on the format of the input files!!
    std::sort(eqclass_files.begin(), eqclass_files.end(), [this](std::string &s1, std::string &s2) {
        uint32_t id1, id2;
        std::stringstream ss1(first_part(last_part(s1, '/'), '_'));
        std::stringstream ss2(first_part(last_part(s2, '/'), '_'));
        if ((ss1 >> id1).fail() || !(ss1 >> std::ws).eof() ||
            (ss2 >> id2).fail() || !(ss2 >> std::ws).eof()) {
            logger->error("file name does not start with a number : {}, {}", s1, s2);
        }
        return id1 < id2;
    });

    num_of_ccBuffers = eqclass_files.size();

    std::string sample_file = prefix + mantis::SAMPLEID_FILE;//(prefix.c_str() , mantis::SAMPLEID_FILE);
    std::ifstream sampleid(sample_file);
    std::string tmp;
    while (sampleid >> tmp >> tmp) {
        numSamples++;
    }
    sampleid.close();
    logger->info("# of experiments: {}", numSamples);
}

MST::MST(CQF<KeyObject>* cqfIn, std::string prefixIn, spdlog::logger* loggerIn, uint32_t numThreads,
        std::string prefixIn1, std::string prefixIn2, uint64_t numColorBuffersIn) :
        cqf(cqfIn), prefix(std::move(prefixIn)),
        prefix1(std::move(prefixIn1)), prefix2(std::move(prefixIn2)),
        lru_cache1(1000), lru_cache2(1000), nThreads(numThreads),
        num_of_ccBuffers(numColorBuffersIn){
    eqclass_files.resize(num_of_ccBuffers);
    logger = loggerIn;//.get();

    // Make sure the prefix is a full folder
    if (prefix.back() != '/') {
        prefix.push_back('/');
    }
    if (prefix1.back() != '/') {
        prefix1.push_back('/');
    }
    if (!mantis::fs::DirExists(prefix1.c_str())) {
        logger->error("Index parent directory for first mst, {}, does not exist", prefix1);
        std::exit(1);
    }
    if (prefix2.back() != '/') {
        prefix2.push_back('/');
    }
    if (!mantis::fs::DirExists(prefix2.c_str())) {
        logger->error("Index parent directory for second mst, {}, does not exist", prefix2);
        std::exit(1);
    }
    // make the output directory if it doesn't exist
    /*if (!mantis::fs::DirExists(prefix.c_str())) {
        logger->error("Index parent directory {} does not exist", prefix);
        std::exit(1);
    }
*/

    std::string sample_file = prefix1 + mantis::SAMPLEID_FILE;//(prefix.c_str() , mantis::SAMPLEID_FILE);
    std::ifstream sampleid(sample_file);
    std::string tmp;
    while (sampleid >> tmp >> tmp) {
        numSamples++;
    }
    sampleid.close();
    numOfFirstMantisSamples = numSamples; // this line is important!

    sample_file = prefix2 + mantis::SAMPLEID_FILE;//(prefix.c_str() , mantis::SAMPLEID_FILE);
    sampleid.open(sample_file);
    while (sampleid >> tmp >> tmp) {
        numSamples++;
    }
    sampleid.close();

    logger->info("# of experiments: {}", numSamples);
    logger->info("{}, {}, {}, {}", numThreads, num_of_ccBuffers, num_colorClasses, num_edges);
}

/**
 * Builds an MST consists of 3 main steps:
 * 1. construct the color graph for all the colorIds derived from dbg
 *      This phase just requires loading the CQF
 * 2. calculate the weights of edges in the color graph
 *      This phase requires at most two buffers of color classes
 * 3. find MST of the weighted color graph
 */
void MST::buildMST() {
    buildEdgeSets();
    calculateWeights();
    encodeColorClassUsingMST();
    logger->info("# of times the node was found in the cache: {}", gcntr);
}

void MST::mergeMSTs() {
    buildEdgeSets();
    calculateMSTBasedWeights();
    encodeColorClassUsingMST();
    logger->info("# of times the node was found in the cache: {}", gcntr);
}
/**
 * iterates over all elements of CQF,
 * find all the existing neighbors, and build a color graph based on that
 * @return true if the color graph build was successful
 */
bool MST::buildEdgeSets() {
    std::vector<spp::sparse_hash_set<Edge, edge_hash>> edgesetList;
    edgeBucketList.resize(num_of_ccBuffers * num_of_ccBuffers);
    edgesetList.resize(num_of_ccBuffers * num_of_ccBuffers);

    logger->info("Reading colored dbg from disk.");
    std::string cqf_file(prefix + mantis::CQF_FILE);
    //CQF<KeyObject> cqf(cqf_file, CQF_FREAD);
    k = cqf->keybits() / 2;
    logger->info("Done loading cdbg. k is {}", k);
    logger->info("Iterating over cqf & building edgeSet ...");
    // max possible value and divisible by 64
    sdsl::bit_vector nodes((1 + (num_of_ccBuffers * mantis::NUM_BV_BUFFER) / 64) * 64, 0);
    uint64_t maxId{0}, numOfKmers{0};

    // build color class edges in a multi-threaded manner
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < nThreads; ++i) {
        threads.emplace_back(std::thread(&MST::buildPairedColorIdEdgesInParallel, this, i,
                                         std::ref(*cqf), std::ref(edgesetList),
                                         std::ref(nodes), std::ref(maxId), std::ref(numOfKmers)));
    }
    for (auto &t : threads) { t.join(); }
    cqf->free();
    logger->info("Total number of kmers observed: {}", numOfKmers);
//    logger->info("Total number of edges observed: {}", num_edges);


    // count total number of color classes:
    /*uint64_t i = 0, maxIdDivisibleBy64 = (maxId / 64) * 64;
    while (i < maxIdDivisibleBy64) {
        if (nodes.get_int(i, 64) != UINT64_MAX) {
            logger->error("Didn't see one of the color classes in the CQF between {} & {}", i, i + 64);
        }
        i += 64;
    }
    uint64_t lastbits = sdsl::bits::cnt(nodes.get_int(i, maxId - maxIdDivisibleBy64));
    if (lastbits != maxId - maxIdDivisibleBy64)
        logger->error("Didn't see one of the color classes in the CQF between {} & {}", i, maxId);*/
    num_colorClasses = maxId + 1;
//    std::exit(1);
    logger->info("Put edges in each bucket in a sorted list.");
    for (uint32_t i = 0; i < nThreads; ++i) {
        std::string filename = "tmp"+std::to_string(i);
        std::ifstream tmp;
        tmp.open(filename, std::ios::in | std::ios::binary);
        uint64_t cnt;
        tmp.read(reinterpret_cast<char *>(&cnt), sizeof(cnt));
        logger->info("file {} has {} edges.", i, cnt);
        std::vector<Edge> edgeList;
        edgeList.resize(cnt);
        tmp.read(reinterpret_cast<char *>(edgeList.data()), sizeof(Edge)*cnt);
        tmp.close();
        std::remove(filename.c_str());
        std::sort(edgeList.begin(), edgeList.end(),
                  [](Edge &e1, Edge &e2) {
                      return e1.n1 == e2.n1 ? e1.n2 < e2.n2 : e1.n1 < e2.n1;
                  });
        edgeList.erase(std::unique(edgeList.begin(), edgeList.end(),
                [](Edge &e1, Edge &e2) {
                    return e1.n1 == e2.n1 and e1.n2 == e2.n2;
                }), edgeList.end());
        for (auto &edge: edgeList) {
            edgeBucketList[getBucketId(edge.n1, edge.n2)].push_back(edge);
        }
    }
    for (auto &bucket: edgeBucketList) {
        std::cerr << "before uniqifying: " << bucket.size() << " ";
        std::sort(bucket.begin(), bucket.end(),
                  [](Edge &e1, Edge &e2) {
                      return e1.n1 == e2.n1 ? e1.n2 < e2.n2 : e1.n1 < e2.n1;
                  });
        bucket.erase(std::unique(bucket.begin(), bucket.end(),
                                   [](Edge &e1, Edge &e2) {
                                       return e1.n1 == e2.n1 and e1.n2 == e2.n2;
                                   }), bucket.end());
        std::cerr << "after: " << bucket.size() << "\n";
    }
    logger->info("Done sorting the edges.");

    // Add an edge between each color class ID and node zero
    logger->info("Adding edges from dummy node zero to each color class Id for {} color classes",
                 num_colorClasses);
    zero = static_cast<colorIdType>(num_colorClasses);
    for (colorIdType colorId = 0; colorId < num_colorClasses; colorId++) {
        /*if (edgeBucketList[getBucketId(colorId, zero)].find(Edge(colorId, zero)) != edgeBucketList[getBucketId(colorId, zero)].end()) {
            logger->error("already existed: {}, {}", colorId, zero);
            std::exit(1);
        }*/
        edgeBucketList[getBucketId(colorId, zero)].push_back(Edge(colorId, zero));
    }
    num_colorClasses++; // zero is now a dummy color class with ID equal to actual num of color classes
//    std::cerr << " zerooo " << zero << "   " << test.size() << "\n";
    return true;
}

void MST::buildPairedColorIdEdgesInParallel(uint32_t threadId,
                                            CQF<KeyObject> &cqf,
                                            std::vector<spp::sparse_hash_set<Edge, edge_hash>> &edgesetList,
                                            sdsl::bit_vector &nodes,
                                            uint64_t &maxId, uint64_t &numOfKmers) {
    //std::cout << "THREAD ..... " << threadId << " " << cqf.range() << "\n";
    uint64_t kmerCntr{0}, localMaxId{0};
    __uint128_t startPoint = threadId * (cqf.range() / (__uint128_t) nThreads);
    __uint128_t endPoint =
            threadId + 1 == nThreads ? cqf.range() + 1 : (threadId + 1) * (cqf.range() / (__uint128_t) nThreads);
        /*std::cerr << threadId << ": s" << (uint64_t) (startPoint/(__uint128_t)0xFFFFFFFFFFFFFFFF) << " "
                  << "sr" << (uint64_t) (startPoint%(__uint128_t)0xFFFFFFFFFFFFFFFF) << " "
                << "e" << (uint64_t) (endPoint/(__uint128_t)0xFFFFFFFFFFFFFFFF) << " "
                << "er" << (uint64_t) (endPoint%(__uint128_t)0xFFFFFFFFFFFFFFFF) << "\n";*/
    auto tmpEdgeListSize = MAX_ALLOWED_TMP_EDGES / nThreads;
    std::vector<Edge> edgeList;
    edgeList.reserve(tmpEdgeListSize);
    auto it = cqf.setIteratorLimits(startPoint, endPoint);
    std::string filename("tmp"+std::to_string(threadId));
    uint64_t cnt = 0;
    std::ofstream tmpfile;
    tmpfile.open(filename, std::ios::out | std::ios::binary);
    tmpfile.write(reinterpret_cast<const char *>(&cnt), sizeof(cnt));
    while (!it.reachedHashLimit()) {
        KeyObject keyObject = *it;
        uint64_t curEqId = keyObject.count - 1;
//        test.insert(curEqId);
        //nodes[curEqId] = 1; // set the seen color class id bit
        localMaxId = curEqId > localMaxId ? curEqId : localMaxId;
        // Add an edge between the color class and each of its neighbors' colors in dbg
        findNeighborEdges(cqf, keyObject, edgeList);
        if (edgeList.size() >= tmpEdgeListSize/* and colorMutex.try_lock()*/) {
            tmpfile.write(reinterpret_cast<const char *>(edgeList.data()), sizeof(Edge)*edgeList.size());
            cnt+=edgeList.size();
            edgeList.clear();
        }
        ++it;
        kmerCntr++;
        if (kmerCntr % 10000000 == 0) {
            std::cerr << "\rthread " << threadId << ": Observed " << kmerCntr << " kmers and " << cnt << " edges";
        }
    }
    tmpfile.write(reinterpret_cast<const char *>(edgeList.data()), sizeof(Edge)*edgeList.size());
    cnt+=edgeList.size();
    colorMutex.lock();
    maxId = localMaxId > maxId ? localMaxId : maxId;
    numOfKmers += kmerCntr;
    std::cerr << "\r";
    logger->info("Thread {}: Observed {} kmers and {} edges", threadId, numOfKmers, cnt/*num_edges*/);
    colorMutex.unlock();
    //}
    tmpfile.seekp(0);
    tmpfile.write(reinterpret_cast<const char *>(&cnt), sizeof(cnt));
    tmpfile.close();
}

/**
 * loads the color class table in parts
 * calculate the hamming distance between the color bitvectors fetched from color class table
 * for each pair of color IDs
 * having w buckets where w is the maximum possible weight (number of experiments)
 * put the pair in its corresponding bucket based on the hamming distance value (weight)
 * @return true if successful
 */
bool MST::calculateWeights() {

    logger->info("Going over all the edges and calculating the weights.");
    uint64_t numEdges = 0;
    weightBuckets.resize(numSamples);
    for (auto i = 0; i < eqclass_files.size(); i++) {
        BitVectorRRR bv1;
        sdsl::load_from_file(bv1, eqclass_files[i]);
        bvp1 = &bv1;
        for (auto j = i; j < eqclass_files.size(); j++) {
            BitVectorRRR bv2;
            auto &edgeBucket = edgeBucketList[i * num_of_ccBuffers + j];
            if (i == j) {
                bvp2 = bvp1;
            } else {
                sdsl::load_from_file(bv2, eqclass_files[j]);
                bvp2 = &bv2;
            }
            std::cerr << "\rEq classes " << i << " and " << j << " -> edgeset size: " << edgeBucket.size();
            std::vector<std::thread> threads;
            for (uint32_t t = 0; t < nThreads; ++t) {
                threads.emplace_back(std::thread(&MST::calcHammingDistInParallel, this, t,
                                                 std::ref(edgeBucket), false));
            }
            for (auto &t : threads) { t.join(); }
            edgeBucket.clear();
        }
    }
    std::cerr << "\r";
    /*if (bvp1 == nullptr)
        return false;
    if (bvp1 == bvp2) {
        delete bvp1;
    } else {
        delete bvp1;
        delete bvp2;
    }*/
    edgeBucketList.clear();
    logger->info("Calculated the weight for the edges");
    return true;
}

bool MST::calculateMSTBasedWeights() {

    mst1 = new MSTQuery(prefix1, k, k, numSamples, logger);
    mst2 = new MSTQuery(prefix2, k, k, numSamples, logger);

    logger->info("loaded the two msts with k={}. MST sizes are {}, {} respectively.", k, mst1->parentbv.size(), mst2->parentbv.size());
    std::ifstream cp(prefix+"newID2oldIDs");
    uint64_t cnt, cIdx, n1s, n2s;
    cp.read(reinterpret_cast<char*>(&cnt), sizeof(cnt));
    logger->info("# of color classes based on count of colorPairs: {}", cnt);
    colorPairs.resize(cnt);
    for (auto i = 0; i < cnt; i++) {
        cp.read(reinterpret_cast<char*>(&cIdx), sizeof(cIdx));
        cp.read(reinterpret_cast<char*>(&n1s), sizeof(n1s));
        cp.read(reinterpret_cast<char*>(&n2s), sizeof(n2s));
        n1s = n1s == 0?mst1->parentbv.size()-1:n1s - 1;
        n2s = n2s == 0?mst2->parentbv.size()-1:n2s - 1;
        //std::cerr << cIdx << " " << n1s << " " << n2s << "\n";
        colorPairs[cIdx] = std::make_pair(n1s, n2s);
    }
    cp.close();

    logger->info("Going over all the edges and calculating the weights for {} eqclass buckets.", eqclass_files.size());
    uint64_t numEdges = 0;
    weightBuckets.resize(numSamples);
    queryStats.numSamples = numSamples;
    queryStats.trySample = true;
    for (auto i = 0; i < eqclass_files.size(); i++) {
        for (auto j = i; j < eqclass_files.size(); j++) {
            auto &edgeBucket = edgeBucketList[i * num_of_ccBuffers + j];
            //std::cerr << "\rEq classes " << i << " and " << j << " -> edgeset size: " << edgeBucket.size();
            //std::vector<std::thread> threads;
//            calcHammingDistInParallel(0, edgeBucket, true);
//            edgeBucket.clear();
            std::cerr << "\rEq classes " << i << " and " << j << " -> edgeset size: " << edgeBucket.size();
            std::vector<std::thread> threads;
            for (uint32_t t = 0; t < nThreads; ++t) {
                threads.emplace_back(std::thread(&MST::calcHammingDistInParallel, this, t,
                                                 std::ref(edgeBucket), true));
            }
            for (auto &t : threads) { t.join(); }
            edgeBucket.clear();
        }
    }
//    std::cerr << "\r";
    edgeBucketList.clear();
    logger->info("Calculated the weight for the edges");
    return true;
}

void MST::calcHammingDistInParallel(uint32_t i, std::vector<Edge> &edgeList, bool isMSTBased) {
    uint64_t srcId = (uint64_t)-1;
    std::vector<uint64_t> srcBV;
    std::vector<std::vector<Edge>> localWeightBucket;
    localWeightBucket.resize(numSamples);
    uint64_t s = 0, e;
    // If the list contains less than a hundred edges, don't bother with multi-threading and
    // just run the first thread
   if (edgeList.size() < 100/* or isMSTBased*/) {
        if (i == 0) {
            e = edgeList.size();
        } else {
            e = 0;
        }
    } else {
        s = edgeList.size() * i / nThreads;
        e = edgeList.size() * (i + 1) / nThreads;
    }
    auto itrStart = edgeList.begin() + s;
    auto itrEnd = e >= edgeList.size() ? edgeList.end() : edgeList.begin() + e;
    //logger->info("Thread {}: {} to {} out of {}", i, s, e, edgeList.size());
    if (!isMSTBased) {
        for (auto edge = itrStart; edge != itrEnd; edge++) {
            auto w = hammingDist(edge->n1, edge->n2,
                                 srcId, srcBV); // hammingDist uses bvp1 and bvp2
            if (w == 0) {
                logger->error("Hamming distance of 0 between edges {} & {}", edge->n1, edge->n2);
                std::exit(1);
            }
            localWeightBucket[w - 1].push_back(*edge);
        }
    } else {
        bool isFirst = true;
        uint64_t mst1Zero = mst1->parentbv.size()-1, mst2Zero = mst2->parentbv.size()-1;
        for (auto edge = itrStart; edge != itrEnd; edge++) {
            auto n1s = edge->n1 == zero? std::make_pair(mst1Zero, mst2Zero):colorPairs[edge->n1];
            auto n2s = edge->n2 == zero? std::make_pair(mst1Zero, mst2Zero):colorPairs[edge->n2];
            auto w1 = mstBasedHammingDist(n1s.first, n2s.first, srcBV, isFirst);
            auto w2 = mstBasedHammingDist(n1s.second, n2s.second, srcBV, !isFirst);
            auto w = w1 + w2;
            if (w == 0) {
                logger->error("Hamming distance of 0:<{},{}> between edges {}:<{},{}> & {}:<{},{}>",
                        w1, w2, edge->n1, n1s.first, n1s.second, edge->n2, n2s.first, n2s.second);
                std::exit(1);
            }
            localWeightBucket[w - 1].push_back(*edge);
//            std::cerr << "w " << edge->n1 << " " << edge->n2 << " " << w << "\n";
        }
    }
    colorMutex.lock();
    for (uint64_t j = 0; j < numSamples; j++) {
        weightBuckets[j].insert(weightBuckets[j].end(), localWeightBucket[j].begin(), localWeightBucket[j].end());
    }
    colorMutex.unlock();
}

/**
 * Finds Minimum Spanning Forest of color graph using Kruskal Algorithm
 *
 * The algorithm's basic implementation taken from
 * https://www.geeksforgeeks.org/kruskals-minimum-spanning-tree-using-stl-in-c/
 * @return List of connected components in the Minimum Spanning Forest
 */
DisjointSets MST::kruskalMSF() {
    uint32_t bucketCnt = numSamples;
    mst.resize(num_colorClasses);
    // Create disjoint sets
    DisjointSets ds(num_colorClasses);

    uint64_t edgeCntr{0}, selectedEdgeCntr{0};
    uint32_t w{0};

    // Iterate through all sorted edges
    for (uint32_t bucketCntr = 0; bucketCntr < bucketCnt; bucketCntr++) {
        uint32_t edgeIdxInBucket = 0;
        w = bucketCntr + 1;
        for (auto &it : weightBuckets[bucketCntr]) {
            colorIdType u = it.n1;
            colorIdType v = it.n2;
            colorIdType root_of_u = ds.find(u);
            colorIdType root_of_v = ds.find(v);

            // Check if the selected edge is causing a cycle or not
            // (A cycle is induced if u and v belong to the same set)
            if (root_of_u != root_of_v) {
                // Merge two sets
                ds.merge(root_of_u, root_of_v, w);
                // Current edge will be in the MST
                mst[u].emplace_back(v, w);
                mst[v].emplace_back(u, w);
                mstTotalWeight += w;
                selectedEdgeCntr++;
            }
            edgeCntr++;
            if (edgeCntr % 1000000 == 0) {
                std::cerr << "\r" << edgeCntr << " edges processed and "
                          << selectedEdgeCntr << " were selected";
            }
            edgeIdxInBucket++;
        }
        weightBuckets[bucketCntr].clear();
    }
    std::cerr << "\r";
    mstTotalWeight++;//1 empty slot for root (zero)
    logger->info("MST Construction finished:"
                 "\n\t# of graph edges: {}"
                 "\n\t# of merges (mst edges): {}"
                 "\n\tmst weight sum: {}",
                 edgeCntr, selectedEdgeCntr, mstTotalWeight);
    return ds;
}

/**
 * calls kruskal algorithm to build an MST of the color graph
 * goes over the MST and fills in the int-vectors parentbv, bbv, and deltabv
 * serializes these three int-vectors as the encoding of color classes
 * @return true if encoding and serializing the DS is successful
 */
bool MST::encodeColorClassUsingMST() {
    // build mst of color class graph
    kruskalMSF();

    uint64_t nodeCntr{0};
    // encode the color classes using mst
    logger->info("Filling ParentBV...");
    sdsl::int_vector<> parentbv(num_colorClasses, 0, ceil(log2(num_colorClasses)));
    // create and fill the deltabv and boundarybv data structures
    sdsl::bit_vector bbv(mstTotalWeight, 0);
    {// putting weightbv inside the scope so its memory is freed after we're done with it
        sdsl::int_vector<> weightbv(num_colorClasses, 0, ceil(log2(numSamples)));
        sdsl::bit_vector visited(num_colorClasses, 0);
        bool check = false;
        std::queue<colorIdType> q;
        q.push(zero); // Root of the tree is zero
        parentbv[zero] = zero; // and it's its own parent (has no parent)
        weightbv[zero] = 1; // adding a dummy weight for a dummy node
        while (!q.empty()) {
            colorIdType parent = q.front();
            q.pop();
            for (auto &neighbor :mst[parent]) {
                if (!visited[neighbor.first]) {
                    parentbv[neighbor.first] = parent;
                    weightbv[neighbor.first] = neighbor.second;
                    q.push(neighbor.first);
                }
            }
            visited[parent] = 1;
            nodeCntr++; // just a counter for the log
            if (nodeCntr % 10000000 == 0) {
                std::cerr << "\rset parent of " << nodeCntr << " ccs";
            }
        }

        std::cerr << "\r";
        // filling bbv
        // resize bbv
        logger->info("Filling BBV...");
        uint64_t deltaOffset{0};
        for (uint64_t i = 0; i < num_colorClasses; i++) {
            deltaOffset += static_cast<uint64_t>(weightbv[i]);
            /*if (bbv[deltaOffset - 1] == 1) {
                std::cerr << "EXCEPTION!! SHOULDN'T HAPPEN " << i << " " << deltaOffset
                          << " " << weightbv[i] << " " << bbv[deltaOffset - 1] << "\n";
                std::exit(1);
            }*/
            bbv[deltaOffset - 1] = 1;
        }
    }
    std::cerr << "\r";
    // fill in deltabv
    logger->info("Filling DeltaBV...");
    sdsl::int_vector<> deltabv(mstTotalWeight, 0, ceil(log2(numSamples)));
    sdsl::bit_vector::select_1_type sbbv = sdsl::bit_vector::select_1_type(&bbv);
    for (auto i = 0; i < 1/*eqclass_files.size()*/; i++) {
        /*BitVectorRRR bv1;
        sdsl::load_from_file(bv1, eqclass_files[i]);
        bvp1 = &bv1;*/
        for (auto j = i; j < 1/*eqclass_files.size()*/; j++) {
            /*BitVectorRRR bv2;
            if (i == j) {
                bvp2 = bvp1;
            } else {
                sdsl::load_from_file(bv2, eqclass_files[j]);
                bvp2 = &bv2;
            }
*/

/*
            struct Delta {
                uint64_t startingOffset{0};
                std::vector<uint32_t> deltaVals;
                Delta() = default;

                Delta(uint64_t so) {
                    startingOffset = so;
                }
            };
            std::vector<Delta> deltas;

            uint64_t mst1Zero = mst1->parentbv.size()-1, mst2Zero = mst2->parentbv.size()-1;
//            std::cerr << "\n\nGot here\n\n";
            for (colorIdType p = 0; p < parentbv.size(); p++) {
                if (getBucketId(p, parentbv[p]) == i * num_of_ccBuffers + j) {
                    auto deltaOffset = (p > 0) ? (sbbv(p) + 1) : 0;
                    deltas.push_back(deltaOffset);
//                    std::cerr << deltaOffset << " ";
                    auto n1s = p == zero? std::make_pair(mst1Zero, mst2Zero):colorPairs[p];
                    auto n2s = parentbv[p] == zero? std::make_pair(mst1Zero, mst2Zero):colorPairs[parentbv[p]];
//                    std::cerr << p << ":[" << n1s.first << "," << n1s.second << "] " <<
//                    parentbv[p] <<":[" << n2s.first << "," << n2s.second << "] " <<  " ";
                    auto firstDelta = getMSTBasedDeltaList(n1s.first, n2s.first, true);
                    auto secondDelta = getMSTBasedDeltaList(n1s.second, n2s.second, false);
//                    std::cerr << firstDelta.size() << " " << secondDelta.size() << "\n";
                    deltas.back().deltaVals = firstDelta;
                    for (auto& v : secondDelta) {
                        v += numOfFirstMantisSamples;
                        deltas.back().deltaVals.push_back(v);
//                        std::cerr << v << "\n";
                    }
                    //std::copy(secondDelta.begin(), secondDelta.end(), deltas.back().deltaVals.end());
                }
            }
            for (auto &v : deltas) {
//                std::cerr << v.startingOffset << ": " << v.deltaVals.size() << "\n";
                for (auto cntr = 0; cntr < v.deltaVals.size(); cntr++)
                    deltabv[v.startingOffset+cntr] = v.deltaVals[cntr];
            }
*/



            std::vector<std::thread> threads;
            for (uint32_t t = 0; t < nThreads; ++t) {
                threads.emplace_back(std::thread(&MST::calcDeltasInParallel, this,
                        t, i, j,
                        std::ref(parentbv), std::ref(deltabv), std::ref(sbbv), true));
            }
            for (auto &t : threads) { t.join(); }
        }
    }
    std::cerr << "\r";
    /*if (bvp1 == nullptr)
        return false;
    if (bvp1 == bvp2) {
        delete bvp1;
    } else {
        delete bvp1;
        delete bvp2;
    }
*/
    logger->info("Serializing data structures parentbv, deltabv, & bbv...");
    sdsl::store_to_file(parentbv, std::string(prefix + mantis::PARENTBV_FILE));
    sdsl::store_to_file(deltabv, std::string(prefix + mantis::DELTABV_FILE));
    sdsl::store_to_file(bbv, std::string(prefix + mantis::BOUNDARYBV_FILE));
    logger->info("Done Serializing.");
    return true;
}

void MST::calcDeltasInParallel(uint32_t threadID, uint64_t cbvID1, uint64_t cbvID2,
                               sdsl::int_vector<> &parentbv, sdsl::int_vector<> &deltabv,
                               sdsl::bit_vector::select_1_type &sbbv,
                               bool isMSTBased) {

    struct Delta {
        uint64_t startingOffset{0};
        std::vector<uint32_t> deltaVals;
        Delta() = default;

        Delta(uint64_t so) {
            startingOffset = so;
        }
    };
    std::vector<Delta> deltas;

    colorIdType s = parentbv.size() * threadID / nThreads;
    colorIdType e = parentbv.size() * (threadID+1) / nThreads;
    if (!isMSTBased) {
        for (colorIdType p = s; p < e; p++) {
            if (getBucketId(p, parentbv[p]) == cbvID1 * num_of_ccBuffers + cbvID2) {
                auto deltaOffset = (p > 0) ? (sbbv(p) + 1) : 0;
                deltas.push_back(deltaOffset);
                deltas.back().deltaVals = getDeltaList(p, parentbv[p]);
            }
        }
    } else {
        uint64_t mst1Zero = mst1->parentbv.size()-1, mst2Zero = mst2->parentbv.size()-1;
        for (colorIdType p = s; p < e; p++) {
            auto deltaOffset = (p > 0) ? (sbbv(p) + 1) : 0;
            deltas.push_back(deltaOffset);
//                    std::cerr << deltaOffset << " ";
            auto n1s = p == zero ? std::make_pair(mst1Zero, mst2Zero) : colorPairs[p];
            auto n2s = parentbv[p] == zero ? std::make_pair(mst1Zero, mst2Zero) : colorPairs[parentbv[p]];
//                    std::cerr << p << ":[" << n1s.first << "," << n1s.second << "] " <<
//                    parentbv[p] <<":[" << n2s.first << "," << n2s.second << "] " <<  " ";
            auto firstDelta = getMSTBasedDeltaList(n1s.first, n2s.first, true);
            auto secondDelta = getMSTBasedDeltaList(n1s.second, n2s.second, false);
//                    std::cerr << firstDelta.size() << " " << secondDelta.size() << "\n";
            deltas.back().deltaVals = firstDelta;
            for (auto &v : secondDelta) {
                v += numOfFirstMantisSamples;
                deltas.back().deltaVals.push_back(v);
//                        std::cerr << v << "\n";
            }
        }
    }
    colorMutex.lock();
    for (auto &v : deltas) {
        for (auto cntr = 0; cntr < v.deltaVals.size(); cntr++)
            deltabv[v.startingOffset+cntr] = v.deltaVals[cntr];
    }
    colorMutex.unlock();

}
/**
 * finds the neighbors of each kmer in the cqf,
 * and adds an edge of the element's colorId and its neighbor's
 * @param cqf (required to query for existence of neighbors)
 * @param it iterator to the elements of cqf
 */
void MST::findNeighborEdges(CQF<KeyObject> &cqf, KeyObject &keyobj, std::vector<Edge> &edgeList) {
    dna::canonical_kmer curr_node(static_cast<int>(k), keyobj.key);
    workItem cur = {curr_node, static_cast<colorIdType>(keyobj.count - 1)};
    uint64_t neighborCnt{0};
    for (auto &nei : neighbors(cqf, cur)) {
        neighborCnt++;
        if (cur.colorId < nei.colorId) {
            Edge e(static_cast<colorIdType>(cur.colorId), static_cast<colorIdType>(nei.colorId));
            edgeList.push_back(e);
        }
    }
}

/**
 * finds neighbors of a node in cqf
 * @param cqf
 * @param n : work_item containing node and colorId (colorId will be filled)
 * @return set of neighbors for current node n and their colorIds
 */
std::set<workItem> MST::neighbors(CQF<KeyObject> &cqf, workItem n) {
    std::set<workItem> result;
    for (const auto b : dna::bases) {
        uint64_t eqid = 0;
        if (exists(cqf, n.node << b, eqid)) {
            if (eqid != n.colorId)
                result.insert(workItem(n.node << b, eqid));
        }
        if (exists(cqf, b >> n.node, eqid)) {
            if (eqid != n.colorId)
                result.insert(workItem(b >> n.node, eqid));
        }
    }
    return result;
}

/**
 * searches for a kmer in cqf and returns the correct colorId if found
 * which is cqf count value - 1
 * @param cqf
 * @param e : search canonical kmer
 * @param eqid : reference to eqid that'll be set
 * @return true if eqid is found
 */
bool MST::exists(CQF<KeyObject> &cqf, dna::canonical_kmer e, uint64_t &eqid) {
    KeyObject key(e.val, 0, 0);
    auto eqidtmp = cqf.query(key, QF_NO_LOCK /*QF_KEY_IS_HASH | QF_NO_LOCK*/);
    if (eqidtmp) {
        eqid = eqidtmp - 1;
        return true;
    }
    return false;
}

/**
 * calculates hamming distance between the bvs of two color class ids
 * @param eqid1 first color class id
 * @param eqid2 second color class id
 * @return
 */
uint64_t MST::hammingDist(uint64_t eqid1, uint64_t eqid2,
                          uint64_t &srcId, std::vector<uint64_t> &srcEq) {
    uint64_t dist{0};
    std::vector<uint64_t> eq1(((numSamples - 1) / 64) + 1, 0), eq2(((numSamples - 1) / 64) + 1, 0);
    // cache the source color ID and BV
    if (eqid1 == srcId) {
        eq1 = srcEq;
    } else {
        buildColor(eq1, eqid1, bvp1);
        srcEq.clear();
        for (auto &eq: eq1) {
            srcEq.push_back(eq);
        }
        srcId = eqid1;
    }
    // fetch the second color ID's BV
    buildColor(eq2, eqid2, bvp2);

    for (uint64_t i = 0; i < eq1.size(); i++) {
        if (eq1[i] != eq2[i])
            dist += sdsl::bits::cnt(eq1[i] ^ eq2[i]);
    }
    return dist;
}

void MST::buildMSTBasedColor(uint64_t eqid, LRUCacheMap& lru_cache, std::mutex& cacheMutex, MSTQuery *mst1, std::vector<uint64_t> & eq) {
    RankScores rs(1);

    nonstd::optional<uint64_t> dummy{nonstd::nullopt};

    auto eq_ptr = lru_cache.lookup_ts(eqid, cacheMutex);
    if (eq_ptr != nullptr) {
//    if (lru_cache.contains(eqid)) {
//        eq = lru_cache[eqid];//.get(eqclass_id);
        eq = (*eq_ptr.get());
        queryStats.cacheCntr++;
    } else {
        queryStats.noCacheCntr++;
        queryStats.trySample = (queryStats.noCacheCntr % 20 == 0);
        toDecode.reset();
        eq = mst1->buildColor(eqid, queryStats, &lru_cache, &rs, toDecode, cacheMutex);
        lru_cache.emplace_ts(eqid, eq, cacheMutex);
        if (queryStats.trySample and toDecode) {
            auto s = mst1->buildColor(*toDecode, queryStats, nullptr, nullptr, dummy, cacheMutex);
            lru_cache.emplace_ts(*toDecode, s, cacheMutex);
        }
    }
}

uint64_t MST::mstBasedHammingDist(uint64_t eqid1, uint64_t eqid2, std::vector<uint64_t> &srcEq, bool isFirst) {

    uint64_t dist{0};
    std::vector<uint64_t> eq1, eq2;

    if (isFirst) {
        buildMSTBasedColor(eqid1, lru_cache1, cacheMutex1, mst1, eq1);
        // fetch the second color ID's BV
        buildMSTBasedColor(eqid2, lru_cache1, cacheMutex1, mst1, eq2);
    }
    else {
        buildMSTBasedColor(eqid1, lru_cache2, cacheMutex2, mst2, eq1);
        // fetch the second color ID's BV
        buildMSTBasedColor(eqid2, lru_cache2, cacheMutex2, mst2, eq2);
    }

    /// calc distance
    auto i{0}, j{0};
    while (i != eq1.size() and j != eq2.size()) {
        if (eq1[i] == eq2[j]) {
            i++;
            j++;
        } else if (eq1[i] < eq2[j]) {
            i++;
            dist++;
        } else {
            j++;
            dist++;
        }
    }
    if (i != eq1.size()) dist+=(eq1.size()-i);
    if (j != eq2.size()) dist+=(eq2.size()-j);
    return dist;
}
/**
 * for two non-zero nodes, lists indices that xor of the bits is 1
 * for one non-zero node, lists indices that the bit is 1
 *
 * @param eqid1
 * @param eqid2
 * @return delta list
 */
std::vector<uint32_t> MST::getDeltaList(uint64_t eqid1, uint64_t eqid2) {
    std::vector<uint32_t> res;
    if (eqid1 == eqid2) return res;
    if (eqid1 > eqid2) std::swap(eqid1, eqid2);
    std::vector<uint64_t> eq1(((numSamples - 1) / 64) + 1, 0), eq2(((numSamples - 1) / 64) + 1, 0);
    buildColor(eq1, eqid1, bvp1);
    buildColor(eq2, eqid2, bvp2);

    for (uint32_t i = 0; i < eq1.size(); i += 1) {
        uint64_t eq12xor = eq1[i] ^eq2[i];
        for (uint32_t j = 0; j < 64; j++) {
            if ((eq12xor >> j) & 0x01) {
                res.push_back(i * 64 + j);
            }
        }
    }

    return res; // rely on c++ optimization
}

std::vector<uint32_t> MST::getMSTBasedDeltaList(uint64_t eqid1, uint64_t eqid2, bool isFirst) {
    std::vector<uint32_t> res;
    if (eqid1 == eqid2) return res;
    std::vector<uint64_t> eq1, eq2;
    if (isFirst) {
        buildMSTBasedColor(eqid1, lru_cache1, cacheMutex1, mst1, eq1);
        buildMSTBasedColor(eqid2, lru_cache1, cacheMutex1, mst1, eq2);
    } else {
        buildMSTBasedColor(eqid1, lru_cache2, cacheMutex2, mst2, eq1);
        buildMSTBasedColor(eqid2, lru_cache2, cacheMutex2, mst2, eq2);
    }
    /// calc delta
    auto i{0}, j{0};
    while (i != eq1.size() and j != eq2.size()) {
        if (eq1[i] == eq2[j]) {
            i++;
            j++;
        } else if (eq1[i] < eq2[j]) {
            res.push_back(eq1[i]);
            i++;
        } else {
            res.push_back(eq2[j]);
            j++;
        }
    }
    while (i != eq1.size()) {res.push_back(eq1[i]);i++;}
    while (j != eq2.size()) {res.push_back(eq2[j]);j++;}

    return res; // rely on c++ optimization
}

/**
 * Loads the bitvector corresponding to eqId
 * @param eq list of words each representing 64 bits of eqId bv (output)
 * @param eqid color id
 * @param bv the large bv collapsing all eq ids color bv in a bucket
 */
void MST::buildColor(std::vector<uint64_t> &eq, uint64_t eqid, BitVectorRRR *bv) {
    if (eqid == zero) return;
    /* colorMutex.lock();
     if (lru_cache.contains(eqid)) {
         eq = lru_cache[eqid];
         gcntr++;
         colorMutex.unlock();
         return;
     }
     colorMutex.unlock();*/
    uint64_t i{0}, bitcnt{0}, wrdcnt{0};
    uint64_t offset = eqid % mantis::NUM_BV_BUFFER;
    while (i < numSamples) {
        bitcnt = std::min(numSamples - i, (uint64_t) 64);
        uint64_t wrd = bv->get_int(offset * numSamples + i, bitcnt);
        eq[wrdcnt++] = wrd;
        i += bitcnt;
    }
//    colorMutex.lock();
//    lru_cache.emplace(eqid, eq);
//    colorMutex.unlock();
}

/**
 * calculates the edge corresponding bucket id c1 <= c2
 * @param c1 first colorId
 * @param c2 second colorId
 * @return bucket id
 */
inline uint64_t MST::getBucketId(uint64_t c1, uint64_t c2) {
    if (c1 == zero or c1 > c2) {
        std::swap(c1, c2);
    }
    uint64_t cb1 = c1 / mantis::NUM_BV_BUFFER;
    uint64_t cb2 = c2 / mantis::NUM_BV_BUFFER;
    if (c2 == zero) // return the corresponding buffer for the non-zero colorId
        return cb1 * num_of_ccBuffers + cb1;
    return cb1 * num_of_ccBuffers + cb2;
}

/**
 ********* MAIN *********
 * main function to call Color graph and MST construction and color class encoding and serializing
 */
int build_mst_main(QueryOpts &opt) {
    MST mst(opt.prefix, opt.console, opt.numThreads);
    mst.buildMST();
    if (opt.remove_colorClasses && !opt.keep_colorclasses) {
        for (auto &f : mantis::fs::GetFilesExt(opt.prefix.c_str(), mantis::EQCLASS_FILE)) {
            std::cerr << f.c_str() << "\n";
            if (std::remove(f.c_str()) != 0) {
                std::cerr << "Unable to delete file " << f << "\n";
                std::exit(1);
            }
        }
    }
    return 0;
}
