// Plain audio container used to move a layer's audio in and out of the plug-in (JUCE-free so it can be tested).
#pragma once
#include <vector>
#include <cstdint>
#include <string>

struct LayerAudio
{
    int channels = 0;            // 1 or 2 (a file with more channels keeps its first two)
    double sampleRate = 0.0;     // Hz
    std::vector<float> ch[2];    // one vector per channel, same length (-1..+1)
    size_t frames() const { return ch[0].size(); }
};

// 16-bit PCM WAV bytes for the audio (what "Save Layer" writes).
std::vector<uint8_t> writeWav16 (const LayerAudio& audio);

// Reads WAV bytes: 8/16/24/32-bit integer PCM or 32/64-bit float, any sample rate, mono or more channels.
// On failure returns false and sets `error` to a sentence for the user.
bool readWav (const uint8_t* data, size_t size, LayerAudio& out, std::string& error);
