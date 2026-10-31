#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <math.h>
#include <vector>

// ============================================================================
// 3D Positional Audio, I3DL2 Occlusion & Pitch Conversion Helpers
// ============================================================================

static constexpr uint32_t  kCSoundSourceVoiceDataOffset        = 0xE8u;
static constexpr uint32_t  kCSoundSourcePositionOffset         = 0x4Cu;
static constexpr uint32_t  kCSoundSourceModeOffset             = 0x84u;
static constexpr uint32_t  kCSoundSourceI3DL2DirectOffset      = 0xC4u;
static constexpr uint32_t  kCSoundSourceI3DL2ObstHfOffset      = 0xD8u;
static constexpr uint32_t  kCSoundSourceI3DL2ObstLfRatioOffset = 0xDCu;
static constexpr uint32_t  kCSoundSourceI3DL2OcclHfOffset      = 0xE0u;
static constexpr uint32_t  kCSoundSourceI3DL2OcclLfRatioOffset = 0xE4u;
static constexpr uint32_t  kCSoundSourceLinkedSourceOffset     = 0x150u;

static constexpr uint32_t  kVoiceDataDistanceVolOffset         = 0x04u;
static constexpr uint32_t  kVoiceDataConeVolOffset             = 0x08u;
static constexpr uint32_t  kVoiceDataI3DL2DirectVolOffset      = 0x1Cu;

static constexpr uint32_t  kXboxDs3dModeHeadRelative           = 1u;
static constexpr uint32_t  kXboxDs3dModeDisable                = 2u;

static constexpr LONG      kMinDirectSoundVolumeMb             = -10000;
static constexpr LONG      kMaxDirectSoundVolumeMb             = 0;
static constexpr float     kMaxStereoPanMillibels              = 3500.0f;

static constexpr double    kXboxPitchReferenceFreqHz           = 48000.0;
static constexpr double    kXboxPitchUnitsPerOctave            = 4096.0;
static constexpr DWORD     kMinDirectSoundFreqHz               = 100u;
static constexpr DWORD     kMaxDirectSoundFreqHz               = 100000u;

DWORD XboxPitchToFrequencyHz(LONG lPitch) {
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

void Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb) {
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

void ComputeMixBinsPan(const XBOX_DSMIXBINS* pMixBins, LONG* outPan) {
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
// Xbox IMA ADPCM Decoder (36 bytes/channel block -> 65 PCM16 samples)
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

void DecodeXboxAdpcmToPcm16(
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
