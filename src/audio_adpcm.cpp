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
static constexpr uint32_t  kVoiceDataI3DL2RoomVolOffset        = 0x20u;
static constexpr uint32_t  kVoiceDataDopplerPitchOffset        = 0x24u;

static constexpr uint32_t  kXboxDs3dModeHeadRelative           = 1u;
static constexpr uint32_t  kXboxDs3dModeDisable                = 2u;

static constexpr float     kMaxStereoPanMillibels              = 3500.0f;

static constexpr double    kXboxPitchReferenceFreqHz           = 48000.0;
static constexpr double    kXboxPitchUnitsPerOctave            = 4096.0;

DWORD XboxPitchToFrequencyHz(LONG lPitch) {
    // Xbox DirectSound & XACT represent pitch in 1/4096ths of an octave relative to 48000 Hz
    // (see _XAudioCalculatePitch@4 at 0x006E00FA, CMcpxVoiceClient::ConvertPitchValue at 0x006E66CC,
    // and XACT::CSoundCue::UpdateTrackPitch at 0x007063A5).
    // Xbox MCPX hardware supports pitch up to +2 octaves (+8192 = 192,000 Hz).
    double freq = kXboxPitchReferenceFreqHz * pow(2.0, static_cast<double>(lPitch) / kXboxPitchUnitsPerOctave);
    if (freq < static_cast<double>(kMinDirectSoundFreqHz)) return kMinDirectSoundFreqHz;
    if (freq > static_cast<double>(kMaxXboxPitchFreqHz))   return kMaxXboxPitchFreqHz;
    return static_cast<DWORD>(freq + 0.5);
}

LONG FrequencyHzToXboxPitch(DWORD freqHz) {
    if (freqHz < kMinDirectSoundFreqHz) freqHz = kMinDirectSoundFreqHz;
    if (freqHz > kMaxXboxPitchFreqHz)   freqHz = kMaxXboxPitchFreqHz;
    double octaves = log(static_cast<double>(freqHz) / kXboxPitchReferenceFreqHz) / log(2.0);
    double pitch   = floor(octaves * kXboxPitchUnitsPerOctave + 0.5);
    if (pitch < -32768.0) pitch = -32768.0;
    if (pitch >   8192.0) pitch =   8192.0;
    return static_cast<LONG>(pitch);
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

void Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb, Xbox3DVoiceMetrics* outMetrics) {
    if (outMetrics) {
        memset(outMetrics, 0, sizeof(*outMetrics));
    }
    if (!outPan || !outAttenDb) return;
    if (!pVoiceData || reinterpret_cast<uintptr_t>(pVoiceData) < kMinValidXboxAddress) return;

    // In CSoundSource::Update3DProperties (0x007022AA), pVoiceData == (uint8_t*)pSoundSource + 0xE8 (_DS3DCALCVOICEDATA).
    // Before calling Set3DVoiceData, Update3DProperties invokes CDirectSound3DCalculator::Calculate3D (0x006E243F)
    // and CDirectSound3DCalculator::GetVoiceData (0x006E0E00), which populate:
    //   pVoiceData + 0x04: int32_t lDistanceVolume    (computed by CFullHRTFSource::GetDistanceVolume at 0x006E3786)
    //   pVoiceData + 0x08: int32_t lConeVolume        (computed by CFullHRTFSource::GetConeVolume at 0x006E3897)
    //   pVoiceData + 0x1C: int32_t lI3DL2DirectVolume (computed by I3DL2 calculator at 0x006E8FD1)
    //   pVoiceData + 0x20: int32_t lI3DL2RoomVolume   (computed by I3DL2 calculator at 0x006E8FF8)
    //   pVoiceData + 0x24: int32_t lDopplerPitch      (computed by CFullHRTFSource::GetDopplerPitch at 0x006E3940)
    const uint8_t* vd = reinterpret_cast<const uint8_t*>(pVoiceData);
    const uint8_t* cs = vd - kCSoundSourceVoiceDataOffset;
    uint32_t dwMode = *reinterpret_cast<const uint32_t*>(cs + kCSoundSourceModeOffset);
    if (dwMode == kXboxDs3dModeDisable) {
        *outPan = 0;
        *outAttenDb = 0;
        return;
    }

    int32_t distVol      = *reinterpret_cast<const int32_t*>(vd + kVoiceDataDistanceVolOffset);
    int32_t coneVol      = *reinterpret_cast<const int32_t*>(vd + kVoiceDataConeVolOffset);
    int32_t vdI3dl2Room  = *reinterpret_cast<const int32_t*>(vd + kVoiceDataI3DL2RoomVolOffset);
    int32_t dopplerPitch = *reinterpret_cast<const int32_t*>(vd + kVoiceDataDopplerPitchOffset);
    if (distVol < kMinDirectSoundVolumeMb) distVol = kMinDirectSoundVolumeMb;
    if (distVol > kMaxDirectSoundVolumeMb) distVol = kMaxDirectSoundVolumeMb;
    if (coneVol < kMinDirectSoundVolumeMb) coneVol = kMinDirectSoundVolumeMb;
    if (coneVol > kMaxDirectSoundVolumeMb) coneVol = kMaxDirectSoundVolumeMb;
    if (vdI3dl2Room < kMinDirectSoundVolumeMb) vdI3dl2Room = kMinDirectSoundVolumeMb;
    if (vdI3dl2Room > kMaxDirectSoundVolumeMb) vdI3dl2Room = kMaxDirectSoundVolumeMb;
    if (dopplerPitch < -16384 || dopplerPitch > 8192) dopplerPitch = 0;

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

    LONG computedPan = 0;
    float sx = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 0);
    float sy = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 4);
    float sz = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 8);
    if (_finite(sx) && _finite(sy) && _finite(sz)) {
        // Global XACT 3D Listener at ADDR_XACT_LISTENER (0x007091E8)
        float lx = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x08);
        float ly = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x0C);
        float lz = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x10);
        float fx = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x20);
        float fy = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x24);
        float fz = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x28);
        float tx = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x2C);
        float ty = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x30);
        float tz = *reinterpret_cast<const float*>(ADDR_XACT_LISTENER + 0x34);

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
        computedPan = static_cast<LONG>(sinAz * kMaxStereoPanMillibels);
    }
    *outPan = computedPan;

    if (outMetrics) {
        outMetrics->pan            = computedPan;
        outMetrics->totalAttenMb   = totalAtten;
        outMetrics->distanceVol    = distVol;
        outMetrics->coneVol        = coneVol;
        outMetrics->dopplerPitch   = dopplerPitch;
        outMetrics->i3dl2DirectVol = i3dl2Vol;
        outMetrics->i3dl2RoomVol   = vdI3dl2Room;
        outMetrics->active3D       = true;
    }
}

void ComputeMixBinPairsPan(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, LONG* outPan) {
    if (!outPan || !pairs || count == 0) return;
    if (count > 8u) count = 8u;

    float leftGain  = 0.0f;
    float rightGain = 0.0f;
    bool  hasLR     = false;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bin = pairs[i].dwMixBin;
        int32_t  vol = pairs[i].lVolume;
        if (vol < -10000) vol = -10000;
        if (vol > 0)      vol = 0;
        float g = powf(10.0f, static_cast<float>(vol) / 2000.0f);
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
        LONG pan = static_cast<LONG>(diff * 3500.0f);
        if (pan < -10000) pan = -10000;
        if (pan >  10000) pan =  10000;
        *outPan = pan;
    }
}

void ComputeMixBinsPan(const XBOX_DSMIXBINS* pMixBins, LONG* outPan) {
    if (!outPan || !pMixBins || pMixBins->dwMixBinCount == 0 || pMixBins->dwMixBinCount > 32 || !pMixBins->lpMixBinVolumePairs) return;
    if ((uintptr_t)pMixBins->lpMixBinVolumePairs < 0x00010000) return;

    XBOX_DSMIXBINVOLUMEPAIR tempPairs[8] = {};
    uint32_t count = (pMixBins->dwMixBinCount > 8u) ? 8u : pMixBins->dwMixBinCount;
    for (uint32_t i = 0; i < count; ++i) {
        tempPairs[i] = pMixBins->lpMixBinVolumePairs[i];
    }
    ComputeMixBinPairsPan(tempPairs, count, outPan);
}

void InitVoiceDefaultMixBins(uint32_t dwFlags, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount) {
    if (!outPairs || !outCount) return;
    memset(outPairs, 0, sizeof(XBOX_DSMIXBINVOLUMEPAIR) * 8);
    // Matches CDirectSoundVoiceSettings::SetMixBins (0x006E1A9C):
    //   If (dwFlags & 0x00200010) != 0 (3D voice), initializes with _DirectSoundDefaultMixBins_3D (0x006FD8BC):
    //     5 bins: {6,0}, {8,0}, {7,0}, {9,0}, {10,0} (3D Front, Back, Center, LFE, I3DL2 Room).
    //   Otherwise initializes with _DirectSoundDefaultMixBins_Stereo (0x006FD8DC):
    //     2 bins: {0,0}, {1,0} (Front Left, Front Right).
    if ((dwFlags & 0x00200010u) != 0) {
        static const uint32_t kDefault3DBins[5] = { 6u, 8u, 7u, 9u, 10u };
        for (uint32_t i = 0; i < 5u; ++i) {
            outPairs[i].dwMixBin = kDefault3DBins[i];
            outPairs[i].lVolume  = 0;
        }
        *outCount = 5u;
    } else {
        outPairs[0].dwMixBin = 0u;
        outPairs[0].lVolume  = 0;
        outPairs[1].dwMixBin = 1u;
        outPairs[1].lVolume  = 0;
        *outCount = 2u;
    }
}

void AssignVoiceMixBins(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount) {
    if (!outPairs || !outCount || !pMixBins || !pMixBins->lpMixBinVolumePairs) return;
    if (reinterpret_cast<uintptr_t>(pMixBins->lpMixBinVolumePairs) < 0x00010000u) return;
    uint32_t count = pMixBins->dwMixBinCount;
    if (count > 8u) count = 8u;
    for (uint32_t i = 0; i < count; ++i) {
        outPairs[i].dwMixBin = pMixBins->lpMixBinVolumePairs[i].dwMixBin;
        int32_t vol = pMixBins->lpMixBinVolumePairs[i].lVolume;
        if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
        if (vol > kMaxDirectSoundVolumeMb) vol = kMaxDirectSoundVolumeMb;
        outPairs[i].lVolume = vol;
    }
    for (uint32_t i = count; i < 8u; ++i) {
        outPairs[i].dwMixBin = 0xFFFFFFFFu;
        outPairs[i].lVolume  = kMinDirectSoundVolumeMb;
    }
    *outCount = count;
}

void UpdateVoiceMixBinVolumes(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR ioPairs[8], uint32_t* ioCount) {
    if (!ioPairs || !ioCount || !pMixBins || !pMixBins->lpMixBinVolumePairs) return;
    if (reinterpret_cast<uintptr_t>(pMixBins->lpMixBinVolumePairs) < 0x00010000u) return;
    uint32_t inCount = pMixBins->dwMixBinCount;
    if (inCount > 32u) inCount = 32u;
    uint32_t curCount = (*ioCount > 8u) ? 8u : *ioCount;

    for (uint32_t i = 0; i < inCount; ++i) {
        uint32_t bin = pMixBins->lpMixBinVolumePairs[i].dwMixBin;
        int32_t  vol = pMixBins->lpMixBinVolumePairs[i].lVolume;
        if (vol < kMinDirectSoundVolumeMb) vol = kMinDirectSoundVolumeMb;
        if (vol > kMaxDirectSoundVolumeMb) vol = kMaxDirectSoundVolumeMb;

        bool found = false;
        for (uint32_t j = 0; j < curCount; ++j) {
            if (ioPairs[j].dwMixBin == bin) {
                ioPairs[j].lVolume = vol;
                found = true;
                break;
            }
        }
        if (!found && curCount < 8u) {
            ioPairs[curCount].dwMixBin = bin;
            ioPairs[curCount].lVolume  = vol;
            ++curCount;
        }
    }
    *ioCount = curCount;
}

void FillVoicePropsMixBins(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, XBOX_DSVOICEPROPS* outProps) {
    if (!outProps) return;
    if (count > 8u) count = 8u;
    outProps->dwMixBinCount = count;
    for (uint32_t i = 0; i < count; ++i) {
        outProps->MixBinVolumePairs[i] = pairs[i];
    }
    // Matches CMcpxVoiceClient::GetVoiceProperties (0x006E72C4): unused MixBin slots filled with {0xFFFFFFFF, -10000}
    for (uint32_t i = count; i < 8u; ++i) {
        outProps->MixBinVolumePairs[i].dwMixBin = 0xFFFFFFFFu;
        outProps->MixBinVolumePairs[i].lVolume  = kMinDirectSoundVolumeMb;
    }
}

bool ComputeDls2FilterCoeffs(const XBOX_DSFILTERDESC* pFilter, uint32_t sampleRateHz, float* outF, float* outD) {
    if (!outF || !outD) return false;
    *outF = 1.0f;
    *outD = 1.0f;
    if (!pFilter || reinterpret_cast<uintptr_t>(pFilter) < 0x00010000u) return false;

    // Verified against XACT::CSoundCue::UpdateTrackFilter (0x007063E9), XACT::FreqToHardwareCoeff (0x00708D62),
    // XACT::dBToHardwareCoeff (0x00708DC0), and CMcpxVoiceClient::SetFilter (0x006E6FF1):
    //   dwMode == 0: DSFILTERMODE_BYPASS
    //   dwMode == 1: DSFILTERMODE_DLS2 (resonant 2-pole Chamberlin state-variable low-pass filter)
    if (pFilter->dwMode != 1u) return false;

    int16_t  fcHw = static_cast<int16_t>(pFilter->adwCoefficients[0] & 0xFFFFu);
    uint16_t qHw  = static_cast<uint16_t>(pFilter->adwCoefficients[1] & 0xFFFFu);

    // In FreqToHardwareCoeff (0x00708D62), cutoff >= 8000 Hz yields fcHw == 0 (full pass).
    if (fcHw == 0 && (qHw == 0u || qHw == 0x8000u)) {
        return false;
    }

    float f48k = 1.0f;
    if (fcHw == -32768) {
        // Cutoff <= 30 Hz floor (0x00708D77: mov eax, 0x8000)
        f48k = 2.0f * sinf(3.14159265f * 30.0f / 48000.0f);
    } else if (fcHw < 0) {
        f48k = powf(2.0f, static_cast<float>(fcHw) / 4096.0f);
    } else {
        f48k = 2.0f * sinf(3.14159265f * 8000.0f / 48000.0f);
    }

    if (sampleRateHz < 8000u) sampleRateHz = 22050u;
    float fScaled = f48k * (48000.0f / static_cast<float>(sampleRateHz));
    if (fScaled < 0.004f) fScaled = 0.004f;
    if (fScaled > 1.40f)  fScaled = 1.40f;

    float dVal = (qHw == 0u) ? 1.0f : (static_cast<float>(qHw) / 32768.0f);
    if (dVal < 0.075f) dVal = 0.075f;
    if (dVal > 2.0f)   dVal = 2.0f;

    *outF = fScaled;
    *outD = dVal;
    return true;
}

void ApplyDls2FilterToPcm16(
    int16_t* pcm, size_t numFrames, uint32_t channels,
    float fCoeff, float dCoeff, float ioLow[2], float ioBand[2])
{
    if (!pcm || numFrames == 0 || channels == 0 || channels > 2) return;
    float low[2]  = { ioLow  ? ioLow[0]  : 0.0f, (ioLow  && channels > 1) ? ioLow[1]  : 0.0f };
    float band[2] = { ioBand ? ioBand[0] : 0.0f, (ioBand && channels > 1) ? ioBand[1] : 0.0f };

    for (size_t f = 0; f < numFrames; ++f) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float x    = static_cast<float>(pcm[f * channels + ch]);
            low[ch]   += fCoeff * band[ch];
            float high = x - low[ch] - dCoeff * band[ch];
            band[ch]  += fCoeff * high;
            int32_t y  = static_cast<int32_t>(low[ch]);
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            pcm[f * channels + ch] = static_cast<int16_t>(y);
        }
    }

    if (ioLow) {
        ioLow[0] = low[0];
        if (channels > 1) ioLow[1] = low[1];
    }
    if (ioBand) {
        ioBand[0] = band[0];
        if (channels > 1) ioBand[1] = band[1];
    }
}

void DecimatePcm16By2(
    const int16_t* srcPcm, size_t srcFrames, uint32_t channels, std::vector<int16_t>& outPcm)
{
    if (!srcPcm || srcFrames == 0 || channels == 0 || channels > 2) {
        outPcm.clear();
        return;
    }
    size_t dstFrames = (srcFrames >= 2u) ? (srcFrames / 2u) : 1u;
    outPcm.resize(dstFrames * channels);
    if (srcFrames < 2u) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            outPcm[ch] = srcPcm[ch];
        }
        return;
    }
    // 2-tap anti-aliased boxcar low-pass filter + 2:1 decimation:
    // Halves the sample count so Windows DirectSound8 can play >100,000 Hz Xbox voices (up to 192,000 Hz)
    // at (effectiveFreq + 1) / 2 <= 96,000 Hz with the exact target pitch and zero fallback.
    for (size_t f = 0; f < dstFrames; ++f) {
        size_t s0 = (f * 2u) * channels;
        size_t s1 = (f * 2u + 1u) * channels;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            int32_t avg = (static_cast<int32_t>(srcPcm[s0 + ch]) + static_cast<int32_t>(srcPcm[s1 + ch])) >> 1;
            outPcm[f * channels + ch] = static_cast<int16_t>(avg);
        }
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
