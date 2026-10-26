#include "gt/duration.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using gt::LoopSpec;

TEST_CASE("duration parsing", "[duration]") {
    CHECK(*gt::parseDurationSeconds("90") == Approx(90));
    CHECK(*gt::parseDurationSeconds("90s") == Approx(90));
    CHECK(*gt::parseDurationSeconds("20m") == Approx(1200));
    CHECK(*gt::parseDurationSeconds("1h30m") == Approx(5400));
    CHECK(*gt::parseDurationSeconds("1.5h") == Approx(5400));
    CHECK(*gt::parseDurationSeconds("500ms") == Approx(0.5));
    CHECK(*gt::parseDurationSeconds(" 2M ") == Approx(120));
    CHECK_FALSE(gt::parseDurationSeconds(""));
    CHECK_FALSE(gt::parseDurationSeconds("abc"));
    CHECK_FALSE(gt::parseDurationSeconds("10x"));
    CHECK_FALSE(gt::parseDurationSeconds("-5s"));
}

TEST_CASE("loop spec parsing", "[duration]") {
    CHECK(LoopSpec::parse("")->mode == LoopSpec::Mode::Single);
    CHECK(LoopSpec::parse("1")->mode == LoopSpec::Mode::Single);
    auto n = LoopSpec::parse("5");
    REQUIRE(n);
    CHECK(n->mode == LoopSpec::Mode::Count);
    CHECK(n->count == 5);
    CHECK(LoopSpec::parse("12x")->count == 12);
    auto t = LoopSpec::parse("20m");
    REQUIRE(t);
    CHECK(t->mode == LoopSpec::Mode::Timed);
    CHECK(t->seconds == Approx(1200));
    CHECK(LoopSpec::parse("inf")->mode == LoopSpec::Mode::Infinite);
    CHECK(LoopSpec::parse("forever")->mode == LoopSpec::Mode::Infinite);
    CHECK_FALSE(LoopSpec::parse("bogus"));
    CHECK_FALSE(LoopSpec::parse("0x"));
}

TEST_CASE("duration formatting", "[duration]") {
    CHECK(gt::formatDuration(12.34) == "12.3s");
    CHECK(gt::formatDuration(245) == "4m 05s");
    CHECK(gt::formatDuration(3723) == "1h 02m 03s");
}
