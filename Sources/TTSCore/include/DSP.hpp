// Small audio helpers: resampling, RMS framing, fades, WAV writing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ttsdc {

/// Band-limited (Kaiser-windowed sinc) resampler for mono float audio.
std::vector<float> resample(const float* in, size_t n, double inRate, double outRate);

/// RMS level in dBFS of consecutive frames of `frameSeconds` (default 10 ms).
std::vector<float> frameRmsDb(const float* x, size_t n, double rate, double frameSeconds = 0.01);

/// Linear fade-in/out of `fadeSamples` at each end, in place.
void applyFades(std::vector<float>& x, size_t fadeSamples);

/// Write mono 16-bit PCM WAV (with TPDF dither off; values are clipped to [-1, 1]).
bool writeWav16(const std::string& path, const float* x, size_t n, int rate, std::string& error);

/// Read a mono 16-bit PCM WAV written by writeWav16 (used by tests and the CLI's checks).
bool readWav16(const std::string& path, std::vector<float>& out, int& rate, std::string& error);

} // namespace ttsdc
