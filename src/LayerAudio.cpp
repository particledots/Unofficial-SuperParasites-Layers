#include "LayerAudio.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace
{
    uint32_t rd32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
    uint16_t rd16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
    void wr32 (std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) (x >> (8 * i))); }
    void wr16 (std::vector<uint8_t>& v, uint16_t x) { v.push_back ((uint8_t) x); v.push_back ((uint8_t) (x >> 8)); }
    void tag (std::vector<uint8_t>& v, const char* t) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) t[i]); }
}

std::vector<uint8_t> writeWav16 (const LayerAudio& a)
{
    const int nch = a.channels < 2 ? 1 : 2;
    const size_t n = a.frames();
    const uint32_t dataBytes = (uint32_t) (n * (size_t) nch * 2);
    std::vector<uint8_t> v;
    v.reserve (44 + dataBytes);
    tag (v, "RIFF"); wr32 (v, 36 + dataBytes); tag (v, "WAVE");
    tag (v, "fmt "); wr32 (v, 16); wr16 (v, 1); wr16 (v, (uint16_t) nch);
    wr32 (v, (uint32_t) std::lround (a.sampleRate)); wr32 (v, (uint32_t) std::lround (a.sampleRate) * (uint32_t) nch * 2);
    wr16 (v, (uint16_t) (nch * 2)); wr16 (v, 16);
    tag (v, "data"); wr32 (v, dataBytes);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < nch; ++c)
        {
            const float x = a.ch[c][i];
            const int s = std::clamp ((int) std::lround (x * 32768.0f), -32768, 32767);
            wr16 (v, (uint16_t) (int16_t) s);
        }
    return v;
}

bool readWav (const uint8_t* d, size_t size, LayerAudio& out, std::string& err)
{
    out = LayerAudio();
    if (size < 12 || std::memcmp (d, "RIFF", 4) != 0 || std::memcmp (d + 8, "WAVE", 4) != 0)
    { err = "That is not a WAV file."; return false; }

    int format = 0, nch = 0, bits = 0; uint32_t rate = 0;
    bool haveFmt = false;
    const uint8_t* pcm = nullptr; size_t pcmBytes = 0;
    size_t pos = 12;
    while (pos + 8 <= size)
    {
        const uint8_t* ck = d + pos;
        const uint32_t len = rd32 (ck + 4);
        const size_t body = pos + 8;
        const size_t avail = size - body;
        if (std::memcmp (ck, "fmt ", 4) == 0 && len >= 16 && avail >= 16)
        {
            format = rd16 (d + body); nch = rd16 (d + body + 2); rate = rd32 (d + body + 4); bits = rd16 (d + body + 14);
            if (format == 0xFFFE && len >= 26 && avail >= 26) format = rd16 (d + body + 24);   // extensible: the real tag is in the sub-format
            haveFmt = true;
        }
        else if (std::memcmp (ck, "data", 4) == 0)
        {
            pcm = d + body;
            pcmBytes = std::min<size_t> (len, avail);   // a streamed file may claim more than it holds
            break;
        }
        pos = body + (size_t) len + (len & 1);
        if (len > avail) break;
    }
    if (! haveFmt || pcm == nullptr) { err = "That WAV file has no audio data I can read."; return false; }
    if (nch < 1 || rate < 1000 || rate > 400000) { err = "That WAV file has an unusual format I can't read."; return false; }
    const bool isFloat = (format == 3);
    if (! ((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (isFloat && (bits == 32 || bits == 64))))
    { err = "Only PCM or float WAV files are supported (8/16/24/32-bit)."; return false; }

    const size_t bytesPer = (size_t) bits / 8, frameBytes = bytesPer * (size_t) nch;
    const size_t frames = pcmBytes / frameBytes;
    if (frames == 0) { err = "That WAV file is empty."; return false; }
    out.channels = nch >= 2 ? 2 : 1;
    out.sampleRate = (double) rate;
    out.ch[0].resize (frames);
    out.ch[1].resize (out.channels == 2 ? frames : 0);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < out.channels; ++c)
        {
            const uint8_t* s = pcm + i * frameBytes + (size_t) c * bytesPer;
            float x = 0.0f;
            if (isFloat)
            {
                if (bits == 32) { float f; std::memcpy (&f, s, 4); x = f; }
                else { double f; std::memcpy (&f, s, 8); x = (float) f; }
            }
            else if (bits == 8)  x = ((int) s[0] - 128) / 128.0f;
            else if (bits == 16) x = (int16_t) rd16 (s) / 32768.0f;
            else if (bits == 24) { int32_t v = (int32_t) ((uint32_t) s[0] << 8 | (uint32_t) s[1] << 16 | (uint32_t) s[2] << 24); x = (float) (v / 2147483648.0); }
            else                 x = (float) ((int32_t) rd32 (s) / 2147483648.0);
            if (! std::isfinite (x)) x = 0.0f;
            out.ch[c][i] = x;
        }
    return true;
}
