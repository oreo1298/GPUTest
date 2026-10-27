#include "gt/bench/h264_params.h"
#include "gt/bench/video_streams.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <utility>

using namespace gt::bench;

namespace {

// SPS (High, level 4.1, 1920x1088 cropped to 1080, POC type 0), PPS (CABAC, 8x8 transform, chroma QP
// offset -2), an IDR slice header (with an emulation-prevention byte) and a P slice header; written by a
// small bit writer, with four- and three-byte start codes mixed.
constexpr std::uint8_t kStream[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x29, 0xac, 0xda, 0x01,
                                    0xe0, 0x08, 0x9f, 0x95, 0x00, 0x00, 0x01, 0x68, 0xee, 0x32, 0xc8,
                                    0xb0, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x00, 0x03,
                                    0x00, 0x18, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x63, 0x55, 0xc0};

} // namespace

TEST_CASE("H.264 NAL units are found with three- and four-byte start codes", "[h264]") {
    const auto nals = h264::nalUnits(kStream);
    REQUIRE(nals.size() == 4);
    CHECK(nals[0].type == 7);
    CHECK(nals[0].offset == 1);
    CHECK(nals[0].payload == 4);
    CHECK(nals[1].type == 8);
    CHECK(nals[1].offset == 15);
    CHECK(nals[2].type == 5);
    CHECK(nals[2].refIdc == 3);
    CHECK(nals[2].offset == 24);
    CHECK(nals[3].type == 1);
    CHECK(nals[3].refIdc == 2);
    CHECK(nals[3].end == sizeof(kStream));
}

TEST_CASE("H.264 SPS and PPS fields are read", "[h264]") {
    std::string error;
    const auto ps = h264::parse(kStream, &error);
    REQUIRE(ps);
    const h264::Sps& s = *ps->sps;
    CHECK(s.profileIdc == 100);
    CHECK(s.levelIdc == 41);
    CHECK(s.chromaFormatIdc == 1);
    CHECK(s.log2MaxFrameNumMinus4 == 0);
    CHECK(s.pocType == 0);
    CHECK(s.log2MaxPocLsbMinus4 == 2);
    CHECK(s.maxNumRefFrames == 1);
    CHECK(s.widthInMbsMinus1 == 119);
    CHECK(s.heightInMapUnitsMinus1 == 67);
    CHECK(s.frameMbsOnly);
    CHECK(s.direct8x8Inference);
    CHECK(s.frameCropping);
    CHECK(s.cropBottom == 4);
    CHECK_FALSE(s.vuiPresent);
    const h264::Pps& p = *ps->pps;
    CHECK(p.entropyCodingMode);
    CHECK(p.chromaQpIndexOffset == -2);
    CHECK(p.deblockingFilterControlPresent);
    CHECK(p.transform8x8Mode);
    CHECK(p.secondChromaQpIndexOffset == -2);
}

TEST_CASE("H.264 slice headers and picture order counts", "[h264]") {
    std::string error;
    const auto ps = h264::parse(kStream, &error);
    REQUIRE(ps);
    const auto nals = h264::nalUnits(kStream);
    const std::span<const std::uint8_t> data(kStream);
    auto header = [&](const h264::NalUnit& n) {
        auto h = h264::parseSliceHeader(data.subspan(n.payload, n.end - n.payload), *ps, &error);
        REQUIRE(h);
        return *h;
    };
    const h264::SliceHeader idr = header(nals[2]);
    CHECK(idr.idr());
    CHECK(idr.intra());
    CHECK(idr.sliceType == 7);
    CHECK(idr.frameNum == 0);
    CHECK(idr.idrPicId == 0);
    CHECK(idr.pocLsb == 0);
    const h264::SliceHeader p = header(nals[3]);
    CHECK_FALSE(p.idr());
    CHECK_FALSE(p.intra());
    CHECK(p.frameNum == 3);
    CHECK(p.pocLsb == 6);

    h264::PocCounter poc;
    CHECK(poc.next(idr, *ps->sps) == std::array<std::int32_t, 2>{0, 0});
    CHECK(poc.next(p, *ps->sps) == std::array<std::int32_t, 2>{6, 6});
    // The LSB wraps at 64 (log2_max_pic_order_cnt_lsb = 6): 30, 54, then 14 continues at 78.
    for (const auto& [lsb, expected] : {std::pair{30u, 30}, std::pair{54u, 54}, std::pair{14u, 78}}) {
        h264::SliceHeader h = p;
        h.pocLsb = lsb;
        CHECK(poc.next(h, *ps->sps)[0] == expected);
    }
}

TEST_CASE("H.264 parameter-set errors are reported", "[h264]") {
    std::string error;
    const std::uint8_t onlyPps[] = {0x00, 0x00, 0x01, 0x68, 0xee, 0x32, 0xc8, 0xb0};
    CHECK_FALSE(h264::parse(onlyPps, &error));
    CHECK_FALSE(error.empty());
    error.clear();
    const std::uint8_t truncated[] = {0x00, 0x00, 0x01, 0x67, 0x64, 0x00};
    CHECK_FALSE(h264::parse(truncated, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("CRC-32 matches zlib", "[video]") {
    const std::string s = "123456789";
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    CHECK(video::crc32(bytes) == 0xCBF43926u);
    CHECK(video::crc32(bytes.subspan(4), video::crc32(bytes.first(4))) == 0xCBF43926u);
}

TEST_CASE("Reference video streams are embedded with their frame CRCs", "[video]") {
    for (const char* key : {"h264", "h265", "av1"}) {
        const video::ReferenceStream* s = video::referenceStream(key);
        REQUIRE(s);
        CHECK(s->width == 1920);
        CHECK(s->height == 1080);
        CHECK(s->crc32.size() == 8);
        CHECK(s->data.size() > 100000);
    }
}

TEST_CASE("The x264 reference stream splits into eight frames", "[h264][video]") {
    const video::ReferenceStream* s = video::referenceStream("h264");
    REQUIRE(s);
    std::string error;
    const auto st = h264::split(s->data, nullptr, &error);
    INFO(error);
    REQUIRE(st);
    // Values from FFmpeg's trace_headers bitstream filter on the same stream.
    const h264::Sps& sps = *st->params.sps;
    CHECK(sps.profileIdc == 100);
    CHECK(sps.levelIdc == 42);
    CHECK(sps.pocType == 2);
    CHECK(sps.maxNumRefFrames == 1);
    CHECK(sps.widthInMbsMinus1 == 119);
    CHECK(sps.heightInMapUnitsMinus1 == 67);
    CHECK(sps.cropBottom == 4);
    CHECK(sps.vuiPresent);
    CHECK(st->params.pps->entropyCodingMode);
    CHECK(st->params.pps->transform8x8Mode);
    CHECK(st->params.pps->chromaQpIndexOffset == -2);
    REQUIRE(st->pictures.size() == 8);
    for (std::size_t f = 0; f < 8; ++f) {
        const h264::Picture& p = st->pictures[f];
        INFO("frame " << f);
        CHECK(p.header.idr() == (f == 0));
        CHECK(p.header.frameNum == f);
        CHECK(p.poc[0] == static_cast<std::int32_t>(2 * f));
        CHECK(p.slices.size() == 1);
        CHECK(p.slices[0] == 0);
        CHECK(p.data.size() > 1000);
    }
}
