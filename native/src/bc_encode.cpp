// BC1/BC3 encoding: endpoints along the block's principal colour axis, then one least-squares refinement of
// the endpoints for the chosen indices (kept only if it lowers the error).
#include "bc_encode.h"

#include <algorithm>
#include <cmath>

namespace rn {

namespace {

uint16_t To565(const float c[3]) {
    const int r = std::clamp(int(c[0] * (31.0f / 255.0f) + 0.5f), 0, 31);
    const int g = std::clamp(int(c[1] * (63.0f / 255.0f) + 0.5f), 0, 63);
    const int b = std::clamp(int(c[2] * (31.0f / 255.0f) + 0.5f), 0, 31);
    return static_cast<uint16_t>(r << 11 | g << 5 | b);
}

void From565(uint16_t c, int out[3]) {
    const int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    out[0] = r << 3 | r >> 2;
    out[1] = g << 2 | g >> 4;
    out[2] = b << 3 | b >> 2;
}

// Indices for 4-colour endpoints c0 > c1 (or c0 == c1, all index 0); returns the squared error. Each texel is
// projected onto the c0 -> c1 line and rounded to the nearest of the four palette steps along it.
uint32_t Fit(const uint8_t t[16][4], uint16_t c0, uint16_t c1, uint32_t* indices) {
    int p[4][3];
    From565(c0, p[0]);
    From565(c1, p[1]);
    for (int k = 0; k < 3; ++k) {
        p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
        p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
    }
    uint32_t bits = 0, error = 0;
    const int d[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const int dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    static constexpr uint32_t kStepIndex[4] = {0, 2, 3, 1};  // steps from c0 to c1 -> BC1 index
    for (int i = 0; i < 16; ++i) {
        int step = 0;
        if (dd > 0) {
            const int dot = (t[i][0] - p[0][0]) * d[0] + (t[i][1] - p[0][1]) * d[1] + (t[i][2] - p[0][2]) * d[2];
            step = std::clamp((dot * 3 + dd / 2) / dd, 0, 3);
        }
        const uint32_t index = kStepIndex[step];
        const int* c = p[index];
        const int er = t[i][0] - c[0], eg = t[i][1] - c[1], eb = t[i][2] - c[2];
        error += uint32_t(er * er + eg * eg + eb * eb);
        bits |= index << (2 * i);
    }
    *indices = bits;
    return error;
}

// Orders quantized endpoints for 4-colour mode.
void Order(uint16_t& c0, uint16_t& c1) {
    if (c0 < c1)
        std::swap(c0, c1);
}

void Write(uint16_t c0, uint16_t c1, uint32_t indices, uint8_t out[8]) {
    out[0] = uint8_t(c0), out[1] = uint8_t(c0 >> 8), out[2] = uint8_t(c1), out[3] = uint8_t(c1 >> 8);
    for (int k = 0; k < 4; ++k)
        out[4 + k] = uint8_t(indices >> (8 * k));
}

void EncodeAlpha(const uint8_t t[16][4], uint8_t out[8]) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i)
        lo = std::min<int>(lo, t[i][3]), hi = std::max<int>(hi, t[i][3]);
    out[0] = uint8_t(hi);
    out[1] = uint8_t(lo);
    uint64_t bits = 0;
    if (hi != lo) {  // 8-value mode (a0 > a1); equal endpoints decode index 0 as a0
        int palette[8] = {hi, lo};
        for (int i = 2; i < 8; ++i)
            palette[i] = ((8 - i) * hi + (i - 1) * lo) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 1 << 30, choice = 0;
            for (int j = 0; j < 8; ++j) {
                const int d = std::abs(t[i][3] - palette[j]);
                if (d < best)
                    best = d, choice = j;
            }
            bits |= uint64_t(choice) << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k)
        out[2 + k] = uint8_t(bits >> (8 * k));
}

}  // namespace

void EncodeBc1(const uint8_t t[16][4], uint8_t out[8]) {
    float mean[3] = {};
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k)
            mean[k] += t[i][k];
    for (float& m : mean)
        m /= 16.0f;
    float cov[3][3] = {};
    for (int i = 0; i < 16; ++i) {
        const float d[3] = {t[i][0] - mean[0], t[i][1] - mean[1], t[i][2] - mean[2]};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                cov[a][b] += d[a] * d[b];
    }
    // Principal axis by power iteration, starting from the channel with the largest variance.
    const int start = cov[0][0] >= cov[1][1] ? (cov[0][0] >= cov[2][2] ? 0 : 2) : (cov[1][1] >= cov[2][2] ? 1 : 2);
    float axis[3] = {cov[start][0], cov[start][1], cov[start][2]};
    for (int iteration = 0; iteration < 3; ++iteration) {
        float next[3];
        for (int a = 0; a < 3; ++a)
            next[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
        const float scale = std::max({std::fabs(next[0]), std::fabs(next[1]), std::fabs(next[2])});
        if (scale < 1e-6f)
            break;
        for (int a = 0; a < 3; ++a)
            axis[a] = next[a] / scale;
    }
    const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    float lo = 0, hi = 0;
    if (length > 1e-6f) {
        for (float& a : axis)
            a /= length;
        lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 16; ++i) {
            const float s = (t[i][0] - mean[0]) * axis[0] + (t[i][1] - mean[1]) * axis[1] + (t[i][2] - mean[2]) * axis[2];
            lo = std::min(lo, s), hi = std::max(hi, s);
        }
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k) {
        e0[k] = mean[k] + axis[k] * hi;
        e1[k] = mean[k] + axis[k] * lo;
    }
    uint16_t c0 = To565(e0), c1 = To565(e1);
    Order(c0, c1);
    uint32_t indices;
    uint32_t error = Fit(t, c0, c1, &indices);

    if (error > 0 && c0 != c1) {
        // Least squares: texel ~ w * end0 + (1 - w) * end1 for each index's weight w.
        static constexpr float kWeight[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        float aa = 0, ab = 0, bb = 0, x0[3] = {}, x1[3] = {};
        for (int i = 0; i < 16; ++i) {
            const float w = kWeight[(indices >> (2 * i)) & 3], v = 1.0f - w;
            aa += w * w, ab += w * v, bb += v * v;
            for (int k = 0; k < 3; ++k)
                x0[k] += w * t[i][k], x1[k] += v * t[i][k];
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) > 1e-6f) {
            float f0[3], f1[3];
            for (int k = 0; k < 3; ++k) {
                f0[k] = (bb * x0[k] - ab * x1[k]) / det;
                f1[k] = (aa * x1[k] - ab * x0[k]) / det;
            }
            uint16_t r0 = To565(f0), r1 = To565(f1);
            Order(r0, r1);
            uint32_t refined;
            const uint32_t refined_error = Fit(t, r0, r1, &refined);
            if (refined_error < error)
                c0 = r0, c1 = r1, indices = refined;
        }
    }
    Write(c0, c1, indices, out);
}

void EncodeBc3(const uint8_t t[16][4], uint8_t out[16]) {
    EncodeAlpha(t, out);
    EncodeBc1(t, out + 8);
}

}  // namespace rn
