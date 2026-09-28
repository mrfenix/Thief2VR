#pragma once
// Minimal 3D math for converting OpenXR poses into Dark engine camera values.
#include <cmath>
#include <cstdint>

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// Row-major 3x3 rotation, m[row][col]; columns are the rotated basis axes.
struct Mat3 {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

inline Mat3 operator*(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

inline Vec3 operator*(const Mat3& a, Vec3 v)
{
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

inline Mat3 Transpose(const Mat3& a)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[j][i];
    return r;
}

inline Mat3 FromQuat(float x, float y, float z, float w)
{
    Mat3 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z);
    r.m[0][1] = 2 * (x * y - z * w);
    r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w);
    r.m[1][1] = 1 - 2 * (x * x + z * z);
    r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w);
    r.m[2][1] = 2 * (y * z + x * w);
    r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

inline Mat3 RotZ(float a)
{
    float c = std::cos(a), s = std::sin(a);
    Mat3 r;
    r.m[0][0] = c;
    r.m[0][1] = -s;
    r.m[1][0] = s;
    r.m[1][1] = c;
    return r;
}

inline Mat3 RotY(float a)
{
    float c = std::cos(a), s = std::sin(a);
    Mat3 r;
    r.m[0][0] = c;
    r.m[0][2] = s;
    r.m[2][0] = -s;
    r.m[2][2] = c;
    return r;
}

inline Mat3 RotX(float a)
{
    float c = std::cos(a), s = std::sin(a);
    Mat3 r;
    r.m[1][1] = c;
    r.m[1][2] = -s;
    r.m[2][1] = s;
    r.m[2][2] = c;
    return r;
}

// OpenXR (x right, y up, z back) -> engine (x forward, y left, z up).
inline Mat3 XrToEngineBasis()
{
    Mat3 c;
    c.m[0][0] = 0; c.m[0][1] = 0;  c.m[0][2] = -1;  // forward = -z
    c.m[1][0] = -1; c.m[1][1] = 0; c.m[1][2] = 0;   // left = -x
    c.m[2][0] = 0; c.m[2][1] = 1;  c.m[2][2] = 0;   // up = y
    return c;
}

constexpr float kPi = 3.14159265358979f;

inline uint16_t RadToAngle16(float rad)
{
    float turns = rad / (2 * kPi);
    turns -= std::floor(turns);
    return (uint16_t)(int)(turns * 65536.0f + 0.5f);
}

inline float Angle16ToRad(uint16_t a)
{
    return a * (2 * kPi / 65536.0f);
}

// Decompose m = RotZ(heading) * RotY(pitch) * RotX(bank) (engine convention).
inline void ToHeadingPitchBank(const Mat3& m, float& heading, float& pitch, float& bank)
{
    float sp = -m.m[2][0];
    sp = sp > 1 ? 1 : sp < -1 ? -1 : sp;
    pitch = std::asin(sp);
    heading = std::atan2(m.m[1][0], m.m[0][0]);
    bank = std::atan2(m.m[2][1], m.m[2][2]);
}

// --- Quaternions (x, y, z, w), OpenXR convention ---
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 Rotate(const Quat& q, Vec3 v)
{
    // v + 2w(q x v) + 2 q x (q x v)
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    Vec3 c{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}

inline Quat Conjugate(const Quat& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

// Rotation about OpenXR's +Y (up) axis.
inline Quat YawQuat(float radians)
{
    return {0, std::sin(radians / 2), 0, std::cos(radians / 2)};
}

inline float Dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Heading (rotation about up) of an engine-space orientation's forward axis.
inline float HeadingOf(const Mat3& m)
{
    return std::atan2(m.m[1][0], m.m[0][0]);
}
