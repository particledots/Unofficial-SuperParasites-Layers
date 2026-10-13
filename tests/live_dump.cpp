// Prints a checksum of the plug-in engine's output for every mode x quality x buffer length x engine rate, with a fixed test
// signal and knob moves. It only uses the HD plug-in's own controls (no layers), so it can be built against the HD plug-in's
// code and against this one: if the two lists are identical, Live here produces exactly the same samples as HD.
#include "SuperParasitesEngine.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

static uint32_t lcg = 12345;
static float noise() { lcg = lcg * 1664525u + 1013904223u; return ((lcg >> 8) & 0xffff) / 32768.0f - 1.0f; }

int main (int argc, char** argv)
{
    const double hostRate = 48000.0;
    const int seconds = argc > 1 ? atoi (argv[1]) : 4;
    const int lengths[2] = { 0, 2 };
    for (int rate = 0; rate < 2; ++rate)
    for (int mode = 0; mode < 8; ++mode)
    for (int quality = 0; quality < 4; ++quality)
    for (int li = 0; li < 2; ++li)
    {
        const int len = lengths[li];
        SuperParasitesEngine e; e.prepare (hostRate);
        lcg = 12345;
        uint64_t h = 1469598103934665603ull;
        double phase = 0.0;
        const int block = 480;
        for (int b = 0; b < seconds * 100; ++b)
        {
            std::vector<float> inL (block), inR (block), outL (block), outR (block);
            for (int i = 0; i < block; ++i)
            {
                phase += 2.0 * 3.14159265358979 * 220.0 / hostRate;
                const float env = (b % 100) < 40 ? 1.0f : 0.15f;
                inL[i] = (0.4f * (float) std::sin (phase) + 0.1f * noise()) * env;
                inR[i] = (0.3f * (float) std::sin (phase * 1.5) + 0.1f * noise()) * env;
            }
            SuperParasitesEngine::Params p;
            p.mode = mode; p.quality = quality; p.bufferLength = len; p.engineRate = rate;
            p.dryWet = 0.8f; p.density = 0.3f + 0.6f * (float) ((b / 50) % 2); p.position = 0.2f + 0.1f * (float) ((b / 30) % 7);
            p.size = 0.3f + 0.1f * (float) ((b / 70) % 5); p.texture = 0.4f + 0.2f * (float) ((b / 90) % 3);
            p.reverb = 0.3f; p.feedback = 0.25f; p.pitchPot = 0.5f + 0.05f * (float) ((b / 100) % 3);
            p.freeze = (b / 250) % 2 == 1; p.gate = (b % 211) < 5;
            if (b % 97 == 0) e.capture();
            e.process (inL.data(), inR.data(), outL.data(), outR.data(), block, p);
            for (int i = 0; i < block; ++i)
            {
                uint32_t x, y; std::memcpy (&x, &outL[i], 4); std::memcpy (&y, &outR[i], 4);
                h = (h ^ x) * 1099511628211ull; h = (h ^ y) * 1099511628211ull;
            }
        }
        std::printf ("rate %d mode %d quality %d len %d  %016llx\n", rate, mode, quality, len, (unsigned long long) h);
    }
}
