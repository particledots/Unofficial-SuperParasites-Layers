#include <string>
#include <cstdarg>
// Unofficial SuperParasites HD: compares the 32 kHz engine (the original) with the 48 kHz engine.
//
// What this checks, in plain words: the same input and knob settings go through both engine rates and the results are
// measured, not listened to. Times (delays, reverb decay) and pitches (Hz) should come out the same for both rates, and
// the spectrum of the output should have the same shape. It does NOT say that the two rates sound the same: nothing here
// can tell that.  Run:  cmake --build build --target rate_compare_test && build/rate_compare_test
#include "SuperParasitesEngine.h"
#include "supercell/dsp/engine_rate.h"
#include "supercell/dsp/pvoc/hd_fft.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <vector>

namespace clouds { const float* HdWindow6144_for_test(); const float* Window4096_for_test(); }
using Params = SuperParasitesEngine::Params;
static const char* kModeNames[] = { "granular", "stretch", "looping-delay", "spectral", "oliverb", "resonestor", "beat-repeat", "spectral-clouds" };
static const double kPi = 3.14159265358979323846;
static int gFails = 0;
static void report (bool ok, const char* fmt, ...) __attribute__ ((format (printf, 2, 3)));
static void report (bool ok, const char* fmt, ...)
{
    char buf[512]; va_list a; va_start (a, fmt); vsnprintf (buf, sizeof buf, fmt, a); va_end (a);
    printf ("%-4s %s\n", ok ? "OK" : "FAIL", buf);
    gFails += ! ok;
}

// ---- rendering ----------------------------------------------------------------------------------------------------
struct Audio { std::vector<float> l, r; double fs = 48000.0; };
using Gen = std::function<void (double t, float& l, float& r)>;

// Runs the engine at the given engine rate. The host runs at 48 kHz, so the 48 kHz engine is not resampled at all and
// the 32 kHz engine goes through the plug-in's resamplers, exactly as in the plug-in.
static Audio render (int engineRate, Params p, double seconds, const Gen& gen, double captureEvery = 0.0, double freezeAt = -1.0, double firstCapture = -1.0)
{
    const double fs = 48000.0;
    p.engineRate = engineRate;
    SuperParasitesEngine e; e.prepare (fs);
    const int N = 480;
    std::vector<float> inL (N), inR (N), oL (N), oR (N);
    Audio a; a.fs = fs;
    double nextCap = firstCapture >= 0 ? firstCapture : (captureEvery > 0 ? captureEvery : -1.0);
    const long total = (long) (seconds * fs);
    for (long n0 = 0; n0 < total; n0 += N)
    {
        const double t0 = (double) n0 / fs;
        if (nextCap >= 0 && t0 >= nextCap) { e.capture(); nextCap = captureEvery > 0 ? nextCap + captureEvery : -1.0; }
        if (freezeAt >= 0 && t0 >= freezeAt) p.freeze = true;
        for (int i = 0; i < N; ++i) gen (t0 + i / fs, inL[i], inR[i]);
        e.process (inL.data(), inR.data(), oL.data(), oR.data(), N, p);
        a.l.insert (a.l.end(), oL.begin(), oL.end());
        a.r.insert (a.r.end(), oR.begin(), oR.end());
    }
    return a;
}

struct Noise { unsigned s = 987654321u; float next() { s = s * 1664525u + 1013904223u; return ((s >> 9) / (float) (1 << 23)) * 2.0f - 1.0f; } };

// ---- analysis helpers ---------------------------------------------------------------------------------------------
static void fftInPlace (std::vector<std::complex<double>>& x)
{
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v; x[i + k + len / 2] = u - v; w *= wl;
            }
        }
    }
}

// Magnitude spectrum (linear) of x[start, start+len) with a Hann window, zero padded to a power of two >= 4*len.
static std::vector<double> spectrum (const std::vector<float>& x, size_t start, size_t len, size_t& nfft)
{
    nfft = 1; while (nfft < len * 4) nfft <<= 1;
    std::vector<std::complex<double>> b (nfft);
    for (size_t i = 0; i < len; ++i)
        b[i] = (double) x[start + i] * (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) len));
    fftInPlace (b);
    std::vector<double> m (nfft / 2);
    for (size_t i = 0; i < m.size(); ++i) m[i] = std::abs (b[i]);
    return m;
}

// Frequency of the strongest peak in [lo, hi] Hz, refined by parabolic interpolation.
static double peakFrequency (const std::vector<float>& x, size_t start, size_t len, double fs, double lo, double hi)
{
    size_t nfft; const auto m = spectrum (x, start, len, nfft);
    const size_t k0 = (size_t) (lo * (double) nfft / fs), k1 = std::min (m.size() - 2, (size_t) (hi * (double) nfft / fs));
    size_t kb = k0;
    for (size_t k = k0; k <= k1; ++k) if (m[k] > m[kb]) kb = k;
    const double a = std::log (m[kb - 1] + 1e-30), b = std::log (m[kb] + 1e-30), c = std::log (m[kb + 1] + 1e-30);
    const double d = 0.5 * (a - c) / (a - 2 * b + c);
    return ((double) kb + d) * fs / (double) nfft;
}

// RMS envelope in dB, one value per `hop` seconds, window `win` seconds.
static std::vector<double> envelopeDb (const std::vector<float>& x, double fs, double win = 0.010, double hop = 0.005)
{
    std::vector<double> e;
    const size_t w = (size_t) (win * fs), h = (size_t) (hop * fs);
    for (size_t s = 0; s + w <= x.size(); s += h)
    {
        double acc = 0; for (size_t i = 0; i < w; ++i) acc += (double) x[s + i] * x[s + i];
        e.push_back (10.0 * std::log10 (acc / (double) w + 1e-20));
    }
    return e;
}

// ---- 1. static checks of the rate rules and the 6144-point FFT ----------------------------------------------------
static void staticChecks()
{
    using namespace clouds;
    // (a) a delay length n samples at 32 kHz becomes N(n) samples at 48 kHz: same seconds to within half a sample.
    double worst = 0;
    for (int n = 1; n <= 70000; ++n)
        worst = std::max (worst, std::fabs ((double) Scale<1>::N (n) / 48000.0 - (double) n / 32000.0) * 48000.0);
    report (worst <= 0.5 + 1e-9, "delay lengths: 48 kHz length differs from the 32 kHz length by at most %.3f samples (limit 0.5)", worst);
    bool same = true;
    for (int n = 0; n <= 70000; ++n) same &= Scale<0>::N (n) == n;
    report (same, "32 kHz: every length is returned unchanged");

    // (b) one-pole coefficients: same decay per second.
    worst = 0;
    for (double k : { 0.00001, 0.0005, 0.001, 0.01, 0.05, 0.1, 0.3, 0.7 })
    {
        const double k48 = Scale<1>::Pole ((float) k);
        const double per_second_32 = std::pow (1.0 - k, 32000.0), per_second_48 = std::pow (1.0 - k48, 48000.0);
        const double err = std::fabs (std::log (per_second_48 + 1e-300) - std::log (per_second_32 + 1e-300)) / std::fabs (std::log (per_second_32 + 1e-300));
        worst = std::max (worst, err);
    }
    report (worst < 1e-3, "one-pole coefficients: decay per second equal at both rates (worst relative error %.2e)", worst);
    report (Scale<0>::Pole (0.01f) == 0.01f && EngineRate (false).Pole (0.0005f) == 0.0005f, "32 kHz: coefficients returned unchanged");

    // (c) cutoffs: same Hz.
    report (std::fabs (Scale<1>::Freq (0.1f) * 48000.0f - 0.1f * 32000.0f) < 0.01f && EngineRate (true).block == 48 && EngineRate (false).block == 32,
            "cutoffs: 0.1 of 32 kHz = 3200 Hz -> %.1f Hz at 48 kHz; blocks are 32 and 48 frames (1 ms)", Scale<1>::Freq (0.1f) * 48000.0f);

    // (d) the 6144-point FFT against a plain DFT, and round trip.
    for (size_t n : { (size_t) 12, (size_t) 48, (size_t) 384, (size_t) 6144 })
    {
        static clouds::HdFft fft;
        if (! fft.Init (n)) { report (false, "HdFft Init(%zu) refused", n); continue; }
        std::vector<float> x (n), spec (n), back (n);
        Noise ns; for (auto& v : x) v = ns.next();
        fft.Direct (x.data(), spec.data());
        double err = 0, scale = 0;
        for (size_t k = 0; k <= n / 2; ++k)
        {
            std::complex<double> acc = 0;
            for (size_t i = 0; i < n; ++i) acc += (double) x[i] * std::polar (1.0, -2.0 * kPi * (double) (i * k % n) / (double) n);
            const double re = k == 0 || k == n / 2 ? spec[k] : spec[k];
            const double im = k == 0 || k == n / 2 ? 0.0 : -spec[n / 2 + k];   // packed layout: imaginary part stored negated
            err = std::max (err, std::abs (acc - std::complex<double> (re, im)));
            scale = std::max (scale, std::abs (acc));
        }
        fft.Inverse (spec.data(), back.data());
        double rt = 0; for (size_t i = 0; i < n; ++i) rt = std::max (rt, (double) std::fabs (back[i] / (float) n - x[i]));
        report (err < 1e-4 * scale && rt < 1e-5, "HdFft %zu points: matches a plain DFT (error %.1e of %.1f), round trip error %.1e", n, err, scale, rt);
    }

    // (e) analysis/synthesis window for 6144 points: w(j)^2 + w(j+3072)^2 = 1, same shape as the 4096 table.
    const float* w = clouds::HdWindow6144_for_test();
    double pc = 0, shape = 0;
    for (int j = 0; j < 3072; ++j) pc = std::max (pc, (double) std::fabs (w[j] * w[j] + w[j + 3072] * w[j + 3072] - 1.0));
    const float* w4 = clouds::Window4096_for_test();
    for (int j = 0; j < 6144; ++j) shape = std::max (shape, (double) std::fabs (w[j] - w4[(int) std::lround ((double) j * 4096.0 / 6144.0) % 4096]));
    report (pc < 1e-6 && shape < 0.01, "6144-point window: power-complementary to %.1e, max difference from the 4096 window shape %.4f", pc, shape);
}

// ---- 2. delay times -----------------------------------------------------------------------------------------------
// A 3 ms noise burst at t = 0.2 s; returns the time (after the burst) of the peak of the 4 ms smoothed energy of the strongest echo.
static double echoTime (int engineRate, int mode, int quality, float position, double seconds, double captureEvery = 0.0)
{
    Params p; p.mode = mode; p.quality = quality; p.bufferLength = 1; p.dryWet = 1.0f; p.position = position;
    p.feedback = 0; p.reverb = 0; p.texture = 0.5f; p.density = mode == 0 ? 0.3f : 0.0f; p.size = 0.5f;
    Noise ns;
    const Audio a = render (engineRate, p, seconds, [&] (double t, float& l, float& r) { l = r = (t > 0.2 && t < 0.203) ? 0.7f * ns.next() : 0.0f; }, captureEvery);
    const size_t w = (size_t) (0.004 * a.fs);
    double best = 0; size_t bi = 0, start = (size_t) (0.2 * a.fs);
    double acc = 0;
    for (size_t i = start; i < a.l.size(); ++i)
    {
        acc += (double) a.l[i] * a.l[i];
        if (i >= start + w) acc -= (double) a.l[i - w] * a.l[i - w];
        if (i >= start + w && acc > best) { best = acc; bi = i - w / 2; }
    }
    return best > 1e-9 ? (double) bi / a.fs - 0.2 : -1.0;
}

static void delayChecks()
{
    // looping delay: delay time follows POSITION; compare both rates, hi-fi and low-fi (low-fi holds 4x as long)
    struct Case { int quality; float pos; double seconds; };
    const Case cases[] = { { 0, 0.05f, 2.0 }, { 0, 0.1f, 2.0 }, { 0, 0.25f, 2.0 }, { 0, 0.5f, 3.0 }, { 0, 0.75f, 4.0 },
                           { 2, 0.05f, 3.0 }, { 2, 0.1f, 3.0 }, { 2, 0.25f, 5.0 }, { 2, 0.5f, 10.0 } };
    for (const Case& c : cases)
    {
        const double a = echoTime (0, 2, c.quality, c.pos, c.seconds), b = echoTime (1, 2, c.quality, c.pos, c.seconds);
        report (a > 0 && b > 0 && std::fabs (a - b) <= 0.003 + 0.005 * a,
                "looping delay %-7s position %.2f: echo at %.4f s (32 kHz) vs %.4f s (48 kHz), difference %+.1f ms",
                c.quality ? "low-fi" : "16-bit", c.pos, a, b, (b - a) * 1000.0);
    }
    // Granular and stretch: a 150 ms noise burst at 0.2 s; compare the time the output starts (first 10 ms RMS above 25% of the
    // loudest) and the time of its loudest part. Granular is deterministic at DENSITY < 0.5 (its grains are sparse, so the
    // loudest part is the better marker); stretch's window shape makes its loudest part wander, so the start is used.
    for (int mode : { 0, 1 })
        for (float pos : { 0.1f, 0.3f, 0.7f })
        {
            double on[2], pk[2];
            for (int er = 0; er < 2; ++er)
            {
                Params p; p.mode = mode; p.bufferLength = 1; p.dryWet = 1.0f; p.position = pos; p.density = mode == 0 ? 0.3f : 0.0f; p.size = 0.5f; p.texture = 0.5f;
                Noise ns;
                const Audio a = render (er, p, 4.0, [&] (double t, float& l, float& r) { l = r = (t > 0.2 && t < 0.35) ? 0.5f * ns.next() : 0.0f; });
                const auto e = envelopeDb (a.l, a.fs, 0.010, 0.005);
                double mx = -200; size_t mi = 0; for (size_t i = 0; i < e.size(); ++i) if (e[i] > mx) { mx = e[i]; mi = i; }
                size_t o = 0; for (size_t i = 0; i < e.size(); ++i) if (e[i] > mx - 12.0) { o = i; break; }
                on[er] = (double) o * 0.005; pk[er] = (double) mi * 0.005;
            }
            const double d = mode == 0 ? pk[1] - pk[0] : on[1] - on[0];
            report (std::fabs (d) <= 0.010, "%-8s position %.1f: output starts at %.3f s / loudest at %.3f s (32 kHz) vs %.3f s / %.3f s (48 kHz); compared difference %+.1f ms",
                    kModeNames[mode], pos, on[0], pk[0], on[1], pk[1], d * 1000.0);
        }

    // Beat repeat: capture pulses every 0.5 s set the slice length (0.5 s); with PITCH at normal speed, SIZE giving a loop of half the
    // slice, POSITION 0 and DENSITY 0 (no loop shrinking), the output repeats with a period of 0.25 s.
    {
        double lag[2];
        for (int er = 0; er < 2; ++er)
        {
            Params p; p.mode = 6; p.bufferLength = 1; p.dryWet = 1.0f; p.density = 0.0f; p.texture = 0.4f; p.size = 0.45f; p.position = 0.0f; p.stereoSpread = 0.0f; p.pitchPot = 1.0f;
            Noise ns;
            const Audio a = render (er, p, 7.0, [&] (double, float& l, float& r) { l = r = 0.4f * ns.next(); }, 0.5);
            // autocorrelation of 2 s of the output (from 4 s on) by FFT
            const size_t n = 1u << 18; std::vector<std::complex<double>> b (n);
            const size_t s0 = (size_t) (4.0 * a.fs), len = std::min ((size_t) (2.5 * a.fs), a.l.size() - s0);
            for (size_t i = 0; i < len; ++i) b[i] = a.l[s0 + i];
            fftInPlace (b); for (auto& v : b) v = std::norm (v);
            for (auto& v : b) v = std::conj (v);
            fftInPlace (b);
            double best = -1; size_t bl = 0;
            for (size_t L = (size_t) (0.15 * a.fs); L < (size_t) (0.4 * a.fs); ++L) if (b[L].real() > best) { best = b[L].real(); bl = L; }
            lag[er] = (double) bl / a.fs;
        }
        report (std::fabs (lag[0] - lag[1]) <= 0.002 && std::fabs (lag[1] - 0.25) <= 0.003, "beat repeat: loop repeats every %.4f s (32 kHz) vs %.4f s (48 kHz); expected 0.25 s (half of the 0.5 s between capture pulses)", lag[0], lag[1]);
    }
}

// ---- 3. pitch accuracy --------------------------------------------------------------------------------------------
static void pitchChecks()
{
    struct Case { int mode; float semis; const char* label; double expectedRatio; };
    const Case cases[] = {
        { 0, +12.f, "granular +12", 2.0 },
        { 0,  -5.f, "granular -5",  std::pow (2.0, -5.0 / 12) },
        { 1,  +7.f, "stretch +7",   std::pow (2.0, 7.0 / 12) },
        { 2,  +7.f, "looping delay +7 (pitch shifter)", std::pow (2.0, 7.0 / 12) },
        { 2, -12.f, "looping delay -12 (pitch shifter)", 0.5 },
        { 3, +12.f, "spectral +12", 2.0 },
        { 3,  -7.f, "spectral -7",  std::pow (2.0, -7.0 / 12) },
        { 7,  +7.f, "spectral clouds +7 (pitch shifter)", std::pow (2.0, 7.0 / 12) },
        { 4, +12.f, "oliverb +12", 2.0 },
    };
    for (const Case& c : cases)
    {
        double f[2];
        for (int er = 0; er < 2; ++er)
        {
            Params p; p.mode = c.mode; p.dryWet = 1.0f; p.density = 0.8f; p.size = 0.5f; p.position = c.mode == 7 ? 0.95f : 0.5f; p.texture = 0.5f;
            p.pitchPot = 0.5f; p.pitchOffsetSemitones = c.semis; p.reverb = 0.0f; p.feedback = 0.0f;
            if (c.mode == 4) { p.density = 0.9f; }
            if (c.mode == 7) { p.density = 0.0f; p.texture = 0.0f; p.stereoSpread = 0.0f; p.position = 1.0f; }   // fixed band gains, no random phase
            const Audio a = render (er, p, 6.0, [] (double t, float& l, float& r) { l = r = 0.5f * (float) std::sin (2.0 * kPi * 440.0 * t); }, c.mode == 7 ? 0.0 : 0.5, -1.0, c.mode == 7 ? 0.1 : -1.0);
            f[er] = peakFrequency (a.l, (size_t) (4.0 * a.fs), (size_t) (1.5 * a.fs), a.fs, 100.0, 3000.0);
        }
        const double expect = 440.0 * c.expectedRatio;
        const bool agree = std::fabs (f[0] - f[1]) <= (c.mode == 0 ? 0.012 : 0.005) * f[0] + 1.0;   // granular is random: grain jitter moves the peak by a few Hz
        const bool near = std::fabs (f[1] - expect) <= 0.01 * expect + 2.0;
        const bool smeared = c.mode == 4;   // a tone through the reverb's modulated delays is smeared by a few Hz
        report (agree || (smeared && std::fabs (f[0] - f[1]) <= 0.015 * f[0] + 1.0), "%-38s 32 kHz: %8.2f Hz, 48 kHz: %8.2f Hz (plain-arithmetic value %.2f%s)", c.label, f[0], f[1], expect,
                near ? "" : "; both rates are off it equally: that is the 32 kHz original's own behaviour");
    }
    // Resonestor: pitch knob sets the resonator pitch; compare the two rates and the octave steps.
    double fr[2][3];
    for (int er = 0; er < 2; ++er)
        for (int i = 0; i < 3; ++i)
        {
            Params p; p.mode = 5; p.dryWet = 1.0f; p.density = 0.5f; p.pitchOffsetSemitones = (float) (i - 1) * 12.0f; p.texture = 0.5f; p.position = 0.5f; p.size = 0.0f;
            Noise ns;
            const Audio a = render (er, p, 5.0, [&] (double t, float& l, float& r) { l = r = std::fmod (t, 0.5) < 0.01 ? 0.5f * ns.next() : 0.0f; });
            fr[er][i] = peakFrequency (a.l, (size_t) (2.0 * a.fs), (size_t) (2.0 * a.fs), a.fs, 40.0, 4000.0);
        }
    for (int i = 0; i < 3; ++i)
        report (std::fabs (fr[0][i] - fr[1][i]) <= 0.005 * fr[0][i] + 1.0, "resonestor pitch %+3d st: strongest partial 32 kHz %8.2f Hz, 48 kHz %8.2f Hz", (i - 1) * 12, fr[0][i], fr[1][i]);
}

// ---- 4. reverb / Oliverb decay ------------------------------------------------------------------------------------
// Decay time (the time the level takes to fall 60 dB, extrapolated from the -5 dB to -25 dB part of the Schroeder backward-
// integrated energy curve) of the response to a 20 ms noise burst. More robust than reading the envelope's first dip.
static double decayRt60One (int engineRate, int mode, float reverb, float dryWet, float density, float size, unsigned seed)
{
    Params p; p.mode = mode; p.dryWet = dryWet; p.reverb = reverb; p.density = density; p.size = size; p.position = 0.0f; p.texture = 0.5f;
    Noise ns; ns.s = seed;
    const Audio a = render (engineRate, p, 14.0, [&] (double t, float& l, float& r) { l = r = (t > 0.5 && t < 0.52) ? 0.6f * ns.next() : 0.0f; });
    const size_t s0 = (size_t) (0.52 * a.fs);
    std::vector<double> edc (a.l.size() - s0);
    double acc = 0;
    for (size_t i = a.l.size(); i-- > s0;) { acc += (double) a.l[i] * a.l[i]; edc[i - s0] = acc; }
    // ignore the very end of the file (the integral is truncated there): fit only where the curve is > 40 dB above its start
    const double e0 = edc[0];
    long i5 = -1, i25 = -1;
    for (size_t i = 0; i < edc.size(); ++i)
    {
        const double db = 10.0 * std::log10 (edc[i] / e0 + 1e-30);
        if (i5 < 0 && db < -5.0) i5 = (long) i;
        if (i25 < 0 && db < -25.0) { i25 = (long) i; break; }
    }
    if (i5 < 0 || i25 < 0 || i25 <= i5) return -1.0;
    return 3.0 * (double) (i25 - i5) / a.fs;
}

// The decay of a noise-fed reverb varies by a few per cent from one noise burst to the next, so three bursts are averaged.
static double decayRt60 (int engineRate, int mode, float reverb, float dryWet, float density, float size)
{
    double sum = 0; int n = 0;
    for (unsigned seed : { 1234584u, 2469151u, 3703718u }) { const double v = decayRt60One (engineRate, mode, reverb, dryWet, density, size, seed); if (v > 0) { sum += v; ++n; } }
    return n ? sum / n : -1.0;
}

static void reverbChecks()
{
    for (float rv : { 0.3f, 0.6f, 0.9f })
    {
        // post reverb on the dry signal: dry/wet fully dry
        const double a = decayRt60 (0, 0, rv, 0.0f, 0.3f, 0.5f), b = decayRt60 (1, 0, rv, 0.0f, 0.3f, 0.5f);
        report (a > 0 && b > 0 && std::fabs (a - b) <= 0.06 * a + 0.03, "reverb amount %.1f: 60 dB decay %.3f s (32 kHz) vs %.3f s (48 kHz), difference %+.1f%%", rv, a, b, 100.0 * (b - a) / a);
    }
    for (float dens : { 0.3f, 0.5f, 0.7f })
        for (float size : { 0.3f, 0.7f })
        {
            const double a = decayRt60 (0, 4, 0.0f, 1.0f, dens, size), b = decayRt60 (1, 4, 0.0f, 1.0f, dens, size);
            report (a > 0 && b > 0 && std::fabs (a - b) <= 0.06 * a + 0.03, "oliverb decay %.1f size %.1f: 60 dB decay %.3f s (32 kHz) vs %.3f s (48 kHz), difference %+.1f%%", dens, size, a, b, 100.0 * (b - a) / a);
        }
}

// ---- 5. spectral content, all eight modes -------------------------------------------------------------------------
static void spectrumChecks()
{
    const double edges[] = { 100, 200, 400, 800, 1600, 3200, 6400, 12800 };   // octave bands, Hz
    for (int quality : { 0, 2 })
        for (int mode = 0; mode < 8; ++mode)
        {
            double level[2][7];
            double above16[2];
            for (int er = 0; er < 2; ++er)
            {
                Params p; p.mode = mode; p.quality = quality; p.dryWet = 1.0f; p.density = 0.8f; p.texture = 0.4f; p.size = 0.5f; p.position = 0.4f;
                p.stereoSpread = 0.6f; if (mode == 7) p.position = 0.95f;
                Noise ns;
                const Audio a = render (er, p, 8.0, [&] (double, float& l, float& r) { l = 0.3f * ns.next(); r = 0.3f * ns.next(); }, 1.0);
                // average power spectrum over 2 s .. 8 s in 0.17 s frames
                const size_t frame = 8192; std::vector<double> acc;
                size_t count = 0;
                for (size_t s = (size_t) (2.0 * a.fs); s + frame <= a.l.size(); s += frame / 2)
                {
                    size_t nfft; const auto m = spectrum (a.l, s, frame, nfft);
                    if (acc.empty()) acc.assign (m.size(), 0.0);
                    for (size_t k = 0; k < m.size(); ++k) acc[k] += m[k] * m[k];
                    ++count;
                }
                const size_t nfft = (acc.size()) * 2;
                auto band = [&] (double lo, double hi) { double s = 0; for (size_t k = (size_t) (lo * (double) nfft / a.fs); k < (size_t) (hi * (double) nfft / a.fs); ++k) s += acc[k]; return s; };
                for (int b = 0; b < 7; ++b) level[er][b] = 10.0 * std::log10 (band (edges[b], edges[b + 1]) + 1e-20);
                above16[er] = 10.0 * std::log10 (band (16500.0, 22000.0) + 1e-20) - 10.0 * std::log10 (band (100.0, 12800.0) + 1e-20);
            }
            // compare the SHAPE (each rate's levels relative to its own total up to 12.8 kHz): 3 dB tolerance per octave band
            double t0 = 0, t1 = 0; for (int b = 0; b < 7; ++b) { t0 += std::pow (10.0, level[0][b] / 10); t1 += std::pow (10.0, level[1][b] / 10); }
            // Interpolation (Hermite reads at fractional positions in beat repeat and in the Oliverb loop) rolls off at a fixed fraction
            // of the sample rate, so at 48 kHz it reaches higher in Hz: the top octave (6.4-12.8 kHz) is allowed 6 dB, the rest 3 dB.
            double worst = 0, worstExcess = -1e9; int wb = 0;
            for (int b = 0; b < 7; ++b)
            {
                const double d = (level[1][b] - 10.0 * std::log10 (t1)) - (level[0][b] - 10.0 * std::log10 (t0));
                const double limit = b == 6 ? 6.0 : 3.0;
                if (std::fabs (d) - limit > worstExcess) { worstExcess = std::fabs (d) - limit; worst = d; wb = b; }
            }
            const double total = 10.0 * std::log10 (t1 / t0);
            report (worstExcess <= 0.0, "%-15s %-8s spectrum shape: worst octave band %5.0f-%5.0f Hz differs by %+.1f dB; total level %+.1f dB; energy above 16.5 kHz: %.0f dB (32 kHz), %.0f dB (48 kHz) re. the band below",
                    kModeNames[mode], quality ? "low-fi" : "16-bit", edges[wb], edges[wb + 1], worst, total, above16[0], above16[1]);
        }
}

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::string (argv[1]) == "quick";
    printf ("--- rate rules and FFT ---\n");  staticChecks();
    printf ("--- delay times ---\n");         delayChecks();
    printf ("--- pitch ---\n");               pitchChecks();
    printf ("--- reverb decay ---\n");        reverbChecks();
    if (! quick) { printf ("--- spectrum, all modes ---\n"); spectrumChecks(); }
    printf (gFails ? "\n%d CHECKS FAILED\n" : "\nall rate-comparison checks passed\n", gFails);
    return gFails != 0;
}
