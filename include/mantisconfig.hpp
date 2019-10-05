#ifndef __MANTIS_CONFIG_HPP__
#define __MANTIS_CONFIG_HPP__

#include <string>

namespace mantis{
    constexpr char major_version[] = "0";
    constexpr char minor_version[] = "2";
    constexpr char patch_version[] = "0";
    constexpr char version[] = "0.2.0";
    constexpr uint32_t index_version = 0;
    constexpr char meta_file_name[] = "/meta_info.json";
    constexpr char CQF_FILE[] = "dbg_cqf.ser";
    constexpr char EQCLASS_FILE[] = "eqclass_rrr.cls";
    constexpr char SAMPLEID_FILE[] = "sampleid.lst";
    constexpr char PARENTBV_FILE[] = "parents.bv";
    constexpr char DELTABV_FILE[] = "deltas.bv";
    constexpr char BOUNDARYBV_FILE[] = "boundaries.bv";

    // Constants for manti merge.
    constexpr char TEMP_DIR[] = "temp/";

    // Constants for LSM tree.
    constexpr uint SCALING_FACTOR = 4;
    constexpr uint64_t KMER_THRESHOLD = 1000000000UL;
    constexpr uint64_t SAMPLE_THRESHOLD = 100;
    constexpr char PARAM_FILE[] = "lsmt-params.json";
	constexpr char PENDING_SAMPLES_LIST[] = "pending-samples.lst";
    constexpr char LSMT_LEVEL_DIR[] = "lsmt-level-";
    constexpr char TEMP_BUILD_IDX_DIR[] = "temp-build-idx/";
    constexpr char TEMP_MERGE_IDX_DIR[] = "temp-merge-idx/";

    constexpr const uint64_t NUM_BV_BUFFER{20000000};
    constexpr const uint64_t INITIAL_EQ_CLASSES{10000};
    constexpr const uint64_t SAMPLE_SIZE{(1ULL << 26)};
} // namespace mantis

#endif // __MANTIS_CONFIG_HPP__
