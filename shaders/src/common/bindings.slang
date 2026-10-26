// Binding conventions shared by every shader (see docs/DECISIONS.md):
//   HLSL registers b#/t#/u#/s# in space 0. Vulkan bindings are derived by slangc
//   shifts: b -> 0+#, t -> 16+#, u -> 32+#, s -> 48+#, all in descriptor set 0.
//   Push constants live at b13: Vulkan push constants, D3D12 root constants,
//   an ordinary constant buffer / UBO on D3D11 and OpenGL.
#pragma once

#define GT_PUSH_CONSTANTS(Type, name) [[vk::push_constant]] ConstantBuffer<Type> name : register(b13)

static const float GT_PI = 3.14159265358979;

// PCG hash: deterministic, cheap, identical on every API.
uint gtHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float gtHashFloat(uint v) { return float(gtHash(v)) * (1.0 / 4294967296.0); }
