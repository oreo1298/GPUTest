#pragma once

#include "gt/results.h"
#include "gt/rhi/rhi.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gt::bench {

enum class Category { Scene, Synthetic, Feature, Diagnostic };
std::string categoryName(Category c); ///< "scene", "synthetic", "feature", "diagnostic"

/// Static description of a test. Everything the GUI shows before running comes from here.
struct TestInfo {
    std::string id;          ///< stable id, e.g. "scene.lattice"
    std::string name;        ///< display name
    std::string description; ///< one or two sentences, plain language
    Category category = Category::Scene;
    std::string group;       ///< sub-group shown in the UI ("Rasterization", "Memory", ...)
    std::vector<rhi::Api> apis; ///< APIs the test implements (empty = every compiled API)
    bool frameBased = true;  ///< reports FPS / frame-time statistics
    std::string primaryMetric = "avg_fps";
    std::string primaryUnit = "fps";
    double reference = 0.0;  ///< primary-metric value worth 1000 points (0 = unscored)
    bool higherIsBetter = true;
    std::uint64_t minVramMB = 0;
    std::vector<std::string> tags;   ///< "cpu-light", "legacy", "rt", ...
    std::vector<std::string> suites; ///< preset suites that include this test ("quick", "standard", ...)
    int order = 100; ///< run order within its category (lower first; ties keep id order)
};

struct SupportResult {
    bool supported = true;
    std::string reason;
    static SupportResult yes() { return {}; }
    static SupportResult no(std::string why) { return {false, std::move(why)}; }
};

class TestContext;

class Test {
public:
    virtual ~Test() = default;
    virtual const TestInfo& info() const = 0;
    /// Device-specific checks beyond the API / VRAM requirements the harness checks itself.
    virtual SupportResult checkSupport(TestContext&) { return SupportResult::yes(); }
    /// Runs the test end to end. Report results through the context; throw on failure.
    virtual void run(TestContext& ctx) = 0;
};

using TestFactory = std::function<std::unique_ptr<Test>()>;

class Registry {
public:
    static Registry& instance();
    bool add(TestFactory factory);
    std::vector<TestInfo> list() const;
    const TestInfo* find(const std::string& id) const;
    std::unique_ptr<Test> create(const std::string& id) const;

private:
    struct Entry {
        TestInfo info;
        TestFactory factory;
    };
    std::vector<Entry> m_entries;
};

} // namespace gt::bench

#define GT_BENCH_CONCAT2(a, b) a##b
#define GT_BENCH_CONCAT(a, b) GT_BENCH_CONCAT2(a, b)
/// Registers a test class (must be default-constructible). Tests are linked as an
/// OBJECT library so these static registrations are never dropped by the linker.
#define GT_REGISTER_TEST(Class)                                                                                \
    namespace {                                                                                                \
    [[maybe_unused]] const bool GT_BENCH_CONCAT(gtRegistered_, __LINE__) =                                     \
        ::gt::bench::Registry::instance().add([] { return std::make_unique<Class>(); });                       \
    }
