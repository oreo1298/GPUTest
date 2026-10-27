#include "gt/bench/context.h"

#include "gt/bench/presenter.h"
#include "gt/log.h"
#include "gt/stats.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <utility>

#include "stb_image_write.h"

namespace gt::bench {

std::string qualityName(Quality q) {
    switch (q) {
    case Quality::Low: return "low";
    case Quality::Medium: return "medium";
    case Quality::High: return "high";
    case Quality::Ultra: return "ultra";
    }
    return "?";
}

std::string durationName(DurationPreset d) {
    switch (d) {
    case DurationPreset::Smoke: return "smoke";
    case DurationPreset::Quick: return "quick";
    case DurationPreset::Standard: return "standard";
    case DurationPreset::Extreme: return "extreme";
    }
    return "?";
}

bool parseQuality(const std::string& s, Quality& out) {
    for (Quality q : {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra})
        if (s == qualityName(q)) return out = q, true;
    return false;
}

bool parseDuration(const std::string& s, DurationPreset& out) {
    for (DurationPreset d : {DurationPreset::Smoke, DurationPreset::Quick, DurationPreset::Standard, DurationPreset::Extreme})
        if (s == durationName(d)) return out = d, true;
    return false;
}

std::uint32_t RunSettings::framesPerPass(std::uint32_t standardFrames) const {
    switch (duration) {
    case DurationPreset::Smoke: return std::max(8u, standardFrames / 40);
    case DurationPreset::Quick: return std::max(30u, standardFrames / 2);
    case DurationPreset::Standard: return standardFrames;
    case DurationPreset::Extreme: return standardFrames * 2;
    }
    return standardFrames;
}

double RunSettings::playbackSpeed() const { return duration == DurationPreset::Quick ? 2.0 : 1.0; }

std::uint32_t RunSettings::realTimePasses() const { return duration == DurationPreset::Extreme ? 3u : 1u; }

double RunSettings::minSeconds() const {
    switch (duration) {
    case DurationPreset::Smoke: return 0.0;
    case DurationPreset::Quick: return 8.0;
    case DurationPreset::Standard: return 25.0;
    case DurationPreset::Extreme: return 90.0;
    }
    return 0.0;
}

double RunSettings::maxSeconds() const {
    switch (duration) {
    case DurationPreset::Smoke: return 60.0;
    case DurationPreset::Quick: return 90.0;
    case DurationPreset::Standard: return 240.0;
    case DurationPreset::Extreme: return 900.0;
    }
    return 240.0;
}

std::string RunSettings::param(const std::string& key, const std::string& fallback) const {
    auto it = params.find(key);
    return it == params.end() ? fallback : it->second;
}

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t h) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string imageSignature(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba) {
    constexpr int kW = 16, kH = 9;
    std::string out;
    out.reserve(kW * kH * 6);
    for (int cy = 0; cy < kH; ++cy) {
        for (int cx = 0; cx < kW; ++cx) {
            const std::uint32_t x0 = width * cx / kW, x1 = std::max(x0 + 1, width * (cx + 1) / kW);
            const std::uint32_t y0 = height * cy / kH, y1 = std::max(y0 + 1, height * (cy + 1) / kH);
            std::uint64_t sum[3] = {0, 0, 0};
            std::uint64_t n = 0;
            for (std::uint32_t y = y0; y < y1 && y < height; ++y)
                for (std::uint32_t x = x0; x < x1 && x < width; ++x) {
                    const std::uint8_t* p = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ++n;
                }
            for (int c = 0; c < 3; ++c) out += std::format("{:02x}", n ? sum[c] / n : 0);
        }
    }
    return out;
}

int compareSignatures(const std::string& a, const std::string& b, double* meanDiff) {
    if (a.size() != b.size() || a.size() % 2 != 0 || a.empty()) return -1;
    int maxDiff = 0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); i += 2) {
        const int va = std::stoi(a.substr(i, 2), nullptr, 16);
        const int vb = std::stoi(b.substr(i, 2), nullptr, 16);
        const int d = std::abs(va - vb);
        maxDiff = std::max(maxDiff, d);
        sum += d;
    }
    if (meanDiff) *meanDiff = sum / static_cast<double>(a.size() / 2);
    return maxDiff;
}

bool writePng(const std::string& path, std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba) {
    return stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba,
                          static_cast<int>(width * 4)) != 0;
}

TestContext::TestContext(rhi::Device& device, const RunSettings& settings, const TestInfo& info, EventSink* events,
                         Presenter* presenter, std::atomic<bool>* stopFlag, std::string* stopReason)
    : m_device(device), m_settings(settings), m_info(info), m_events(events), m_presenter(presenter),
      m_stopFlag(stopFlag), m_stopReason(stopReason) {
    m_outputWidth = settings.width;
    m_outputHeight = settings.height;
    if (settings.upscaler != Upscaler::Off && info.category == Category::Scene && info.frameBased) {
        const double ratio = upscaleRatio(settings.upscaleMode);
        m_settings.width = renderExtent(settings.width, ratio);
        m_settings.height = renderExtent(settings.height, ratio);
        m_upscaling = true;
    }
    m_result.id = info.id;
    m_result.name = info.name;
    m_result.category = categoryName(info.category);
    m_result.group = info.group;
    m_result.api = std::string(rhi::apiName(device.api()));
}

void TestContext::addMetric(const std::string& name, const std::string& label, double value, const std::string& unit,
                            bool higherIsBetter) {
    for (auto& m : m_result.metrics) {
        if (m.name == name) {
            m = {name, label, value, unit, higherIsBetter};
            return;
        }
    }
    m_result.metrics.push_back({name, label, value, unit, higherIsBetter});
}

void TestContext::setVerdict(results::Verdict v, const std::string& message) {
    m_result.verdict = v;
    m_result.verdictMessage = message;
}

bool TestContext::stopRequested() const { return m_stopFlag && m_stopFlag->load(std::memory_order_relaxed); }

void TestContext::checkStop() const {
    if (stopRequested()) throw StopRequested{m_stopReason ? *m_stopReason : std::string("stop requested")};
}

void TestContext::log(const std::string& message) { gt::log::info("[{}] {}", m_info.id, message); }

void TestContext::progress(double fraction, const std::string& status) {
    if (!status.empty() && status != m_lastStatus) {
        log::debug("{}: {}", m_info.id, status); // the stage a hang happened in shows in the log
        m_lastStatus = status;
    }
    if (!m_events) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastProgress < std::chrono::milliseconds(200) && fraction < 1.0) return;
    m_lastProgress = now;
    nlohmann::json e{{"type", "progress"}, {"test", m_info.id}, {"fraction", std::clamp(fraction, 0.0, 1.0)}};
    if (!status.empty()) e["status"] = status;
    m_events->emit(e);
}

void TestContext::setFrameResult(const FrameLoopResult& r) {
    m_result.frameTimesMs.clear();
    m_result.frameTimesMs.reserve(r.frameMs.size());
    for (double f : r.frameMs) m_result.frameTimesMs.push_back(static_cast<float>(f));
    m_result.gpuTimesMs.clear();
    std::vector<double> gpuValid;
    for (double g : r.gpuMs) {
        m_result.gpuTimesMs.push_back(static_cast<float>(g));
        if (std::isfinite(g)) gpuValid.push_back(g);
    }
    m_result.frames = static_cast<std::uint32_t>(r.frameMs.size());
    m_result.timeLimited = r.timeLimited;
    const FrameStats fs = computeFrameStats(r.frameMs);
    m_result.frameStats = fs;
    addMetric("avg_fps", "Average FPS", fs.avgFps, "fps");
    addMetric("low_1pct_fps", "1% low FPS", fs.low1Fps, "fps");
    addMetric("p99_ms", "99th percentile frame time", fs.p99Ms, "ms", false);
    if (!gpuValid.empty() && fs.totalMs > 0.0) {
        double sum = 0.0;
        for (double g : gpuValid) sum += g;
        const double mean = sum / static_cast<double>(gpuValid.size());
        m_result.gpuBusyPercent = std::min(100.0, mean / fs.meanMs * 100.0);
        addMetric("gpu_ms", "Average GPU time per frame", mean, "ms", false);
    }
}

FrameLoopResult TestContext::runFrames(const FrameLoopDesc& d) {
    using clock = std::chrono::steady_clock;
    constexpr std::uint32_t kInFlight = kFramesInFlight;
    FrameLoopResult out;
    const bool async = static_cast<bool>(d.recordAsync);
    std::vector<std::unique_ptr<rhi::CommandList>> lists, computeLists, postLists;
    for (std::uint32_t i = 0; i < kInFlight; ++i) {
        lists.push_back(m_device.createCommandList(rhi::QueueType::Graphics));
        if (async) {
            computeLists.push_back(m_device.createCommandList(rhi::QueueType::Compute));
            postLists.push_back(m_device.createCommandList(rhi::QueueType::Graphics));
        }
    }
    // Timestamp pairs: one per frame in flight; twice as many in async mode, where a frame's end timestamp is
    // written by its composite one iteration later (a pair is re-used only after that composite finished).
    const std::uint32_t pairs = async ? 2 * kInFlight : kInFlight;
    std::unique_ptr<rhi::QueryPool> qp;
    if (m_device.features().timestamps) qp = m_device.createQueryPool(rhi::QueryType::Timestamp, pairs * 2);
    constexpr std::int64_t kNoFrame = -1; ///< slotFrame value of a pair that holds no timed frame
    std::vector<std::int64_t> slotFrame(pairs, kNoFrame);
    auto readSlot = [&](std::uint32_t slot) {
        if (!qp || slotFrame[slot] == kNoFrame) return;
        std::uint64_t t[2] = {0, 0};
        if (qp->read(slot * 2, 2, t) && t[1] >= t[0])
            out.gpuMs[static_cast<std::size_t>(slotFrame[slot])] = static_cast<double>(t[1] - t[0]) / 1e6;
        slotFrame[slot] = kNoFrame;
    };

    // Deterministic step: smoke runs and the golden frames. Real-time passes follow the clock instead.
    const double dt = d.simDuration / std::max(1u, d.framesPerPass);
    const bool realTime = d.realTime && m_settings.duration != DurationPreset::Smoke;
    const double speed = realTime ? m_settings.playbackSpeed() : 1.0;
    const std::uint32_t realTimePasses = m_settings.realTimePasses();
    const double minS = d.minSeconds >= 0.0 ? d.minSeconds : m_settings.minSeconds();
    const double maxS = d.maxSeconds > 0.0 ? d.maxSeconds : m_settings.maxSeconds();
    // Expected length of the timed part, for progress.
    const double plannedS = realTime ? realTimePasses * d.simDuration / speed : minS;
    const std::uint32_t warmup = m_settings.duration == DurationPreset::Smoke ? std::min(d.warmupFrames, 2u)
                                                                              : d.warmupFrames;
    enum class Kind { Warmup, Golden, Timed };
    std::uint64_t counter = 0;
    auto start = clock::now();
    auto lastFrameEnd = start;
    auto lastEvent = start;
    std::size_t lastEventFrame = 0;
    std::vector<std::string> hud;
    bool stop = false;
    // Upscaled scenes: the output (render size) goes through the upscaler; its result is what the
    // window shows and the golden frames hash.
    std::unique_ptr<UpscalePass> upscale;
    if (m_upscaling && d.output) {
        upscale = std::make_unique<UpscalePass>(m_device, m_settings.upscaler, d.output->desc().width,
                                                d.output->desc().height, m_outputWidth, m_outputHeight,
                                                m_settings.sharpness);
        m_result.details["upscaler"] = std::format("{} {}", upscalerName(m_settings.upscaler),
                                                   upscaleModeName(m_settings.upscaleMode));
        m_result.details["render_resolution"] = std::format("{}x{}", m_settings.width, m_settings.height);
    }
    rhi::Texture* shown = upscale ? upscale->output() : d.output;

    // Where the run is, for the HUD and the progress events.
    auto position = [&](std::uint32_t index, std::uint32_t pass, double simTime) {
        return realTime ? std::format("Pass {}  {:.1f}/{:.0f} s", pass + 1, simTime, d.simDuration)
                        : std::format("Pass {}  frame {}/{}", pass + 1, index + 1, d.framesPerPass);
    };

    // Records the end of a frame (timestamp, HUD, presentation) into `fin` and submits it.
    auto finish = [&](rhi::CommandList& fin, std::uint32_t pair, std::uint32_t index, std::uint32_t pass,
                      double simTime, Kind kind, std::span<const rhi::QueueWait> waits) {
        if (upscale) upscale->record(fin, d.output); // part of the frame's GPU time
        if (qp && kind == Kind::Timed) {
            fin.writeTimestamp(qp.get(), pair * 2 + 1);
            fin.resolveQueries(qp.get(), pair * 2, 2); // D3D12 copies results to readable memory here
        }
        bool presented = false;
        if (m_presenter) {
            if (kind == Kind::Timed && m_presenter->hudEnabled()) {
                const auto& ft = out.frameMs;
                const double lastMs = ft.empty() ? 0.0 : ft.back();
                hud = {std::format("{}  [{}]", m_info.name, rhi::apiDisplayName(m_device.api())),
                       position(index, pass, simTime),
                       std::format("{:.1f} fps  {:.2f} ms", lastMs > 0 ? 1000.0 / lastMs : 0.0, lastMs)};
                if (hudStatus) {
                    auto extra = hudStatus();
                    hud.insert(hud.end(), extra.begin(), extra.end());
                }
            } else if (kind == Kind::Warmup) {
                hud = {std::format("{}  warming up...", m_info.name)};
            } else if (kind == Kind::Golden) {
                hud = {std::format("{}  reference frames...", m_info.name)};
            }
            presented = m_presenter->record(fin, shown, hud);
        }
        fin.end();
        rhi::CommandList* l[] = {&fin};
        rhi::SubmitInfo si{l, waits};
        if (presented) si.present = m_presenter->swapchain();
        m_device.submit(rhi::QueueType::Graphics, si);
        if (presented) m_presenter->present();
    };

    // Async mode: frame i submits [graphics: record(i)] and [compute: recordAsync(i), waiting for it], then the
    // composite of frame i-1 [graphics: recordPost(i-1), waiting for its compute work]. Submitting the composite
    // after the next frame's graphics work lets the compute queue overlap it (a queue stalls at a semaphore wait).
    std::function<void()> pendingPost;
    auto flushPost = [&] {
        if (pendingPost) std::exchange(pendingPost, {})();
    };

    auto frame = [&](std::uint32_t index, std::uint32_t pass, double simTime, double step, Kind kind) {
        const std::uint32_t slot = static_cast<std::uint32_t>(counter % kInFlight);
        const std::uint32_t pair = static_cast<std::uint32_t>(counter % pairs);
        ++counter;
        rhi::CommandList& cmd = *lists[slot];
        cmd.begin();
        readSlot(pair);
        if (qp && kind == Kind::Timed) {
            cmd.resetQueries(qp.get(), pair * 2, 2);
            cmd.writeTimestamp(qp.get(), pair * 2);
        }
        const bool isWarmup = kind == Kind::Warmup;
        FrameContext fc{cmd, index, pass, simTime, step, isWarmup, slot};
        d.record(fc);
        if (!async) {
            finish(cmd, pair, index, pass, simTime, kind, {});
        } else {
            cmd.end();
            rhi::CommandList* gl[] = {&cmd};
            const std::uint64_t gv = m_device.submit(rhi::QueueType::Graphics, rhi::SubmitInfo{gl});
            rhi::CommandList& ccmd = *computeLists[slot];
            ccmd.begin();
            FrameContext cfc{ccmd, index, pass, simTime, step, isWarmup, slot};
            d.recordAsync(cfc);
            ccmd.end();
            rhi::CommandList* cl[] = {&ccmd};
            const rhi::QueueWait cw[] = {{rhi::QueueType::Graphics, gv}};
            const std::uint64_t cv = m_device.submit(rhi::QueueType::Compute, rhi::SubmitInfo{cl, cw});
            flushPost();
            pendingPost = [&, slot, pair, index, pass, isWarmup, simTime, step, kind, cv] {
                rhi::CommandList& post = *postLists[slot];
                post.begin();
                FrameContext pfc{post, index, pass, simTime, step, isWarmup, slot};
                if (d.recordPost) d.recordPost(pfc);
                const rhi::QueueWait pw[] = {{rhi::QueueType::Compute, cv}};
                finish(post, pair, index, pass, simTime, kind, pw);
            };
        }
        if (m_presenter && !m_presenter->pumpEvents()) {
            if (m_stopFlag) m_stopFlag->store(true);
            if (m_stopReason && m_stopReason->empty()) *m_stopReason = "benchmark window closed";
        }
        const auto now = clock::now();
        if (kind == Kind::Timed) {
            out.frameMs.push_back(std::chrono::duration<double, std::milli>(now - lastFrameEnd).count());
            out.gpuMs.push_back(std::numeric_limits<double>::quiet_NaN());
            slotFrame[pair] = static_cast<std::int64_t>(out.gpuMs.size() - 1);
        }
        lastFrameEnd = clock::now();
        if (m_events && now - lastEvent >= std::chrono::milliseconds(250) && kind == Kind::Timed) {
            const double elapsed = std::chrono::duration<double>(now - start).count();
            nlohmann::json batch = nlohmann::json::array();
            for (std::size_t i = lastEventFrame; i < out.frameMs.size(); ++i)
                batch.push_back(std::round(out.frameMs[i] * 1000.0) / 1000.0);
            lastEventFrame = out.frameMs.size();
            const double done =
                realTime ? (pass * d.simDuration + simTime) / std::max(1e-9, realTimePasses * d.simDuration)
                         : (pass * d.framesPerPass + index + 1.0) / std::max(1.0, d.framesPerPass * 1.0);
            const double frac =
                std::max(std::min(1.0, done), plannedS > 0 ? std::min(1.0, elapsed / plannedS) : 0.0);
            m_events->emit({{"type", "frames"}, {"test", m_info.id}, {"frame_ms", batch}, {"pass", pass},
                            {"elapsed_s", elapsed}});
            progress(std::min(frac, 0.999), position(index, pass, simTime));
            lastEvent = now;
        }
    };

    // Golden frames first, at their deterministic times: dumping drains the GPU, which must not happen
    // between timed frames.
    if (m_settings.dumpFrames && d.output)
        for (const std::uint32_t g : d.goldenFrames) {
            if (stopRequested()) {
                stop = true;
                break;
            }
            frame(g, 0, g * dt, dt, Kind::Golden);
            flushPost(); // the image must be complete before it is dumped
            m_device.waitIdle();
            dumpTexture(shown, std::format("frame{:04}", g));
        }
    const auto warmupStart = clock::now();
    for (std::uint32_t i = 0; i < warmup && !stop; ++i) {
        frame(0, 0, 0.0, 0.0, Kind::Warmup);
        stop = stopRequested();
        // Slow GPUs: two seconds of warm-up are enough.
        if (i >= 1 && std::chrono::duration<double>(clock::now() - warmupStart).count() >= 2.0) break;
    }
    start = clock::now();
    lastEvent = start;
    lastFrameEnd = start;
    for (std::uint32_t pass = 0; !stop; ++pass) {
        const auto passStart = clock::now();
        double previous = 0.0;
        for (std::uint32_t f = 0;; ++f) {
            if (stopRequested()) {
                stop = true;
                break;
            }
            const auto now = clock::now();
            if (std::chrono::duration<double>(now - start).count() > maxS) {
                out.timeLimited = true;
                stop = true;
                break;
            }
            double simTime = f * dt;
            bool last = false;
            if (realTime) {
                simTime = std::chrono::duration<double>(now - passStart).count() * speed;
                if (simTime >= d.simDuration) {
                    simTime = d.simDuration;
                    last = true;
                }
            } else if (f >= d.framesPerPass) {
                break;
            }
            frame(f, pass, simTime, simTime - previous, Kind::Timed);
            previous = simTime;
            if (last) break;
        }
        if (!stop) out.passes = pass + 1;
        if (realTime ? out.passes >= realTimePasses
                     : std::chrono::duration<double>(clock::now() - start).count() >= minS)
            break;
    }
    flushPost();
    m_device.waitIdle();
    for (std::uint32_t s = 0; s < pairs; ++s) readSlot(s);
    out.elapsedS = std::chrono::duration<double>(clock::now() - start).count();
    if (out.timeLimited)
        gt::log::warn("[{}] time limit ({:.0f} s) reached after {} frames; result is partial", m_info.id, maxS,
                      out.frameMs.size());
    return out;
}

double TestContext::timeGpu(const std::function<void(rhi::CommandList&)>& record, rhi::QueueType queue,
                            double* submitCpuMs) {
    if (!m_timerList || m_timerList->queueType() != queue) m_timerList = m_device.createCommandList(queue);
    const bool ts = m_device.features().timestamps &&
                    (queue == rhi::QueueType::Graphics || m_device.features().timestampsOnComputeQueue);
    if (ts && !m_timerQueries) m_timerQueries = m_device.createQueryPool(rhi::QueryType::Timestamp, 2);
    rhi::CommandList& cmd = *m_timerList;
    cmd.begin();
    if (ts) {
        cmd.resetQueries(m_timerQueries.get(), 0, 2);
        cmd.writeTimestamp(m_timerQueries.get(), 0);
    }
    record(cmd);
    if (ts) {
        cmd.writeTimestamp(m_timerQueries.get(), 1);
        cmd.resolveQueries(m_timerQueries.get(), 0, 2);
    }
    cmd.end();
    const auto t0 = std::chrono::steady_clock::now();
    rhi::CommandList* l[] = {&cmd};
    const std::uint64_t v = m_device.submit(queue, rhi::SubmitInfo{l});
    if (submitCpuMs)
        *submitCpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m_device.wait(queue, v);
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ts) {
        std::uint64_t t[2] = {0, 0};
        if (m_timerQueries->read(0, 2, t) && t[1] > t[0]) return static_cast<double>(t[1] - t[0]) / 1e6;
    }
    return cpuMs;
}

std::uint64_t TestContext::dumpTexture(rhi::Texture* texture, const std::string& name) {
    const auto& desc = texture->desc();
    const rhi::Format f = desc.format;
    if (f != rhi::Format::RGBA8_UNORM && f != rhi::Format::RGBA8_SRGB && f != rhi::Format::BGRA8_UNORM &&
        f != rhi::Format::BGRA8_SRGB) {
        gt::log::warn("[{}] cannot dump texture '{}' with format {}", m_info.id, name, rhi::formatInfo(f).name);
        return 0;
    }
    std::vector<std::uint8_t> px = m_device.readbackTexture(texture);
    if (f == rhi::Format::BGRA8_UNORM || f == rhi::Format::BGRA8_SRGB)
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255; // alpha is irrelevant for comparisons
    const std::uint64_t hash = fnv1a(px.data(), px.size());
    m_result.details["golden"][name] = {{"hash", std::format("{:016x}", hash)},
                                        {"signature", imageSignature(desc.width, desc.height, px.data())},
                                        {"width", desc.width},
                                        {"height", desc.height}};
    if (m_settings.dumpDir.empty()) return hash;
    std::error_code ec;
    std::filesystem::create_directories(m_settings.dumpDir, ec);
    std::string file = m_info.id + "_" + name + ".png";
    std::replace(file.begin(), file.end(), '/', '_');
    const std::string path = (std::filesystem::path(m_settings.dumpDir) / file).string();
    if (!writePng(path, desc.width, desc.height, px.data())) gt::log::warn("failed to write {}", path);
    else gt::log::info("[{}] wrote {}", m_info.id, path);
    return hash;
}

} // namespace gt::bench
