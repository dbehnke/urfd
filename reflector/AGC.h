#pragma once

#include <cstdint>
#include <vector>
#include <cmath>

class CAGC
{
public:
    CAGC();
    ~CAGC() = default;

    // Process a block of audio samples in-place
    // Each sample is assumed to be 16-bit signed PCM
    // Size is typically 160 samples (AMBE frame) or 320 samples (Codec2)
    void Process(int16_t* samples, size_t count);

    // Set AGC enablement state
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    // Set Target Level (Linear 0.0-1.0)
    void SetTargetLevel(float level) { m_target_level = level; }

private:
    bool m_enabled;
    float m_gain;
    float m_peak_env;
    
    // Configurable parameters (could be exposed later)
    float m_target_level;    // e.g. -3 dBFS = 0.707 linear
    float m_max_gain;        // Max boost (e.g. 12dB = 4.0x)
    float m_attack_coeff;    // Gain reduction speed
    float m_release_coeff;   // Gain recovery speed
};
