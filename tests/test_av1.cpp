#include "gt/bench/av1_params.h"
#include "gt/bench/video_streams.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

using namespace gt::bench;

// Expected values come from FFmpeg's trace_headers bitstream filter on the same stream.
TEST_CASE("The libaom reference stream's sequence header is read", "[av1][video]") {
    const video::ReferenceStream* s = video::referenceStream("av1");
    REQUIRE(s);
    std::string error;
    const auto seq = av1::parseSequenceHeader(s->data, &error);
    INFO(error);
    REQUIRE(seq);
    CHECK(seq->profile == 0);
    CHECK(seq->levelIdx == 9);
    CHECK_FALSE(seq->timingInfoPresent);
    CHECK(seq->frameWidthBitsMinus1 == 10);
    CHECK(seq->maxFrameWidthMinus1 == 1919);
    CHECK(seq->maxFrameHeightMinus1 == 1079);
    CHECK(seq->use128x128Superblock);
    CHECK(seq->enableFilterIntra);
    CHECK(seq->enableWarpedMotion);
    CHECK(seq->enableOrderHint);
    CHECK(seq->enableRefFrameMvs);
    CHECK_FALSE(seq->enableJntComp);
    CHECK(seq->seqForceScreenContentTools == 2);
    CHECK(seq->seqForceIntegerMv == 2);
    CHECK(seq->orderHintBits == 7);
    CHECK(seq->enableCdef);
    CHECK(seq->enableRestoration);
    CHECK(seq->bitDepth == 8);
    CHECK(seq->subsamplingX == 1);
    CHECK(seq->subsamplingY == 1);
    CHECK_FALSE(seq->filmGrainParamsPresent);
}

TEST_CASE("The libaom reference stream's frame headers and tiles are read", "[av1][video]") {
    const video::ReferenceStream* s = video::referenceStream("av1");
    REQUIRE(s);
    std::string error;
    const auto st = av1::parse(s->data, nullptr, &error);
    INFO(error);
    REQUIRE(st);
    REQUIRE(st->frames.size() == 8);
    struct Expected {
        std::uint32_t frameType, refresh, primaryRef, baseQ, lrShift;
        std::array<std::uint32_t, 3> lrType; ///< FrameRestorationType (lr_type 2 is Wiener = 1)
        std::array<std::uint32_t, 7> refIdx;
    };
    const std::array<Expected, 8> expected = {{
        {0, 0xFF, 7, 72, 1, {1, 0, 0}, {}},
        {1, 2, 6, 116, 2, {1, 0, 1}, {0, 0, 0, 0, 0, 0, 0}},
        {1, 4, 3, 80, 1, {1, 0, 0}, {0, 0, 0, 1, 0, 0, 0}},
        {1, 8, 3, 80, 1, {1, 0, 0}, {1, 0, 0, 2, 0, 0, 0}},
        {1, 16, 3, 80, 1, {1, 0, 1}, {2, 1, 0, 3, 0, 0, 0}},
        {1, 32, 3, 80, 1, {1, 0, 0}, {3, 2, 1, 4, 0, 0, 0}},
        {1, 64, 3, 80, 1, {1, 1, 0}, {4, 3, 2, 5, 0, 1, 0}},
        {1, 128, 3, 80, 1, {1, 0, 1}, {5, 4, 3, 6, 0, 2, 1}},
    }};
    for (std::size_t f = 0; f < 8; ++f) {
        const av1::Frame& fr = st->frames[f];
        const av1::FrameHeader& h = fr.header;
        const Expected& e = expected[f];
        INFO("frame " << f);
        CHECK(h.frameType == e.frameType);
        CHECK(h.showFrame);
        CHECK(h.orderHint == f);
        CHECK(h.refreshFrameFlags == e.refresh);
        CHECK(h.primaryRefFrame == e.primaryRef);
        CHECK(h.baseQIdx == e.baseQ);
        CHECK(h.frameWidth == 1920);
        CHECK(h.frameHeight == 1080);
        CHECK(h.miCols == 480);
        CHECK(h.miRows == 270);
        CHECK(h.tiles.cols == 1);
        CHECK(h.tiles.rows == 1);
        CHECK(h.tiles.widthInSbsMinus1 == std::vector<std::uint16_t>{14});
        CHECK(h.tiles.heightInSbsMinus1 == std::vector<std::uint16_t>{8});
        CHECK(h.tiles.miColStarts == std::vector<std::uint16_t>{0, 480});
        CHECK(h.tiles.miRowStarts == std::vector<std::uint16_t>{0, 270});
        CHECK(h.loopFilterLevel[0] == 0);
        CHECK(h.loopFilterDeltaEnabled);
        CHECK(h.loopFilterRefDeltas == std::array<std::int8_t, 8>{1, 0, 0, 0, -1, 0, -1, -1});
        CHECK(h.cdefDampingMinus3 == 1);
        CHECK(h.cdefBits == 1);
        CHECK(h.cdefYSec[1] == 2);
        CHECK(h.cdefUvPri[0] == 11);
        CHECK(h.frameRestorationType == e.lrType);
        CHECK(h.loopRestorationSize[0] == (256u >> (2 - e.lrShift)));
        CHECK(h.usesLr);
        CHECK(h.usesChromaLr == (e.lrType[1] || e.lrType[2]));
        if (h.usesChromaLr) CHECK(h.loopRestorationSize[1] == h.loopRestorationSize[0]); // lr_uv_shift 0
        CHECK(h.txMode == 2);
        CHECK_FALSE(h.referenceSelect);
        CHECK_FALSE(h.skipModePresent);
        CHECK(fr.tileSizes.size() == 1);
        if (f > 0) {
            CHECK(h.refFrameIdx == e.refIdx);
            CHECK(h.allowHighPrecisionMv);
            CHECK(h.useRefFrameMvs);
            CHECK(h.allowWarpedMotion);
            CHECK(h.interpolationFilter == 0);
            for (std::size_t i = 0; i < 7; ++i) {
                // Each reference slot holds the frame that last refreshed it (frame k refreshes slot k).
                CHECK(fr.refFrames[i] == static_cast<std::int32_t>(e.refIdx[i]));
                CHECK(h.orderHints[1 + i] == e.refIdx[i]);
                CHECK_FALSE(h.refFrameSignBias[1 + i]);
            }
            CHECK(h.gmType == std::array<std::uint32_t, 8>{});
        }
    }
    // Tile data positions: OBU header and 3-byte size, then the frame header (11 bytes for the key frame, 17
    // for frame 2; the trace counts bits from the start of the OBU).
    CHECK(st->frames[0].tileOffsets[0] == 4 + 11);
    CHECK(st->frames[0].tileSizes[0] == 304656 - 11);
    CHECK(st->frames[2].tileOffsets[0] == 4 + 17);
    CHECK(st->frames[2].tileSizes[0] == 126644 - 17);
}

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A frame-header value under the name FFmpeg's trace gives it; nullopt for names the test does not map.
std::optional<std::int64_t> traced(const av1::FrameHeader& h, const std::string& name) {
    static const std::regex indexed(R"((\w+)\[(\d+)\](?:\[(\d+)\])?)");
    std::smatch m;
    if (std::regex_match(name, m, indexed)) {
        const std::string base = m[1];
        const auto i = static_cast<std::size_t>(std::stoul(m[2]));
        const std::size_t j = m[3].matched ? static_cast<std::size_t>(std::stoul(m[3])) : 0;
        if (base == "ref_frame_idx") return h.refFrameIdx.at(i);
        if (base == "loop_filter_level") return h.loopFilterLevel.at(i);
        if (base == "cdef_y_pri_strength") return h.cdefYPri.at(i);
        if (base == "cdef_y_sec_strength") return h.cdefYSec.at(i);
        if (base == "cdef_uv_pri_strength") return h.cdefUvPri.at(i);
        if (base == "cdef_uv_sec_strength") return h.cdefUvSec.at(i);
        if (base == "lr_type") {
            static constexpr std::array<std::uint32_t, 4> kToSyntax = {0, 2, 3, 1}; // type -> lr_type
            return kToSyntax.at(h.frameRestorationType.at(i));
        }
        if (base == "feature_enabled") return (h.featureEnabled.at(i) >> j) & 1u;
        if (base == "feature_value") return h.featureData.at(i).at(j);
        if (base == "is_global") return h.gmType.at(i) != 0;
        if (base == "is_rot_zoom") return h.gmType.at(i) == 2;
        if (base == "is_translation") return h.gmType.at(i) == 1;
        if (base == "loop_filter_ref_deltas") return h.loopFilterRefDeltas.at(i);
        if (base == "loop_filter_mode_deltas") return h.loopFilterModeDeltas.at(i);
        return std::nullopt;
    }
    const std::map<std::string, std::int64_t> scalar = {
        {"show_existing_frame", h.showExistingFrame},
        {"frame_type", h.frameType},
        {"show_frame", h.showFrame},
        {"error_resilient_mode", h.errorResilientMode},
        {"disable_cdf_update", h.disableCdfUpdate},
        {"allow_screen_content_tools", h.allowScreenContentTools},
        {"frame_size_override_flag", h.frameSizeOverride},
        {"order_hint", h.orderHint},
        {"primary_ref_frame", h.primaryRefFrame},
        {"refresh_frame_flags", h.refreshFrameFlags},
        {"frame_refs_short_signaling", h.frameRefsShortSignaling},
        {"render_and_frame_size_different", h.renderAndFrameSizeDifferent},
        {"allow_high_precision_mv", h.allowHighPrecisionMv},
        {"is_filter_switchable", h.isFilterSwitchable},
        {"interpolation_filter", h.interpolationFilter},
        {"is_motion_mode_switchable", h.isMotionModeSwitchable},
        {"use_ref_frame_mvs", h.useRefFrameMvs},
        {"disable_frame_end_update_cdf", h.disableFrameEndUpdateCdf},
        {"uniform_tile_spacing_flag", h.tiles.uniformSpacing},
        {"tile_cols_log2", h.tiles.colsLog2},
        {"tile_rows_log2", h.tiles.rowsLog2},
        {"context_update_tile_id", h.tiles.contextUpdateTileId},
        {"tile_size_bytes_minus1", h.tiles.tileSizeBytesMinus1},
        {"base_q_idx", h.baseQIdx},
        {"using_qmatrix", h.usingQmatrix},
        {"segmentation_enabled", h.segmentationEnabled},
        {"segmentation_update_map", h.segmentationUpdateMap},
        {"segmentation_temporal_update", h.segmentationTemporalUpdate},
        {"segmentation_update_data", h.segmentationUpdateData},
        {"delta_q_present", h.deltaQPresent},
        {"delta_q_res", h.deltaQRes},
        {"delta_lf_present", h.deltaLfPresent},
        {"delta_lf_res", h.deltaLfRes},
        {"delta_lf_multi", h.deltaLfMulti},
        {"loop_filter_sharpness", h.loopFilterSharpness},
        {"loop_filter_delta_enabled", h.loopFilterDeltaEnabled},
        {"loop_filter_delta_update", h.loopFilterDeltaUpdate},
        {"cdef_damping_minus_3", h.cdefDampingMinus3},
        {"cdef_bits", h.cdefBits},
        {"tx_mode", h.txMode},
        {"reference_select", h.referenceSelect},
        {"skip_mode_present", h.skipModePresent},
        {"allow_warped_motion", h.allowWarpedMotion},
        {"reduced_tx_set", h.reducedTxSet},
        {"allow_intrabc", h.allowIntrabc},
    };
    if (auto it = scalar.find(name); it != scalar.end()) return it->second;
    return std::nullopt;
}

} // namespace

TEST_CASE("AV1 frame headers with tiles, segmentation and global motion match FFmpeg's trace",
          "[av1][video]") {
    for (const char* name : {"av1_tiles_segmentation", "av1_global_motion"}) {
        INFO(name);
        const std::string base = std::string(GT_TEST_DATA_DIR) + "/video/" + name;
        const std::vector<std::uint8_t> data = readFile(base + ".obu");
        REQUIRE_FALSE(data.empty());
        const auto expected = nlohmann::json::parse(std::ifstream(base + ".json"));
        std::string error;
        const auto st = av1::parse(data, nullptr, &error);
        INFO(error);
        REQUIRE(st);
        REQUIRE(st->frames.size() == expected["frames"].size());
        std::size_t checked = 0;
        for (std::size_t f = 0; f < st->frames.size(); ++f) {
            const av1::Frame& fr = st->frames[f];
            const auto& e = expected["frames"][f];
            for (const auto& [field, value] : e["fields"].items()) {
                const auto got = traced(fr.header, field);
                if (!got) continue;
                INFO("frame " << f << ": " << field);
                CHECK(*got == value.get<std::int64_t>());
                ++checked;
            }
            // The header (and with it every variable-length field, such as global motion) has the trace's
            // length: the first tile starts after it (and the tile group header and first tile size, with
            // several tiles).
            const std::uint32_t tiles = fr.header.tiles.cols * fr.header.tiles.rows;
            const std::uint64_t headerEnd = e.contains("tile_group_end_bit")
                                                ? e["tile_group_end_bit"].get<std::uint64_t>()
                                                : e["header_end_bit"].get<std::uint64_t>();
            const std::uint64_t firstTile =
                (headerEnd + 7) / 8 + (tiles > 1 ? fr.header.tiles.tileSizeBytesMinus1 + 1 : 0);
            INFO("frame " << f);
            CHECK(fr.tileOffsets.size() == tiles);
            CHECK(fr.tileOffsets.front() == firstTile);
            CHECK(fr.tileOffsets.back() + fr.tileSizes.back() == fr.data.size());
        }
        CHECK(checked > 100);
    }
}

TEST_CASE("AV1 OBUs without size fields get them", "[av1][video]") {
    // A temporal delimiter with a size field, then a frame OBU with an extension header and none (it runs to
    // the end of the data).
    const std::vector<std::uint8_t> in = {0x12, 0x00, 0x34, 0x20, 0xAA, 0xBB, 0xCC};
    const std::vector<std::uint8_t> want = {0x12, 0x00, 0x36, 0x20, 0x03, 0xAA, 0xBB, 0xCC};
    CHECK(av1::withSizeFields(in) == want);

    // Payloads of 128 bytes and more take a two-byte leb128 size.
    std::vector<std::uint8_t> big = {0x30};
    big.resize(1 + 200, 0x5A);
    const std::vector<std::uint8_t> out = av1::withSizeFields(big);
    REQUIRE(out.size() == big.size() + 2);
    CHECK(out[0] == 0x32);
    CHECK(out[1] == ((200 & 0x7F) | 0x80));
    CHECK(out[2] == 200 >> 7);

    // Data that already has them is unchanged, and still reads the same.
    const video::ReferenceStream* s = video::referenceStream("av1");
    REQUIRE(s);
    const std::vector<std::uint8_t> ref(s->data.begin(), s->data.end());
    CHECK(av1::withSizeFields(ref) == ref);
    CHECK_THROWS(av1::withSizeFields(std::vector<std::uint8_t>{0x32, 0x05, 0x00}));
}
