#pragma once
// Recording buffer for backends without native command lists (OpenGL, Direct3D 11): commands are
// small closures placement-constructed into 64 KiB blocks and replayed in order on the device's
// context at submit. Blocks are reused across begin()s, so steady-state recording does not
// allocate.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace gt::rhi {

template <typename Executor>
class CommandArena {
public:
    CommandArena() = default;
    ~CommandArena() { reset(); }
    CommandArena(const CommandArena&) = delete;
    CommandArena& operator=(const CommandArena&) = delete;

    /// Appends `fn(Executor&)`; it runs when the arena is executed.
    template <typename F>
    void push(F&& fn) {
        using T = std::decay_t<F>;
        static_assert(alignof(T) <= kAlign, "command payload is over-aligned");
        auto* h = static_cast<Header*>(allocate(sizeof(Header) + sizeof(T)));
        new (h + 1) T(std::forward<F>(fn));
        h->run = [](void* p, Executor& e) { (*static_cast<T*>(p))(e); };
        if constexpr (std::is_trivially_destructible_v<T>)
            h->destroy = nullptr;
        else
            h->destroy = [](void* p) { static_cast<T*>(p)->~T(); };
        h->next = nullptr;
        if (m_tail)
            m_tail->next = h;
        else
            m_head = h;
        m_tail = h;
        ++m_count;
    }

    void execute(Executor& e) const {
        for (Header* h = m_head; h; h = h->next)
            h->run(h + 1, e);
    }

    void reset() {
        for (Header* h = m_head; h; h = h->next)
            if (h->destroy) h->destroy(h + 1);
        m_head = m_tail = nullptr;
        m_count = 0;
        m_large.clear();
        m_block = 0;
        m_used = m_blocks.empty() ? kBlock : 0;
    }

    std::size_t count() const { return m_count; }

private:
    static constexpr std::size_t kAlign = 16;
    static constexpr std::size_t kBlock = 64 * 1024;
    struct alignas(kAlign) Header {
        void (*run)(void*, Executor&);
        void (*destroy)(void*);
        Header* next;
    };

    static void* align(std::byte* p) {
        const auto v = reinterpret_cast<std::uintptr_t>(p);
        return reinterpret_cast<void*>((v + kAlign - 1) / kAlign * kAlign);
    }

    void* allocate(std::size_t bytes) {
        bytes = (bytes + kAlign - 1) / kAlign * kAlign;
        if (bytes > kBlock) { // payloads bigger than a block get their own allocation
            m_large.push_back(std::make_unique<std::byte[]>(bytes + kAlign));
            return align(m_large.back().get());
        }
        if (m_used + bytes > kBlock) {
            if (m_blocks.empty() || m_block + 1 >= m_blocks.size()) {
                m_blocks.push_back(std::make_unique<std::byte[]>(kBlock + kAlign));
                m_block = m_blocks.size() - 1;
            } else {
                ++m_block;
            }
            m_used = 0;
        }
        void* p = static_cast<std::byte*>(align(m_blocks[m_block].get())) + m_used;
        m_used += bytes;
        return p;
    }

    std::vector<std::unique_ptr<std::byte[]>> m_blocks;
    std::vector<std::unique_ptr<std::byte[]>> m_large;
    std::size_t m_block = 0, m_used = kBlock;
    Header* m_head = nullptr;
    Header* m_tail = nullptr;
    std::size_t m_count = 0;
};

} // namespace gt::rhi
