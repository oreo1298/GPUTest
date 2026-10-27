#include "gt/bench/h265_params.h"
#include "gt/bench/video_streams.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace gt::bench;

// Expected values come from FFmpeg's trace_headers bitstream filter on the same stream.
TEST_CASE("The x265 reference stream's parameter sets are read", "[h265][video]") {
    const video::ReferenceStream* s = video::referenceStream("h265");
    REQUIRE(s);
    std::string error;
    const auto ps = h265::parse(s->data, &error);
    INFO(error);
    REQUIRE(ps);
    const h265::Vps& v = *ps->vps;
    CHECK(v.id == 0);
    CHECK(v.maxSubLayersMinus1 == 0);
    CHECK(v.temporalIdNesting);
    CHECK(v.ptl.profileIdc == 1);
    CHECK(v.ptl.levelIdc == 123);
    CHECK(v.ptl.progressiveSource);
    CHECK(v.ptl.frameOnlyConstraint);
    CHECK(v.subLayerOrderingInfoPresent);
    CHECK(v.dpb.maxDecPicBufferingMinus1[0] == 2);
    CHECK(v.dpb.maxLatencyIncreasePlus1[0] == 1);
    CHECK_FALSE(v.timingInfoPresent);
    const h265::Sps& sps = *ps->sps;
    CHECK(sps.width == 1920);
    CHECK(sps.height == 1080);
    CHECK_FALSE(sps.conformanceWindow);
    CHECK(sps.chromaFormatIdc == 1);
    CHECK(sps.log2MaxPocLsbMinus4 == 4);
    CHECK(sps.dpb.maxDecPicBufferingMinus1[0] == 2);
    CHECK(sps.log2MinCbMinus3 == 0);
    CHECK(sps.log2DiffMaxMinCb == 3);
    CHECK(sps.ctbSize() == 64);
    CHECK(sps.log2MinTbMinus2 == 0);
    CHECK(sps.log2DiffMaxMinTb == 3);
    CHECK(sps.saoEnabled);
    CHECK_FALSE(sps.ampEnabled);
    CHECK(sps.rps.empty());
    CHECK(sps.temporalMvpEnabled);
    CHECK(sps.strongIntraSmoothing);
    CHECK(sps.vuiPresent);
    const h265::Pps& p = *ps->pps;
    CHECK(p.signDataHiding);
    CHECK_FALSE(p.cuQpDeltaEnabled); // fixed QP, no adaptive quantization
    CHECK(p.weightedPred);
    CHECK(p.entropyCodingSync);
    CHECK_FALSE(p.tilesEnabled);
    CHECK(p.loopFilterAcrossSlices);
    CHECK_FALSE(p.deblockingControlPresent);
}

TEST_CASE("The x265 reference stream splits into pictures with reference picture sets", "[h265][video]") {
    const video::ReferenceStream* s = video::referenceStream("h265");
    REQUIRE(s);
    std::string error;
    const auto st = h265::split(s->data, nullptr, &error);
    INFO(error);
    REQUIRE(st);
    REQUIRE(st->pictures.size() == 8);
    const h265::Picture& idr = st->pictures[0];
    CHECK(idr.header.nalType == 20);
    CHECK(idr.header.idr());
    CHECK(idr.header.irap());
    CHECK(idr.header.sliceType == 2);
    CHECK(idr.poc == 0);
    for (std::size_t f = 1; f < 8; ++f) {
        const h265::Picture& p = st->pictures[f];
        INFO("frame " << f);
        CHECK(p.header.nalType == 1);
        CHECK(p.header.reference());
        CHECK(p.header.sliceType == 1);
        CHECK(p.header.pocLsb == f);
        CHECK(p.poc == static_cast<std::int32_t>(f));
        CHECK_FALSE(p.header.shortTermRefPicSetSps);
        CHECK(p.header.temporalMvpEnabled);
        CHECK(p.segments.size() == 1);
        const h265::ShortTermRps& r = p.header.rps;
        CHECK(r.deltaPocS1.empty());
        if (f == 1) {
            CHECK(r.deltaPocS0 == std::vector<std::int32_t>{-1});
            CHECK(p.header.numBitsForStRefPicSet == 6);
        } else {
            CHECK(r.deltaPocS0 == std::vector<std::int32_t>{-1, -2});
            CHECK(r.usedS0 == std::vector<bool>{true, true});
            CHECK(p.header.numBitsForStRefPicSet == 8);
        }
    }
}

TEST_CASE("H.265 reference picture sets resolve to earlier pictures", "[h265][video]") {
    const video::ReferenceStream* s = video::referenceStream("h265");
    REQUIRE(s);
    std::string error;
    auto st = h265::split(s->data, nullptr, &error);
    REQUIRE(st);
    auto refs = h265::references(*st, &error);
    INFO(error);
    REQUIRE(refs);
    REQUIRE(refs->size() == 8);
    CHECK((*refs)[0].before.empty());
    CHECK((*refs)[1].before == std::vector<std::uint32_t>{0});
    for (std::uint32_t f = 2; f < 8; ++f) {
        INFO("frame " << f);
        CHECK((*refs)[f].before == std::vector<std::uint32_t>{f - 1, f - 2});
        CHECK((*refs)[f].after.empty());
        CHECK((*refs)[f].follow.empty());
    }

    // A picture kept only for later pictures goes to the StFoll list; a missing used one is an error.
    st->pictures[3].header.rps.usedS0 = {true, false};
    refs = h265::references(*st, &error);
    REQUIRE(refs);
    CHECK((*refs)[3].before == std::vector<std::uint32_t>{2});
    CHECK((*refs)[3].follow == std::vector<std::uint32_t>{1});
    st->pictures[1].header.rps.deltaPocS0 = {-2};
    CHECK_FALSE(h265::references(*st, &error));
    CHECK(error.find("before the stream starts") != std::string::npos);
}
