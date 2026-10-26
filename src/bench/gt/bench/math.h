#pragma once
// Minimal column-major math for test setup (matches Slang's column-major matrices:
// shaders compute mul(M, v)). Clip space: D3D/Vulkan conventions, depth 0..1, y up.

#include <cmath>

namespace gt::bench {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}

struct Mat4 {
    float m[4][4] = {}; ///< m[column][row]
    static Mat4 identity() {
        Mat4 r;
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k][rr] * b.m[c][k];
            r.m[c][rr] = s;
        }
    return r;
}

/// Right-handed view matrix (camera looks down -Z).
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r = Mat4::identity();
    r.m[0][0] = s.x;
    r.m[1][0] = s.y;
    r.m[2][0] = s.z;
    r.m[0][1] = u.x;
    r.m[1][1] = u.y;
    r.m[2][1] = u.z;
    r.m[0][2] = -f.x;
    r.m[1][2] = -f.y;
    r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] = dot(f, eye);
    return r;
}

/// Right-handed perspective, depth mapped to [0, 1].
inline Mat4 perspective(float fovyRadians, float aspect, float zNear, float zFar) {
    const float t = 1.0f / std::tan(fovyRadians * 0.5f);
    Mat4 r;
    r.m[0][0] = t / aspect;
    r.m[1][1] = t;
    r.m[2][2] = zFar / (zNear - zFar);
    r.m[2][3] = -1.0f;
    r.m[3][2] = zNear * zFar / (zNear - zFar);
    return r;
}

inline constexpr float kPi = 3.14159265358979f;

} // namespace gt::bench
