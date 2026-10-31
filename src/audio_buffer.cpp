#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <mmsystem.h>
#include <dsound.h>
#include <vector>

#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "winmm.lib")

void DecodeXboxAdpcmToPcm16(
    const uint8_t* src, uint32_t srcBytes, uint32_t channels, std::vector<int16_t>& outPcm);

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

static constexpr LONG     kMinDirectSoundVolumeMb  = -10000;
static constexpr LONG     kMaxDirectSoundVolumeMb  = 0;
static constexpr LONG     kMinDirectSoundPanMb     = -10000;
static constexpr LONG     kMaxDirectSoundPanMb     = 10000;
static constexpr DWORD    kMinDirectSoundFreqHz    = 100u;
static constexpr DWORD    kMaxDirectSoundFreqHz    = 100000u;
static constexpr DWORD    kDefaultSampleRateHz     = 44100u;
static constexpr uint32_t kDefaultStreamBytesPerSec= 176400u;

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
    void**              vtable;
    LONG                refCount;
    WAVEFORMATEX        wfx;
    bool                isAdpcm;
    uint8_t*            rawBuf;
    uint32_t            rawSize;
    bool                ownsRawBuf;
    uint32_t            playStart;
    uint32_t            playLength;
    uint32_t            loopStart;
    uint32_t            loopLength;
    LONG                volume;
    LONG                pan;
    LONG                atten3D;
    DWORD               freq;
    DWORD               status;
    IDirectSoundBuffer* hostBuf;
    uint32_t            hostBufBytes;
    uint64_t            hostCacheKey;
    bool                dirty;
    bool                isStreamingLockBuf;
    uint8_t             subLoopPhase;
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

void InitXboxAudioBuffers() {
    InstallBridge(0x006E3096, &Bridge_DirectSoundCreate,                  "_DirectSoundCreate@12");

    // IDirectSound methods
    InstallBridge(0x006E00AA, &DS_Release,                                "_IDirectSound_Release@4");
    InstallBridge(0x006E2E88, &Bridge_IDirectSound_CreateSoundBuffer,     "_IDirectSound_CreateSoundBuffer@16");
    InstallBridge(0x006E1799, &Bridge_IDirectSound_DownloadEffectsImage,  "_IDirectSound_DownloadEffectsImage@20");
    InstallBridge(0x006E17C0, &Bridge_IDirectSound_SetMixBinHeadroom,     "_IDirectSound_SetMixBinHeadroom@12");
    InstallBridge(0x006E2ED0, &Bridge_IDirectSound_SetI3DL2Listener,      "_IDirectSound_SetI3DL2Listener@12");
    InstallBridge(0x006E2C20, &Bridge_IDirectSound_CommitDeferredSettings,"_IDirectSound_CommitDeferredSettings@4");

    // IDirectSoundBuffer methods
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
}
