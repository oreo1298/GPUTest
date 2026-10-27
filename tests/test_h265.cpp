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

TEST_CASE("H.265 parameter sets written by the writer read back the same", "[h265][video]") {
    const video::ReferenceStream* s = video::referenceStream("h265");
    REQUIRE(s);
    std::string error;
    const auto ps = h265::parse(s->data, &error);
    REQUIRE(ps);
    h265::ParameterSets in = *ps;
    in.sps->vuiPresent = false;
    in.pps->deblockingControlPresent = true;
    in.pps->deblockingOverrideEnabled = true;
    in.pps->betaOffsetDiv2 = -1;
    in.pps->tcOffsetDiv2 = 2;
    const std::vector<std::uint8_t> bytes = h265::write(in);
    const auto out = h265::parse(bytes, &error);
    INFO(error);
    REQUIRE(out);
    const h265::Vps& v = *out->vps;
    CHECK(v.id == in.vps->id);
    CHECK(v.maxSubLayersMinus1 == in.vps->maxSubLayersMinus1);
    CHECK(v.ptl.profileIdc == in.vps->ptl.profileIdc);
    CHECK(v.ptl.levelIdc == in.vps->ptl.levelIdc);
    CHECK(v.dpb.maxDecPicBufferingMinus1 == in.vps->dpb.maxDecPicBufferingMinus1);
    const h265::Sps& a = *in.sps;
    const h265::Sps& b = *out->sps;
    CHECK(b.ptl.profileIdc == a.ptl.profileIdc);
    CHECK(b.ptl.levelIdc == a.ptl.levelIdc);
    CHECK(b.ptl.progressiveSource == a.ptl.progressiveSource);
    CHECK(b.width == a.width);
    CHECK(b.height == a.height);
    CHECK(b.conformanceWindow == a.conformanceWindow);
    CHECK(b.log2MaxPocLsbMinus4 == a.log2MaxPocLsbMinus4);
    CHECK(b.dpb.maxDecPicBufferingMinus1 == a.dpb.maxDecPicBufferingMinus1);
    CHECK(b.log2MinCbMinus3 == a.log2MinCbMinus3);
    CHECK(b.log2DiffMaxMinCb == a.log2DiffMaxMinCb);
    CHECK(b.log2MinTbMinus2 == a.log2MinTbMinus2);
    CHECK(b.log2DiffMaxMinTb == a.log2DiffMaxMinTb);
    CHECK(b.maxTransformHierarchyDepthInter == a.maxTransformHierarchyDepthInter);
    CHECK(b.ampEnabled == a.ampEnabled);
    CHECK(b.saoEnabled == a.saoEnabled);
    REQUIRE(b.rps.size() == a.rps.size());
    for (std::size_t i = 0; i < a.rps.size(); ++i) {
        INFO("rps " << i);
        CHECK(b.rps[i].interPrediction == a.rps[i].interPrediction);
        CHECK(b.rps[i].deltaPocS0 == a.rps[i].deltaPocS0);
        CHECK(b.rps[i].deltaPocS1 == a.rps[i].deltaPocS1);
        CHECK(b.rps[i].usedS0 == a.rps[i].usedS0);
        CHECK(b.rps[i].usedS1 == a.rps[i].usedS1);
    }
    CHECK(b.temporalMvpEnabled == a.temporalMvpEnabled);
    CHECK(b.strongIntraSmoothing == a.strongIntraSmoothing);
    CHECK_FALSE(b.vuiPresent);
    const h265::Pps& p = *in.pps;
    const h265::Pps& q = *out->pps;
    CHECK(q.signDataHiding == p.signDataHiding);
    CHECK(q.initQpMinus26 == p.initQpMinus26);
    CHECK(q.cuQpDeltaEnabled == p.cuQpDeltaEnabled);
    CHECK(q.cbQpOffset == p.cbQpOffset);
    CHECK(q.entropyCodingSync == p.entropyCodingSync);
    CHECK(q.loopFilterAcrossSlices == p.loopFilterAcrossSlices);
    CHECK(q.deblockingControlPresent);
    CHECK(q.deblockingOverrideEnabled);
    CHECK(q.betaOffsetDiv2 == -1);
    CHECK(q.tcOffsetDiv2 == 2);
    CHECK(q.log2ParallelMergeLevelMinus2 == p.log2ParallelMergeLevelMinus2);
    // The stream's slice headers (with their own reference picture sets) still parse.
    const auto st = h265::split(s->data, &*out, &error);
    INFO(error);
    REQUIRE(st);
    CHECK(st->pictures.size() == 8);
    CHECK(st->pictures[5].header.rps.deltaPocS0 == std::vector<std::int32_t>{-1, -2});
}

TEST_CASE("H.265 SPS reference picture sets with prediction are written as coded", "[h265][video]") {
    const video::ReferenceStream* s = video::referenceStream("h265");
    REQUIRE(s);
    std::string error;
    auto ps = h265::parse(s->data, &error);
    REQUIRE(ps);
    // Set 0: {-1, -3}; set 1 predicted from set 0 with deltaRps -1, keeping all entries: {-1, -2, -4}.
    h265::ShortTermRps r0;
    r0.deltaPocS0 = {-1, -3};
    r0.usedS0 = {true, false};
    h265::ShortTermRps r1;
    r1.interPrediction = true;
    r1.deltaRpsSign = true;
    r1.absDeltaRpsMinus1 = 0;
    r1.refRpsNumDeltaPocs = 2;
    r1.usedByCurrPicFlag = 0b111;
    r1.useDeltaFlag = 0b111;
    ps->sps->rps = {r0, r1};
    const auto out = h265::parse(h265::write(*ps), &error);
    INFO(error);
    REQUIRE(out);
    REQUIRE(out->sps->rps.size() == 2);
    CHECK(out->sps->rps[0].deltaPocS0 == std::vector<std::int32_t>{-1, -3});
    CHECK(out->sps->rps[0].usedS0 == std::vector<bool>{true, false});
    CHECK(out->sps->rps[1].interPrediction);
    CHECK(out->sps->rps[1].deltaPocS0 == std::vector<std::int32_t>{-1, -2, -4});
    CHECK(out->sps->rps[1].usedS0 == std::vector<bool>{true, true, true});
}
