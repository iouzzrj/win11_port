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

struct XboxHrtfFilterPair {
    const uint8_t* pLeft;
    const uint8_t* pRight;
};

using PFN_CFullHRTFSource_GetFilterPair = void(__stdcall*)(float flAzimuth, float flElevation, int fRearAsFront, XboxHrtfFilterPair* pFilterPair);
using PFN_CHRTFSource_SetFullHRTF5Channel = void(__stdcall*)();

static inline int DecodeMcpSignMag8(uint8_t b) {
    int mag = static_cast<int>(b & 0x7Fu);
    return (b & 0x80u) ? -mag : mag;
}

static void EvaluateHrtfFirResponse(const uint8_t* fir32, float* outRmsGain, float* outHfRatio) {
    if (!fir32) {
        if (outRmsGain) *outRmsGain = 1.0f;
        if (outHfRatio) *outHfRatio = 1.0f;
        return;
    }
    int32_t dcSum   = 0;
    float   eTotal  = 0.0f;
    float   eDiff   = 0.0f;
    int     prevTap = 0;
    for (int i = 0; i < 31; ++i) {
        int tap = DecodeMcpSignMag8(fir32[i]);
        dcSum  += tap;
        eTotal += static_cast<float>(tap * tap);
        int d   = tap - prevTap;
        eDiff  += static_cast<float>(d * d);
        prevTap = tap;
    }
    // Reference bypass FIR at 0x006FDA2C / 0° front has RMS ~85..127 and DC sum ~200
    float dcGain  = fabsf(static_cast<float>(dcSum)) / 200.0f;
    float rmsGain = sqrtf(eTotal) / 110.0f;
    float gain    = 0.65f * rmsGain + 0.35f * dcGain;
    if (gain < 0.02f) gain = 0.02f;
    if (gain > 1.25f) gain = 1.25f;
    if (outRmsGain) *outRmsGain = gain;
    if (outHfRatio) {
        float hf = (eTotal > 1.0f) ? sqrtf(eDiff / (eTotal * 2.0f)) : 1.0f;
        if (hf < 0.10f) hf = 0.10f;
        if (hf > 1.00f) hf = 1.00f;
        *outHfRatio = hf;
    }
}

void Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb, Xbox3DVoiceMetrics* outMetrics) {
    if (outMetrics) {
        memset(outMetrics, 0, sizeof(*outMetrics));
    }
    if (!outPan || !outAttenDb) return;
    if (!pVoiceData || reinterpret_cast<uintptr_t>(pVoiceData) < kMinValidXboxAddress) return;

    // Ensure CHRTFSource::m_vtable (0x006FE22C..0x006FE250) is initialized to CFullHRTFSource
    if (*reinterpret_cast<const uint32_t*>(0x006FE250) == 0) {
        reinterpret_cast<PFN_CHRTFSource_SetFullHRTF5Channel>(0x006E3C2D)();
    }

    // In CSoundSource::Update3DProperties (0x007022AA), pVoiceData == (uint8_t*)pSoundSource + 0xE8 (_DS3DCALCVOICEDATA).
    // CDirectSound3DCalculator::GetVoiceData (0x006E0E00) populates:
    //   +0x00: uint32_t dwValid
    //   +0x04: int32_t  lDistanceVolume    (CFullHRTFSource::GetDistanceVolume at 0x006E3786)
    //   +0x08: int32_t  lConeVolume        (CFullHRTFSource::GetConeVolume at 0x006E3897)
    //   +0x0C: int32_t  lFrontVolume       (CFullHRTFSource::GetFrontRearVolume at 0x006E3927)
    //   +0x10: int32_t  lRearVolume        (CFullHRTFSource::GetFrontRearVolume at 0x006E3927)
    //   +0x14: int32_t  lCenterVolume      (CFullHRTFSource::GetCenterVolume at 0x006E39F6)
    //   +0x18: int32_t  lLFEVolume         (CFullHRTFSource::GetCenterVolume at 0x006E39F6)
    //   +0x1C: int32_t  lI3DL2DirectVolume (CI3DL2Source::CalculateI3DL2 at 0x006E107B)
    //   +0x20: int32_t  lI3DL2RoomVolume   (CI3DL2Source::CalculateI3DL2 at 0x006E107B)
    //   +0x24: int32_t  lDopplerPitch      (CFullHRTFSource::GetDopplerPitch at 0x006E3A98)
    //   +0x28: float    flAzimuth          (CFullHRTFSource::CalcPolarCoords at 0x006E3542)
    //   +0x2C: float    flElevation        (CFullHRTFSource::CalcPolarCoords at 0x006E3542)
    //   +0x30: int32_t  nI3DL2DirectFilter (CI3DL2Source::CalculateI3DL2 at 0x006E107B)
    //   +0x34: int32_t  nI3DL2RoomFilter   (CI3DL2Source::CalculateI3DL2 at 0x006E107B)
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
    int32_t frontVol     = *reinterpret_cast<const int32_t*>(vd + 0x0C);
    int32_t rearVol      = *reinterpret_cast<const int32_t*>(vd + 0x10);
    int32_t vdI3dl2Room  = *reinterpret_cast<const int32_t*>(vd + kVoiceDataI3DL2RoomVolOffset);
    int32_t dopplerPitch = *reinterpret_cast<const int32_t*>(vd + kVoiceDataDopplerPitchOffset);
    int16_t vdDirectFc   = static_cast<int16_t>(*reinterpret_cast<const int32_t*>(vd + 0x30) & 0xFFFF);
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

    // Extract or compute polar azimuth [-180..+180] and elevation [-90..+90] in degrees
    float azimuthDeg   = *reinterpret_cast<const float*>(vd + 0x28);
    float elevationDeg = *reinterpret_cast<const float*>(vd + 0x2C);
    bool  validPolar   = _finite(azimuthDeg) && _finite(elevationDeg) &&
                         (fabsf(azimuthDeg) > 0.001f || fabsf(elevationDeg) > 0.001f);

    float sx = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 0);
    float sy = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 4);
    float sz = *reinterpret_cast<const float*>(cs + kCSoundSourcePositionOffset + 8);
    if (!validPolar && _finite(sx) && _finite(sy) && _finite(sz)) {
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
        if (dist > 0.001f) {
            float rx = 1.0f, ry = 0.0f, rz = 0.0f;
            float ux = 0.0f, uy = 1.0f, uz = 0.0f;
            float zx = 0.0f, zy = 0.0f, zz = 1.0f;
            if (dwMode != kXboxDs3dModeHeadRelative) {
                rx = ty * fz - tz * fy;
                ry = tz * fx - tx * fz;
                rz = tx * fy - ty * fx;
                float rLen = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rLen > 0.001f) { rx /= rLen; ry /= rLen; rz /= rLen; }
                else { rx = 1.0f; ry = 0.0f; rz = 0.0f; }
                ux = tx; uy = ty; uz = tz;
                zx = fx; zy = fy; zz = fz;
            }
            float localX = (dx * rx + dy * ry + dz * rz) / dist;
            float localY = (dx * ux + dy * uy + dz * uz) / dist;
            float localZ = (dx * zx + dy * zy + dz * zz) / dist;
            if (localY < -1.0f) localY = -1.0f;
            if (localY >  1.0f) localY =  1.0f;
            elevationDeg = asinf(localY) * (180.0f / 3.14159265f);
            azimuthDeg   = atan2f(localX, localZ) * (180.0f / 3.14159265f);
            validPolar   = true;
        }
    }

    LONG computedPan = 0;
    int32_t hrtfShadowAttenMb = 0;
    uint16_t hrtfFcCoeff = static_cast<uint16_t>(vdDirectFc);

    if (validPolar) {
        if (azimuthDeg < -180.0f) azimuthDeg = -180.0f;
        if (azimuthDeg >  180.0f) azimuthDeg =  180.0f;
        if (elevationDeg < -90.0f) elevationDeg = -90.0f;
        if (elevationDeg >  90.0f) elevationDeg =  90.0f;

        // Query Ghost.exe's native CFullHRTFSource::GetFilterPair (0x006E3B11) for the 31-tap FIR pair & ITD
        XboxHrtfFilterPair filterPair = { nullptr, nullptr };
        reinterpret_cast<PFN_CFullHRTFSource_GetFilterPair>(0x006E3B11)(azimuthDeg, elevationDeg, 1, &filterPair);

        float gainL = 1.0f, gainR = 1.0f;
        float hfL   = 1.0f, hfR   = 1.0f;
        EvaluateHrtfFirResponse(filterPair.pLeft,  &gainL, &hfL);
        EvaluateHrtfFirResponse(filterPair.pRight, &gainR, &hfR);

        // Incorporate ITD psychoacoustic precedence effect (byte 31 is sample delay 0..63 at 48kHz)
        int itdSamples = 0;
        if (filterPair.pLeft && filterPair.pRight) {
            itdSamples = (azimuthDeg >= 0.0f)
                ? static_cast<int>(filterPair.pLeft[31])
                : -static_cast<int>(filterPair.pRight[31]);
        }
        float azRad = azimuthDeg * (3.14159265f / 180.0f);
        float elRad = elevationDeg * (3.14159265f / 180.0f);
        float sinAz = sinf(azRad) * cosf(elRad);

        // Combine FIR Interaural Level Difference (ILD) with spherical head-shadow & ITD
        float firIldDb = 20.0f * log10f((gainR + 0.01f) / (gainL + 0.01f));
        float itdPanMb = static_cast<float>(itdSamples) * 28.0f;
        float geomPanMb = sinAz * kMaxStereoPanMillibels;
        float combinedPan = 0.45f * (firIldDb * 100.0f + itdPanMb) + 0.55f * geomPanMb;
        if (combinedPan < -kMaxStereoPanMillibels) combinedPan = -kMaxStereoPanMillibels;
        if (combinedPan >  kMaxStereoPanMillibels) combinedPan =  kMaxStereoPanMillibels;
        computedPan = static_cast<LONG>(combinedPan);

        // Rear-hemisphere & elevation head-shadow attenuation (from CFullHRTFSource::GetFrontRearVolume + FIR energy)
        float absAz = fabsf(azimuthDeg);
        if (absAz > 90.0f) {
            float rearFactor = (absAz - 90.0f) / 90.0f; // 0 at 90 deg, 1 at 180 deg
            hrtfShadowAttenMb -= static_cast<int32_t>(rearFactor * 450.0f);
            // Rear-hemisphere pinna low-pass head shadow (reduces HF cutoff when sound is behind listener)
            int16_t rearFc = static_cast<int16_t>(-2200.0f * rearFactor * (1.1f - 0.3f * (hfL + hfR)));
            if (rearFc < static_cast<int16_t>(hrtfFcCoeff)) {
                hrtfFcCoeff = static_cast<uint16_t>(rearFc);
            }
        }
        if (frontVol < 0 && rearVol < 0 && frontVol > kMinDirectSoundVolumeMb && rearVol > kMinDirectSoundVolumeMb) {
            // Stereo downmix of 4-channel Front/Rear power law (-3 dB rear fold-down)
            float pFront = powf(10.0f, static_cast<float>(frontVol) / 1000.0f);
            float pRear  = powf(10.0f, static_cast<float>(rearVol)  / 1000.0f) * 0.5f;
            float pTotal = pFront + pRear;
            if (pTotal > 0.0001f && pTotal < 1.0f) {
                int32_t foldMb = static_cast<int32_t>(1000.0f * log10f(pTotal));
                if (foldMb < hrtfShadowAttenMb) hrtfShadowAttenMb = foldMb;
            }
        }
    }

    LONG totalAtten = static_cast<LONG>(distVol + coneVol + i3dl2Vol + hrtfShadowAttenMb);
    if (totalAtten < kMinDirectSoundVolumeMb) totalAtten = kMinDirectSoundVolumeMb;
    if (totalAtten > kMaxDirectSoundVolumeMb) totalAtten = kMaxDirectSoundVolumeMb;
    *outAttenDb = totalAtten;
    *outPan     = computedPan;

    if (outMetrics) {
        outMetrics->pan             = computedPan;
        outMetrics->totalAttenMb    = totalAtten;
        outMetrics->distanceVol     = distVol;
        outMetrics->coneVol         = coneVol;
        outMetrics->dopplerPitch    = dopplerPitch;
        outMetrics->i3dl2DirectVol  = i3dl2Vol;
        outMetrics->i3dl2RoomVol    = vdI3dl2Room;
        outMetrics->hrtfFilterCoeff = hrtfFcCoeff;
        outMetrics->active3D        = true;
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

static float DecodeMcpxFilterFreqCoeff(int16_t fcHw, uint32_t sampleRateHz) {
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
    return fScaled;
}

static float DecodeMcpxFilterQCoeff(uint16_t qHw) {
    float dVal = (qHw == 0u) ? 1.0f : (static_cast<float>(qHw) / 32768.0f);
    if (dVal < 0.075f) dVal = 0.075f;
    if (dVal > 2.0f)   dVal = 2.0f;
    return dVal;
}

bool ComputeDls2FilterCoeffs(
    const XBOX_DSFILTERDESC* pFilter, uint32_t sampleRateHz,
    float* outF, float* outD,
    uint32_t* outMode, float* outF2, float* outD2)
{
    if (!outF || !outD) return false;
    *outF = 1.0f;
    *outD = 1.0f;
    if (outMode) *outMode = 0u;
    if (outF2)   *outF2   = 1.0f;
    if (outD2)   *outD2   = 1.0f;
    if (!pFilter || reinterpret_cast<uintptr_t>(pFilter) < 0x00010000u) return false;

    // Verified against CMcpxVoiceClient::SetFilter (0x006E6FF1):
    //   dwMode == 0: DSFILTERMODE_BYPASS
    //   dwMode == 1: DSFILTERMODE_DLS2    (2-pole resonant low-pass + 2-pole parametric EQ)
    //   dwMode == 2: DSFILTERMODE_PARAMEQ (two cascaded 2-pole parametric EQ stages)
    //   dwMode == 3: DSFILTERMODE_MULTI   (cascaded 2-pole state-variable stages)
    uint32_t mode = pFilter->dwMode & 3u;
    if (mode == 0u) return false;

    int16_t  fc0 = static_cast<int16_t>(pFilter->adwCoefficients[0] & 0xFFFFu);
    uint16_t q0  = static_cast<uint16_t>(pFilter->adwCoefficients[1] & 0xFFFFu);
    int16_t  fc1 = static_cast<int16_t>(pFilter->adwCoefficients[2] & 0xFFFFu);
    uint16_t q1  = static_cast<uint16_t>(pFilter->adwCoefficients[3] & 0xFFFFu);

    bool s0Active = !(fc0 == 0 && (q0 == 0u || q0 == 0x8000u));
    bool s1Active = !(fc1 == 0 && (q1 == 0u || q1 == 0x8000u));
    if (!s0Active && !s1Active) {
        return false;
    }

    if (outMode) *outMode = mode;
    if (s0Active) {
        *outF = DecodeMcpxFilterFreqCoeff(fc0, sampleRateHz);
        *outD = DecodeMcpxFilterQCoeff(q0);
    }
    if (s1Active && outF2 && outD2) {
        *outF2 = DecodeMcpxFilterFreqCoeff(fc1, sampleRateHz);
        *outD2 = DecodeMcpxFilterQCoeff(q1);
    }
    return true;
}

void ApplyDls2FilterToPcm16(
    int16_t* pcm, size_t numFrames, uint32_t channels,
    float fCoeff, float dCoeff, float ioLow[2], float ioBand[2],
    uint32_t filterMode, float fCoeff2, float dCoeff2, XboxVoiceFilterState* ioState2)
{
    if (!pcm || numFrames == 0 || channels == 0 || channels > 2) return;
    float low[2]  = { ioLow  ? ioLow[0]  : 0.0f, (ioLow  && channels > 1) ? ioLow[1]  : 0.0f };
    float band[2] = { ioBand ? ioBand[0] : 0.0f, (ioBand && channels > 1) ? ioBand[1] : 0.0f };
    float low2[2]  = { ioState2 ? ioState2->low2[0]  : 0.0f, (ioState2 && channels > 1) ? ioState2->low2[1]  : 0.0f };
    float band2[2] = { ioState2 ? ioState2->band2[0] : 0.0f, (ioState2 && channels > 1) ? ioState2->band2[1] : 0.0f };

    bool hasStage1 = !(fCoeff >= 0.999f && fabsf(dCoeff - 1.0f) < 0.001f);
    bool hasStage2 = !(fCoeff2 >= 0.999f && fabsf(dCoeff2 - 1.0f) < 0.001f);

    for (size_t f = 0; f < numFrames; ++f) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float x = static_cast<float>(pcm[f * channels + ch]);
            float y = x;

            if (hasStage1) {
                low[ch]   += fCoeff * band[ch];
                float high = y - low[ch] - dCoeff * band[ch];
                band[ch]  += fCoeff * high;
                if (filterMode == 2u) {
                    // DSFILTERMODE_PARAMEQ: parametric peaking/notch response around center frequency
                    float peakGain = (1.0f / dCoeff) - 1.0f;
                    y = y + peakGain * dCoeff * band[ch];
                } else {
                    // DSFILTERMODE_DLS2 (1) / DSFILTERMODE_MULTI (3): 2-pole resonant low-pass
                    y = low[ch];
                }
            }

            if (hasStage2) {
                low2[ch]   += fCoeff2 * band2[ch];
                float high2 = y - low2[ch] - dCoeff2 * band2[ch];
                band2[ch]  += fCoeff2 * high2;
                if (filterMode == 1u || filterMode == 2u) {
                    // Stage 2 in DLS2 and PARAMEQ is a parametric EQ stage
                    float peakGain2 = (1.0f / dCoeff2) - 1.0f;
                    y = y + peakGain2 * dCoeff2 * band2[ch];
                } else {
                    // Stage 2 in DSFILTERMODE_MULTI is a cascaded 2-pole low-pass (4-pole total)
                    y = low2[ch];
                }
            }

            int32_t outSample = static_cast<int32_t>(y);
            if (outSample >  32767) outSample =  32767;
            if (outSample < -32768) outSample = -32768;
            pcm[f * channels + ch] = static_cast<int16_t>(outSample);
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
    if (ioState2) {
        ioState2->low[0]   = low[0];
        ioState2->band[0]  = band[0];
        ioState2->low2[0]  = low2[0];
        ioState2->band2[0] = band2[0];
        if (channels > 1) {
            ioState2->low[1]   = low[1];
            ioState2->band[1]  = band[1];
            ioState2->low2[1]  = low2[1];
            ioState2->band2[1] = band2[1];
        }
    }
}

void ApplyStatefulI3DL2ReverbToPcm16(
    XboxI3DL2ReverbState* state,
    int16_t* pcm, size_t numFrames, uint32_t channels, uint32_t sampleRateHz,
    int32_t roomMb, int32_t roomHfMb)
{
    if (!state || !pcm || numFrames == 0 || channels == 0 || channels > 2) return;
    if (sampleRateHz < 8000u) sampleRateHz = 22050u;

    const XBOX_DSI3DL2LISTENER& rev = GetXboxI3DL2ListenerState();
    int32_t totalRoomMb = rev.lRoom + roomMb;
    if (totalRoomMb <= -9000 || (rev.lReverb <= -9000 && rev.lReflections <= -9000)) {
        return;
    }

    float roomLin = powf(10.0f, static_cast<float>(totalRoomMb) / 2000.0f);
    float reflLin = roomLin * powf(10.0f, static_cast<float>(rev.lReflections) / 2000.0f);
    float revLin  = roomLin * powf(10.0f, static_cast<float>(rev.lReverb) / 2000.0f);
    if (reflLin > 0.55f) reflLin = 0.55f;
    if (revLin  > 0.45f) revLin  = 0.45f;
    if (reflLin < 0.002f && revLin < 0.002f) return;

    float decayTime = rev.flDecayTime;
    if (decayTime < 0.1f)  decayTime = 0.1f;
    if (decayTime > 10.0f) decayTime = 10.0f;

    int32_t totalHfMb = rev.lRoomHF + roomHfMb;
    float hfRatio = rev.flDecayHFRatio;
    if (hfRatio < 0.1f) hfRatio = 0.1f;
    if (hfRatio > 2.0f) hfRatio = 2.0f;
    float damp = 0.25f + 0.45f * (1.0f - (hfRatio * 0.5f));
    if (totalHfMb < -600) damp += 0.15f;
    if (damp < 0.10f) damp = 0.10f;
    if (damp > 0.80f) damp = 0.80f;

    static const float kCombMs[4] = { 29.7f, 37.1f, 41.1f, 43.7f };
    size_t combDelay[4];
    float  combGain[4];
    for (int c = 0; c < 4; ++c) {
        size_t d = static_cast<size_t>((kCombMs[c] * 0.001f) * static_cast<float>(sampleRateHz));
        if (d < 16u) d = 16u;
        if (d >= XboxI3DL2ReverbState::kMaxCombSamples) d = XboxI3DL2ReverbState::kMaxCombSamples - 1u;
        combDelay[c] = d;
        float g = powf(10.0f, (-3.0f * (kCombMs[c] * 0.001f)) / decayTime);
        if (g > 0.88f) g = 0.88f;
        combGain[c] = g;
    }

    float reflDelaySec = rev.flReflectionsDelay;
    if (reflDelaySec < 0.002f) reflDelaySec = 0.007f;
    if (reflDelaySec > 0.080f) reflDelaySec = 0.080f;
    size_t earlyDelay = static_cast<size_t>(reflDelaySec * static_cast<float>(sampleRateHz));
    if (earlyDelay < 8u) earlyDelay = 8u;
    if (earlyDelay >= XboxI3DL2ReverbState::kMaxEarlySamples) earlyDelay = XboxI3DL2ReverbState::kMaxEarlySamples - 1u;

    for (size_t f = 0; f < numFrames; ++f) {
        float monoIn = 0.0f;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            monoIn += static_cast<float>(pcm[f * channels + ch]);
        }
        monoIn /= static_cast<float>(channels);

        float earlyOut = state->earlyBuf[state->earlyIdx];
        state->earlyBuf[state->earlyIdx] = monoIn;
        if (++state->earlyIdx >= earlyDelay) state->earlyIdx = 0;

        float combSumL = 0.0f;
        float combSumR = 0.0f;
        for (int c = 0; c < 4; ++c) {
            size_t idx = state->combIdx[c];
            float delayed = state->combBuf[c][idx];
            state->combLp[c] = delayed * (1.0f - damp) + state->combLp[c] * damp;
            state->combBuf[c][idx] = monoIn + state->combLp[c] * combGain[c];
            if (++idx >= combDelay[c]) idx = 0;
            state->combIdx[c] = idx;
            if ((c & 1) == 0) combSumL += delayed;
            else              combSumR += delayed;
        }
        float wetL = earlyOut * reflLin + (combSumL * 0.5f) * revLin;
        float wetR = earlyOut * reflLin + (combSumR * 0.5f) * revLin;

        if (channels == 1u) {
            int32_t y = static_cast<int32_t>(pcm[f] + (wetL + wetR) * 0.5f);
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            pcm[f] = static_cast<int16_t>(y);
        } else {
            int32_t yL = static_cast<int32_t>(pcm[f * 2u + 0u] + wetL);
            int32_t yR = static_cast<int32_t>(pcm[f * 2u + 1u] + wetR);
            if (yL >  32767) yL =  32767;
            if (yL < -32768) yL = -32768;
            if (yR >  32767) yR =  32767;
            if (yR < -32768) yR = -32768;
            pcm[f * 2u + 0u] = static_cast<int16_t>(yL);
            pcm[f * 2u + 1u] = static_cast<int16_t>(yR);
        }
    }
}

void EvaluateXboxVoiceLfoAndEg(
    const XBOX_DSLFODESC lfo[2], const bool lfoActive[2],
    const XBOX_DSENVELOPEDESC eg[2], const bool egActive[2],
    uint32_t elapsedMs, bool isReleasing, uint32_t releaseElapsedMs,
    int32_t* outVolModMb, float* outPitchRatio, float* outFilterCutoffRatio)
{
    if (outVolModMb)          *outVolModMb          = 0;
    if (outPitchRatio)        *outPitchRatio        = 1.0f;
    if (outFilterCutoffRatio) *outFilterCutoffRatio = 1.0f;

    // Xbox MCPX APU hardware envelope/LFO step clock is 48000 Hz / 512 = 93.75 Hz (~10.667 ms per step).
    // Verified against CMcpxVoiceClient::SetLFO (0x006E6BF0) and CMcpxVoiceClient::SetEG (0x006E6CA8).
    float stepCount = static_cast<float>(elapsedMs) * (93.75f / 1000.0f);
    float pitchOctaves = 0.0f;
    float cutoffOctaves = 0.0f;
    int32_t volModMb = 0;

    if (lfo && lfoActive) {
        for (int i = 0; i < 2; ++i) {
            if (!lfoActive[i]) continue;
            const XBOX_DSLFODESC& d = lfo[i];
            float delaySteps = static_cast<float>(d.dwDelay);
            if (stepCount < delaySteps) continue;
            float activeSteps = stepCount - delaySteps;
            // dwDelta is 24-bit phase increment per 93.75 Hz step (full cycle = 32768 units in hardware)
            float phase = fmodf(activeSteps * (static_cast<float>(d.dwDelta) / 32768.0f), 1.0f);
            // Triangle LFO in [-1.0, +1.0]
            float tri = (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);

            if (d.lAmplitudeModulation != 0) {
                // lAmplitudeModulation in [-128..127] -> up to +/- 12 dB (1200 mB)
                float ampNorm = static_cast<float>(d.lAmplitudeModulation) / 128.0f;
                volModMb += static_cast<int32_t>((tri - 1.0f) * 0.5f * fabsf(ampNorm) * 1200.0f);
            }
            if (d.lPitchModulation != 0) {
                // lPitchModulation in [-128..127] -> +/- 1 octave at +/- 128
                pitchOctaves += tri * (static_cast<float>(d.lPitchModulation) / 128.0f);
            }
            if (d.lFilterCutOffRange != 0) {
                // lFilterCutOffRange in [-128..127] -> +/- 2 octaves at +/- 128
                cutoffOctaves += tri * (static_cast<float>(d.lFilterCutOffRange) / 64.0f);
            }
        }
    }

    if (eg && egActive) {
        for (int i = 0; i < 2; ++i) {
            if (!egActive[i]) continue;
            const XBOX_DSENVELOPEDESC& e = eg[i];
            if (e.dwMode == 0u) continue; // DSEGMODE_DISABLE

            float envLevel = 1.0f;
            float sustainNorm = static_cast<float>(e.dwSustain > 255u ? 255u : e.dwSustain) / 255.0f;

            if (isReleasing && e.dwRelease > 0u) {
                float relSteps = static_cast<float>(releaseElapsedMs) * (93.75f / 1000.0f);
                float relFrac = 1.0f - (relSteps / static_cast<float>(e.dwRelease));
                if (relFrac < 0.0f) relFrac = 0.0f;
                envLevel = sustainNorm * relFrac;
            } else {
                float delaySteps  = static_cast<float>(e.dwDelay);
                float attackSteps = static_cast<float>(e.dwAttack);
                float holdSteps   = static_cast<float>(e.dwHold);
                float decaySteps  = static_cast<float>(e.dwDecay);

                if (stepCount < delaySteps) {
                    envLevel = 0.0f;
                } else {
                    float t = stepCount - delaySteps;
                    if (attackSteps > 0.0f && t < attackSteps) {
                        envLevel = t / attackSteps;
                    } else {
                        t -= attackSteps;
                        if (t < holdSteps) {
                            envLevel = 1.0f;
                        } else {
                            t -= holdSteps;
                            if (decaySteps > 0.0f && t < decaySteps) {
                                float decFrac = t / decaySteps;
                                envLevel = 1.0f - decFrac * (1.0f - sustainNorm);
                            } else {
                                envLevel = sustainNorm;
                            }
                        }
                    }
                }
            }

            if (i == 0 || e.dwMode == 1u) {
                // EG0 (Amplitude Envelope): modulates voice attenuation when dwMode != 0 and amplitude envelope is active
                if (e.dwAttack > 0u || e.dwDecay > 0u || e.dwSustain < 255u || isReleasing) {
                    if (envLevel <= 0.0001f) {
                        volModMb -= 10000;
                    } else {
                        int32_t envMb = static_cast<int32_t>(2000.0f * log10f(envLevel));
                        volModMb += envMb;
                    }
                }
            }
            if (e.lPitchScale != 0) {
                pitchOctaves += (envLevel - 1.0f) * (static_cast<float>(e.lPitchScale) / 128.0f);
            }
            if (e.lFilterCutOff != 0) {
                cutoffOctaves += (envLevel - 1.0f) * (static_cast<float>(e.lFilterCutOff) / 64.0f);
            }
        }
    }

    if (outVolModMb) {
        if (volModMb < -10000) volModMb = -10000;
        if (volModMb >  2400)  volModMb =  2400;
        *outVolModMb = volModMb;
    }
    if (outPitchRatio) {
        if (pitchOctaves < -4.0f) pitchOctaves = -4.0f;
        if (pitchOctaves >  4.0f) pitchOctaves =  4.0f;
        *outPitchRatio = powf(2.0f, pitchOctaves);
    }
    if (outFilterCutoffRatio) {
        if (cutoffOctaves < -4.0f) cutoffOctaves = -4.0f;
        if (cutoffOctaves >  4.0f) cutoffOctaves =  4.0f;
        *outFilterCutoffRatio = powf(2.0f, cutoffOctaves);
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
