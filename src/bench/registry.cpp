#include "gt/bench/test.h"

#include <algorithm>

namespace gt::bench {

std::string categoryName(Category c) {
    switch (c) {
    case Category::Scene: return "scene";
    case Category::Synthetic: return "synthetic";
    case Category::Feature: return "feature";
    case Category::Diagnostic: return "diagnostic";
    }
    return "?";
}

Registry& Registry::instance() {
    static Registry r;
    return r;
}

bool Registry::add(TestFactory factory) {
    auto probe = factory();
    m_entries.push_back({probe->info(), std::move(factory)});
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.info.id < b.info.id; });
    return true;
}

std::vector<TestInfo> Registry::list() const {
    std::vector<TestInfo> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) out.push_back(e.info);
    return out;
}

const TestInfo* Registry::find(const std::string& id) const {
    for (const auto& e : m_entries)
        if (e.info.id == id) return &e.info;
    return nullptr;
}

std::unique_ptr<Test> Registry::create(const std::string& id) const {
    for (const auto& e : m_entries)
        if (e.info.id == id) return e.factory();
    return nullptr;
}

} // namespace gt::bench
