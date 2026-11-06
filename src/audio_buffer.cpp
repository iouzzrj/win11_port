#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <vector>

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "winmm.lib")

void DecodeXboxAdpcmToPcm16(
    const uint8_t* src, uint32_t srcBytes, uint32_t channels, std::vector<int16_t>& outPcm);
void DecimatePcm16By2(
    const int16_t* srcPcm, size_t srcFrames, uint32_t channels, std::vector<int16_t>& outPcm);

// ============================================================================
// Xbox DirectSoundBuffer Structures & Constants
// ============================================================================

#pragma pack(push, 1)
struct XBOX_DSBUFFERDESC {
    uint32_t            dwSize;
    uint32_t            dwFlags;
    uint32_t            dwBufferBytes;
    const WAVEFORMATEX* lpwfxFormat;
    const void*         lpMixBins;
    uint32_t            dwInputMixBin;
};

#pragma pack(pop)

static constexpr uint32_t kDefaultStreamBytesPerSec = 192000u;
static constexpr uint32_t kXboxMixBinI3DL2          = 10u;

static XBOX_DSI3DL2LISTENER g_i3dl2Listener = {
    -10000, 0, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f
};
static XBOX_DSI3DL2LISTENER g_i3dl2ListenerDeferred = g_i3dl2Listener;
static bool                 g_hasDeferredI3DL2      = false;

const XBOX_DSI3DL2LISTENER& GetXboxI3DL2ListenerState() {
    return g_i3dl2Listener;
}

// ============================================================================
// Windows IDirectSound8 Host Backend & Buffer Pool
// ============================================================================

static IDirectSound8* g_hostDS = nullptr;

static void EnsureHostDirectSound() {
    if (g_hostDS || !g_hWnd) return;
    if (SUCCEEDED(DirectSoundCreate8(nullptr, &g_hostDS, nullptr))) {
        g_hostDS->SetCooperativeLevel(g_hWnd, DSSCL_PRIORITY);
    }
}

enum XboxBufferSubLoopPhase : uint8_t {
    kSubLoopPhaseNone    = 0,
    kSubLoopPhaseAttack  = 1,
    kSubLoopPhaseSustain = 2,
    kSubLoopPhaseRelease = 3,
};

struct XboxDSBufferObject {
    void**                  vtable;
    LONG                    refCount;
    WAVEFORMATEX            wfx;
    bool                    isAdpcm;
    uint8_t*                rawBuf;
    uint32_t                rawSize;
    bool                    ownsRawBuf;
    uint32_t                playStart;
    uint32_t                playLength;
    uint32_t                loopStart;
    uint32_t                loopLength;
    LONG                    volume;
    LONG                    headroomMb;
    LONG                    pan;
    LONG                    atten3D;
    LONG                    lPitch;
    DWORD                   freq;
    DWORD                   status;
    IDirectSoundBuffer*     hostBuf;
    uint32_t                hostBufBytes;
    uint64_t                hostCacheKey;
    bool                    dirty;
    bool                    isStreamingLockBuf;
    bool                    isLooping;
    bool                    isDecimated2x;
    uint8_t                 subLoopPhase;
    bool                    hasI3DL2Send;
    int32_t                 i3dl2SendMb;
    XBOX_DSMIXBINVOLUMEPAIR mixBinPairs[8];
    uint32_t                mixBinCount;
    Xbox3DVoiceMetrics      voice3D;
    XBOX_DSFILTERDESC       filterDesc;
    bool                    hasActiveFilter;
    XboxVoiceFilterState    filterState;
    XBOX_DSLFODESC          lfo[2];
    bool                    lfoActive[2];
    XBOX_DSENVELOPEDESC     eg[2];
    bool                    egActive[2];
    XboxDSBufferObject*     outputBuffer;
    DWORD                   playStartTickMs;
    bool                    isReleasing;
    DWORD                   releaseStartTickMs;
    int32_t                 lfoEgVolModMb;
    float                   lfoEgPitchRatio;
    float                   lfoEgCutoffRatio;
};

struct PooledHostBuffer {
    IDirectSoundBuffer* hostBuf;
    uint32_t            hostBufBytes;
    uint64_t            cacheKey;
    uint16_t            nChannels;
    uint16_t            wBitsPerSample;
};

static std::vector<XboxDSBufferObject*> g_activeBuffers;
static std::vector<PooledHostBuffer>    g_hostBufPool;
CRITICAL_SECTION                        g_audioCs;
bool                                    g_audioCsInit = false;

static void ExtractMixBinPairsI3DL2Send(
    const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, bool* outHasSend, int32_t* outSendMb)
{
    if (!outHasSend || !outSendMb || !pairs) return;
    if (count > 8u) count = 8u;
    for (uint32_t i = 0; i < count; ++i) {
        if (pairs[i].dwMixBin == kXboxMixBinI3DL2) {
            int32_t v = pairs[i].lVolume;
            if (v > -9000) {
                *outHasSend = true;
                *outSendMb  = (v > 0) ? 0 : v;
            } else {
                *outHasSend = false;
                *outSendMb  = kMinDirectSoundVolumeMb;
            }
            return;
        }
    }
}

void ApplyXboxOutputBufferRouting(
    XboxDSBufferObject* outBuf, LONG* ioVolMb, LONG* ioPanMb, bool* ioHasI3DL2, int32_t* ioI3DL2SendMb)
{
    if (!outBuf) return;
    if (ioVolMb) {
        int32_t v = *ioVolMb + (outBuf->volume - outBuf->headroomMb) + outBuf->atten3D;
        if (v < kMinDirectSoundVolumeMb) v = kMinDirectSoundVolumeMb;
        if (v > kMaxDirectSoundVolumeMb) v = kMaxDirectSoundVolumeMb;
        *ioVolMb = v;
    }
    if (ioPanMb && outBuf->pan != 0) {
        int32_t p = *ioPanMb + outBuf->pan;
        if (p < kMinDirectSoundPanMb) p = kMinDirectSoundPanMb;
        if (p > kMaxDirectSoundPanMb) p = kMaxDirectSoundPanMb;
        *ioPanMb = p;
    }
    if (outBuf->hasI3DL2Send) {
        if (ioHasI3DL2)   *ioHasI3DL2   = true;
        if (ioI3DL2SendMb && outBuf->i3dl2SendMb > *ioI3DL2SendMb) {
            *ioI3DL2SendMb = outBuf->i3dl2SendMb;
        }
    }
}

static DWORD GetBufferEffectiveFreqHz(const XboxDSBufferObject* b) {
    if (!b) return kDefaultSampleRateHz;
    DWORD eff = b->freq ? b->freq : kDefaultSampleRateHz;
    if (b->voice3D.active3D && b->voice3D.dopplerPitch != 0) {
        eff = XboxPitchToFrequencyHz(b->lPitch + b->voice3D.dopplerPitch);
    }
    if (b->lfoEgPitchRatio > 0.01f && fabsf(b->lfoEgPitchRatio - 1.0f) > 0.001f) {
        eff = static_cast<DWORD>(static_cast<float>(eff) * b->lfoEgPitchRatio + 0.5f);
    }
    if (eff < kMinDirectSoundFreqHz) eff = kMinDirectSoundFreqHz;
    if (eff > kMaxXboxPitchFreqHz)   eff = kMaxXboxPitchFreqHz;
    return eff;
}

static uint32_t GetQuantizedI3DL2StateHash(const XboxDSBufferObject* b) {
    if (!b || g_i3dl2Listener.lRoom <= -9000) return 0u;
    bool hasSend = b->hasI3DL2Send;
    int32_t sendMb = b->i3dl2SendMb;
    if (b->outputBuffer && b->outputBuffer->hasI3DL2Send) {
        hasSend = true;
        if (b->outputBuffer->i3dl2SendMb > sendMb) sendMb = b->outputBuffer->i3dl2SendMb;
    }
    if (!hasSend) return 0u;
    int32_t roomBucket = (g_i3dl2Listener.lRoom + 10000) / 250;
    int32_t revBucket  = (g_i3dl2Listener.lReverb + 10000) / 250;
    int32_t decBucket  = static_cast<int32_t>(g_i3dl2Listener.flDecayTime * 4.0f);
    int32_t sendBucket = (sendMb + 10000) / 500;
    return static_cast<uint32_t>(
        ((roomBucket & 0x3Fu) << 18) |
        ((revBucket  & 0x3Fu) << 12) |
        ((decBucket  & 0x3Fu) << 6)  |
        (sendBucket  & 0x3Fu) | 0x80000000u);
}

static uint32_t GetQuantizedFilterStateHash(const XboxDSBufferObject* b) {
    if (!b) return 0u;
    uint32_t h = 0u;
    if (b->hasActiveFilter && (b->filterDesc.dwMode & 3u) != 0u) {
        uint32_t mode = b->filterDesc.dwMode & 3u;
        int16_t  fc0  = static_cast<int16_t>(b->filterDesc.adwCoefficients[0] & 0xFFFFu);
        uint16_t q0   = static_cast<uint16_t>(b->filterDesc.adwCoefficients[1] & 0xFFFFu);
        int16_t  fc1  = static_cast<int16_t>(b->filterDesc.adwCoefficients[2] & 0xFFFFu);
        uint16_t q1   = static_cast<uint16_t>(b->filterDesc.adwCoefficients[3] & 0xFFFFu);
        bool s0 = !(fc0 == 0 && (q0 == 0u || q0 == 0x8000u));
        bool s1 = !(fc1 == 0 && (q1 == 0u || q1 == 0x8000u));
        if (s0 || s1) {
            uint32_t fc0B = static_cast<uint32_t>((static_cast<int32_t>(fc0) + 32768) >> 9) & 0x7Fu;
            uint32_t q0B  = static_cast<uint32_t>(q0 >> 12) & 0x0Fu;
            uint32_t fc1B = static_cast<uint32_t>((static_cast<int32_t>(fc1) + 32768) >> 10) & 0x3Fu;
            uint32_t q1B  = static_cast<uint32_t>(q1 >> 13) & 0x07u;
            h |= (mode << 28) | (fc0B << 17) | (q0B << 13) | (fc1B << 7) | (q1B << 4);
        }
    }
    if (b->voice3D.active3D && b->voice3D.hrtfFilterCoeff > 0.0f && b->voice3D.hrtfFilterCoeff < 0.92f) {
        uint32_t hrtfB = static_cast<uint32_t>(b->voice3D.hrtfFilterCoeff * 15.0f) & 0x0Fu;
        h |= 0x08000000u | hrtfB;
    }
    return h;
}

static void ApplyI3DL2ReverbToPcm16(
    std::vector<int16_t>& pcm, uint32_t channels, uint32_t sampleRate, int32_t sendMb)
{
    if (pcm.empty() || channels == 0 || channels > 2 || g_i3dl2Listener.lRoom <= -9000) return;

    float roomLin = powf(10.0f, static_cast<float>(g_i3dl2Listener.lRoom) / 2000.0f);
    float sendLin = powf(10.0f, static_cast<float>(sendMb) / 2000.0f);
    if (roomLin > 1.0f) roomLin = 1.0f;
    if (sendLin > 1.0f) sendLin = 1.0f;

    float reflLin = roomLin * sendLin * powf(10.0f, static_cast<float>(g_i3dl2Listener.lReflections) / 2000.0f);
    float revLin  = roomLin * sendLin * powf(10.0f, static_cast<float>(g_i3dl2Listener.lReverb) / 2000.0f);
    if (reflLin > 0.35f) reflLin = 0.35f;
    if (revLin  > 0.35f) revLin  = 0.35f;
    if (reflLin < 0.002f && revLin < 0.002f) return;

    float decaySec = g_i3dl2Listener.flDecayTime;
    if (!_finite(decaySec) || decaySec < 0.1f) decaySec = 0.1f;
    if (decaySec > 10.0f) decaySec = 10.0f;

    float hfRatio = g_i3dl2Listener.flDecayHFRatio;
    if (!_finite(hfRatio) || hfRatio < 0.1f) hfRatio = 0.1f;
    if (hfRatio > 2.0f) hfRatio = 2.0f;

    float hfGain = powf(10.0f, static_cast<float>(g_i3dl2Listener.lRoomHF) / 4000.0f);
    if (!_finite(hfGain) || hfGain < 0.05f) hfGain = 0.05f;
    if (hfGain > 1.0f) hfGain = 1.0f;

    float damp = 1.0f - (0.45f * hfGain * hfRatio);
    if (damp < 0.10f) damp = 0.10f;
    if (damp > 0.75f) damp = 0.75f;

    if (sampleRate < 8000u) sampleRate = 22050u;
    float rateScale = static_cast<float>(sampleRate) / 22050.0f;

    // Zero-allocation scratch delay lines (eliminates 5 heap allocations per SyncBufferToHost call).
    static thread_local float s_combBuf[4][8192];
    static thread_local float s_reflBuf[6600];

    static const int kBaseCombDelays[4] = { 659, 751, 853, 967 };
    int combDelays[4];
    float combFb[4];
    float combLp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int combIdx[4]  = { 0, 0, 0, 0 };

    for (int i = 0; i < 4; ++i) {
        int d = static_cast<int>(kBaseCombDelays[i] * rateScale + 0.5f);
        if (d < 64)   d = 64;
        if (d > 8192) d = 8192;
        combDelays[i] = d;
        float fb = powf(0.001f, static_cast<float>(d) / (decaySec * static_cast<float>(sampleRate)));
        if (fb < 0.0f)  fb = 0.0f;
        if (fb > 0.84f) fb = 0.84f;
        combFb[i] = fb;
        memset(s_combBuf[i], 0, static_cast<size_t>(d) * sizeof(float));
    }

    float reflDelaySec = g_i3dl2Listener.flReflectionsDelay;
    if (!_finite(reflDelaySec) || reflDelaySec < 0.002f) reflDelaySec = 0.007f;
    if (reflDelaySec > 0.25f) reflDelaySec = 0.25f;
    int reflDelay = static_cast<int>(reflDelaySec * static_cast<float>(sampleRate) + 0.5f);
    if (reflDelay < 16)   reflDelay = 16;
    if (reflDelay > 6600) reflDelay = 6600;
    memset(s_reflBuf, 0, static_cast<size_t>(reflDelay) * sizeof(float));
    int reflIdx = 0;

    size_t numFrames = pcm.size() / channels;
    for (size_t f = 0; f < numFrames; ++f) {
        float dryL = static_cast<float>(pcm[f * channels]);
        float dryR = (channels == 2) ? static_cast<float>(pcm[f * channels + 1]) : dryL;
        float mono = 0.5f * (dryL + dryR);

        float early = s_reflBuf[reflIdx];
        s_reflBuf[reflIdx] = mono;
        if (++reflIdx >= reflDelay) reflIdx = 0;

        float combSum = 0.0f;
        for (int i = 0; i < 4; ++i) {
            float cOut = s_combBuf[i][combIdx[i]];
            combLp[i] = cOut * (1.0f - damp) + combLp[i] * damp;
            s_combBuf[i][combIdx[i]] = mono + combLp[i] * combFb[i];
            if (++combIdx[i] >= combDelays[i]) combIdx[i] = 0;
            combSum += cOut;
        }

        float wet = early * reflLin + (0.25f * combSum) * revLin;
        int32_t outL = static_cast<int32_t>(dryL + wet);
        if (outL >  32767) outL =  32767;
        if (outL < -32768) outL = -32768;
        pcm[f * channels] = static_cast<int16_t>(outL);

        if (channels == 2) {
            int32_t outR = static_cast<int32_t>(dryR + wet);
            if (outR >  32767) outR =  32767;
            if (outR < -32768) outR = -32768;
            pcm[f * channels + 1] = static_cast<int16_t>(outR);
        }
    }
}

static uint64_t ComputeWaveRegionCacheKey(
    const uint8_t* regionPtr, uint32_t regionBytes, uint16_t nChannels, uint16_t wBitsPerSample,
    uint8_t subLoopPhase, uint32_t i3dl2Hash, uint32_t filterHash, bool decimated2x)
{
    if (!regionPtr || regionBytes == 0) return 0;
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(reinterpret_cast<uintptr_t>(regionPtr));
    mix(regionBytes);
    mix((static_cast<uint64_t>(nChannels) << 16) |
        wBitsPerSample |
        (static_cast<uint64_t>(subLoopPhase) << 24) |
        (decimated2x ? (1ULL << 32) : 0ULL));
    mix(i3dl2Hash);
    mix(filterHash);
    uint32_t sampleBytes = (regionBytes < 64u) ? regionBytes : 64u;
    for (uint32_t i = 0; i < sampleBytes; ++i) {
        h ^= regionPtr[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static void ReturnHostBufferToPool(XboxDSBufferObject* b) {
    if (!b || !b->hostBuf) return;
    b->hostBuf->Stop();
    if (!b->isStreamingLockBuf && b->hostCacheKey != 0 && b->hostBufBytes > 0) {
        if (g_hostBufPool.size() >= 192) {
            if (g_hostBufPool[0].hostBuf) {
                g_hostBufPool[0].hostBuf->Release();
            }
            g_hostBufPool.erase(g_hostBufPool.begin());
        }
        PooledHostBuffer p = {};
        p.hostBuf        = b->hostBuf;
        p.hostBufBytes   = b->hostBufBytes;
        p.cacheKey       = b->hostCacheKey;
        p.nChannels      = b->wfx.nChannels ? b->wfx.nChannels : 1;
        p.wBitsPerSample = (b->wfx.wBitsPerSample == 8 && !b->isAdpcm && !b->isDecimated2x && !b->hasActiveFilter) ? 8 : 16;
        g_hostBufPool.push_back(p);
    } else {
        b->hostBuf->Release();
    }
    b->hostBuf      = nullptr;
    b->hostBufBytes = 0;
    b->hostCacheKey = 0;
}

static void ApplyBufferVolumeAndPan(XboxDSBufferObject* b) {
    if (!b || !b->hostBuf) return;
    LONG baseVol = b->volume - b->headroomMb;
    LONG vol = baseVol + b->atten3D + b->lfoEgVolModMb;
    LONG pan = b->pan;
    if (b->outputBuffer) {
        ApplyXboxOutputBufferRouting(b->outputBuffer, &vol, &pan, nullptr, nullptr);
    }
    if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
    if (vol > kMaxDirectSoundVolumeMb) vol = kMaxDirectSoundVolumeMb;
    b->hostBuf->SetVolume(vol);

    if (pan < kMinDirectSoundPanMb) pan = kMinDirectSoundPanMb;
    if (pan > kMaxDirectSoundPanMb) pan = kMaxDirectSoundPanMb;
    b->hostBuf->SetPan(pan);

    DWORD effectiveFreq = GetBufferEffectiveFreqHz(b);
    DWORD hostFreq      = b->isDecimated2x ? ((effectiveFreq + 1u) / 2u) : effectiveFreq;
    if (hostFreq < kMinDirectSoundFreqHz) hostFreq = kMinDirectSoundFreqHz;
    if (hostFreq > kMaxDirectSoundFreqHz) hostFreq = kMaxDirectSoundFreqHz;
    b->hostBuf->SetFrequency(hostFreq);
}

struct BufferSubLoopGeometry {
    uint32_t playStartOff;
    uint32_t playRegionBytes;
    uint32_t alignedLoopStart;
    uint32_t alignedLoopLength;
    uint32_t rawBlockAlign;
    bool     hasSubLoop;
    bool     hasAttack;
    bool     hasRelease;
};

static BufferSubLoopGeometry GetBufferSubLoopGeometry(const XboxDSBufferObject* b) {
    BufferSubLoopGeometry g = {};
    if (!b || !b->rawBuf || b->rawSize == 0) return g;

    uint16_t nCh = b->wfx.nChannels ? b->wfx.nChannels : 1u;
    uint16_t bits = (!b->isAdpcm && b->wfx.wBitsPerSample == 8) ? 8u : 16u;
    uint32_t pcmBlockAlign = (nCh * bits) / 8u;
    if (pcmBlockAlign == 0) pcmBlockAlign = 1u;
    g.rawBlockAlign = b->isAdpcm ? (36u * nCh) : pcmBlockAlign;
    if (g.rawBlockAlign == 0) g.rawBlockAlign = 1u;

    g.playStartOff = (!b->isStreamingLockBuf && b->playStart < b->rawSize) ? b->playStart : 0u;
    g.playRegionBytes = (!b->isStreamingLockBuf && b->playLength > 0 && g.playStartOff + b->playLength <= b->rawSize)
                        ? b->playLength
                        : (b->rawSize - g.playStartOff);

    if (!b->isStreamingLockBuf && b->loopLength >= g.rawBlockAlign) {
        g.alignedLoopStart  = (b->loopStart / g.rawBlockAlign) * g.rawBlockAlign;
        g.alignedLoopLength = (b->loopLength / g.rawBlockAlign) * g.rawBlockAlign;
        if (g.alignedLoopLength >= g.rawBlockAlign &&
            g.alignedLoopStart + g.alignedLoopLength <= g.playRegionBytes &&
            (g.alignedLoopStart > 0 || g.alignedLoopLength < g.playRegionBytes))
        {
            g.hasSubLoop = true;
            g.hasAttack  = (g.alignedLoopStart >= g.rawBlockAlign);
            g.hasRelease = (g.alignedLoopStart + g.alignedLoopLength + g.rawBlockAlign <= g.playRegionBytes);
        }
    }
    return g;
}

static void SyncBufferToHost(XboxDSBufferObject* b, uint8_t targetPhase = kSubLoopPhaseNone, bool preservePlaybackInPlace = false) {
    EnsureHostDirectSound();
    if (!g_hostDS || !b || !b->rawBuf || b->rawSize == 0) return;

    WAVEFORMATEX pcmWfx = {};
    pcmWfx.wFormatTag = WAVE_FORMAT_PCM;
    pcmWfx.nChannels = b->wfx.nChannels ? b->wfx.nChannels : 1;
    pcmWfx.nSamplesPerSec = b->wfx.nSamplesPerSec ? b->wfx.nSamplesPerSec : 22050;
    pcmWfx.wBitsPerSample = 16;
    pcmWfx.nBlockAlign = (pcmWfx.nChannels * pcmWfx.wBitsPerSample) / 8;
    pcmWfx.nAvgBytesPerSec = pcmWfx.nSamplesPerSec * pcmWfx.nBlockAlign;

    // Respect SetPlayRegion (b->playStart, b->playLength) used by CWaveBank::SetBufferData (0x007030DF)
    // and Xbox MCPX 3-stage SetLoopRegion (Attack [0..loopStart) -> Sustain [loopStart..loopStart+loopLength)
    // -> Release [loopStart+loopLength..playLength)) used by XACT::CSoundCue::PlayWave (0x00708830)
    // and CMcpxBuffer::StopEx(DSBSTOPEX_RELEASEWAVEFORM) (0x006E8981).
    BufferSubLoopGeometry g = GetBufferSubLoopGeometry(b);
    uint32_t startOff    = g.playStartOff;
    uint32_t regionBytes = g.playRegionBytes;
    uint8_t  actualPhase = kSubLoopPhaseNone;

    if (targetPhase == kSubLoopPhaseAttack && g.hasSubLoop && g.hasAttack) {
        startOff    = g.playStartOff;
        regionBytes = g.alignedLoopStart;
        actualPhase = kSubLoopPhaseAttack;
    } else if (targetPhase == kSubLoopPhaseSustain && g.hasSubLoop) {
        startOff    = g.playStartOff + g.alignedLoopStart;
        regionBytes = g.alignedLoopLength;
        actualPhase = kSubLoopPhaseSustain;
    } else if (targetPhase == kSubLoopPhaseRelease && g.hasSubLoop && g.hasRelease) {
        startOff    = g.playStartOff + g.alignedLoopStart + g.alignedLoopLength;
        regionBytes = g.playRegionBytes - (g.alignedLoopStart + g.alignedLoopLength);
        actualPhase = kSubLoopPhaseRelease;
    }

    DWORD effectiveFreq   = GetBufferEffectiveFreqHz(b);
    bool  needDecimate2x  = (!b->isStreamingLockBuf && effectiveFreq > kMaxDirectSoundFreqHz);
    float filterF = 1.0f, filterD = 1.0f, filterF2 = 1.0f, filterD2 = 1.0f;
    uint32_t filterMode = 0u;
    bool  needFilter      = (!b->isStreamingLockBuf && b->hasActiveFilter &&
                             ComputeDls2FilterCoeffs(&b->filterDesc, pcmWfx.nSamplesPerSec, &filterF, &filterD, &filterMode, &filterF2, &filterD2));
    if (!b->isStreamingLockBuf && b->voice3D.active3D && b->voice3D.hrtfFilterCoeff > 0.0f && b->voice3D.hrtfFilterCoeff < 0.92f) {
        if (!needFilter) {
            needFilter = true;
            filterMode = 1u;
            filterF    = b->voice3D.hrtfFilterCoeff;
            filterD    = 1.0f;
        } else {
            filterF   *= b->voice3D.hrtfFilterCoeff;
            if (filterF < 0.004f) filterF = 0.004f;
        }
    }
    if (needFilter && b->lfoEgCutoffRatio > 0.01f && fabsf(b->lfoEgCutoffRatio - 1.0f) > 0.01f) {
        filterF *= b->lfoEgCutoffRatio;
        if (filterF < 0.004f) filterF = 0.004f;
        if (filterF > 1.40f)  filterF = 1.40f;
    }

    bool hasI3dl2Send = b->hasI3DL2Send;
    int32_t i3dl2SendMb = b->i3dl2SendMb;
    if (b->outputBuffer && b->outputBuffer->hasI3DL2Send) {
        hasI3dl2Send = true;
        if (b->outputBuffer->i3dl2SendMb > i3dl2SendMb) i3dl2SendMb = b->outputBuffer->i3dl2SendMb;
    }
    bool  needReverb      = (!b->isStreamingLockBuf && hasI3dl2Send && g_i3dl2Listener.lRoom > -9000);
    bool  needPcm16Dsp    = (needDecimate2x || needFilter || needReverb);

    const uint8_t* regionPtr = b->rawBuf + startOff;
    uint16_t targetBits = (!b->isAdpcm && b->wfx.wBitsPerSample == 8 && !needPcm16Dsp) ? 8 : 16;
    uint32_t i3dl2Hash  = needReverb ? GetQuantizedI3DL2StateHash(b) : 0u;
    uint32_t filterHash = needFilter ? GetQuantizedFilterStateHash(b) : 0u;
    uint64_t cacheKey   = (!b->isStreamingLockBuf)
        ? ComputeWaveRegionCacheKey(regionPtr, regionBytes, pcmWfx.nChannels, targetBits, actualPhase, i3dl2Hash, filterHash, needDecimate2x)
        : 0ULL;

    if (!b->isStreamingLockBuf && cacheKey != 0 && !preservePlaybackInPlace) {
        if (b->hostBuf && b->hostCacheKey == cacheKey) {
            b->isDecimated2x = needDecimate2x;
            b->dirty         = false;
            b->subLoopPhase  = actualPhase;
            ApplyBufferVolumeAndPan(b);
            return;
        }
        for (size_t i = 0; i < g_hostBufPool.size(); ++i) {
            if (g_hostBufPool[i].cacheKey == cacheKey && g_hostBufPool[i].hostBuf != nullptr) {
                PooledHostBuffer hit = g_hostBufPool[i];
                g_hostBufPool.erase(g_hostBufPool.begin() + i);
                if (b->hostBuf) {
                    ReturnHostBufferToPool(b);
                }
                b->hostBuf       = hit.hostBuf;
                b->hostBufBytes  = hit.hostBufBytes;
                b->hostCacheKey  = hit.cacheKey;
                b->isDecimated2x = needDecimate2x;
                b->dirty         = false;
                b->subLoopPhase  = actualPhase;
                ApplyBufferVolumeAndPan(b);
                return;
            }
        }
    }

    std::vector<int16_t> decodedPcm;
    std::vector<int16_t> decimatedPcm;
    const void* srcPcm   = regionPtr;
    uint32_t srcPcmBytes = regionBytes;

    if (b->isAdpcm) {
        DecodeXboxAdpcmToPcm16(regionPtr, regionBytes, pcmWfx.nChannels, decodedPcm);
    } else if (b->wfx.wBitsPerSample == 8) {
        if (needPcm16Dsp) {
            decodedPcm.resize(regionBytes);
            for (uint32_t i = 0; i < regionBytes; ++i) {
                decodedPcm[i] = static_cast<int16_t>((static_cast<int32_t>(regionPtr[i]) - 128) << 8);
            }
        } else {
            pcmWfx.wBitsPerSample  = 8;
            pcmWfx.nBlockAlign     = pcmWfx.nChannels;
            pcmWfx.nAvgBytesPerSec = pcmWfx.nSamplesPerSec * pcmWfx.nBlockAlign;
        }
    } else if (needPcm16Dsp && regionBytes >= sizeof(int16_t)) {
        size_t numSamples = regionBytes / sizeof(int16_t);
        decodedPcm.resize(numSamples);
        memcpy(decodedPcm.data(), regionPtr, numSamples * sizeof(int16_t));
    }

    if (!decodedPcm.empty()) {
        if (needFilter) {
            float svfLow[2]  = { 0.0f, 0.0f };
            float svfBand[2] = { 0.0f, 0.0f };
            XboxVoiceFilterState state2 = {};
            ApplyDls2FilterToPcm16(
                decodedPcm.data(), decodedPcm.size() / pcmWfx.nChannels, pcmWfx.nChannels,
                filterF, filterD, svfLow, svfBand,
                filterMode, filterF2, filterD2, &state2);
        }
        if (needReverb) {
            ApplyI3DL2ReverbToPcm16(decodedPcm, pcmWfx.nChannels, pcmWfx.nSamplesPerSec, i3dl2SendMb);
        }
        if (needDecimate2x) {
            DecimatePcm16By2(
                decodedPcm.data(), decodedPcm.size() / pcmWfx.nChannels, pcmWfx.nChannels, decimatedPcm);
            srcPcm      = decimatedPcm.data();
            srcPcmBytes = static_cast<uint32_t>(decimatedPcm.size() * sizeof(int16_t));
        } else {
            srcPcm      = decodedPcm.data();
            srcPcmBytes = static_cast<uint32_t>(decodedPcm.size() * sizeof(int16_t));
        }
    }

    if (srcPcmBytes < 64) return;

    DWORD savedPlayPos = 0;
    bool  wasPlayingBeforeRebake = false;
    if (preservePlaybackInPlace && b->hostBuf) {
        DWORD st = 0;
        b->hostBuf->GetStatus(&st);
        if ((st & DSBSTATUS_PLAYING) != 0) {
            wasPlayingBeforeRebake = true;
            b->hostBuf->GetCurrentPosition(&savedPlayPos, nullptr);
        }
    }

    bool recreatedBuf = false;
    if (!b->hostBuf || b->hostBufBytes != srcPcmBytes) {
        if (b->hostBuf) {
            ReturnHostBufferToPool(b);
        }
        DSBUFFERDESC dsbd = {};
        dsbd.dwSize = sizeof(dsbd);
        dsbd.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
        dsbd.dwBufferBytes = srcPcmBytes;
        dsbd.lpwfxFormat = &pcmWfx;
        if (FAILED(g_hostDS->CreateSoundBuffer(&dsbd, &b->hostBuf, nullptr))) {
            return;
        }
        b->hostBufBytes = srcPcmBytes;
        recreatedBuf    = true;
    }

    void* ptr1 = nullptr;
    DWORD bytes1 = 0;
    if (SUCCEEDED(b->hostBuf->Lock(0, srcPcmBytes, &ptr1, &bytes1, nullptr, nullptr, 0))) {
        memcpy(ptr1, srcPcm, bytes1);
        b->hostBuf->Unlock(ptr1, bytes1, nullptr, 0);
    }
    b->hostCacheKey  = cacheKey;
    b->isDecimated2x = needDecimate2x;
    b->dirty         = false;
    b->subLoopPhase  = actualPhase;
    ApplyBufferVolumeAndPan(b);

    if (wasPlayingBeforeRebake && recreatedBuf && b->hostBuf) {
        if (savedPlayPos >= srcPcmBytes) savedPlayPos = 0;
        bool loopNow = b->isLooping && (actualPhase == kSubLoopPhaseNone || actualPhase == kSubLoopPhaseSustain);
        b->hostBuf->SetCurrentPosition(savedPlayPos);
        b->hostBuf->Play(0, 0, loopNow ? DSBPLAY_LOOPING : 0);
    }
}

static void RebakeBufferOnParameterChange(XboxDSBufferObject* b) {
    if (!b) return;
    if (b->status == 0 || !b->hostBuf) {
        b->dirty = true;
    } else if (!b->isStreamingLockBuf) {
        DWORD st = 0;
        b->hostBuf->GetStatus(&st);
        if ((st & DSBSTATUS_PLAYING) != 0) {
            SyncBufferToHost(b, b->subLoopPhase, true);
        } else {
            b->dirty = true;
        }
    }
}

static void RefreshBufferPitchAndFrequency(XboxDSBufferObject* b) {
    if (!b) return;
    DWORD effectiveFreq  = GetBufferEffectiveFreqHz(b);
    bool  needDecimate2x = (!b->isStreamingLockBuf && effectiveFreq > kMaxDirectSoundFreqHz);
    if (b->hostBuf && !b->isStreamingLockBuf && needDecimate2x != b->isDecimated2x) {
        bool wasPlaying = (b->status == 1);
        if (wasPlaying) {
            DWORD st = 0;
            b->hostBuf->GetStatus(&st);
            wasPlaying = ((st & DSBSTATUS_PLAYING) != 0);
        }
        SyncBufferToHost(b, b->subLoopPhase, wasPlaying);
    } else {
        ApplyBufferVolumeAndPan(b);
    }
}

static void UpdateBufferLfoAndEg(XboxDSBufferObject* b) {
    if (!b) return;
    if (!b->lfoActive[0] && !b->lfoActive[1] && !b->egActive[0] && !b->egActive[1]) {
        b->lfoEgVolModMb    = 0;
        b->lfoEgPitchRatio  = 1.0f;
        b->lfoEgCutoffRatio = 1.0f;
        return;
    }
    DWORD now = GetTickCount();
    uint32_t elapsedMs = (b->playStartTickMs != 0 && now >= b->playStartTickMs) ? (now - b->playStartTickMs) : 0u;
    uint32_t relElapsedMs = (b->isReleasing && b->releaseStartTickMs != 0 && now >= b->releaseStartTickMs)
                            ? (now - b->releaseStartTickMs) : 0u;
    EvaluateXboxVoiceLfoAndEg(
        b->lfo, b->lfoActive, b->eg, b->egActive,
        elapsedMs, b->isReleasing, relElapsedMs,
        &b->lfoEgVolModMb, &b->lfoEgPitchRatio, &b->lfoEgCutoffRatio);
}

static void AdvanceBufferSubLoopIfNeeded(XboxDSBufferObject* b) {
    if (!b || b->status == 0 || !b->hostBuf) return;
    if (b->lfoActive[0] || b->lfoActive[1] || b->egActive[0] || b->egActive[1]) {
        UpdateBufferLfoAndEg(b);
        ApplyBufferVolumeAndPan(b);
    }
    if (b->subLoopPhase == kSubLoopPhaseAttack) {
        DWORD st = 0;
        b->hostBuf->GetStatus(&st);
        if ((st & DSBSTATUS_PLAYING) == 0) {
            SyncBufferToHost(b, kSubLoopPhaseSustain);
            if (b->hostBuf) {
                b->hostBuf->SetCurrentPosition(0);
                b->hostBuf->Play(0, 0, DSBPLAY_LOOPING);
            }
        }
    } else if (b->subLoopPhase == kSubLoopPhaseRelease) {
        DWORD st = 0;
        b->hostBuf->GetStatus(&st);
        if ((st & DSBSTATUS_PLAYING) == 0) {
            b->status = 0;
            b->isReleasing = false;
            b->subLoopPhase = kSubLoopPhaseNone;
        }
    } else if (b->isReleasing && b->egActive[0] && b->eg[0].dwRelease > 0u) {
        if (b->lfoEgVolModMb <= -9500) {
            b->hostBuf->Stop();
            b->status = 0;
            b->isLooping = false;
            b->isReleasing = false;
            b->subLoopPhase = kSubLoopPhaseNone;
        }
    }
}

void AdvanceActiveAudioBuffersSubLoop() {
    for (size_t bi = 0; bi < g_activeBuffers.size(); ++bi) {
        AdvanceBufferSubLoopIfNeeded(g_activeBuffers[bi]);
    }
}

// ============================================================================
// IDirectSoundBuffer Virtual & C-API Methods
// ============================================================================

static ULONG __stdcall Buffer_AddRef(XboxDSBufferObject* b) {
    return b ? (ULONG)InterlockedIncrement(&b->refCount) : 1;
}

static ULONG __stdcall Buffer_Release(XboxDSBufferObject* b) {
    if (!b) return 0;
    LONG r = InterlockedDecrement(&b->refCount);
    if (r <= 0) {
        EnterCriticalSection(&g_audioCs);
        for (size_t i = 0; i < g_activeBuffers.size(); ++i) {
            if (g_activeBuffers[i] == b) {
                g_activeBuffers.erase(g_activeBuffers.begin() + i);
                break;
            }
        }
        for (size_t i = 0; i < g_activeBuffers.size(); ++i) {
            if (g_activeBuffers[i]->outputBuffer == b) {
                g_activeBuffers[i]->outputBuffer = nullptr;
            }
        }
        if (b->hostBuf) {
            ReturnHostBufferToPool(b);
        }
        if (b->ownsRawBuf && b->rawBuf) {
            free(b->rawBuf);
            b->rawBuf = nullptr;
        }
        LeaveCriticalSection(&g_audioCs);
        delete b;
        return 0;
    }
    return (ULONG)r;
}

static HRESULT __stdcall Buffer_Play(XboxDSBufferObject* b, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags) {
    (void)dwReserved1; (void)dwReserved2;
    if (!b) return S_OK;
    EnterCriticalSection(&g_audioCs);
    bool wasStopped = (b->status == 0);
    b->status = 1; // DSBSTATUS_PLAYING
    b->isReleasing = false;
    if (wasStopped || b->playStartTickMs == 0) {
        b->playStartTickMs = GetTickCount();
    }
    UpdateBufferLfoAndEg(b);
    bool isLooping = (dwFlags & 1u) != 0;
    b->isLooping   = isLooping;
    if (b->isStreamingLockBuf) {
        if (!b->hostBuf || b->isAdpcm || b->dirty || b->hostBufBytes != b->rawSize) {
            SyncBufferToHost(b, kSubLoopPhaseNone);
            if (b->hostBuf) {
                b->hostBuf->SetCurrentPosition(0);
                b->hostBuf->Play(0, 0, isLooping ? DSBPLAY_LOOPING : 0);
            }
        } else {
            // Streaming PCM circular buffer (e.g. Bink audio) already kept in sync via Buffer_Unlock:
            // do NOT overwrite hostBuf or reset play cursor on looping buffers!
            ApplyBufferVolumeAndPan(b);
            if (!isLooping) {
                b->hostBuf->SetCurrentPosition(0);
            }
            b->hostBuf->Play(0, 0, isLooping ? DSBPLAY_LOOPING : 0);
        }
    } else {
        AdvanceBufferSubLoopIfNeeded(b);
        BufferSubLoopGeometry g = GetBufferSubLoopGeometry(b);
        if (isLooping && g.hasSubLoop) {
            bool alreadyActiveSubLoop = (!b->dirty && b->hostBuf &&
                (b->subLoopPhase == kSubLoopPhaseAttack || b->subLoopPhase == kSubLoopPhaseSustain));
            if (alreadyActiveSubLoop) {
                DWORD st = 0;
                b->hostBuf->GetStatus(&st);
                if ((st & DSBSTATUS_PLAYING) != 0) {
                    ApplyBufferVolumeAndPan(b);
                    LeaveCriticalSection(&g_audioCs);
                    return S_OK;
                }
            }
            uint8_t initialPhase = g.hasAttack ? kSubLoopPhaseAttack : kSubLoopPhaseSustain;
            SyncBufferToHost(b, initialPhase);
            if (b->hostBuf) {
                b->hostBuf->SetCurrentPosition(0);
                b->hostBuf->Play(0, 0, (initialPhase == kSubLoopPhaseSustain) ? DSBPLAY_LOOPING : 0);
            }
        } else {
            bool wasDirty = (!b->hostBuf || b->dirty || b->subLoopPhase != kSubLoopPhaseNone);
            if (wasDirty) {
                SyncBufferToHost(b, kSubLoopPhaseNone);
            } else {
                RefreshBufferPitchAndFrequency(b);
            }
            if (b->hostBuf) {
                bool alreadyPlayingLoop = false;
                if (!wasDirty && isLooping) {
                    DWORD st = 0;
                    b->hostBuf->GetStatus(&st);
                    alreadyPlayingLoop = ((st & DSBSTATUS_PLAYING) != 0);
                }
                if (!alreadyPlayingLoop) {
                    b->hostBuf->SetCurrentPosition(0);
                }
                b->hostBuf->Play(0, 0, isLooping ? DSBPLAY_LOOPING : 0);
            }
        }
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_Stop(XboxDSBufferObject* b) {
    if (!b) return S_OK;
    EnterCriticalSection(&g_audioCs);
    b->status = 0;
    b->isLooping = false;
    b->isReleasing = false;
    b->subLoopPhase = kSubLoopPhaseNone;
    if (b->hostBuf) b->hostBuf->Stop();
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_StopEx(XboxDSBufferObject* b, uint32_t rtLow, uint32_t rtHigh, DWORD dwFlags) {
    (void)rtLow; (void)rtHigh;
    if (!b) return S_OK;
    // In Xbox CMcpxBuffer::Stop (0x006E8718) and CMcpxBuffer::StopEx (0x006E8981, called by XACT::CSoundSource::Stop at 0x00701D1D):
    //   1. DSBSTOPEX_ENVELOPE (0x1): triggers the EG release stage while continuing waveform playback until the envelope decays.
    //   2. DSBSTOPEX_RELEASEWAVEFORM (0x2): transitions to the release tail [loopStart + loopLength .. playLength) or clears looping.
    if ((dwFlags & 3u) != 0) {
        EnterCriticalSection(&g_audioCs);
        AdvanceBufferSubLoopIfNeeded(b);
        if ((dwFlags & 1u) != 0 && b->status == 1 && b->egActive[0] && b->eg[0].dwRelease > 0u) {
            b->isReleasing = true;
            b->releaseStartTickMs = GetTickCount();
            if ((dwFlags & 2u) == 0) {
                LeaveCriticalSection(&g_audioCs);
                return S_OK;
            }
        }
        if ((dwFlags & 2u) != 0) {
            if (b->status == 1 && (b->subLoopPhase == kSubLoopPhaseAttack || b->subLoopPhase == kSubLoopPhaseSustain)) {
                BufferSubLoopGeometry g = GetBufferSubLoopGeometry(b);
                if (g.hasSubLoop && g.hasRelease) {
                    b->isLooping = false;
                    SyncBufferToHost(b, kSubLoopPhaseRelease);
                    if (b->hostBuf) {
                        b->hostBuf->SetCurrentPosition(0);
                        b->hostBuf->Play(0, 0, 0);
                        LeaveCriticalSection(&g_audioCs);
                        return S_OK;
                    }
                }
            } else if (b->status == 1 && b->subLoopPhase == kSubLoopPhaseRelease) {
                LeaveCriticalSection(&g_audioCs);
                return S_OK;
            }
            if (b->status == 1 && b->isLooping && b->hostBuf) {
                DWORD st = 0;
                b->hostBuf->GetStatus(&st);
                if ((st & DSBSTATUS_PLAYING) != 0) {
                    b->isLooping    = false;
                    b->subLoopPhase = kSubLoopPhaseRelease;
                    b->hostBuf->Play(0, 0, 0);
                    LeaveCriticalSection(&g_audioCs);
                    return S_OK;
                }
            }
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return Buffer_Stop(b);
}

static HRESULT __stdcall Buffer_GetStatus(XboxDSBufferObject* b, DWORD* pdwStatus) {
    if (!pdwStatus) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    if (b) {
        AdvanceBufferSubLoopIfNeeded(b);
    }
    if (b && b->hostBuf) {
        DWORD st = 0;
        b->hostBuf->GetStatus(&st);
        *pdwStatus = (st & DSBSTATUS_PLAYING) ? 1 : 0;
    } else {
        *pdwStatus = 0;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_GetCurrentPosition(XboxDSBufferObject* b, DWORD* pdwPlay, DWORD* pdwWrite) {
    HRESULT hr = S_OK;
    EnterCriticalSection(&g_audioCs);
    if (b && b->hostBuf) {
        hr = b->hostBuf->GetCurrentPosition(pdwPlay, pdwWrite);
    } else {
        if (pdwPlay)  *pdwPlay = 0;
        if (pdwWrite) *pdwWrite = 0;
    }
    LeaveCriticalSection(&g_audioCs);
    return hr;
}

static HRESULT __stdcall Buffer_SetCurrentPosition(XboxDSBufferObject* b, DWORD dwNewPosition) {
    EnterCriticalSection(&g_audioCs);
    if (b && b->hostBuf) {
        b->hostBuf->SetCurrentPosition(dwNewPosition);
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_Lock(
    XboxDSBufferObject* b, DWORD dwOffset, DWORD dwBytes,
    void** ppvAudioPtr1, DWORD* pdwAudioBytes1,
    void** ppvAudioPtr2, DWORD* pdwAudioBytes2, DWORD dwFlags)
{
    if (!b) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    b->isStreamingLockBuf = true;
    if (!b->rawBuf) {
        b->rawSize = dwBytes ? dwBytes : 65536;
        b->rawBuf = (uint8_t*)calloc(1, b->rawSize);
        b->ownsRawBuf = true;
    }
    if (dwFlags & 1) { // DSBLOCK_FROMWRITECURSOR
        if (b->hostBuf) {
            DWORD writePos = 0;
            b->hostBuf->GetCurrentPosition(nullptr, &writePos);
            dwOffset = writePos;
        }
    }
    if (dwFlags & 2) { // DSBLOCK_ENTIREBUFFER
        dwBytes = b->rawSize;
    }
    if (b->rawSize > 0) {
        dwOffset %= b->rawSize;
        if (dwBytes > b->rawSize) dwBytes = b->rawSize;
    } else {
        dwOffset = 0;
        dwBytes = 0;
    }

    DWORD bytes1 = (dwOffset + dwBytes <= b->rawSize) ? dwBytes : (b->rawSize - dwOffset);
    DWORD bytes2 = (bytes1 < dwBytes) ? (dwBytes - bytes1) : 0;

    if (ppvAudioPtr1)   *ppvAudioPtr1 = b->rawBuf + dwOffset;
    if (pdwAudioBytes1) *pdwAudioBytes1 = bytes1;
    if (ppvAudioPtr2)   *ppvAudioPtr2 = (bytes2 > 0) ? b->rawBuf : nullptr;
    if (pdwAudioBytes2) *pdwAudioBytes2 = bytes2;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_Unlock(
    XboxDSBufferObject* b, void* pvAudioPtr1, DWORD dwAudioBytes1, void* pvAudioPtr2, DWORD dwAudioBytes2)
{
    if (!b) return S_OK;
    EnterCriticalSection(&g_audioCs);
    if (b->isStreamingLockBuf && !b->isAdpcm && b->rawBuf && b->rawSize > 0) {
        if (!b->hostBuf || b->hostBufBytes != b->rawSize) {
            SyncBufferToHost(b, kSubLoopPhaseNone);
        } else {
            if (pvAudioPtr1 && dwAudioBytes1 > 0 &&
                (uintptr_t)pvAudioPtr1 >= (uintptr_t)b->rawBuf &&
                (uintptr_t)pvAudioPtr1 < (uintptr_t)(b->rawBuf + b->rawSize))
            {
                DWORD offset1 = (DWORD)((uintptr_t)pvAudioPtr1 - (uintptr_t)b->rawBuf);
                DWORD copy1 = (offset1 + dwAudioBytes1 <= b->rawSize) ? dwAudioBytes1 : (b->rawSize - offset1);
                void* hPtr1 = nullptr; DWORD hBytes1 = 0;
                void* hPtr2 = nullptr; DWORD hBytes2 = 0;
                if (copy1 > 0 && SUCCEEDED(b->hostBuf->Lock(offset1, copy1, &hPtr1, &hBytes1, &hPtr2, &hBytes2, 0))) {
                    if (hPtr1 && hBytes1 > 0) memcpy(hPtr1, pvAudioPtr1, hBytes1);
                    if (hPtr2 && hBytes2 > 0) memcpy(hPtr2, (const uint8_t*)pvAudioPtr1 + hBytes1, hBytes2);
                    b->hostBuf->Unlock(hPtr1, hBytes1, hPtr2, hBytes2);
                }
            }
            if (pvAudioPtr2 && dwAudioBytes2 > 0 &&
                (uintptr_t)pvAudioPtr2 >= (uintptr_t)b->rawBuf &&
                (uintptr_t)pvAudioPtr2 < (uintptr_t)(b->rawBuf + b->rawSize))
            {
                DWORD offset2 = (DWORD)((uintptr_t)pvAudioPtr2 - (uintptr_t)b->rawBuf);
                DWORD copy2 = (offset2 + dwAudioBytes2 <= b->rawSize) ? dwAudioBytes2 : (b->rawSize - offset2);
                void* hPtr1 = nullptr; DWORD hBytes1 = 0;
                void* hPtr2 = nullptr; DWORD hBytes2 = 0;
                if (copy2 > 0 && SUCCEEDED(b->hostBuf->Lock(offset2, copy2, &hPtr1, &hBytes1, &hPtr2, &hBytes2, 0))) {
                    if (hPtr1 && hBytes1 > 0) memcpy(hPtr1, pvAudioPtr2, hBytes1);
                    if (hPtr2 && hBytes2 > 0) memcpy(hPtr2, (const uint8_t*)pvAudioPtr2 + hBytes1, hBytes2);
                    b->hostBuf->Unlock(hPtr1, hBytes1, hPtr2, hBytes2);
                }
            }
        }
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetBufferData(XboxDSBufferObject* b, void* pvBufferData, DWORD dwBufferBytes) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        if (b->ownsRawBuf && b->rawBuf) free(b->rawBuf);
        b->rawBuf = reinterpret_cast<uint8_t*>(pvBufferData);
        b->rawSize = dwBufferBytes;
        b->ownsRawBuf = false;
        b->playStart = 0;
        b->playLength = 0;
        b->loopStart = 0;
        b->loopLength = 0;
        b->dirty = true;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFormat(XboxDSBufferObject* b, const WAVEFORMATEX* pwfx) {
    if (b && pwfx) {
        EnterCriticalSection(&g_audioCs);
        b->wfx = *pwfx;
        b->isAdpcm = (pwfx->wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
        if (pwfx->nSamplesPerSec) {
            b->freq   = pwfx->nSamplesPerSec;
            b->lPitch = FrequencyHzToXboxPitch(b->freq);
        }
        b->dirty = true;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetVolume(XboxDSBufferObject* b, LONG lVolume) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        b->volume = lVolume;
        ApplyBufferVolumeAndPan(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetHeadroom(XboxDSBufferObject* b, DWORD dwHeadroom) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        b->headroomMb = (dwHeadroom <= 10000u) ? static_cast<LONG>(dwHeadroom) : 10000;
        ApplyBufferVolumeAndPan(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetPitch(XboxDSBufferObject* b, LONG lPitch) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        b->lPitch = lPitch;
        b->freq   = XboxPitchToFrequencyHz(lPitch);
        RefreshBufferPitchAndFrequency(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFrequency(XboxDSBufferObject* b, DWORD dwFrequency) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        if (dwFrequency < kMinDirectSoundFreqHz) dwFrequency = kMinDirectSoundFreqHz;
        if (dwFrequency > kMaxXboxPitchFreqHz)   dwFrequency = kMaxXboxPitchFreqHz;
        b->freq   = dwFrequency;
        b->lPitch = FrequencyHzToXboxPitch(dwFrequency);
        RefreshBufferPitchAndFrequency(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetLFO(XboxDSBufferObject* b, const XBOX_DSLFODESC* pLFODesc) {
    if (!b) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    if (pLFODesc && reinterpret_cast<uintptr_t>(pLFODesc) >= 0x00010000u) {
        uint32_t idx = (pLFODesc->dwLFO == 1u) ? 1u : 0u;
        b->lfo[idx] = *pLFODesc;
        b->lfoActive[idx] = (pLFODesc->dwDelta > 0u) &&
                            (pLFODesc->lPitchModulation != 0 ||
                             pLFODesc->lFilterCutOffRange != 0 ||
                             pLFODesc->lAmplitudeModulation != 0);
    } else {
        b->lfoActive[0] = false;
        b->lfoActive[1] = false;
    }
    UpdateBufferLfoAndEg(b);
    RefreshBufferPitchAndFrequency(b);
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetEG(XboxDSBufferObject* b, const XBOX_DSENVELOPEDESC* pEnvelopeDesc) {
    if (!b) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    if (pEnvelopeDesc && reinterpret_cast<uintptr_t>(pEnvelopeDesc) >= 0x00010000u) {
        uint32_t idx = (pEnvelopeDesc->dwEG == 1u) ? 1u : 0u;
        b->eg[idx] = *pEnvelopeDesc;
        b->egActive[idx] = (pEnvelopeDesc->dwMode != 0u);
    } else {
        b->egActive[0] = false;
        b->egActive[1] = false;
    }
    UpdateBufferLfoAndEg(b);
    RefreshBufferPitchAndFrequency(b);
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetOutputBuffer(XboxDSBufferObject* b, XboxDSBufferObject* pOutputBuffer) {
    if (!b) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    uint32_t oldI3dl2Hash = GetQuantizedI3DL2StateHash(b);
    b->outputBuffer = (pOutputBuffer != b) ? pOutputBuffer : nullptr;
    uint32_t newI3dl2Hash = GetQuantizedI3DL2StateHash(b);
    if (newI3dl2Hash != oldI3dl2Hash) {
        RebakeBufferOnParameterChange(b);
    }
    ApplyBufferVolumeAndPan(b);
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFilter(XboxDSBufferObject* b, const XBOX_DSFILTERDESC* pFilterDesc) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        uint32_t oldHash = GetQuantizedFilterStateHash(b);
        if (pFilterDesc && reinterpret_cast<uintptr_t>(pFilterDesc) >= 0x00010000u && (pFilterDesc->dwMode & 3u) != 0u) {
            b->filterDesc      = *pFilterDesc;
            b->hasActiveFilter = true;
        } else {
            memset(&b->filterDesc, 0, sizeof(b->filterDesc));
            b->hasActiveFilter = false;
        }
        uint32_t newHash = GetQuantizedFilterStateHash(b);
        if (newHash != oldHash) {
            RebakeBufferOnParameterChange(b);
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_Set3DVoiceData(XboxDSBufferObject* b, const void* pVoiceData) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        uint32_t oldFilterHash = GetQuantizedFilterStateHash(b);
        b->hasI3DL2Send = true;
        Compute3DAudioPanAndAtten(pVoiceData, &b->pan, &b->atten3D, &b->voice3D);
        uint32_t newFilterHash = GetQuantizedFilterStateHash(b);
        if (newFilterHash != oldFilterHash) {
            RebakeBufferOnParameterChange(b);
        }
        RefreshBufferPitchAndFrequency(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_Use3DVoiceData(XboxDSBufferObject* b, const void* pVoiceData) {
    return Buffer_Set3DVoiceData(b, pVoiceData);
}

static HRESULT __stdcall Buffer_SetMixBins(XboxDSBufferObject* b, const XBOX_DSMIXBINS* pMixBins) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        uint32_t oldI3dl2Hash = GetQuantizedI3DL2StateHash(b);
        AssignVoiceMixBins(pMixBins, b->mixBinPairs, &b->mixBinCount);
        ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan);
        ExtractMixBinPairsI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
        uint32_t newI3dl2Hash = GetQuantizedI3DL2StateHash(b);
        if (newI3dl2Hash != oldI3dl2Hash) {
            RebakeBufferOnParameterChange(b);
        }
        ApplyBufferVolumeAndPan(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetMixBinVolumes(XboxDSBufferObject* b, const XBOX_DSMIXBINS* pMixBins) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        uint32_t oldI3dl2Hash = GetQuantizedI3DL2StateHash(b);
        UpdateVoiceMixBinVolumes(pMixBins, b->mixBinPairs, &b->mixBinCount);
        ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan);
        ExtractMixBinPairsI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
        uint32_t newI3dl2Hash = GetQuantizedI3DL2StateHash(b);
        if (newI3dl2Hash != oldI3dl2Hash) {
            RebakeBufferOnParameterChange(b);
        }
        ApplyBufferVolumeAndPan(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetPlayRegion(XboxDSBufferObject* b, DWORD dwPlayStart, DWORD dwPlayLength) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        b->playStart = dwPlayStart;
        b->playLength = dwPlayLength;
        b->isStreamingLockBuf = false;
        b->dirty = true;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetLoopRegion(XboxDSBufferObject* b, DWORD dwLoopStart, DWORD dwLoopLength) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        if (b->loopStart != dwLoopStart || b->loopLength != dwLoopLength) {
            b->loopStart = dwLoopStart;
            b->loopLength = dwLoopLength;
            b->dirty = true;
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_GetVoiceProperties(XboxDSBufferObject* b, XBOX_DSVOICEPROPS* pProps) {
    if (!pProps) return E_INVALIDARG;
    memset(pProps, 0, sizeof(*pProps));
    if (b) {
        EnterCriticalSection(&g_audioCs);
        // Populates all 92 bytes (0x5C) of XBOX_DSVOICEPROPS matching CMcpxVoiceClient::GetVoiceProperties (0x006E71C6):
        //   +0x00: dwMixBinCount
        //   +0x04..+0x43: MixBinVolumePairs[0..7] (unused slots filled with {0xFFFFFFFF, -10000})
        //   +0x44: lPitch
        //   +0x48..+0x58: 3D voice attenuation & Doppler pitch fields
        FillVoicePropsMixBins(b->mixBinPairs, b->mixBinCount, pProps);
        pProps->lPitch             = b->lPitch;
        pProps->l3DDistanceVolume  = b->voice3D.distanceVol;
        pProps->l3DConeVolume      = b->voice3D.coneVol;
        pProps->l3DDopplerPitch    = b->voice3D.dopplerPitch;
        pProps->lI3DL2DirectVolume = b->voice3D.i3dl2DirectVol;
        pProps->lI3DL2RoomVolume   = b->voice3D.i3dl2RoomVol;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static void* g_bufferVtable[4] = {
    reinterpret_cast<void*>(&Buffer_Release),
    reinterpret_cast<void*>(&Buffer_AddRef),
    reinterpret_cast<void*>(&Buffer_Release),
    reinterpret_cast<void*>(&Buffer_Release),
};

// ============================================================================
// IDirectSound Device Creation & Methods
// ============================================================================

static XboxDSObject g_singletonDS = { nullptr, 1 };

static ULONG __stdcall DS_AddRef(XboxDSObject* ds) {
    return ds ? (ULONG)InterlockedIncrement(&ds->refCount) : 1;
}

static ULONG __stdcall DS_Release(XboxDSObject* ds) {
    (void)ds;
    return 1;
}

static void* g_dsVtable[4] = {
    reinterpret_cast<void*>(&DS_Release),
    reinterpret_cast<void*>(&DS_AddRef),
    reinterpret_cast<void*>(&DS_Release),
    reinterpret_cast<void*>(&DS_Release),
};

static HRESULT __stdcall Bridge_DirectSoundCreate(const GUID* pguid, XboxDSObject** ppDS, void* pUnkOuter) {
    (void)pguid; (void)pUnkOuter;
    EnsureHostDirectSound();
    g_singletonDS.vtable = g_dsVtable;
    if (ppDS) *ppDS = &g_singletonDS;
    *reinterpret_cast<XboxDSObject**>(ADDR_DSOUND_SINGLETON_PTR) = &g_singletonDS; // m_pDirectSound
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_CreateSoundBuffer(
    XboxDSObject* pDS, const XBOX_DSBUFFERDESC* pDesc, XboxDSBufferObject** ppBuffer, void* pUnkOuter)
{
    (void)pDS; (void)pUnkOuter;
    if (!ppBuffer) return E_INVALIDARG;

    XboxDSBufferObject* b = new XboxDSBufferObject();
    memset(b, 0, sizeof(*b));
    b->vtable = g_bufferVtable;
    b->refCount = 1;
    b->freq = kDefaultSampleRateHz;
    b->lPitch = 0;
    b->i3dl2SendMb = kMinDirectSoundVolumeMb;
    b->lfoEgPitchRatio  = 1.0f;
    b->lfoEgCutoffRatio = 1.0f;
    InitVoiceDefaultMixBins(pDesc ? pDesc->dwFlags : 0u, b->mixBinPairs, &b->mixBinCount);
    if (pDesc) {
        if (pDesc->lpwfxFormat) {
            b->wfx = *pDesc->lpwfxFormat;
            b->isAdpcm = (b->wfx.wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
            if (b->wfx.nSamplesPerSec) {
                b->freq   = b->wfx.nSamplesPerSec;
                b->lPitch = FrequencyHzToXboxPitch(b->freq);
            }
        } else {
            b->wfx.wFormatTag = WAVE_FORMAT_PCM;
            b->wfx.nChannels = 2;
            b->wfx.nSamplesPerSec = kDefaultSampleRateHz;
            b->wfx.wBitsPerSample = 16;
            b->wfx.nBlockAlign = 4;
            b->wfx.nAvgBytesPerSec = kDefaultStreamBytesPerSec;
        }
        if (pDesc->dwBufferBytes > 0) {
            b->rawSize = pDesc->dwBufferBytes;
            b->rawBuf = (uint8_t*)calloc(1, b->rawSize);
            b->ownsRawBuf = true;
        }
        if (pDesc->lpMixBins) {
            const XBOX_DSMIXBINS* pMixBins = reinterpret_cast<const XBOX_DSMIXBINS*>(pDesc->lpMixBins);
            AssignVoiceMixBins(pMixBins, b->mixBinPairs, &b->mixBinCount);
            ComputeMixBinPairsPan(b->mixBinPairs, b->mixBinCount, &b->pan);
            ExtractMixBinPairsI3DL2Send(b->mixBinPairs, b->mixBinCount, &b->hasI3DL2Send, &b->i3dl2SendMb);
        }
    }
    EnterCriticalSection(&g_audioCs);
    g_activeBuffers.push_back(b);
    LeaveCriticalSection(&g_audioCs);
    *ppBuffer = b;
    return S_OK;
}

static XBOX_DSEFFECTIMAGEDESC* g_activeDspImageDesc = nullptr;
static std::vector<uint8_t>    g_dspScratchMemory;

static HRESULT __stdcall Bridge_IDirectSound_DownloadEffectsImage(
    XboxDSObject* pDS, const void* pvImageBuffer, DWORD dwImageSize, const void* pImageLoc, void** ppImageDesc)
{
    (void)pDS; (void)pImageLoc;
    EnterCriticalSection(&g_audioCs);

    // Parse the Xbox MCPX GP DSP effects image (e.g. dsstdfx.bin / nsidsp.bin) using the exact binary layout
    // verified against CMcpxGPDspManager::ParseEffectImageInfo (0x006E3D30):
    //   +0x804: total DSP code words (24-bit words stored in 32-bit DWORDs)
    //   +0x80C: total DSP state words
    //   +0x818: start of concatenated code + state blocks
    //   +0x818 + (codeWords + stateWords) * 4: XBOX_DSEFFECTIMAGEDESC { dwEffectCount, dwTotalScratchSize, aEffectMaps[...] }
    const uint8_t* img = static_cast<const uint8_t*>(pvImageBuffer);
    if (img && dwImageSize > 0x820u && reinterpret_cast<uintptr_t>(img) >= 0x00010000u) {
        uint32_t codeWords  = *reinterpret_cast<const uint32_t*>(img + 0x804);
        uint32_t stateWords = *reinterpret_cast<const uint32_t*>(img + 0x80C);
        uint64_t descOff64  = 0x818ULL + static_cast<uint64_t>(codeWords + stateWords) * 4ULL;
        if (descOff64 + 8ULL <= dwImageSize) {
            const XBOX_DSEFFECTIMAGEDESC* srcDesc = reinterpret_cast<const XBOX_DSEFFECTIMAGEDESC*>(img + descOff64);
            uint32_t fxCount = srcDesc->dwEffectCount;
            if (fxCount <= 64u && descOff64 + 8ULL + static_cast<uint64_t>(fxCount) * sizeof(XBOX_DSEFFECTMAP) <= dwImageSize) {
                size_t allocBytes = 8u + static_cast<size_t>(fxCount) * sizeof(XBOX_DSEFFECTMAP);
                if (g_activeDspImageDesc) {
                    free(g_activeDspImageDesc);
                    g_activeDspImageDesc = nullptr;
                }
                g_activeDspImageDesc = static_cast<XBOX_DSEFFECTIMAGEDESC*>(calloc(1, allocBytes));
                memcpy(g_activeDspImageDesc, srcDesc, allocBytes);

                uint32_t scratchSize = (srcDesc->dwTotalScratchSize > 0x4000u) ? srcDesc->dwTotalScratchSize : 0x10000u;
                g_dspScratchMemory.assign(scratchSize + 0x8000u, 0);
                uint8_t* stateBase = g_dspScratchMemory.data();
                uint8_t* scratchBase = stateBase + 0x8000u;

                // Copy initial DSP effect state words from the image into our host state buffer
                uint64_t stateImgOff = 0x818ULL + static_cast<uint64_t>(codeWords) * 4ULL;
                uint64_t stateBytes  = static_cast<uint64_t>(stateWords) * 4ULL;
                if (stateImgOff + stateBytes <= dwImageSize && stateBytes <= 0x8000u) {
                    memcpy(stateBase, img + stateImgOff, static_cast<size_t>(stateBytes));
                }

                // Populate valid host pointers for each effect's state/scratch segments
                for (uint32_t i = 0; i < fxCount; ++i) {
                    XBOX_DSEFFECTMAP& m = g_activeDspImageDesc->aEffectMaps[i];
                    uint32_t stOff = reinterpret_cast<uintptr_t>(m.lpvStateSegment) & 0x7FFFCu;
                    uint32_t scOff = reinterpret_cast<uintptr_t>(m.lpvScratchSegment) & (scratchSize - 1u);
                    m.lpvStateSegment   = stateBase + stOff;
                    m.lpvScratchSegment = scratchBase + scOff;
                }

                // If the DSP image contains an I3DL2 Reverb state block, sync initial reverb parameters
                if (ppImageDesc) {
                    *ppImageDesc = g_activeDspImageDesc;
                }
                LeaveCriticalSection(&g_audioCs);
                return S_OK;
            }
        }
    }

    if (!g_activeDspImageDesc) {
        g_activeDspImageDesc = static_cast<XBOX_DSEFFECTIMAGEDESC*>(calloc(1, 256));
    }
    if (ppImageDesc) *ppImageDesc = g_activeDspImageDesc;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_SetMixBinHeadroom(XboxDSObject*, DWORD, DWORD) { return S_OK; }

static HRESULT __stdcall Bridge_IDirectSound_SetI3DL2Listener(
    XboxDSObject* pDS, const void* pI3DL2, DWORD dwFlags)
{
    (void)pDS;
    if (!pI3DL2 || reinterpret_cast<uintptr_t>(pI3DL2) < 0x00010000u) return S_OK;
    EnterCriticalSection(&g_audioCs);
    const XBOX_DSI3DL2LISTENER* src = reinterpret_cast<const XBOX_DSI3DL2LISTENER*>(pI3DL2);
    if ((dwFlags & 1u) != 0) { // DS3D_DEFERRED
        g_i3dl2ListenerDeferred = *src;
        g_hasDeferredI3DL2      = true;
    } else {
        g_i3dl2Listener         = *src;
        g_i3dl2ListenerDeferred = *src;
        g_hasDeferredI3DL2      = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_CommitDeferredSettings(XboxDSObject* pDS) {
    (void)pDS;
    EnterCriticalSection(&g_audioCs);
    if (g_hasDeferredI3DL2) {
        g_i3dl2Listener    = g_i3dl2ListenerDeferred;
        g_hasDeferredI3DL2 = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

void InitXboxAudioBuffers() {
    InstallBridge(0x006E3096, &Bridge_DirectSoundCreate);                   // _DirectSoundCreate@12

    // IDirectSound methods
    InstallBridge(0x006E00AA, &DS_Release);                                 // _IDirectSound_Release@4
    InstallBridge(0x006E2E88, &Bridge_IDirectSound_CreateSoundBuffer);      // _IDirectSound_CreateSoundBuffer@16
    InstallBridge(0x006E1799, &Bridge_IDirectSound_DownloadEffectsImage);   // _IDirectSound_DownloadEffectsImage@20
    InstallBridge(0x006E17C0, &Bridge_IDirectSound_SetMixBinHeadroom);      // _IDirectSound_SetMixBinHeadroom@12
    InstallBridge(0x006E2ED0, &Bridge_IDirectSound_SetI3DL2Listener);       // _IDirectSound_SetI3DL2Listener@12
    InstallBridge(0x006E2C20, &Bridge_IDirectSound_CommitDeferredSettings); // _IDirectSound_CommitDeferredSettings@4

    // IDirectSoundBuffer methods
    InstallBridge(0x006E00C5, &Buffer_Release);                             // _IDirectSoundBuffer_Release@4
    InstallBridge(0x006E00C0, &Buffer_Unlock);                              // _IDirectSoundBuffer_Unlock@20
    InstallBridge(0x006E18A4, &Buffer_Play);                                // _IDirectSoundBuffer_Play@16
    InstallBridge(0x006E18C8, &Buffer_Stop);                                // _IDirectSoundBuffer_Stop@4
    InstallBridge(0x006E18E0, &Buffer_StopEx);                              // _IDirectSoundBuffer_StopEx@16
    InstallBridge(0x006E1904, &Buffer_SetLoopRegion);                       // _IDirectSoundBuffer_SetLoopRegion@12
    InstallBridge(0x006E1924, &Buffer_GetStatus);                           // _IDirectSoundBuffer_GetStatus@8
    InstallBridge(0x006E1940, &Buffer_GetCurrentPosition);                  // _IDirectSoundBuffer_GetCurrentPosition@12
    InstallBridge(0x006E1960, &Buffer_SetCurrentPosition);                  // _IDirectSoundBuffer_SetCurrentPosition@8
    InstallBridge(0x006E197C, &Buffer_Lock);                                // _IDirectSoundBuffer_Lock@32
    InstallBridge(0x006E19AC, &Buffer_GetVoiceProperties);                  // _IDirectSoundBuffer_GetVoiceProperties@8
    InstallBridge(0x006E19C8, &Buffer_Set3DVoiceData);                      // _IDirectSoundBuffer_Set3DVoiceData@8
    InstallBridge(0x006E19E4, &Buffer_Use3DVoiceData);                      // _IDirectSoundBuffer_Use3DVoiceData@8
    InstallBridge(0x006E17E0, &Buffer_SetVolume);                           // _IDirectSoundBuffer_SetVolume@8
    InstallBridge(0x006E17FC, &Buffer_SetPitch);                            // _IDirectSoundBuffer_SetPitch@8
    InstallBridge(0x006E1818, &Buffer_SetLFO);                              // _IDirectSoundBuffer_SetLFO@8
    InstallBridge(0x006E1834, &Buffer_SetEG);                               // _IDirectSoundBuffer_SetEG@8
    InstallBridge(0x006E1850, &Buffer_SetFilter);                           // _IDirectSoundBuffer_SetFilter@8
    InstallBridge(0x006E186C, &Buffer_SetHeadroom);                         // _IDirectSoundBuffer_SetHeadroom@8
    InstallBridge(0x006E1888, &Buffer_SetMixBinVolumes);                    // _IDirectSoundBuffer_SetMixBinVolumes@8
    InstallBridge(0x006E207D, &Buffer_SetFrequency);                        // _IDirectSoundBuffer_SetFrequency@8
    InstallBridge(0x006E2099, &Buffer_SetOutputBuffer);                     // _IDirectSoundBuffer_SetOutputBuffer@8
    InstallBridge(0x006E20B5, &Buffer_SetMixBins);                          // _IDirectSoundBuffer_SetMixBins@8
    InstallBridge(0x006E20D1, &Buffer_SetPlayRegion);                       // _IDirectSoundBuffer_SetPlayRegion@12
    InstallBridge(0x006E271F, &Buffer_SetFormat);                           // _IDirectSoundBuffer_SetFormat@8
    InstallBridge(0x006E273B, &Buffer_SetBufferData);                       // _IDirectSoundBuffer_SetBufferData@12
}
