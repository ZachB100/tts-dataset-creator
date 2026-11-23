#include "DSP.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ttsdc {

namespace {
double besselI0(double x) {
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 50; ++k) {
        term *= q / (k * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
} // namespace

std::vector<float> resample(const float* in, size_t n, double inRate, double outRate) {
    if (n == 0) return {};
    if (std::abs(inRate - outRate) < 1e-6) return std::vector<float>(in, in + n);

    const double ratio = outRate / inRate;
    const double cutoff = std::min(1.0, ratio) * 0.97; // relative to input Nyquist
    const int half = 24;                               // taps on each side (at input rate, scaled when downsampling)
    const double beta = 8.0;
    const int phases = 1024;
    const double scale = std::min(1.0, ratio);
    const int halfTaps = static_cast<int>(std::ceil(half / scale));

    // kernel table over [0, halfTaps] in input-sample units, `phases` points per sample
    std::vector<float> table(static_cast<size_t>(halfTaps) * phases + 2);
    const double i0b = besselI0(beta);
    for (size_t k = 0; k < table.size(); ++k) {
        double x = static_cast<double>(k) / phases; // distance in input samples
        double sinc = x == 0 ? 1.0 : std::sin(M_PI * x * cutoff) / (M_PI * x * cutoff);
        double r = x / halfTaps;
        double w = r >= 1 ? 0 : besselI0(beta * std::sqrt(1 - r * r)) / i0b;
        table[k] = static_cast<float>(sinc * w * cutoff);
    }
    auto kernel = [&](double d) {
        d = std::abs(d) * phases;
        size_t i = static_cast<size_t>(d);
        if (i + 1 >= table.size()) return 0.0f;
        float f = static_cast<float>(d - i);
        return table[i] + (table[i + 1] - table[i]) * f;
    };

    const size_t outN = static_cast<size_t>(std::floor(n * ratio));
    std::vector<float> out(outN);
    for (size_t j = 0; j < outN; ++j) {
        double t = j / ratio; // position in input samples
        long c = static_cast<long>(std::floor(t));
        long lo = std::max(0L, c - halfTaps + 1), hi = std::min(static_cast<long>(n) - 1, c + halfTaps);
        double acc = 0;
        for (long k = lo; k <= hi; ++k) acc += in[k] * kernel(t - k);
        out[j] = static_cast<float>(acc);
    }
    return out;
}

std::vector<float> frameRmsDb(const float* x, size_t n, double rate, double frameSeconds) {
    size_t f = std::max<size_t>(1, static_cast<size_t>(std::lround(rate * frameSeconds)));
    std::vector<float> out((n + f - 1) / f);
    for (size_t i = 0; i < out.size(); ++i) {
        size_t a = i * f, b = std::min(n, a + f);
        double s = 0;
        for (size_t k = a; k < b; ++k) s += static_cast<double>(x[k]) * x[k];
        double rms = std::sqrt(s / std::max<size_t>(1, b - a));
        out[i] = static_cast<float>(20 * std::log10(std::max(rms, 1e-6)));
    }
    return out;
}

void applyFades(std::vector<float>& x, size_t fade) {
    fade = std::min(fade, x.size() / 2);
    for (size_t i = 0; i < fade; ++i) {
        float g = static_cast<float>(i) / fade;
        x[i] *= g;
        x[x.size() - 1 - i] *= g;
    }
}

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

bool writeWav16(const std::string& path, const float* x, size_t n, int rate, std::string& error) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    uint32_t dataBytes = static_cast<uint32_t>(n * 2);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, static_cast<uint32_t>(rate));
    put32(f, static_cast<uint32_t>(rate * 2));
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, dataBytes);
    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        float v = std::clamp(x[i], -1.0f, 1.0f);
        pcm[i] = static_cast<int16_t>(std::lround(v * 32767.0f));
    }
    bool ok = fwrite(pcm.data(), 2, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok) error = "short write " + path;
    return ok;
}

bool readWav16(const std::string& path, std::vector<float>& out, int& rate, std::string& error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    char hdr[44];
    if (fread(hdr, 1, 44, f) != 44 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f);
        error = "not a canonical WAV";
        return false;
    }
    uint16_t ch, bits;
    uint32_t sr, bytes;
    memcpy(&ch, hdr + 22, 2);
    memcpy(&sr, hdr + 24, 4);
    memcpy(&bits, hdr + 34, 2);
    memcpy(&bytes, hdr + 40, 4);
    if (ch != 1 || bits != 16) {
        fclose(f);
        error = "expected mono 16-bit";
        return false;
    }
    std::vector<int16_t> pcm(bytes / 2);
    size_t got = fread(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    out.resize(got);
    for (size_t i = 0; i < got; ++i) out[i] = pcm[i] / 32768.0f;
    rate = static_cast<int>(sr);
    return true;
}

} // namespace ttsdc
