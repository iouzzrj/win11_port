#include "ghost_runtime.h"
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <vector>

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "winmm.lib")

// ============================================================================
// Xbox DirectSound Structures & Constants
// ============================================================================

#define XBOX_WAVE_FORMAT_PCM        1
#define XBOX_WAVE_FORMAT_XBOX_ADPCM 0x0069

#pragma pack(push, 1)
struct XBOX_DSBUFFERDESC {
    uint32_t           dwSize;
    uint32_t           dwFlags;
    uint32_t           dwBufferBytes;
    const WAVEFORMATEX* lpwfxFormat;
    const void*        lpMixBins;
    uint32_t           dwInputMixBin;
};

struct XBOX_DSSTREAMDESC {
    uint32_t           dwFlags;
    uint32_t           dwMaxAttachedPackets;
    const WAVEFORMATEX* lpwfxFormat;
    void (__stdcall *lpfnCallback)(void* pStreamContext, void* pPacketContext, uint32_t dwStatus);
    void*              lpvContext;
    const void*        lpMixBins;
};

struct XBOX_XMEDIAPACKET {
    void*     pvBuffer;
    uint32_t  dwMaxSize;
    uint32_t* pdwCompletedSize;
    uint32_t* pdwStatus;
    union {
        HANDLE   hCompletionEvent;
        void*    pContext;
    };
    uint64_t* prtTimestamp;
};

struct XBOX_XMEDIAINFO {
    uint32_t dwFlags;
    uint32_t dwInputSize;
    uint32_t dwOutputSize;
    uint32_t dwMaxLookahead;
};

struct XBOX_DSVOICEPROPS {
    uint32_t dwMixBinCount;
    uint8_t  MixBins[64];
};

struct XBOX_DSMIXBINVOLUMEPAIR {
    uint32_t dwMixBin;
    int32_t  lVolume;
};

struct XBOX_DSMIXBINS {
    uint32_t                       dwMixBinCount;
    const XBOX_DSMIXBINVOLUMEPAIR* lpMixBinVolumePairs;
};
#pragma pack(pop)

// ============================================================================
// 3D Positional Audio, I3DL2 Occlusion & Pitch Conversion Helpers
// ============================================================================

static constexpr uintptr_t kMinValidXboxAddress               = 0x00400000u;
static constexpr uint32_t  kCSoundSourceVoiceDataOffset       = 0xE8u;
static constexpr uint32_t  kCSoundSourcePositionOffset        = 0x4Cu;
static constexpr uint32_t  kCSoundSourceModeOffset            = 0x84u;
static constexpr uint32_t  kCSoundSourceI3DL2DirectOffset     = 0xC4u;
static constexpr uint32_t  kCSoundSourceI3DL2ObstHfOffset     = 0xD8u;
static constexpr uint32_t  kCSoundSourceI3DL2ObstLfRatioOffset= 0xDCu;
static constexpr uint32_t  kCSoundSourceI3DL2OcclHfOffset     = 0xE0u;
static constexpr uint32_t  kCSoundSourceI3DL2OcclLfRatioOffset= 0xE4u;
static constexpr uint32_t  kCSoundSourceLinkedSourceOffset    = 0x150u;

static constexpr uint32_t  kVoiceDataDistanceVolOffset        = 0x04u;
static constexpr uint32_t  kVoiceDataConeVolOffset            = 0x08u;
static constexpr uint32_t  kVoiceDataI3DL2DirectVolOffset     = 0x1Cu;

static constexpr uint32_t  kXboxDs3dModeHeadRelative          = 1u;
static constexpr uint32_t  kXboxDs3dModeDisable               = 2u;

static constexpr LONG      kMinDirectSoundVolumeMb            = -10000;
static constexpr LONG      kMaxDirectSoundVolumeMb            = 0;
static constexpr LONG      kMinDirectSoundPanMb               = -10000;
static constexpr LONG      kMaxDirectSoundPanMb               = 10000;
static constexpr float     kMaxStereoPanMillibels             = 3500.0f;

static constexpr double    kXboxPitchReferenceFreqHz          = 48000.0;
static constexpr double    kXboxPitchUnitsPerOctave           = 4096.0;
static constexpr DWORD     kMinDirectSoundFreqHz              = 100u;
static constexpr DWORD     kMaxDirectSoundFreqHz              = 100000u;
static constexpr DWORD     kDefaultSampleRateHz               = 44100u;

static constexpr DWORD     kXboxStreamStatusReady             = 0x00000001u; // XMO_STATUSF_ACCEPT_INPUT_DATA | DSSTREAMSTATUS_READY
static constexpr DWORD     kXboxStreamStatusPlaying           = 0x00010000u; // DSSTREAMSTATUS_PLAYING
static constexpr DWORD     kXboxStreamStatusPaused            = 0x00020000u; // DSSTREAMSTATUS_PAUSED
static constexpr DWORD     kXboxStreamStatusStarved           = 0x00080000u; // DSSTREAMSTATUS_STARVED

static constexpr DWORD     kXboxStreamPauseResume             = 0u;          // DSSTREAMPAUSE_RESUME
static constexpr DWORD     kXboxStreamPausePause              = 1u;          // DSSTREAMPAUSE_PAUSE
static constexpr DWORD     kXboxStreamPausePauseEx            = 2u;          // DSSTREAMPAUSE_PAUSEEX
static constexpr uint32_t  kXboxMediaPacketStatusSuccess      = 0u;          // XMEDIAPACKET_STATUS_SUCCESS
static constexpr uint32_t  kXboxMediaPacketStatusPending      = 0x8000000Au; // XMEDIAPACKET_STATUS_PENDING
static constexpr uint32_t  kXboxMediaPacketStatusFlushed      = 0x40000005u; // XMEDIAPACKET_STATUS_FLUSHED

static constexpr uint32_t  kMinStreamPacketDurationMs         = 5u;
static constexpr uint32_t  kDefaultStreamBytesPerSec          = 176400u;
static constexpr int32_t   kWaveOutFallbackTimeoutMs          = 2000;
static constexpr uint16_t  kWaveOutStereoChannels             = 2u;
static constexpr uint16_t  kPcm16BitsPerSample                = 16u;
static constexpr int32_t   kPcm16MinSampleValue               = -32768;
static constexpr int32_t   kPcm16MaxSampleValue               = 32767;
static constexpr float     kMillibelsPerTwentyDecibels        = 2000.0f;

// cDivxVideo (.vid movie player in VIDPlayerXB.obj) addresses and struct offsets.
// Original cDivxVideo::FrameMove (0x0043DE80) and cDivxVideo::Render (0x0043DEE0) timed frames
// using `(rdtsc * 3) / 2200000`, which assumed the Original Xbox 733.333 MHz Pentium III clock
// and ran 5x-6x too fast on modern GHz CPUs.
static constexpr uint32_t  kAddrDivxVideoFrameMove            = 0x0043DE80u;
static constexpr uint32_t  kAddrDivxVideoRender               = 0x0043DEE0u;
static constexpr uint32_t  kAddrVidSimplePlayerDecodeNextFrame= 0x0043EB60u;
static constexpr uint32_t  kAddrVidSimplePlayerHandleAudio    = 0x0043EC00u;
static constexpr uint32_t  kAddrVidSimplePlayerStartPlayback  = 0x0043EC10u;
static constexpr uint32_t  kAddrVidRenderToOverlay            = 0x0043E420u;
static constexpr uint32_t  kAddrVidRenderToTexture            = 0x0043E5E0u;
static constexpr uint32_t  kAddrVidRenderToSurface            = 0x0043E670u;

static constexpr uint32_t  kDivxRenderModeOffset              = 0x000u;
static constexpr uint32_t  kDivxPlayerOffset                  = 0x004u;
static constexpr uint32_t  kDivxRenderOffset                  = 0x294u;
static constexpr uint32_t  kDivxDecodeNextFrameOffset         = 0x2BCu;
static constexpr uint32_t  kDivxFirstFrameOffset              = 0x2C0u;
static constexpr uint32_t  kDivxLastTimeMsOffset              = 0x2C4u;
static constexpr uint32_t  kDivxCurrentTimeMsOffset           = 0x2C8u;
static constexpr uint32_t  kDivxTargetTimeMsOffset            = 0x2D0u;
static constexpr uint32_t  kDivxFrameDurationMsOffset         = 0x2D8u;
static constexpr uint32_t  kDivxPausedOffset                  = 0x2DCu;

static constexpr uint32_t  kDivxRenderModeTextureSwizzled     = 0u;
static constexpr uint32_t  kDivxRenderModeTextureLinear       = 1u;
static constexpr uint32_t  kDivxRenderModeSurface             = 2u;
static constexpr uint32_t  kDivxRenderModeOverlay             = 3u;
static constexpr double    kDivxDefaultFrameDurationMs        = 1000.0 / 30.0;
static constexpr double    kDivxSleepThresholdMs              = 2.0;
static constexpr double    kDivxMaxLagCatchupMs               = 100.0;
static constexpr DWORD     kDivxWaitSleepMs                   = 1u;

static DWORD XboxPitchToFrequencyHz(LONG lPitch) {
    // Xbox DirectSound & XACT represent pitch in 1/4096ths of an octave relative to 48000 Hz
    // (see _XAudioCalculatePitch@4 at 0x006E00FA and XACT::CSoundCue::UpdatePitch at 0x007063A5).
    double freq = kXboxPitchReferenceFreqHz * pow(2.0, static_cast<double>(lPitch) / kXboxPitchUnitsPerOctave);
    if (freq < static_cast<double>(kMinDirectSoundFreqHz)) return kMinDirectSoundFreqHz;
    if (freq > static_cast<double>(kMaxDirectSoundFreqHz)) return kMaxDirectSoundFreqHz;
    return static_cast<DWORD>(freq + 0.5);
}

static int32_t GetSourceI3DL2DirectAttenMb(const uint8_t* srcCs) {
    // Extracts I3DL2 direct + obstruction/occlusion attenuation from a CSoundSource instance
    // (populated by cGameVoice::APISetOccluded at 0x00535180 -> CSoundSource::SetI3DL2Source at 0x00702C48).
    if (!srcCs || reinterpret_cast<uintptr_t>(srcCs) < kMinValidXboxAddress) return 0;
    int32_t lDirect     = *reinterpret_cast<const int32_t*>(srcCs + kCSoundSourceI3DL2DirectOffset);
    int32_t obstHfLevel = *reinterpret_cast<const int32_t*>(srcCs + kCSoundSourceI3DL2ObstHfOffset);
    float   obstLfRatio = *reinterpret_cast<const float*>(srcCs + kCSoundSourceI3DL2ObstLfRatioOffset);
    int32_t occlHfLevel = *reinterpret_cast<const int32_t*>(srcCs + kCSoundSourceI3DL2OcclHfOffset);
    float   occlLfRatio = *reinterpret_cast<const float*>(srcCs + kCSoundSourceI3DL2OcclLfRatioOffset);

    float lfAtten = 0.0f;
    if (_finite(obstLfRatio) && obstLfRatio >= 0.0f && obstLfRatio <= 1.0f) {
        lfAtten += static_cast<float>(obstHfLevel) * obstLfRatio;
    }
    if (_finite(occlLfRatio) && occlLfRatio >= 0.0f && occlLfRatio <= 1.0f) {
        lfAtten += static_cast<float>(occlHfLevel) * occlLfRatio;
    }
    int32_t total = lDirect + static_cast<int32_t>(lfAtten);
    if (total < kMinDirectSoundVolumeMb) total = kMinDirectSoundVolumeMb;
    if (total > kMaxDirectSoundVolumeMb) total = kMaxDirectSoundVolumeMb;
    return total;
}

static void Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb) {
    if (!outPan || !outAttenDb) return;
    if (!pVoiceData || reinterpret_cast<uintptr_t>(pVoiceData) < kMinValidXboxAddress) return;

    // In CSoundSource::Update3DProperties (0x007022AA), pVoiceData == (uint8_t*)pSoundSource + 0xE8 (_DS3DCALCVOICEDATA).
    // Before calling Set3DVoiceData, Update3DProperties invokes CDirectSound3DCalculator::Calculate3D (0x006E243F)
    // and CDirectSound3DCalculator::GetVoiceData (0x006E0E00), which populate:
    //   pVoiceData + 0x04: int32_t lDistanceVolume    (computed by CFullHRTFSource::GetDistanceVolume at 0x006E3786)
    //   pVoiceData + 0x08: int32_t lConeVolume        (computed by CFullHRTFSource::GetConeVolume at 0x006E3897)
    //   pVoiceData + 0x1C: int32_t lI3DL2DirectVolume (computed by I3DL2 calculator at 0x006E8FD1)
    const uint8_t* vd = reinterpret_cast<const uint8_t*>(pVoiceData);
    const uint8_t* cs = vd - kCSoundSourceVoiceDataOffset;
    uint32_t dwMode = *reinterpret_cast<const uint32_t*>(cs + kCSoundSourceModeOffset);
    if (dwMode == kXboxDs3dModeDisable) {
        *outPan = 0;
        *outAttenDb = 0;
        return;
    }

    int32_t distVol  = *reinterpret_cast<const int32_t*>(vd + kVoiceDataDistanceVolOffset);
    int32_t coneVol  = *reinterpret_cast<const int32_t*>(vd + kVoiceDataConeVolOffset);
    int32_t i3dl2Vol = GetSourceI3DL2DirectAttenMb(cs);
    if (i3dl2Vol == 0) {
        const uint8_t* linkedCs = *reinterpret_cast<const uint8_t* const*>(cs + kCSoundSourceLinkedSourceOffset);
        if (linkedCs && reinterpret_cast<uintptr_t>(linkedCs) >= kMinValidXboxAddress) {
            i3dl2Vol = GetSourceI3DL2DirectAttenMb(linkedCs);
        }
    }
    if (i3dl2Vol == 0) {
        int32_t vdI3dl2 = *reinterpret_cast<const int32_t*>(vd + kVoiceDataI3DL2DirectVolOffset);
        if (vdI3dl2 < 0 && vdI3dl2 >= kMinDirectSoundVolumeMb) {
            i3dl2Vol = vdI3dl2;
        }
    }

    LONG totalAtten = static_cast<LONG>(distVol + coneVol + i3dl2Vol);
    if (totalAtten < kMinDirectSoundVolumeMb) totalAtten = kMinDirectSoundVolumeMb;
    if (totalAtten > kMaxDirectSoundVolumeMb) totalAtten = kMaxDirectSoundVolumeMb;
    *outAttenDb = totalAtten;

    float sx = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 0);
    float sy = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 4);
    float sz = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 8);
    if (!_finite(sx) || !_finite(sy) || !_finite(sz)) {
        *outPan = 0;
        return;
    }

    // Global XACT 3D Listener at 0x007091E8
    float lx = *reinterpret_cast<const float*>(0x007091F0);
    float ly = *reinterpret_cast<const float*>(0x007091F4);
    float lz = *reinterpret_cast<const float*>(0x007091F8);
    float fx = *reinterpret_cast<const float*>(0x00709208);
    float fy = *reinterpret_cast<const float*>(0x0070920C);
    float fz = *reinterpret_cast<const float*>(0x00709210);
    float tx = *reinterpret_cast<const float*>(0x00709214);
    float ty = *reinterpret_cast<const float*>(0x00709218);
    float tz = *reinterpret_cast<const float*>(0x0070921C);

    float dx = (dwMode == kXboxDs3dModeHeadRelative) ? sx : (sx - lx);
    float dy = (dwMode == kXboxDs3dModeHeadRelative) ? sy : (sy - ly);
    float dz = (dwMode == kXboxDs3dModeHeadRelative) ? sz : (sz - lz);
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    float rx = 1.0f, ry = 0.0f, rz = 0.0f;
    if (dwMode != kXboxDs3dModeHeadRelative) {
        // Left-handed coordinate system: Right = Top x Front
        rx = ty * fz - tz * fy;
        ry = tz * fx - tx * fz;
        rz = tx * fy - ty * fx;
        float rLen = sqrtf(rx * rx + ry * ry + rz * rz);
        if (rLen > 0.001f) {
            rx /= rLen; ry /= rLen; rz /= rLen;
        } else {
            rx = 1.0f; ry = 0.0f; rz = 0.0f;
        }
    }

    float sinAz = (dist > 0.001f) ? ((dx * rx + dy * ry + dz * rz) / dist) : 0.0f;
    if (sinAz < -1.0f) sinAz = -1.0f;
    if (sinAz >  1.0f) sinAz =  1.0f;
    *outPan = static_cast<LONG>(sinAz * kMaxStereoPanMillibels);
}

static void ComputeMixBinsPan(const XBOX_DSMIXBINS* pMixBins, LONG* outPan) {
    if (!outPan || !pMixBins || pMixBins->dwMixBinCount == 0 || pMixBins->dwMixBinCount > 32 || !pMixBins->lpMixBinVolumePairs) return;
    if ((uintptr_t)pMixBins->lpMixBinVolumePairs < 0x00010000) return;

    float leftGain  = 0.0f;
    float rightGain = 0.0f;
    bool  hasLR     = false;

    for (uint32_t i = 0; i < pMixBins->dwMixBinCount; ++i) {
        uint32_t bin = pMixBins->lpMixBinVolumePairs[i].dwMixBin;
        int32_t  vol = pMixBins->lpMixBinVolumePairs[i].lVolume;
        if (vol < -10000) vol = -10000;
        if (vol > 0)      vol = 0;
        float g = powf(10.0f, (float)vol / 2000.0f);
        if (bin == 0 || bin == 4) { // FRONT_LEFT or BACK_LEFT
            leftGain += g;
            hasLR = true;
        } else if (bin == 1 || bin == 5) { // FRONT_RIGHT or BACK_RIGHT
            rightGain += g;
            hasLR = true;
        } else if (bin == 2) { // FRONT_CENTER
            leftGain  += g * 0.707f;
            rightGain += g * 0.707f;
        }
    }

    if (hasLR && (leftGain > 0.0001f || rightGain > 0.0001f)) {
        float diff = (rightGain - leftGain) / (rightGain + leftGain);
        LONG pan = (LONG)(diff * 3500.0f);
        if (pan < -10000) pan = -10000;
        if (pan >  10000) pan =  10000;
        *outPan = pan;
    }
}

// ============================================================================
// Xbox IMA ADPCM Decoder (36 bytes/channel block -> 64 PCM16 samples)
// ============================================================================

static const int kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t DecodeImaNibble(uint8_t nibble, int& predictor, int& stepIndex) {
    int step = kImaStepTable[stepIndex];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) predictor -= diff;
    else            predictor += diff;
    if (predictor > 32767)  predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    stepIndex += kImaIndexTable[nibble & 0x0F];
    if (stepIndex < 0)  stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    return (int16_t)predictor;
}

static void DecodeXboxAdpcmToPcm16(
    const uint8_t* src, uint32_t srcBytes, uint32_t channels, std::vector<int16_t>& outPcm)
{
    if (channels == 0 || channels > 2) channels = 1;
    uint32_t blockAlign = 36 * channels;
    uint32_t numBlocks = srcBytes / blockAlign;
    outPcm.resize(numBlocks * 65 * channels);
    size_t outIdx = 0;

    for (uint32_t b = 0; b < numBlocks; ++b) {
        const uint8_t* block = src + b * blockAlign;
        int pred[2] = { 0, 0 };
        int stepIdx[2] = { 0, 0 };
        for (uint32_t ch = 0; ch < channels; ++ch) {
            pred[ch] = (int)*reinterpret_cast<const int16_t*>(block + ch * 4);
            stepIdx[ch] = block[ch * 4 + 2];
            if (stepIdx[ch] < 0)  stepIdx[ch] = 0;
            if (stepIdx[ch] > 88) stepIdx[ch] = 88;
            outPcm[outIdx + ch] = (int16_t)pred[ch];
        }
        outIdx += channels;

        const uint8_t* dataPtr = block + 4 * channels;
        // 8 sub-blocks of 4 bytes per channel (8 samples per sub-block = 64 samples)
        for (int sub = 0; sub < 8; ++sub) {
            uint32_t words[2] = { 0, 0 };
            for (uint32_t ch = 0; ch < channels; ++ch) {
                words[ch] = *reinterpret_cast<const uint32_t*>(dataPtr + (sub * channels + ch) * 4);
            }
            for (int s = 0; s < 8; ++s) {
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    uint8_t nib = (uint8_t)((words[ch] >> (s * 4)) & 0x0F);
                    outPcm[outIdx++] = DecodeImaNibble(nib, pred[ch], stepIdx[ch]);
                }
            }
        }
    }
}

// ============================================================================
// Windows IDirectSound8 Host Backend
// ============================================================================

static IDirectSound8* g_hostDS = nullptr;

static void EnsureHostDirectSound() {
    if (g_hostDS || !g_hWnd) return;
    if (SUCCEEDED(DirectSoundCreate8(nullptr, &g_hostDS, nullptr))) {
        g_hostDS->SetCooperativeLevel(g_hWnd, DSSCL_PRIORITY);
    }
}

// ============================================================================
// Mock Xbox DirectSound, Buffer & Stream Objects
// ============================================================================

struct XboxDSObject {
    void**   vtable;
    LONG     refCount;
};

enum XboxBufferSubLoopPhase : uint8_t {
    kSubLoopPhaseNone    = 0,
    kSubLoopPhaseAttack  = 1,
    kSubLoopPhaseSustain = 2,
    kSubLoopPhaseRelease = 3,
};

struct XboxDSBufferObject {
    void**                vtable;
    LONG                  refCount;
    WAVEFORMATEX          wfx;
    bool                  isAdpcm;
    uint8_t*              rawBuf;
    uint32_t              rawSize;
    bool                  ownsRawBuf;
    uint32_t              playStart;
    uint32_t              playLength;
    uint32_t              loopStart;
    uint32_t              loopLength;
    LONG                  volume;
    LONG                  pan;
    LONG                  atten3D;
    DWORD                 freq;
    DWORD                 status;
    IDirectSoundBuffer*   hostBuf;
    uint32_t              hostBufBytes;
    uint64_t              hostCacheKey;
    bool                  dirty;
    bool                  isStreamingLockBuf;
    uint8_t               subLoopPhase;
};

struct PooledHostBuffer {
    IDirectSoundBuffer*   hostBuf;
    uint32_t              hostBufBytes;
    uint64_t              cacheKey;
    uint16_t              nChannels;
    uint16_t              wBitsPerSample;
};

struct PooledWaveOut {
    HWAVEOUT              hWaveOut;
    uint32_t              sampleRate;
    std::vector<WAVEHDR*> activeWaveHdrs;
};

struct PendingStreamPacket {
    void*     pContext;
    uint32_t* pdwStatus;
    uint32_t* pdwCompletedSize;
    uint32_t  completedBytes;
    HANDLE    hEvent;
    bool      hasEvent;
    uint32_t  completeTick;
    WAVEHDR*  wh;
};

struct XboxDSStreamObject {
    void**                vtable; // Points to 7-entry XMO vtable at +0x00!
    LONG                  refCount;
    WAVEFORMATEX          wfx;
    bool                  isAdpcm;
    void (__stdcall *lpfnCallback)(void* pStreamContext, void* pPacketContext, uint32_t dwStatus);
    void*                 lpvContext;
    LONG                  volume;
    LONG                  pan;
    LONG                  atten3D;
    DWORD                 freq;
    bool                  paused;
    HWAVEOUT              hWaveOut;
    uint32_t              waveOutRate;
    uint16_t              waveOutChannels;
    uint32_t              nextCompleteTick;
    double                resamplePos;
    int16_t               prevSampleL;
    int16_t               prevSampleR;
    bool                  hasPrevSample;
    std::vector<PendingStreamPacket> pending;
    std::vector<WAVEHDR*> activeWaveHdrs;
};

static std::vector<XboxDSStreamObject*> g_activeStreams;
static std::vector<XboxDSBufferObject*> g_activeBuffers;
static std::vector<PooledHostBuffer>    g_hostBufPool;
static std::vector<PooledWaveOut>       g_waveOutPool;
static CRITICAL_SECTION g_audioCs;
static bool g_audioCsInit = false;

static uint64_t ComputeWaveRegionCacheKey(
    const uint8_t* regionPtr, uint32_t regionBytes, uint16_t nChannels, uint16_t wBitsPerSample, uint8_t subLoopPhase)
{
    if (!regionPtr || regionBytes == 0) return 0;
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(reinterpret_cast<uintptr_t>(regionPtr));
    mix(regionBytes);
    mix((static_cast<uint64_t>(nChannels) << 16) | wBitsPerSample | (static_cast<uint64_t>(subLoopPhase) << 24));
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
        p.wBitsPerSample = (b->wfx.wBitsPerSample == 8 && !b->isAdpcm) ? 8 : 16;
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
    LONG baseVol = b->volume;
    // cVideoTexture::Initialize (0x005F4A1C) calls _BinkSetVolume(hBink, 0, 0x2000),
    // which maps to -1225 (-12.25 dB) on PCM Bink streaming buffers. Boost to 0 dB so
    // briefing & cutscene voiceover tracks play at full clarity.
    if (b->isStreamingLockBuf && !b->isAdpcm && baseVol >= -1500 && baseVol < 0) {
        baseVol = 0;
    }
    LONG vol = baseVol + b->atten3D;
    if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
    if (vol > kMaxDirectSoundVolumeMb) vol = kMaxDirectSoundVolumeMb;
    b->hostBuf->SetVolume(vol);

    LONG pan = b->pan;
    if (pan < kMinDirectSoundPanMb) pan = kMinDirectSoundPanMb;
    if (pan > kMaxDirectSoundPanMb) pan = kMaxDirectSoundPanMb;
    b->hostBuf->SetPan(pan);

    if (b->freq >= kMinDirectSoundFreqHz && b->freq <= kMaxDirectSoundFreqHz) {
        b->hostBuf->SetFrequency(b->freq);
    }
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

static void SyncBufferToHost(XboxDSBufferObject* b, uint8_t targetPhase = kSubLoopPhaseNone) {
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

    const uint8_t* regionPtr = b->rawBuf + startOff;
    uint16_t targetBits = (!b->isAdpcm && b->wfx.wBitsPerSample == 8) ? 8 : 16;
    uint64_t cacheKey = (!b->isStreamingLockBuf)
        ? ComputeWaveRegionCacheKey(regionPtr, regionBytes, pcmWfx.nChannels, targetBits, actualPhase)
        : 0ULL;

    if (!b->isStreamingLockBuf && cacheKey != 0) {
        if (b->hostBuf && b->hostCacheKey == cacheKey) {
            b->dirty        = false;
            b->subLoopPhase = actualPhase;
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
                b->hostBuf      = hit.hostBuf;
                b->hostBufBytes = hit.hostBufBytes;
                b->hostCacheKey = hit.cacheKey;
                b->dirty        = false;
                b->subLoopPhase = actualPhase;
                ApplyBufferVolumeAndPan(b);
                return;
            }
        }
    }

    std::vector<int16_t> decodedPcm;
    const void* srcPcm = regionPtr;
    uint32_t srcPcmBytes = regionBytes;

    if (b->isAdpcm) {
        DecodeXboxAdpcmToPcm16(regionPtr, regionBytes, pcmWfx.nChannels, decodedPcm);
        srcPcm = decodedPcm.data();
        srcPcmBytes = (uint32_t)(decodedPcm.size() * sizeof(int16_t));
    } else if (b->wfx.wBitsPerSample == 8) {
        pcmWfx.wBitsPerSample = 8;
        pcmWfx.nBlockAlign = pcmWfx.nChannels;
        pcmWfx.nAvgBytesPerSec = pcmWfx.nSamplesPerSec * pcmWfx.nBlockAlign;
    }

    if (srcPcmBytes < 64) return;

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
    }

    void* ptr1 = nullptr;
    DWORD bytes1 = 0;
    if (SUCCEEDED(b->hostBuf->Lock(0, srcPcmBytes, &ptr1, &bytes1, nullptr, nullptr, 0))) {
        memcpy(ptr1, srcPcm, bytes1);
        b->hostBuf->Unlock(ptr1, bytes1, nullptr, 0);
    }
    b->hostCacheKey = cacheKey;
    b->dirty        = false;
    b->subLoopPhase = actualPhase;
    ApplyBufferVolumeAndPan(b);
}

static void AdvanceBufferSubLoopIfNeeded(XboxDSBufferObject* b) {
    if (!b || b->status == 0 || !b->hostBuf) return;
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
            b->subLoopPhase = kSubLoopPhaseNone;
        }
    }
}

// ============================================================================
// IDirectSoundStream Virtual & C-API Methods
// ============================================================================

static ULONG __stdcall Stream_AddRef(XboxDSStreamObject* s) {
    return s ? (ULONG)InterlockedIncrement(&s->refCount) : 1;
}

static void ReapWaveHeaders(XboxDSStreamObject* s, bool forceAll) {
    if (!s || !s->hWaveOut) return;
    EnterCriticalSection(&g_audioCs);
    for (size_t i = 0; i < s->activeWaveHdrs.size(); ) {
        WAVEHDR* wh = s->activeWaveHdrs[i];
        bool referencedByPending = false;
        if (!forceAll) {
            for (size_t pi = 0; pi < s->pending.size(); ++pi) {
                if (s->pending[pi].wh == wh) {
                    referencedByPending = true;
                    break;
                }
            }
        }
        if (forceAll || ((wh->dwFlags & WHDR_DONE) && !referencedByPending)) {
            waveOutUnprepareHeader(s->hWaveOut, wh, sizeof(WAVEHDR));
            free(wh);
            s->activeWaveHdrs.erase(s->activeWaveHdrs.begin() + i);
        } else {
            ++i;
        }
    }
    LeaveCriticalSection(&g_audioCs);
}

static void ReapPooledWaveHeaders(PooledWaveOut& p, bool forceAll) {
    if (!p.hWaveOut) return;
    for (size_t i = 0; i < p.activeWaveHdrs.size(); ) {
        WAVEHDR* wh = p.activeWaveHdrs[i];
        if (forceAll || (wh->dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(p.hWaveOut, wh, sizeof(WAVEHDR));
            free(wh);
            p.activeWaveHdrs.erase(p.activeWaveHdrs.begin() + i);
        } else {
            ++i;
        }
    }
}

static void ReleaseStreamWaveOutToPool(XboxDSStreamObject* s) {
    if (!s || !s->hWaveOut) return;
    ReapWaveHeaders(s, false);
    uint32_t now = GetTickCount();
    if (!s->activeWaveHdrs.empty() && static_cast<int32_t>(s->nextCompleteTick - now) > 150) {
        waveOutReset(s->hWaveOut);
        ReapWaveHeaders(s, true);
    }
    if (s->paused) {
        waveOutRestart(s->hWaveOut);
        s->paused = false;
    }
    EnterCriticalSection(&g_audioCs);
    for (size_t i = 0; i < g_waveOutPool.size(); ++i) {
        ReapPooledWaveHeaders(g_waveOutPool[i], false);
    }
    if (g_waveOutPool.size() >= 48) {
        for (size_t i = 0; i < g_waveOutPool.size(); ++i) {
            if (g_waveOutPool[i].activeWaveHdrs.empty()) {
                waveOutClose(g_waveOutPool[i].hWaveOut);
                g_waveOutPool.erase(g_waveOutPool.begin() + i);
                break;
            }
        }
    }
    PooledWaveOut p = {};
    p.hWaveOut       = s->hWaveOut;
    p.sampleRate     = s->waveOutRate;
    p.activeWaveHdrs = std::move(s->activeWaveHdrs);
    g_waveOutPool.push_back(std::move(p));
    s->hWaveOut        = nullptr;
    s->waveOutRate     = 0;
    s->waveOutChannels = 0;
    s->activeWaveHdrs.clear();
    LeaveCriticalSection(&g_audioCs);
}

static ULONG __stdcall Stream_Release(XboxDSStreamObject* s) {
    if (!s) return 0;
    LONG r = InterlockedDecrement(&s->refCount);
    if (r <= 0) {
        EnterCriticalSection(&g_audioCs);
        for (size_t i = 0; i < g_activeStreams.size(); ++i) {
            if (g_activeStreams[i] == s) {
                g_activeStreams.erase(g_activeStreams.begin() + i);
                break;
            }
        }
        LeaveCriticalSection(&g_audioCs);
        if (s->hWaveOut) {
            ReleaseStreamWaveOutToPool(s);
        }
        delete s;
        return 0;
    }
    return (ULONG)r;
}

static HRESULT __stdcall Stream_GetInfo(XboxDSStreamObject* s, XBOX_XMEDIAINFO* pInfo) {
    (void)s;
    if (pInfo) {
        pInfo->dwFlags = 0x0000000B; // XMO_STREAMF_FIXED_SAMPLE_SIZE | INPUT_ASYNC
        pInfo->dwInputSize = 36;
        pInfo->dwOutputSize = 0;
        pInfo->dwMaxLookahead = 4096;
    }
    return S_OK;
}

static HRESULT __stdcall Stream_GetStatus(XboxDSStreamObject* s, DWORD* pdwStatus) {
    if (pdwStatus) {
        DWORD st = kXboxStreamStatusReady;
        if (s) {
            EnterCriticalSection(&g_audioCs);
            bool hasPending = !s->pending.empty();
            bool isPaused   = s->paused;
            LeaveCriticalSection(&g_audioCs);

            // VIDAudioHelper::Update (0x0043D4AB) checks DSSTREAMSTATUS_PAUSED (0x00020000) to unpause streams;
            // XACT::CSoundCue_StreamWork (0x00708BC7) checks DSSTREAMSTATUS_STARVED (0x00080000) to detect EOF drain.
            if (isPaused) {
                st |= kXboxStreamStatusPaused;
            }
            if (hasPending) {
                if (!isPaused) {
                    st |= kXboxStreamStatusPlaying;
                }
            } else {
                st |= kXboxStreamStatusStarved;
            }
        }
        *pdwStatus = st;
    }
    return S_OK;
}

static void EnsureStreamWaveOut(XboxDSStreamObject* s) {
    if (!s) return;
    // Always open pooled HWAVEOUT handles at a unified 48000 Hz stereo PCM16 format so all
    // pooled waveOut instances are 100% interchangeable without rate-mismatch reopening, and
    // dynamic XACT pitch/frequency envelopes (s->freq) can be resampled per packet in Stream_Process.
    uint32_t targetRate = kDefaultSampleRateHz;
    if (s->hWaveOut && s->waveOutRate == targetRate && s->waveOutChannels == kWaveOutStereoChannels) {
        return;
    }

    if (s->hWaveOut) {
        ReleaseStreamWaveOutToPool(s);
    }

    EnterCriticalSection(&g_audioCs);
    for (size_t i = 0; i < g_waveOutPool.size(); ++i) {
        ReapPooledWaveHeaders(g_waveOutPool[i], false);
        if (g_waveOutPool[i].sampleRate == targetRate && g_waveOutPool[i].activeWaveHdrs.empty()) {
            s->hWaveOut        = g_waveOutPool[i].hWaveOut;
            s->waveOutRate     = targetRate;
            s->waveOutChannels = kWaveOutStereoChannels;
            g_waveOutPool.erase(g_waveOutPool.begin() + i);
            LeaveCriticalSection(&g_audioCs);
            if (s->paused && s->hWaveOut) {
                waveOutPause(s->hWaveOut);
            }
            return;
        }
    }
    LeaveCriticalSection(&g_audioCs);

    WAVEFORMATEX pcmWfx = {};
    pcmWfx.wFormatTag      = WAVE_FORMAT_PCM;
    pcmWfx.nChannels       = kWaveOutStereoChannels;
    pcmWfx.nSamplesPerSec  = targetRate;
    pcmWfx.wBitsPerSample  = kPcm16BitsPerSample;
    pcmWfx.nBlockAlign     = (pcmWfx.nChannels * pcmWfx.wBitsPerSample) / 8u;
    pcmWfx.nAvgBytesPerSec = pcmWfx.nSamplesPerSec * pcmWfx.nBlockAlign;
    if (waveOutOpen(&s->hWaveOut, WAVE_MAPPER, &pcmWfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
        s->waveOutRate     = targetRate;
        s->waveOutChannels = kWaveOutStereoChannels;
        if (s->paused && s->hWaveOut) {
            waveOutPause(s->hWaveOut);
        }
    } else {
        s->hWaveOut        = nullptr;
        s->waveOutRate     = 0;
        s->waveOutChannels = 0;
    }
}

static HRESULT __stdcall Stream_Process(
    XboxDSStreamObject* s, const XBOX_XMEDIAPACKET* pInputPacket, const XBOX_XMEDIAPACKET* pOutputPacket)
{
    (void)pOutputPacket;
    if (!s || !pInputPacket) return S_OK;

    uint32_t bytes = pInputPacket->dwMaxSize;
    if (pInputPacket->pdwCompletedSize) {
        *pInputPacket->pdwCompletedSize = bytes;
    }

    if (pInputPacket->pvBuffer && bytes > 0) {
        EnsureStreamWaveOut(s);
    }
    ReapWaveHeaders(s, false);

    // Play audio packet through waveOut if valid PCM/ADPCM data
    uint32_t durationMs = kMinStreamPacketDurationMs;
    WAVEHDR* submittedWh = nullptr;
    if (pInputPacket->pvBuffer && bytes > 0 && s->hWaveOut) {
        std::vector<int16_t> decoded;
        const int16_t* pcmPtr = reinterpret_cast<const int16_t*>(pInputPacket->pvBuffer);
        uint32_t pcmBytes = bytes;
        uint32_t ch = s->wfx.nChannels ? s->wfx.nChannels : kWaveOutStereoChannels;
        if (s->isAdpcm) {
            DecodeXboxAdpcmToPcm16(reinterpret_cast<const uint8_t*>(pInputPacket->pvBuffer), bytes, ch, decoded);
            pcmPtr = decoded.data();
            pcmBytes = static_cast<uint32_t>(decoded.size() * sizeof(int16_t));
        }

        uint32_t inFrames = (ch > 0) ? (pcmBytes / (ch * sizeof(int16_t))) : 0u;
        if (inFrames > 0) {
            uint32_t inRate  = (s->freq >= kMinDirectSoundFreqHz && s->freq <= kMaxDirectSoundFreqHz)
                               ? s->freq
                               : (s->wfx.nSamplesPerSec ? s->wfx.nSamplesPerSec : kDefaultSampleRateHz);
            uint32_t outRate = s->waveOutRate ? s->waveOutRate : kDefaultSampleRateHz;

            if (!s->hasPrevSample) {
                s->prevSampleL   = pcmPtr[0];
                s->prevSampleR   = (ch >= kWaveOutStereoChannels) ? pcmPtr[1] : pcmPtr[0];
                s->resamplePos   = 0.0;
                s->hasPrevSample = true;
            }

            double step = static_cast<double>(inRate) / static_cast<double>(outRate);
            double startPos = s->resamplePos;
            bool exactRateMatch = (inRate == outRate && startPos == 0.0);
            uint32_t outFrames = 0u;
            if (exactRateMatch) {
                outFrames = inFrames;
            } else if (startPos < static_cast<double>(inFrames)) {
                outFrames = static_cast<uint32_t>((static_cast<double>(inFrames) - startPos - 1e-9) / step) + 1u;
            }

            if (outFrames > 0) {
                durationMs = (outFrames * 1000u) / outRate;
                if (durationMs < kMinStreamPacketDurationMs) durationMs = kMinStreamPacketDurationMs;

                // Always output 2-channel Stereo PCM16 frames at outRate (48000 Hz) to waveOut,
                // resampling from inRate (which reflects Stream_SetPitch / Stream_SetFrequency) with
                // continuous cross-packet fractional phase (s->resamplePos) and last-sample history
                // (s->prevSampleL/R), plus 3D panning gains.
                uint32_t outBytes = outFrames * kWaveOutStereoChannels * sizeof(int16_t);
                WAVEHDR* wh = static_cast<WAVEHDR*>(calloc(1, sizeof(WAVEHDR) + outBytes));
                if (wh) {
                    wh->lpData = reinterpret_cast<LPSTR>(wh + 1);
                    wh->dwBufferLength = outBytes;
                    int16_t* dstSamples = reinterpret_cast<int16_t*>(wh->lpData);

                    LONG totVol = s->volume + s->atten3D;
                    LONG pan    = s->pan;
                    if (totVol < kMinDirectSoundVolumeMb) totVol = kMinDirectSoundVolumeMb;
                    if (totVol > kMaxDirectSoundVolumeMb) totVol = kMaxDirectSoundVolumeMb;
                    float baseGain = (totVol < 0) ? powf(10.0f, static_cast<float>(totVol) / kMillibelsPerTwentyDecibels) : 1.0f;
                    float gainL = baseGain * ((pan > 0) ? powf(10.0f, -static_cast<float>(pan) / kMillibelsPerTwentyDecibels) : 1.0f);
                    float gainR = baseGain * ((pan < 0) ? powf(10.0f,  static_cast<float>(pan) / kMillibelsPerTwentyDecibels) : 1.0f);

                    if (exactRateMatch) {
                        if (ch >= kWaveOutStereoChannels) {
                            if (totVol == 0 && pan == 0 && ch == kWaveOutStereoChannels) {
                                memcpy(dstSamples, pcmPtr, outBytes);
                            } else {
                                for (uint32_t f = 0; f < outFrames; ++f) {
                                    int32_t sl = static_cast<int32_t>(pcmPtr[f * ch + 0] * gainL);
                                    int32_t sr = static_cast<int32_t>(pcmPtr[f * ch + 1] * gainR);
                                    dstSamples[f * 2 + 0] = static_cast<int16_t>(
                                        sl < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sl > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sl));
                                    dstSamples[f * 2 + 1] = static_cast<int16_t>(
                                        sr < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sr > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sr));
                                }
                            }
                        } else {
                            for (uint32_t f = 0; f < outFrames; ++f) {
                                float sm = static_cast<float>(pcmPtr[f]);
                                int32_t sl = static_cast<int32_t>(sm * gainL);
                                int32_t sr = static_cast<int32_t>(sm * gainR);
                                dstSamples[f * 2 + 0] = static_cast<int16_t>(
                                    sl < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sl > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sl));
                                dstSamples[f * 2 + 1] = static_cast<int16_t>(
                                    sr < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sr > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sr));
                            }
                        }
                    } else {
                        if (ch >= kWaveOutStereoChannels) {
                            for (uint32_t f = 0; f < outFrames; ++f) {
                                double srcPos = startPos + static_cast<double>(f) * step;
                                uint32_t idx = static_cast<uint32_t>(srcPos);
                                if (idx >= inFrames) idx = inFrames - 1u;
                                float frac = static_cast<float>(srcPos - static_cast<double>(idx));
                                if (frac < 0.0f) frac = 0.0f;
                                if (frac > 1.0f) frac = 1.0f;
                                float l0 = (idx == 0u) ? static_cast<float>(s->prevSampleL) : static_cast<float>(pcmPtr[(idx - 1u) * ch + 0]);
                                float l1 = static_cast<float>(pcmPtr[idx * ch + 0]);
                                float r0 = (idx == 0u) ? static_cast<float>(s->prevSampleR) : static_cast<float>(pcmPtr[(idx - 1u) * ch + 1]);
                                float r1 = static_cast<float>(pcmPtr[idx * ch + 1]);
                                int32_t sl = static_cast<int32_t>((l0 + (l1 - l0) * frac) * gainL);
                                int32_t sr = static_cast<int32_t>((r0 + (r1 - r0) * frac) * gainR);
                                dstSamples[f * 2 + 0] = static_cast<int16_t>(
                                    sl < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sl > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sl));
                                dstSamples[f * 2 + 1] = static_cast<int16_t>(
                                    sr < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sr > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sr));
                            }
                        } else {
                            for (uint32_t f = 0; f < outFrames; ++f) {
                                double srcPos = startPos + static_cast<double>(f) * step;
                                uint32_t idx = static_cast<uint32_t>(srcPos);
                                if (idx >= inFrames) idx = inFrames - 1u;
                                float frac = static_cast<float>(srcPos - static_cast<double>(idx));
                                if (frac < 0.0f) frac = 0.0f;
                                if (frac > 1.0f) frac = 1.0f;
                                float m0 = (idx == 0u) ? static_cast<float>(s->prevSampleL) : static_cast<float>(pcmPtr[idx - 1u]);
                                float m1 = static_cast<float>(pcmPtr[idx]);
                                float sm = m0 + (m1 - m0) * frac;
                                int32_t sl = static_cast<int32_t>(sm * gainL);
                                int32_t sr = static_cast<int32_t>(sm * gainR);
                                dstSamples[f * 2 + 0] = static_cast<int16_t>(
                                    sl < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sl > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sl));
                                dstSamples[f * 2 + 1] = static_cast<int16_t>(
                                    sr < kPcm16MinSampleValue ? kPcm16MinSampleValue : (sr > kPcm16MaxSampleValue ? kPcm16MaxSampleValue : sr));
                            }
                        }
                    }

                    if (waveOutPrepareHeader(s->hWaveOut, wh, sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
                        waveOutWrite(s->hWaveOut, wh, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                    {
                        submittedWh = wh;
                        EnterCriticalSection(&g_audioCs);
                        s->activeWaveHdrs.push_back(wh);
                        LeaveCriticalSection(&g_audioCs);
                    } else {
                        free(wh);
                    }
                }
            }

            double nextPos = exactRateMatch
                             ? 0.0
                             : (startPos + static_cast<double>(outFrames) * step - static_cast<double>(inFrames));
            s->resamplePos   = (nextPos > 0.0) ? nextPos : 0.0;
            s->prevSampleL   = pcmPtr[(inFrames - 1u) * ch + 0];
            s->prevSampleR   = (ch >= kWaveOutStereoChannels) ? pcmPtr[(inFrames - 1u) * ch + 1] : s->prevSampleL;
            s->hasPrevSample = true;
        }
    }

    // IMPORTANT: On Original Xbox, XMEDIAPACKET_STATUS_PENDING is 0x8000000A (E_PENDING).
    // Both callback-driven streams (Bink/DSound) and polling-driven streams (XACT CSoundSource)
    // require *pdwStatus == 0x8000000A until _DirectSoundDoWork@0 completes the packet!
    if (pInputPacket->pdwStatus) {
        *pInputPacket->pdwStatus = kXboxMediaPacketStatusPending;
    }

    EnterCriticalSection(&g_audioCs);
    uint32_t now = GetTickCount();
    if (static_cast<int32_t>(s->nextCompleteTick - now) < 0) {
        s->nextCompleteTick = now;
    }
    s->nextCompleteTick += durationMs;

    PendingStreamPacket pkt = {};
    pkt.pContext = pInputPacket->pContext;
    pkt.pdwStatus = pInputPacket->pdwStatus;
    pkt.pdwCompletedSize = pInputPacket->pdwCompletedSize;
    pkt.completedBytes = bytes;
    pkt.hEvent = (!s->lpfnCallback) ? pInputPacket->hCompletionEvent : nullptr;
    pkt.hasEvent = (!s->lpfnCallback && pInputPacket->hCompletionEvent != nullptr);
    pkt.completeTick = s->nextCompleteTick;
    pkt.wh = submittedWh;
    s->pending.push_back(pkt);
    LeaveCriticalSection(&g_audioCs);

    return S_OK;
}

static HRESULT __stdcall Stream_Discontinuity(XboxDSStreamObject* s) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->resamplePos   = 0.0;
        s->prevSampleL   = 0;
        s->prevSampleR   = 0;
        s->hasPrevSample = false;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static void CompleteAllPendingPackets(XboxDSStreamObject* s, uint32_t status) {
    if (!s) return;
    std::vector<PendingStreamPacket> toComplete;
    EnterCriticalSection(&g_audioCs);
    toComplete.swap(s->pending);
    s->nextCompleteTick = GetTickCount();
    LeaveCriticalSection(&g_audioCs);

    for (size_t i = 0; i < toComplete.size(); ++i) {
        if (toComplete[i].pdwCompletedSize) *toComplete[i].pdwCompletedSize = toComplete[i].completedBytes;
        if (toComplete[i].pdwStatus) *toComplete[i].pdwStatus = status;
        if (s->lpfnCallback) {
            s->lpfnCallback(s->lpvContext, toComplete[i].pContext, status);
        } else if (toComplete[i].hasEvent && toComplete[i].hEvent &&
                   (uintptr_t)toComplete[i].hEvent < kMinValidXboxAddress &&
                   ((uintptr_t)toComplete[i].hEvent & 1u) == 0)
        {
            SetEvent(toComplete[i].hEvent);
        }
    }
}

static HRESULT __stdcall Stream_Flush(XboxDSStreamObject* s) {
    if (s) {
        CompleteAllPendingPackets(s, kXboxMediaPacketStatusFlushed);
        EnterCriticalSection(&g_audioCs);
        s->resamplePos   = 0.0;
        s->prevSampleL   = 0;
        s->prevSampleR   = 0;
        s->hasPrevSample = false;
        LeaveCriticalSection(&g_audioCs);
        if (s->hWaveOut) {
            ReapWaveHeaders(s, false);
            uint32_t now = GetTickCount();
            if (!s->activeWaveHdrs.empty() && static_cast<int32_t>(s->nextCompleteTick - now) > 150) {
                waveOutReset(s->hWaveOut);
                ReapWaveHeaders(s, true);
            }
        }
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Pause(XboxDSStreamObject* s, DWORD dwPause) {
    if (s) {
        // In Xbox CMcpxStream::Pause (0x006E5947):
        //   dwPause == 0 (DSSTREAMPAUSE_RESUME) resumes playback;
        //   dwPause == 1 (DSSTREAMPAUSE_PAUSE) or 2 (DSSTREAMPAUSE_PAUSEEX) pauses playback;
        //   dwPause == 3 (DSSTREAMPAUSE_SYNCPAUSE, called by XACT::CSoundSource::Reset at 0x0070299C)
        //   only clears sync-pause without pausing an active stream.
        if (dwPause == kXboxStreamPauseResume) {
            s->paused = false;
            if (s->hWaveOut) waveOutRestart(s->hWaveOut);
        } else if (dwPause == kXboxStreamPausePause || dwPause == kXboxStreamPausePauseEx) {
            s->paused = true;
            if (s->hWaveOut) waveOutPause(s->hWaveOut);
        }
    }
    return S_OK;
}

static HRESULT __stdcall Stream_FlushEx(XboxDSStreamObject* s, uint32_t rtLow, uint32_t rtHigh, DWORD dwFlags) {
    (void)rtLow; (void)rtHigh;
    // In Xbox DirectSound (CMcpxStream::FlushEx, called by XACT::CSoundSource::FlushEx at 0x006E0D96
    // from XACT::CSoundCue::StreamData at 0x00708094 with DSSTREAMFLUSHEX_ASYNC = 0x1),
    // asynchronous FlushEx schedules the stream to transition to starved/stopped AFTER all currently
    // queued input packets finish playing. Calling Stream_Flush(s) here previously aborted active
    // packets immediately on EOF, cutting off short/looping streamed sounds every frame!
    if ((dwFlags & 1u) != 0) {
        return S_OK;
    }
    return Stream_Flush(s);
}

static HRESULT __stdcall Stream_SetVolume(XboxDSStreamObject* s, LONG lVolume) {
    if (s) s->volume = lVolume;
    return S_OK;
}

static HRESULT __stdcall Stream_SetPitch(XboxDSStreamObject* s, LONG lPitch) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->freq = XboxPitchToFrequencyHz(lPitch);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetFrequency(XboxDSStreamObject* s, DWORD dwFrequency) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->freq = dwFrequency;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_GetVoiceProperties(XboxDSStreamObject* s, XBOX_DSVOICEPROPS* pProps) {
    (void)s;
    if (pProps) memset(pProps, 0, sizeof(*pProps));
    return S_OK;
}

static HRESULT __stdcall Stream_SetFormat(XboxDSStreamObject* s, const WAVEFORMATEX* pwfx) {
    if (s && pwfx) {
        EnterCriticalSection(&g_audioCs);
        s->wfx = *pwfx;
        s->isAdpcm = (pwfx->wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
        if (pwfx->nSamplesPerSec) {
            s->freq = pwfx->nSamplesPerSec;
        }
        s->resamplePos   = 0.0;
        s->prevSampleL   = 0;
        s->prevSampleR   = 0;
        s->hasPrevSample = false;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Set3DVoiceData(XboxDSStreamObject* s, const void* pVoiceData) {
    if (s) {
        Compute3DAudioPanAndAtten(pVoiceData, &s->pan, &s->atten3D);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Use3DVoiceData(XboxDSStreamObject* s, const void* pVoiceData) {
    return Stream_Set3DVoiceData(s, pVoiceData);
}

static HRESULT __stdcall Stream_SetMixBins(XboxDSStreamObject* s, const XBOX_DSMIXBINS* pMixBins) {
    if (s) {
        ComputeMixBinsPan(pMixBins, &s->pan);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Stub2(XboxDSStreamObject*, uint32_t) { return S_OK; }

static void* g_streamVtable[7] = {
    reinterpret_cast<void*>(&Stream_AddRef),
    reinterpret_cast<void*>(&Stream_Release),
    reinterpret_cast<void*>(&Stream_GetInfo),
    reinterpret_cast<void*>(&Stream_GetStatus),
    reinterpret_cast<void*>(&Stream_Process),
    reinterpret_cast<void*>(&Stream_Discontinuity),
    reinterpret_cast<void*>(&Stream_Flush),
};

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
    b->status = 1; // DSBSTATUS_PLAYING
    bool isLooping = (dwFlags & 1u) != 0;
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
                ApplyBufferVolumeAndPan(b);
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
    b->subLoopPhase = kSubLoopPhaseNone;
    if (b->hostBuf) b->hostBuf->Stop();
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Buffer_StopEx(XboxDSBufferObject* b, uint32_t rtLow, uint32_t rtHigh, DWORD dwFlags) {
    (void)rtLow; (void)rtHigh;
    if (!b) return S_OK;
    // In Xbox CMcpxBuffer::StopEx (0x006E8981, called by XACT::CSoundSource::StopEx at 0x00701D1D),
    // DSBSTOPEX_RELEASEWAVEFORM (0x2) exits the sub-loop sustain region and plays the release tail
    // [loopStart + loopLength .. playLength) once before stopping.
    if ((dwFlags & 2u) != 0) {
        EnterCriticalSection(&g_audioCs);
        AdvanceBufferSubLoopIfNeeded(b);
        if (b->status == 1 && (b->subLoopPhase == kSubLoopPhaseAttack || b->subLoopPhase == kSubLoopPhaseSustain)) {
            BufferSubLoopGeometry g = GetBufferSubLoopGeometry(b);
            if (g.hasSubLoop && g.hasRelease) {
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
            b->freq = pwfx->nSamplesPerSec;
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

static HRESULT __stdcall Buffer_SetPitch(XboxDSBufferObject* b, LONG lPitch) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        b->freq = XboxPitchToFrequencyHz(lPitch);
        if (b->hostBuf) {
            b->hostBuf->SetFrequency(b->freq);
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_Set3DVoiceData(XboxDSBufferObject* b, const void* pVoiceData) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        Compute3DAudioPanAndAtten(pVoiceData, &b->pan, &b->atten3D);
        ApplyBufferVolumeAndPan(b);
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
        ComputeMixBinsPan(pMixBins, &b->pan);
        ApplyBufferVolumeAndPan(b);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Buffer_SetFrequency(XboxDSBufferObject* b, DWORD dwFrequency) {
    if (b) {
        EnterCriticalSection(&g_audioCs);
        b->freq = dwFrequency;
        if (b->hostBuf && dwFrequency >= kMinDirectSoundFreqHz && dwFrequency <= kMaxDirectSoundFreqHz) {
            b->hostBuf->SetFrequency(dwFrequency);
        }
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
    (void)b;
    if (pProps) memset(pProps, 0, sizeof(*pProps));
    return S_OK;
}

static HRESULT __stdcall Buffer_Stub2(XboxDSBufferObject*, uint32_t) { return S_OK; }

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
    *reinterpret_cast<XboxDSObject**>(0x006FE200) = &g_singletonDS; // m_pDirectSound
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
    if (pDesc) {
        if (pDesc->lpwfxFormat) {
            b->wfx = *pDesc->lpwfxFormat;
            b->isAdpcm = (b->wfx.wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
            if (b->wfx.nSamplesPerSec) {
                b->freq = b->wfx.nSamplesPerSec;
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
            ComputeMixBinsPan(reinterpret_cast<const XBOX_DSMIXBINS*>(pDesc->lpMixBins), &b->pan);
        }
    }
    EnterCriticalSection(&g_audioCs);
    g_activeBuffers.push_back(b);
    LeaveCriticalSection(&g_audioCs);
    *ppBuffer = b;
    return S_OK;
}

static HRESULT __stdcall Bridge_DirectSoundCreateStream(const XBOX_DSSTREAMDESC* pDesc, XboxDSStreamObject** ppStream) {
    if (!ppStream) return E_INVALIDARG;

    XboxDSStreamObject* s = new XboxDSStreamObject();
    s->vtable = g_streamVtable;
    s->refCount = 1;
    memset(&s->wfx, 0, sizeof(s->wfx));
    s->isAdpcm = false;
    s->lpfnCallback = nullptr;
    s->lpvContext = nullptr;
    s->volume = 0;
    s->pan = 0;
    s->atten3D = 0;
    s->freq = kDefaultSampleRateHz;
    s->paused = false;
    s->hWaveOut = nullptr;
    s->waveOutRate = 0;
    s->waveOutChannels = 0;
    s->nextCompleteTick = GetTickCount();
    s->resamplePos = 0.0;
    s->prevSampleL = 0;
    s->prevSampleR = 0;
    s->hasPrevSample = false;

    if (pDesc) {
        s->lpfnCallback = pDesc->lpfnCallback;
        s->lpvContext = pDesc->lpvContext;
        if (pDesc->lpwfxFormat) {
            s->wfx = *pDesc->lpwfxFormat;
            s->isAdpcm = (s->wfx.wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
            s->freq = s->wfx.nSamplesPerSec;
        }
        if (pDesc->lpMixBins) {
            ComputeMixBinsPan(reinterpret_cast<const XBOX_DSMIXBINS*>(pDesc->lpMixBins), &s->pan);
        }
    }

    EnterCriticalSection(&g_audioCs);
    g_activeStreams.push_back(s);
    LeaveCriticalSection(&g_audioCs);

    *ppStream = s;
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_CreateSoundStream(
    XboxDSObject* pDS, const XBOX_DSSTREAMDESC* pDesc, XboxDSStreamObject** ppStream, void* pUnkOuter)
{
    (void)pDS; (void)pUnkOuter;
    return Bridge_DirectSoundCreateStream(pDesc, ppStream);
}

static HRESULT __stdcall Bridge_IDirectSound_DownloadEffectsImage(
    XboxDSObject* pDS, const void* pvImageBuffer, DWORD dwImageSize, const void* pImageLoc, void** ppImageDesc)
{
    (void)pDS; (void)pvImageBuffer; (void)dwImageSize; (void)pImageLoc;
    static uint8_t s_dummyFxDesc[256] = { 0 };
    if (ppImageDesc) *ppImageDesc = s_dummyFxDesc;
    return S_OK;
}

static HRESULT __stdcall Bridge_IDirectSound_SetMixBinHeadroom(XboxDSObject*, DWORD, DWORD) { return S_OK; }
static HRESULT __stdcall Bridge_IDirectSound_SetI3DL2Listener(XboxDSObject*, const void*, DWORD) { return S_OK; }
static HRESULT __stdcall Bridge_IDirectSound_CommitDeferredSettings(XboxDSObject*) { return S_OK; }

void UpdateXboxAudioTick() {
    if (!g_audioCsInit) return;
    uint32_t now = GetTickCount();

    EnterCriticalSection(&g_audioCs);
    for (size_t bi = 0; bi < g_activeBuffers.size(); ++bi) {
        AdvanceBufferSubLoopIfNeeded(g_activeBuffers[bi]);
    }
    std::vector<XboxDSStreamObject*> streams = g_activeStreams;
    LeaveCriticalSection(&g_audioCs);

    for (size_t si = 0; si < streams.size(); ++si) {
        XboxDSStreamObject* s = streams[si];
        if (!s || s->paused) continue;

        std::vector<PendingStreamPacket> ready;
        EnterCriticalSection(&g_audioCs);
        for (size_t pi = 0; pi < s->pending.size(); ) {
            const PendingStreamPacket& pkt = s->pending[pi];
            bool isDone = false;
            if (s->hWaveOut && pkt.wh) {
                isDone = ((pkt.wh->dwFlags & WHDR_DONE) != 0) ||
                         (static_cast<int32_t>(now - pkt.completeTick) >= kWaveOutFallbackTimeoutMs);
            } else {
                isDone = (static_cast<int32_t>(now - pkt.completeTick) >= 0);
            }

            if (isDone) {
                ready.push_back(pkt);
                s->pending.erase(s->pending.begin() + pi);
            } else {
                ++pi;
            }
        }
        LeaveCriticalSection(&g_audioCs);

        ReapWaveHeaders(s, false);

        for (size_t ri = 0; ri < ready.size(); ++ri) {
            if (ready[ri].pdwCompletedSize) *ready[ri].pdwCompletedSize = ready[ri].completedBytes;
            if (ready[ri].pdwStatus) *ready[ri].pdwStatus = kXboxMediaPacketStatusSuccess;
            if (s->lpfnCallback) {
                s->lpfnCallback(s->lpvContext, ready[ri].pContext, kXboxMediaPacketStatusSuccess);
            } else if (ready[ri].hasEvent && ready[ri].hEvent &&
                       (uintptr_t)ready[ri].hEvent < kMinValidXboxAddress &&
                       ((uintptr_t)ready[ri].hEvent & 1u) == 0)
            {
                SetEvent(ready[ri].hEvent);
            }
        }
    }
}

static void __stdcall Bridge_DirectSoundDoWork() {
    UpdateXboxAudioTick();
}

static volatile uint32_t g_mcpxSampleCounter = 0;

static const uint32_t kCSoundCueStopCont = 0x007071C1;
static void __declspec(naked) Bridge_XACT_CSoundCue_Stop() {
    __asm {
        test ecx, ecx
        jnz  valid_stop_this
        xor  eax, eax
        ret  4
    valid_stop_this:
        mov  eax, dword ptr [esp+4]
        push esi
        mov  esi, ecx
        jmp  dword ptr [kCSoundCueStopCont]
    }
}

static const uint32_t kCSoundCuePlayCont = 0x00708CF1;
static void __declspec(naked) Bridge_XACT_CSoundCue_Play() {
    __asm {
        test ecx, ecx
        jnz  valid_play_this
        xor  eax, eax
        ret  4
    valid_play_this:
        mov  eax, dword ptr [esp+4]
        test al, al
        jmp  dword ptr [kCSoundCuePlayCont]
    }
}

static void __fastcall Bridge_XACT_CEngine_GetTime(void* thisPtr, int dummyEdx, int64_t* pTime) {
    (void)thisPtr; (void)dummyEdx;
    uint64_t ms = GetTickCount64();
    g_mcpxSampleCounter = (uint32_t)(ms * 48ULL);
    if (pTime) {
        *pTime = (int64_t)(ms * 10000ULL);
    }
}

// ============================================================================
// cDivxVideo (.vid Movie Player) Wall-Clock Frame Pacing Bridges
// ============================================================================
// Original cDivxVideo::FrameMove (0x0043DE80) and cDivxVideo::Render (0x0043DEE0)
// computed elapsed milliseconds via `(rdtsc * 3) / 2200000`, assuming the Original Xbox
// 733.333 MHz Pentium III CPU. On modern GHz CPUs, RDTSC ticks 5x-6x faster, causing
// blizzard_logo.vid, Nihilistic_Logo.vid, and Attract_Mode.vid to play in fast-forward
// and overflow VIDAudioHelper's packet ring buffer.

static HRESULT __fastcall Bridge_cDivxVideo_FrameMove(uint8_t* thisPtr, int dummyEdx) {
    (void)dummyEdx;
    if (!thisPtr) return 1;

    uint32_t bFirstFrame = *reinterpret_cast<uint32_t*>(thisPtr + kDivxFirstFrameOffset);
    if (bFirstFrame == 0) {
        *reinterpret_cast<uint32_t*>(thisPtr + kDivxDecodeNextFrameOffset) = 1u;
    }

    if (*reinterpret_cast<uint32_t*>(thisPtr + kDivxDecodeNextFrameOffset) != 0) {
        typedef uint8_t (__fastcall *PFN_DecodeNextFrame)(void* playerThis, int edx);
        PFN_DecodeNextFrame fnDecode = reinterpret_cast<PFN_DecodeNextFrame>(kAddrVidSimplePlayerDecodeNextFrame);
        uint8_t decodedOk = fnDecode(thisPtr + kDivxPlayerOffset, 0);
        if (!decodedOk) {
            return 1; // End of .vid stream
        }
    }
    return 0;
}

static HRESULT __fastcall Bridge_cDivxVideo_Render(uint8_t* thisPtr, int dummyEdx) {
    (void)dummyEdx;
    if (!thisPtr) return 0;

    typedef uint8_t (__fastcall *PFN_RenderToTexture)(void* renderThis, int edx, int mode);
    typedef uint8_t (__fastcall *PFN_RenderNoArg)(void* renderThis, int edx);
    void* pRender = thisPtr + kDivxRenderOffset;
    uint32_t renderMode = *reinterpret_cast<uint32_t*>(thisPtr + kDivxRenderModeOffset);

    switch (renderMode) {
        case kDivxRenderModeTextureSwizzled:
            reinterpret_cast<PFN_RenderToTexture>(kAddrVidRenderToTexture)(pRender, 0, 0);
            break;
        case kDivxRenderModeTextureLinear:
            reinterpret_cast<PFN_RenderToTexture>(kAddrVidRenderToTexture)(pRender, 0, 1);
            break;
        case kDivxRenderModeSurface:
            reinterpret_cast<PFN_RenderNoArg>(kAddrVidRenderToSurface)(pRender, 0);
            break;
        case kDivxRenderModeOverlay:
            reinterpret_cast<PFN_RenderNoArg>(kAddrVidRenderToOverlay)(pRender, 0);
            break;
        default:
            break;
    }

    typedef void (__fastcall *PFN_PlayerNoArg)(void* playerThis, int edx);
    void* pPlayer = thisPtr + kDivxPlayerOffset;

    if (*reinterpret_cast<uint32_t*>(thisPtr + kDivxPausedOffset) == 0) {
        uint32_t* pFirstFrame    = reinterpret_cast<uint32_t*>(thisPtr + kDivxFirstFrameOffset);
        uint32_t* pLastTimeMs    = reinterpret_cast<uint32_t*>(thisPtr + kDivxLastTimeMsOffset);
        double*   pCurrentTimeMs = reinterpret_cast<double*>(thisPtr + kDivxCurrentTimeMsOffset);
        double*   pTargetTimeMs  = reinterpret_cast<double*>(thisPtr + kDivxTargetTimeMsOffset);
        float     frameDurMs     = *reinterpret_cast<float*>(thisPtr + kDivxFrameDurationMsOffset);
        double    frameStepMs    = (_finite(frameDurMs) && frameDurMs > 1.0f && frameDurMs < 200.0f)
                                   ? static_cast<double>(frameDurMs)
                                   : kDivxDefaultFrameDurationMs;

        uint32_t nowMs = GetTickCount();
        if (*pFirstFrame != 0) {
            *pFirstFrame    = 0;
            *pLastTimeMs    = nowMs;
            *pCurrentTimeMs = 0.0;
            *pTargetTimeMs  = 0.0;
        } else {
            uint32_t lastMs    = *pLastTimeMs;
            double   currentMs = *pCurrentTimeMs + static_cast<double>(nowMs - lastMs);
            double   targetMs  = *pTargetTimeMs + frameStepMs;
            lastMs = nowMs;

            while (currentMs < targetMs) {
                UpdateXboxKernelTick();
                UpdateXboxInputTick();
                UpdateXboxAudioTick();
                if (targetMs - currentMs > kDivxSleepThresholdMs) {
                    Sleep(kDivxWaitSleepMs);
                } else {
                    SwitchToThread();
                }
                nowMs = GetTickCount();
                currentMs += static_cast<double>(nowMs - lastMs);
                lastMs = nowMs;
            }

            if (currentMs > targetMs + kDivxMaxLagCatchupMs) {
                currentMs = targetMs;
            }
            *pLastTimeMs    = lastMs;
            *pCurrentTimeMs = currentMs;
            *pTargetTimeMs  = targetMs;
        }

        reinterpret_cast<PFN_PlayerNoArg>(kAddrVidSimplePlayerHandleAudio)(pPlayer, 0);
    }

    reinterpret_cast<PFN_PlayerNoArg>(kAddrVidSimplePlayerStartPlayback)(pPlayer, 0);
    return 0;
}

// ============================================================================
// Initialize Xbox Audio Bridges
// ============================================================================

void InitXboxAudio() {
    if (!g_audioCsInit) {
        InitializeCriticalSection(&g_audioCs);
        g_audioCsInit = true;
    }

    // 1. Neutralize Xbox-specific IRQL & FPU state helpers in DSOUND and XACTENG
    // Note: DirectSoundEnterCriticalSection (0x006E001E) and XACTEnterCriticalSection (0x006FE3E0)
    // already have their `movzx eax, byte ptr fs:[0x24]` patched to `xor eax, eax; nop x6` in
    // InitXboxKernelAndXapi(), so they properly acquire and release their critical sections.
    PatchRet(0x006DFE10, 0, 0, false); // ?Raise@CIrql@DirectSound@@QAEXXZ
    PatchRet(0x006DFE32, 0, 0, false); // ?Lower@CIrql@DirectSound@@QAEXXZ
    PatchRet(0x006DFE49, 0, 0, false); // ?Save@CFpState@DirectSound@@QAEXXZ
    PatchRet(0x006DFE6E, 0, 0, false); // ?Restore@CFpState@DirectSound@@QAEXXZ
    PatchRet(0x006FE90A, 0, 0, false); // ?Raise@CIrql@XACT@@QAEXXZ
    PatchRet(0x006FE92C, 0, 0, false); // ?Lower@CIrql@XACT@@QAEXXZ

    // Redirect MCPX APU Sample Counter (0xFE80200C) reads in XACTENG & XPP to g_mcpxSampleCounter
    PatchU32(0x00703BF7, reinterpret_cast<uint32_t>(&g_mcpxSampleCounter));
    PatchU32(0x0074E54B, reinterpret_cast<uint32_t>(&g_mcpxSampleCounter));
    InstallBridge(0x00703BDF, &Bridge_XACT_CEngine_GetTime,               "?GetTime@CEngine@XACT@@QAEXPA_J@Z");
    InstallBridge(0x007071BA, &Bridge_XACT_CSoundCue_Stop,                "?Stop@CSoundCue@XACT@@QAEJK@Z");
    InstallBridge(0x00708CEB, &Bridge_XACT_CSoundCue_Play,                "?Play@CSoundCue@XACT@@QAEJK@Z");

    // 2. Top-level DirectSound Creation & DoWork
    InstallBridge(0x006E3096, &Bridge_DirectSoundCreate,                  "_DirectSoundCreate@12");
    InstallBridge(0x006E30DD, &Bridge_DirectSoundCreateStream,            "_DirectSoundCreateStream@8");
    InstallBridge(0x006E1A58, &Bridge_DirectSoundDoWork,                  "_DirectSoundDoWork@0");
    // Initialize CHRTFSource::m_vtable (0x006FE22C) via SetFullHRTF5Channel (0x006E3C2D)
    reinterpret_cast<void (__stdcall *)()>(0x006E3C2D)();

    // 3. IDirectSound methods
    InstallBridge(0x006E00AA, &DS_Release,                                "_IDirectSound_Release@4");
    InstallBridge(0x006E2E88, &Bridge_IDirectSound_CreateSoundBuffer,     "_IDirectSound_CreateSoundBuffer@16");
    InstallBridge(0x006E2EAC, &Bridge_IDirectSound_CreateSoundStream,     "_IDirectSound_CreateSoundStream@16");
    InstallBridge(0x006E1799, &Bridge_IDirectSound_DownloadEffectsImage,  "_IDirectSound_DownloadEffectsImage@20");
    InstallBridge(0x006E17C0, &Bridge_IDirectSound_SetMixBinHeadroom,     "_IDirectSound_SetMixBinHeadroom@12");
    InstallBridge(0x006E2ED0, &Bridge_IDirectSound_SetI3DL2Listener,      "_IDirectSound_SetI3DL2Listener@12");
    InstallBridge(0x006E2C20, &Bridge_IDirectSound_CommitDeferredSettings,"_IDirectSound_CommitDeferredSettings@4");

    // 4. IDirectSoundBuffer methods
    InstallBridge(0x006E00C5, &Buffer_Release,                            "_IDirectSoundBuffer_Release@4");
    InstallBridge(0x006E00C0, &Buffer_Unlock,                             "_IDirectSoundBuffer_Unlock@20");
    InstallBridge(0x006E18A4, &Buffer_Play,                               "_IDirectSoundBuffer_Play@16");
    InstallBridge(0x006E18C8, &Buffer_Stop,                               "_IDirectSoundBuffer_Stop@4");
    InstallBridge(0x006E18E0, &Buffer_StopEx,                             "_IDirectSoundBuffer_StopEx@16");
    InstallBridge(0x006E1904, &Buffer_SetLoopRegion,                      "_IDirectSoundBuffer_SetLoopRegion@12");
    InstallBridge(0x006E1924, &Buffer_GetStatus,                          "_IDirectSoundBuffer_GetStatus@8");
    InstallBridge(0x006E1940, &Buffer_GetCurrentPosition,                 "_IDirectSoundBuffer_GetCurrentPosition@12");
    InstallBridge(0x006E1960, &Buffer_SetCurrentPosition,                 "_IDirectSoundBuffer_SetCurrentPosition@8");
    InstallBridge(0x006E197C, &Buffer_Lock,                               "_IDirectSoundBuffer_Lock@32");
    InstallBridge(0x006E19AC, &Buffer_GetVoiceProperties,                 "_IDirectSoundBuffer_GetVoiceProperties@8");
    InstallBridge(0x006E19C8, &Buffer_Set3DVoiceData,                     "_IDirectSoundBuffer_Set3DVoiceData@8");
    InstallBridge(0x006E19E4, &Buffer_Use3DVoiceData,                     "_IDirectSoundBuffer_Use3DVoiceData@8");
    InstallBridge(0x006E17E0, &Buffer_SetVolume,                          "_IDirectSoundBuffer_SetVolume@8");
    InstallBridge(0x006E17FC, &Buffer_SetPitch,                           "_IDirectSoundBuffer_SetPitch@8");
    InstallBridge(0x006E1818, &Buffer_Stub2,                              "_IDirectSoundBuffer_SetLFO@8");
    InstallBridge(0x006E1834, &Buffer_Stub2,                              "_IDirectSoundBuffer_SetEG@8");
    InstallBridge(0x006E1850, &Buffer_Stub2,                              "_IDirectSoundBuffer_SetFilter@8");
    InstallBridge(0x006E186C, &Buffer_Stub2,                              "_IDirectSoundBuffer_SetHeadroom@8");
    InstallBridge(0x006E1888, &Buffer_SetMixBins,                         "_IDirectSoundBuffer_SetMixBinVolumes@8");
    InstallBridge(0x006E207D, &Buffer_SetFrequency,                       "_IDirectSoundBuffer_SetFrequency@8");
    InstallBridge(0x006E2099, &Buffer_Stub2,                              "_IDirectSoundBuffer_SetOutputBuffer@8");
    InstallBridge(0x006E20B5, &Buffer_SetMixBins,                         "_IDirectSoundBuffer_SetMixBins@8");
    InstallBridge(0x006E20D1, &Buffer_SetPlayRegion,                      "_IDirectSoundBuffer_SetPlayRegion@12");
    InstallBridge(0x006E271F, &Buffer_SetFormat,                          "_IDirectSoundBuffer_SetFormat@8");
    InstallBridge(0x006E273B, &Buffer_SetBufferData,                      "_IDirectSoundBuffer_SetBufferData@12");

    // 5. IDirectSoundStream / CDirectSoundStream target methods (called by 5-byte thunks at 0x006E1A00..0x006E275B)
    InstallBridge(0x006E14B9, &Stream_SetVolume,                          "CDirectSoundStream::SetVolume");
    InstallBridge(0x006E1467, &Stream_SetPitch,                           "CDirectSoundStream::SetPitch");
    InstallBridge(0x006E150B, &Stream_Stub2,                              "CDirectSoundStream::SetLFO");
    InstallBridge(0x006E155D, &Stream_Stub2,                              "CDirectSoundStream::SetEG");
    InstallBridge(0x006E15AF, &Stream_Stub2,                              "CDirectSoundStream::SetFilter");
    InstallBridge(0x006E1601, &Stream_Stub2,                              "CDirectSoundStream::SetHeadroom");
    InstallBridge(0x006E1653, &Stream_SetMixBins,                         "CDirectSoundStream::SetMixBinVolumes");
    InstallBridge(0x006E0D45, &Stream_Pause,                              "CDirectSoundStream::Pause");
    InstallBridge(0x006E0D96, &Stream_FlushEx,                            "CDirectSoundStream::FlushEx");
    InstallBridge(0x006E16A5, &Stream_GetVoiceProperties,                 "CDirectSoundStream::GetVoiceProperties");
    InstallBridge(0x006E16F7, &Stream_Set3DVoiceData,                     "CDirectSoundStream::Set3DVoiceData");
    InstallBridge(0x006E1749, &Stream_Use3DVoiceData,                     "CDirectSoundStream::Use3DVoiceData");
    InstallBridge(0x006E1F87, &Stream_SetFrequency,                       "CDirectSoundStream::SetFrequency");
    InstallBridge(0x006E1FD9, &Stream_Stub2,                              "CDirectSoundStream::SetOutputBuffer");
    InstallBridge(0x006E202B, &Stream_SetMixBins,                         "CDirectSoundStream::SetMixBins");
    InstallBridge(0x006E26CD, &Stream_SetFormat,                          "CDirectSoundStream::SetFormat");

    // 6. cDivxVideo (.vid Movie Player) wall-clock frame pacing bridges
    InstallBridge(kAddrDivxVideoFrameMove, &Bridge_cDivxVideo_FrameMove,  "?FrameMove@cDivxVideo@@QAEJXZ");
    InstallBridge(kAddrDivxVideoRender,    &Bridge_cDivxVideo_Render,     "?Render@cDivxVideo@@QAEJXZ");

    LogPrintf("[Init] Xbox DirectSound / XACT audio bridges installed.");
}
