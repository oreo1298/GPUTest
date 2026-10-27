// gt_glslgen: SPIR-V (from slangc) -> desktop GLSL for the OpenGL backend, using SPIRV-Cross.
//
//   gt_glslgen <in.spv> <out.glsl> <glsl-version>
//
// GL has no descriptor sets, so binding decorations are stripped and every resource gets a
// predictable name; a header of comment lines tells the backend which RHI slot each name is:
//   // gt:ubo <block> <slot>        uniform buffer b<slot>
//   // gt:push <block>              push constants (a uniform block)
//   // gt:ssbo <block> t|u<slot>    read-only (t) / read-write (u) storage buffer
//   // gt:image <name> <slot>       storage texture u<slot>
//   // gt:tex <name> <t> <s|->      combined texture t<t> + sampler s<s> (- = no sampler)
// The backend assigns GL binding points per program at link time (glUniformBlockBinding,
// glShaderStorageBlockBinding, glUniform1i), which works on GLSL 330 as well as 430+.
// Vertex inputs are renamed gta_<location>, stage varyings gtv_<location> (so stages compiled
// separately still match by name on GLSL 330), fragment outputs gto_<location>. Clip-space
// depth is rewritten from [0, w] to [-w, w].

#include <spirv_glsl.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Vulkan binding shifts used by slangc (cmake/Shaders.cmake): b 0+, t 16+, u 32+, s 48+.
char registerClass(std::uint32_t binding) {
    return binding < 16 ? 'b' : binding < 32 ? 't' : binding < 48 ? 'u' : 's';
}
std::uint32_t registerSlot(std::uint32_t binding) {
    return binding % 16;
}

std::vector<std::uint32_t> readWords(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("cannot open ") + path);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4) throw std::runtime_error(std::string(path) + " is not SPIR-V");
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: gt_glslgen <in.spv> <out.glsl> <glsl-version>\n");
        return 2;
    }
    try {
        spirv_cross::CompilerGLSL c(readWords(argv[1]));
        spirv_cross::CompilerGLSL::Options opts = c.get_common_options();
        opts.version = static_cast<std::uint32_t>(std::stoul(argv[3]));
        opts.es = false;
        opts.vulkan_semantics = false;
        opts.enable_420pack_extension = false;
        opts.emit_push_constant_as_uniform_buffer = true;
        opts.vertex.support_nonzero_base_instance = true;
        // Depth [0, w] -> [-w, w] wherever a position for the rasterizer is written (never in
        // tessellation control, which SPIRV-Cross would otherwise patch too).
        const spv::ExecutionModel model = c.get_execution_model();
        bool writesPosition = false;
        for (const auto& b : c.get_shader_resources().builtin_outputs)
            writesPosition |= b.builtin == spv::BuiltInPosition;
        opts.vertex.fixup_clipspace = writesPosition && (model == spv::ExecutionModelVertex ||
                                                         model == spv::ExecutionModelTessellationEvaluation ||
                                                         model == spv::ExecutionModelGeometry);
        c.set_common_options(opts);

        std::ostringstream header;
        auto binding = [&](spirv_cross::ID id) { return c.get_decoration(id, spv::DecorationBinding); };
        auto strip = [&](spirv_cross::ID id) {
            c.unset_decoration(id, spv::DecorationBinding);
            c.unset_decoration(id, spv::DecorationDescriptorSet);
        };

        // Combined image samplers first: they change the resource list.
        const spirv_cross::VariableID dummySampler = c.build_dummy_sampler_for_combined_images();
        c.build_combined_image_samplers();
        spirv_cross::ShaderResources res = c.get_shader_resources();

        for (const auto& r : res.uniform_buffers) {
            const std::uint32_t b = binding(r.id);
            const std::string name = "gtb_b" + std::to_string(registerSlot(b));
            c.set_name(r.base_type_id, name);
            header << "// gt:ubo " << name << ' ' << registerSlot(b) << '\n';
            strip(r.id);
        }
        for (const auto& r : res.push_constant_buffers) {
            c.set_name(r.base_type_id, "gtb_push");
            header << "// gt:push gtb_push\n";
        }
        for (const auto& r : res.storage_buffers) {
            const std::uint32_t b = binding(r.id);
            const char cls = registerClass(b);
            const std::string name = std::string("gts_") + cls + std::to_string(registerSlot(b));
            c.set_name(r.base_type_id, name);
            header << "// gt:ssbo " << name << ' ' << cls << registerSlot(b) << '\n';
            strip(r.id);
        }
        for (const auto& r : res.storage_images) {
            const std::uint32_t b = binding(r.id);
            const std::string name = "gti_u" + std::to_string(registerSlot(b));
            c.set_name(r.id, name);
            header << "// gt:image " << name << ' ' << registerSlot(b) << '\n';
            strip(r.id);
        }
        for (const auto& s : c.get_combined_image_samplers()) {
            const std::uint32_t t = registerSlot(binding(s.image_id));
            const bool noSampler = s.sampler_id == dummySampler;
            const std::string smp = noSampler ? "-" : std::to_string(registerSlot(binding(s.sampler_id)));
            const std::string name =
                "gtc_t" + std::to_string(t) + "_s" + (noSampler ? std::string("x") : smp);
            c.set_name(s.combined_id, name);
            header << "// gt:tex " << name << ' ' << t << ' ' << smp << '\n';
            strip(s.combined_id);
        }
        if (!res.acceleration_structures.empty())
            throw std::runtime_error("ray tracing has no OpenGL equivalent");

        // Stage interface names by location (built-ins keep theirs).
        for (const auto& r : res.stage_inputs) {
            const std::uint32_t loc = c.get_decoration(r.id, spv::DecorationLocation);
            c.set_name(r.id, (model == spv::ExecutionModelVertex ? "gta_" : "gtv_") + std::to_string(loc));
        }
        for (const auto& r : res.stage_outputs) {
            const std::uint32_t loc = c.get_decoration(r.id, spv::DecorationLocation);
            c.set_name(r.id, (model == spv::ExecutionModelFragment ? "gto_" : "gtv_") + std::to_string(loc));
        }

        std::string glsl = c.compile();
        if (model == spv::ExecutionModelTessellationControl) {
            // GLSL requires per-vertex outputs to be indexed with gl_InvocationID itself, not a copy.
            static const std::regex copy(R"((?:uint|int) (_\d+) = (?:uint\()?gl_InvocationID\)?;)");
            std::vector<std::string> temps;
            for (std::sregex_iterator it(glsl.begin(), glsl.end(), copy), end; it != end; ++it)
                temps.push_back((*it)[1]);
            for (const std::string& t : temps)
                glsl = std::regex_replace(glsl, std::regex("\\[" + t + "\\]"), "[gl_InvocationID]");
        }
        std::ofstream out(argv[2], std::ios::binary);
        if (!out) throw std::runtime_error(std::string("cannot write ") + argv[2]);
        // #version must stay the first line; the binding table follows it.
        const std::size_t eol = glsl.find('\n');
        out << glsl.substr(0, eol + 1) << header.str() << glsl.substr(eol + 1);
        return out ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gt_glslgen: %s: %s\n", argv[1], e.what());
        return 1;
    }
}
