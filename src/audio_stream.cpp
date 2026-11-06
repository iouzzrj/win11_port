#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <mmsystem.h>
#include <math.h>
#include <vector>

void DecodeXboxAdpcmToPcm16(
    const uint8_t* src, uint32_t srcBytes, uint32_t channels, std::vector<int16_t>& outPcm);

// ============================================================================
// Xbox DirectSoundStream Structures & Constants
// ============================================================================

#pragma pack(push, 1)
struct XBOX_DSSTREAMDESC {
    uint32_t            dwFlags;
    uint32_t            dwMaxAttachedPackets;
    const WAVEFORMATEX* lpwfxFormat;
    void (__stdcall *lpfnCallback)(void* pStreamContext, void* pPacketContext, uint32_t dwStatus);
    void*               lpvContext;
    const void*         lpMixBins;
};

struct XBOX_XMEDIAPACKET {
    void*     pvBuffer;
    uint32_t  dwMaxSize;
    uint32_t* pdwCompletedSize;
    uint32_t* pdwStatus;
    union {
        HANDLE hCompletionEvent;
        void*  pContext;
    };
    uint64_t* prtTimestamp;
};

struct XBOX_XMEDIAINFO {
    uint32_t dwFlags;
    uint32_t dwInputSize;
    uint32_t dwOutputSize;
    uint32_t dwMaxLookahead;
};
#pragma pack(pop)

static constexpr DWORD     kXboxStreamStatusReady            = 0x00000001u; // XMO_STATUSF_ACCEPT_INPUT_DATA | DSSTREAMSTATUS_READY
static constexpr DWORD     kXboxStreamStatusPlaying          = 0x00010000u; // DSSTREAMSTATUS_PLAYING
static constexpr DWORD     kXboxStreamStatusPaused           = 0x00020000u; // DSSTREAMSTATUS_PAUSED
static constexpr DWORD     kXboxStreamStatusStarved          = 0x00040000u; // DSSTREAMSTATUS_STARVED
static constexpr DWORD     kXboxStreamStatusEnvelopeComplete = 0x00080000u; // Checked by XACT::CSoundCue::DoWork at 0x00708BD0

static constexpr DWORD     kXboxStreamPauseResume        = 0u;          // DSSTREAMPAUSE_RESUME
static constexpr DWORD     kXboxStreamPausePause         = 1u;          // DSSTREAMPAUSE_PAUSE
static constexpr DWORD     kXboxStreamPausePauseEx       = 2u;          // DSSTREAMPAUSE_PAUSEEX
static constexpr uint32_t  kXboxMediaPacketStatusSuccess = 0u;          // XMEDIAPACKET_STATUS_SUCCESS
static constexpr uint32_t  kXboxMediaPacketStatusPending = 0x8000000Au; // XMEDIAPACKET_STATUS_PENDING
static constexpr uint32_t  kXboxMediaPacketStatusFlushed = 0x40000005u; // XMEDIAPACKET_STATUS_FLUSHED

static constexpr uint32_t  kMinStreamPacketDurationMs    = 5u;
static constexpr int32_t   kWaveOutFallbackTimeoutMs     = 2000;
static constexpr uint16_t  kWaveOutStereoChannels        = 2u;
static constexpr uint16_t  kPcm16BitsPerSample           = 16u;
static constexpr int32_t   kPcm16MinSampleValue          = -32768;
static constexpr int32_t   kPcm16MaxSampleValue          = 32767;
static constexpr float     kMillibelsPerTwentyDecibels   = 2000.0f;

// ============================================================================
// IDirectSoundStream Objects, WaveOut Pool & Virtual/C-API Methods
// ============================================================================

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

struct XboxDSBufferObject;
void ApplyXboxOutputBufferRouting(
    XboxDSBufferObject* outBuf, LONG* ioVolMb, LONG* ioPanMb, bool* ioHasI3DL2, int32_t* ioI3DL2SendMb);

static constexpr uint32_t kXboxMixBinI3DL2 = 10u;

static void ExtractStreamMixBinI3DL2Send(
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

struct XboxDSStreamObject {
    void**                vtable; // Points to 7-entry XMO vtable at +0x00!
    LONG                  refCount;
    WAVEFORMATEX          wfx;
    bool                  isAdpcm;
    uint32_t              maxAttachedPackets;
    void (__stdcall *lpfnCallback)(void* pStreamContext, void* pPacketContext, uint32_t dwStatus);
    void*                 lpvContext;
    LONG                  volume;
    LONG                  headroomMb;
    LONG                  pan;
    LONG                  atten3D;
    LONG                  lPitch;
    DWORD                 freq;
    bool                  paused;
    bool                  discontinuity;
    bool                  flushExPending;
    HWAVEOUT              hWaveOut;
    uint32_t              waveOutRate;
    uint16_t              waveOutChannels;
    uint32_t              nextCompleteTick;
    double                resamplePos;
    int16_t               prevSampleL;
    int16_t               prevSampleR;
    bool                  hasPrevSample;
    bool                  hasI3DL2Send;
    int32_t               i3dl2SendMb;
    XBOX_DSMIXBINVOLUMEPAIR mixBinPairs[8];
    uint32_t              mixBinCount;
    Xbox3DVoiceMetrics    voice3D;
    XBOX_DSFILTERDESC     filterDesc;
    bool                  hasActiveFilter;
    float                 svfLow[2];
    float                 svfBand[2];
    XboxVoiceFilterState  filterState;
    XboxI3DL2ReverbState  reverbState;
    XBOX_DSLFODESC        lfo[2];
    bool                  lfoActive[2];
    XBOX_DSENVELOPEDESC   eg[2];
    bool                  egActive[2];
    XboxDSBufferObject*   outputBuffer;
    DWORD                 playStartTickMs;
    bool                  isReleasing;
    DWORD                 releaseStartTickMs;
    std::vector<PendingStreamPacket> pending;
    std::vector<WAVEHDR*> activeWaveHdrs;
};

static std::vector<XboxDSStreamObject*> g_activeStreams;
static std::vector<PooledWaveOut>       g_waveOutPool;
static std::vector<WAVEHDR*>            g_waveHdrFreePool;

static WAVEHDR* AllocWaveHeader(uint32_t dataBytes) {
    uint32_t allocCap = (dataBytes + 4095u) & ~4095u;
    if (allocCap < 4096u) allocCap = 4096u;

    EnterCriticalSection(&g_audioCs);
    for (size_t i = 0; i < g_waveHdrFreePool.size(); ++i) {
        WAVEHDR* wh = g_waveHdrFreePool[i];
        uint32_t cap = static_cast<uint32_t>(wh->dwUser);
        if (cap >= dataBytes && cap <= allocCap * 4u) {
            g_waveHdrFreePool.erase(g_waveHdrFreePool.begin() + i);
            LeaveCriticalSection(&g_audioCs);
            memset(wh, 0, sizeof(WAVEHDR));
            wh->dwUser         = cap;
            wh->lpData         = reinterpret_cast<LPSTR>(wh + 1);
            wh->dwBufferLength = dataBytes;
            return wh;
        }
    }
    LeaveCriticalSection(&g_audioCs);

    WAVEHDR* wh = static_cast<WAVEHDR*>(calloc(1, sizeof(WAVEHDR) + allocCap));
    if (wh) {
        wh->dwUser         = allocCap;
        wh->lpData         = reinterpret_cast<LPSTR>(wh + 1);
        wh->dwBufferLength = dataBytes;
    }
    return wh;
}

static void FreeWaveHeaderLocked(WAVEHDR* wh) {
    if (!wh) return;
    if (g_waveHdrFreePool.size() < 128u && wh->dwUser >= 4096u && wh->dwUser <= 262144u) {
        g_waveHdrFreePool.push_back(wh);
    } else {
        free(wh);
    }
}

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
            FreeWaveHeaderLocked(wh);
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
            FreeWaveHeaderLocked(wh);
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
        DWORD st = 0u;
        if (s) {
            EnterCriticalSection(&g_audioCs);
            size_t pendingCount = s->pending.size();
            uint32_t maxPkts    = s->maxAttachedPackets ? s->maxAttachedPackets : 16u;
            bool hasPending     = (pendingCount > 0u);
            bool isPaused       = s->paused;
            if (pendingCount < maxPkts) {
                st |= kXboxStreamStatusReady;
            }
            LeaveCriticalSection(&g_audioCs);

            // VIDAudioHelper::Update (0x0043D4AB) checks DSSTREAMSTATUS_PAUSED (0x00020000) to unpause streams;
            // CMcpxStream::GetStatus (0x006E4E64) sets 0x00040000 (STARVED) and XACT::CSoundCue::DoWork (0x00708BD0)
            // checks 0x00080000 (test byte ptr [ebp-2], 8) to detect EOF/envelope completion.
            if (isPaused) {
                st |= kXboxStreamStatusPaused;
            }
            if (hasPending) {
                if (!isPaused) {
                    st |= kXboxStreamStatusPlaying;
                }
            } else {
                st |= (kXboxStreamStatusStarved | kXboxStreamStatusEnvelopeComplete);
            }
        } else {
            st = kXboxStreamStatusReady | kXboxStreamStatusStarved | kXboxStreamStatusEnvelopeComplete;
        }
        *pdwStatus = st;
    }
    return S_OK;
}

static void EnsureStreamWaveOut(XboxDSStreamObject* s) {
    if (!s) return;
    // Always open pooled HWAVEOUT handles at a unified 48000 Hz stereo PCM16 format so all
    // pooled waveOut instances are 100% interchangeable without rate-mismatch reopening, and
    // dynamic XACT pitch/frequency envelopes (s->freq up to 192,000 Hz) can be resampled per packet
    // in Stream_Process with zero waveOutOpen/waveOutClose churn.
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
            if (s->playStartTickMs == 0) {
                s->playStartTickMs = GetTickCount();
            }
            int32_t lfoEgVolModMb = 0;
            float   lfoEgPitchRatio = 1.0f;
            float   lfoEgCutoffRatio = 1.0f;
            if (s->lfoActive[0] || s->lfoActive[1] || s->egActive[0] || s->egActive[1]) {
                DWORD nowTick = GetTickCount();
                uint32_t elapsedMs = (nowTick >= s->playStartTickMs) ? (nowTick - s->playStartTickMs) : 0u;
                uint32_t relElapsedMs = (s->isReleasing && s->releaseStartTickMs != 0 && nowTick >= s->releaseStartTickMs)
                                        ? (nowTick - s->releaseStartTickMs) : 0u;
                EvaluateXboxVoiceLfoAndEg(
                    s->lfo, s->lfoActive, s->eg, s->egActive,
                    elapsedMs, s->isReleasing, relElapsedMs,
                    &lfoEgVolModMb, &lfoEgPitchRatio, &lfoEgCutoffRatio);
            }

            uint32_t inRate = (s->freq >= kMinDirectSoundFreqHz && s->freq <= kMaxXboxPitchFreqHz)
                              ? s->freq
                              : (s->wfx.nSamplesPerSec ? s->wfx.nSamplesPerSec : kDefaultSampleRateHz);
            if (s->voice3D.active3D && s->voice3D.dopplerPitch != 0) {
                inRate = XboxPitchToFrequencyHz(s->lPitch + s->voice3D.dopplerPitch);
            }
            if (lfoEgPitchRatio > 0.01f && fabsf(lfoEgPitchRatio - 1.0f) > 0.001f) {
                inRate = static_cast<uint32_t>(static_cast<float>(inRate) * lfoEgPitchRatio + 0.5f);
                if (inRate < kMinDirectSoundFreqHz) inRate = kMinDirectSoundFreqHz;
                if (inRate > kMaxXboxPitchFreqHz)   inRate = kMaxXboxPitchFreqHz;
            }
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
                // resampling from inRate (which reflects Stream_SetPitch / Stream_SetFrequency / 3D Doppler
                // up to 192,000 Hz with zero fallback) with continuous cross-packet fractional phase
                // (s->resamplePos) and last-sample history (s->prevSampleL/R), plus 3D panning gains.
                uint32_t outBytes = outFrames * kWaveOutStereoChannels * sizeof(int16_t);
                WAVEHDR* wh = AllocWaveHeader(outBytes);
                if (wh) {
                    int16_t* dstSamples = reinterpret_cast<int16_t*>(wh->lpData);

                    LONG totVol = s->volume - s->headroomMb + s->atten3D + lfoEgVolModMb;
                    LONG pan    = s->pan;
                    bool hasI3DL2Send = s->hasI3DL2Send;
                    int32_t i3dl2SendMb = s->i3dl2SendMb;
                    if (s->outputBuffer) {
                        ApplyXboxOutputBufferRouting(s->outputBuffer, &totVol, &pan, &hasI3DL2Send, &i3dl2SendMb);
                    }
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

                    // Apply continuous 2-pole/4-pole Chamberlin SVF filter (DLS2, PARAMEQ, MULTI) + HRTF head-shadow
                    float filterF = 1.0f, filterD = 1.0f, filterF2 = 1.0f, filterD2 = 1.0f;
                    uint32_t filterMode = 0u;
                    bool needFilter = s->hasActiveFilter &&
                        ComputeDls2FilterCoeffs(&s->filterDesc, outRate, &filterF, &filterD, &filterMode, &filterF2, &filterD2);
                    if (s->voice3D.active3D && s->voice3D.hrtfFilterCoeff > 0.0f && s->voice3D.hrtfFilterCoeff < 0.92f) {
                        if (!needFilter) {
                            needFilter = true;
                            filterMode = 1u;
                            filterF    = s->voice3D.hrtfFilterCoeff;
                            filterD    = 1.0f;
                        } else {
                            filterF   *= s->voice3D.hrtfFilterCoeff;
                            if (filterF < 0.004f) filterF = 0.004f;
                        }
                    }
                    if (needFilter) {
                        if (lfoEgCutoffRatio > 0.01f && fabsf(lfoEgCutoffRatio - 1.0f) > 0.01f) {
                            filterF *= lfoEgCutoffRatio;
                            if (filterF < 0.004f) filterF = 0.004f;
                            if (filterF > 1.40f)  filterF = 1.40f;
                        }
                        ApplyDls2FilterToPcm16(
                            dstSamples, outFrames, kWaveOutStereoChannels,
                            filterF, filterD, s->svfLow, s->svfBand,
                            filterMode, filterF2, filterD2, &s->filterState);
                    }

                    // Apply continuous stateful I3DL2 Reverb across stream packets when routed to MixBin 10 (I3DL2)
                    if (hasI3DL2Send) {
                        ApplyStatefulI3DL2ReverbToPcm16(
                            &s->reverbState, dstSamples, outFrames, kWaveOutStereoChannels, outRate,
                            i3dl2SendMb + s->voice3D.i3dl2RoomVol, 0);
                    }

                    if (waveOutPrepareHeader(s->hWaveOut, wh, sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
                        waveOutWrite(s->hWaveOut, wh, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                    {
                        submittedWh = wh;
                        EnterCriticalSection(&g_audioCs);
                        s->activeWaveHdrs.push_back(wh);
                        LeaveCriticalSection(&g_audioCs);
                    } else {
                        EnterCriticalSection(&g_audioCs);
                        FreeWaveHeaderLocked(wh);
                        LeaveCriticalSection(&g_audioCs);
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
    s->discontinuity  = false;
    s->flushExPending = false;
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
        s->discontinuity = true;
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
        s->discontinuity  = false;
        s->flushExPending = false;
        s->isReleasing    = false;
        s->playStartTickMs = 0;
        s->resamplePos    = 0.0;
        s->prevSampleL    = 0;
        s->prevSampleR    = 0;
        s->hasPrevSample  = false;
        s->svfLow[0]      = 0.0f;
        s->svfLow[1]      = 0.0f;
        s->svfBand[0]     = 0.0f;
        s->svfBand[1]     = 0.0f;
        memset(&s->filterState, 0, sizeof(s->filterState));
        memset(&s->reverbState, 0, sizeof(s->reverbState));
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
    // asynchronous FlushEx schedules the stream to transition to starved/envelope-complete AFTER all
    // currently queued input packets finish playing, without cutting off active packets!
    // DSBSTOPEX_ENVELOPE (0x2 on FlushEx) also triggers the EG release stage.
    if ((dwFlags & 3u) != 0) {
        if (s) {
            EnterCriticalSection(&g_audioCs);
            s->flushExPending = true;
            if ((dwFlags & 2u) != 0 && s->egActive[0] && s->eg[0].dwRelease > 0u) {
                s->isReleasing = true;
                s->releaseStartTickMs = GetTickCount();
            }
            LeaveCriticalSection(&g_audioCs);
        }
        return S_OK;
    }
    return Stream_Flush(s);
}

static HRESULT __stdcall Stream_SetVolume(XboxDSStreamObject* s, LONG lVolume) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->volume = lVolume;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetHeadroom(XboxDSStreamObject* s, DWORD dwHeadroom) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->headroomMb = (dwHeadroom <= 10000u) ? static_cast<LONG>(dwHeadroom) : 10000;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetPitch(XboxDSStreamObject* s, LONG lPitch) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->lPitch = lPitch;
        s->freq   = XboxPitchToFrequencyHz(lPitch);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetFrequency(XboxDSStreamObject* s, DWORD dwFrequency) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        if (dwFrequency < kMinDirectSoundFreqHz) dwFrequency = kMinDirectSoundFreqHz;
        if (dwFrequency > kMaxXboxPitchFreqHz)   dwFrequency = kMaxXboxPitchFreqHz;
        s->freq   = dwFrequency;
        s->lPitch = FrequencyHzToXboxPitch(dwFrequency);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetLFO(XboxDSStreamObject* s, const XBOX_DSLFODESC* pLFODesc) {
    if (!s) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    if (pLFODesc && reinterpret_cast<uintptr_t>(pLFODesc) >= 0x00010000u) {
        uint32_t idx = (pLFODesc->dwLFO == 1u) ? 1u : 0u;
        s->lfo[idx] = *pLFODesc;
        s->lfoActive[idx] = (pLFODesc->dwDelta > 0u) &&
                            (pLFODesc->lPitchModulation != 0 ||
                             pLFODesc->lFilterCutOffRange != 0 ||
                             pLFODesc->lAmplitudeModulation != 0);
    } else {
        s->lfoActive[0] = false;
        s->lfoActive[1] = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Stream_SetEG(XboxDSStreamObject* s, const XBOX_DSENVELOPEDESC* pEnvelopeDesc) {
    if (!s) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    if (pEnvelopeDesc && reinterpret_cast<uintptr_t>(pEnvelopeDesc) >= 0x00010000u) {
        uint32_t idx = (pEnvelopeDesc->dwEG == 1u) ? 1u : 0u;
        s->eg[idx] = *pEnvelopeDesc;
        s->egActive[idx] = (pEnvelopeDesc->dwMode != 0u);
    } else {
        s->egActive[0] = false;
        s->egActive[1] = false;
    }
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Stream_SetOutputBuffer(XboxDSStreamObject* s, XboxDSBufferObject* pOutputBuffer) {
    if (!s) return E_INVALIDARG;
    EnterCriticalSection(&g_audioCs);
    s->outputBuffer = pOutputBuffer;
    LeaveCriticalSection(&g_audioCs);
    return S_OK;
}

static HRESULT __stdcall Stream_SetFilter(XboxDSStreamObject* s, const XBOX_DSFILTERDESC* pFilterDesc) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        if (pFilterDesc && reinterpret_cast<uintptr_t>(pFilterDesc) >= 0x00010000u && (pFilterDesc->dwMode & 3u) != 0u) {
            s->filterDesc      = *pFilterDesc;
            s->hasActiveFilter = true;
        } else {
            memset(&s->filterDesc, 0, sizeof(s->filterDesc));
            s->hasActiveFilter = false;
            s->svfLow[0]       = 0.0f;
            s->svfLow[1]       = 0.0f;
            s->svfBand[0]      = 0.0f;
            s->svfBand[1]      = 0.0f;
            memset(&s->filterState, 0, sizeof(s->filterState));
        }
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_GetVoiceProperties(XboxDSStreamObject* s, XBOX_DSVOICEPROPS* pProps) {
    if (!pProps) return E_INVALIDARG;
    memset(pProps, 0, sizeof(*pProps));
    if (s) {
        EnterCriticalSection(&g_audioCs);
        FillVoicePropsMixBins(s->mixBinPairs, s->mixBinCount, pProps);
        pProps->lPitch             = s->lPitch;
        pProps->l3DDistanceVolume  = s->voice3D.distanceVol;
        pProps->l3DConeVolume      = s->voice3D.coneVol;
        pProps->l3DDopplerPitch    = s->voice3D.dopplerPitch;
        pProps->lI3DL2DirectVolume = s->voice3D.i3dl2DirectVol;
        pProps->lI3DL2RoomVolume   = s->voice3D.i3dl2RoomVol;
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetFormat(XboxDSStreamObject* s, const WAVEFORMATEX* pwfx) {
    if (s && pwfx) {
        EnterCriticalSection(&g_audioCs);
        s->wfx = *pwfx;
        s->isAdpcm = (pwfx->wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
        if (pwfx->nSamplesPerSec) {
            s->freq   = pwfx->nSamplesPerSec;
            s->lPitch = FrequencyHzToXboxPitch(s->freq);
        }
        s->resamplePos   = 0.0;
        s->prevSampleL   = 0;
        s->prevSampleR   = 0;
        s->hasPrevSample = false;
        s->svfLow[0]     = 0.0f;
        s->svfLow[1]     = 0.0f;
        s->svfBand[0]    = 0.0f;
        s->svfBand[1]    = 0.0f;
        memset(&s->filterState, 0, sizeof(s->filterState));
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Set3DVoiceData(XboxDSStreamObject* s, const void* pVoiceData) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        s->hasI3DL2Send = true;
        Compute3DAudioPanAndAtten(pVoiceData, &s->pan, &s->atten3D, &s->voice3D);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Use3DVoiceData(XboxDSStreamObject* s, const void* pVoiceData) {
    return Stream_Set3DVoiceData(s, pVoiceData);
}

static HRESULT __stdcall Stream_SetMixBins(XboxDSStreamObject* s, const XBOX_DSMIXBINS* pMixBins) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        AssignVoiceMixBins(pMixBins, s->mixBinPairs, &s->mixBinCount);
        ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan);
        ExtractStreamMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static HRESULT __stdcall Stream_SetMixBinVolumes(XboxDSStreamObject* s, const XBOX_DSMIXBINS* pMixBins) {
    if (s) {
        EnterCriticalSection(&g_audioCs);
        UpdateVoiceMixBinVolumes(pMixBins, s->mixBinPairs, &s->mixBinCount);
        ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan);
        ExtractStreamMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
        LeaveCriticalSection(&g_audioCs);
    }
    return S_OK;
}

static void* g_streamVtable[7] = {
    reinterpret_cast<void*>(&Stream_AddRef),
    reinterpret_cast<void*>(&Stream_Release),
    reinterpret_cast<void*>(&Stream_GetInfo),
    reinterpret_cast<void*>(&Stream_GetStatus),
    reinterpret_cast<void*>(&Stream_Process),
    reinterpret_cast<void*>(&Stream_Discontinuity),
    reinterpret_cast<void*>(&Stream_Flush),
};

static HRESULT __stdcall Bridge_DirectSoundCreateStream(const XBOX_DSSTREAMDESC* pDesc, XboxDSStreamObject** ppStream) {
    if (!ppStream) return E_INVALIDARG;

    XboxDSStreamObject* s = new XboxDSStreamObject();
    s->vtable = g_streamVtable;
    s->refCount = 1;
    memset(&s->wfx, 0, sizeof(s->wfx));
    s->isAdpcm = false;
    s->maxAttachedPackets = 16u;
    s->lpfnCallback = nullptr;
    s->lpvContext = nullptr;
    s->volume = 0;
    s->headroomMb = 0;
    s->pan = 0;
    s->atten3D = 0;
    s->lPitch = 0;
    s->freq = kDefaultSampleRateHz;
    s->paused = false;
    s->discontinuity = false;
    s->flushExPending = false;
    s->hWaveOut = nullptr;
    s->waveOutRate = 0;
    s->waveOutChannels = 0;
    s->nextCompleteTick = GetTickCount();
    s->resamplePos = 0.0;
    s->prevSampleL = 0;
    s->prevSampleR = 0;
    s->hasPrevSample = false;
    s->hasI3DL2Send = false;
    s->i3dl2SendMb = kMinDirectSoundVolumeMb;
    memset(&s->voice3D, 0, sizeof(s->voice3D));
    memset(&s->filterDesc, 0, sizeof(s->filterDesc));
    s->hasActiveFilter = false;
    s->svfLow[0] = s->svfLow[1] = 0.0f;
    s->svfBand[0] = s->svfBand[1] = 0.0f;
    memset(&s->filterState, 0, sizeof(s->filterState));
    memset(&s->reverbState, 0, sizeof(s->reverbState));
    memset(s->lfo, 0, sizeof(s->lfo));
    s->lfoActive[0] = s->lfoActive[1] = false;
    memset(s->eg, 0, sizeof(s->eg));
    s->egActive[0] = s->egActive[1] = false;
    s->outputBuffer = nullptr;
    s->playStartTickMs = 0;
    s->isReleasing = false;
    s->releaseStartTickMs = 0;
    InitVoiceDefaultMixBins(pDesc ? pDesc->dwFlags : 0u, s->mixBinPairs, &s->mixBinCount);

    if (pDesc) {
        if (pDesc->dwMaxAttachedPackets > 0u) {
            s->maxAttachedPackets = pDesc->dwMaxAttachedPackets;
        }
        s->lpfnCallback = pDesc->lpfnCallback;
        s->lpvContext = pDesc->lpvContext;
        if (pDesc->lpwfxFormat) {
            s->wfx = *pDesc->lpwfxFormat;
            s->isAdpcm = (s->wfx.wFormatTag == XBOX_WAVE_FORMAT_XBOX_ADPCM);
            if (s->wfx.nSamplesPerSec) {
                s->freq   = s->wfx.nSamplesPerSec;
                s->lPitch = FrequencyHzToXboxPitch(s->freq);
            }
        }
        if (pDesc->lpMixBins) {
            const XBOX_DSMIXBINS* pMixBins = reinterpret_cast<const XBOX_DSMIXBINS*>(pDesc->lpMixBins);
            AssignVoiceMixBins(pMixBins, s->mixBinPairs, &s->mixBinCount);
            ComputeMixBinPairsPan(s->mixBinPairs, s->mixBinCount, &s->pan);
            ExtractStreamMixBinI3DL2Send(s->mixBinPairs, s->mixBinCount, &s->hasI3DL2Send, &s->i3dl2SendMb);
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

void UpdateXboxAudioTick() {
    if (!g_audioCsInit) return;
    uint32_t now = GetTickCount();

    EnterCriticalSection(&g_audioCs);
    AdvanceActiveAudioBuffersSubLoop();
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

static void __fastcall Bridge_XACT_CEngine_GetTime(void* thisPtr, int dummyEdx, int64_t* pTime) {
    (void)thisPtr; (void)dummyEdx;
    uint64_t ms = GetTickCount64();
    g_mcpxSampleCounter = (uint32_t)(ms * 48ULL);
    if (pTime) {
        *pTime = (int64_t)(ms * 10000ULL);
    }
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
    InstallBridge(0x00703BDF, &Bridge_XACT_CEngine_GetTime);                // ?GetTime@CEngine@XACT@@QAEXPA_J@Z

    // 2. Top-level DirectSound Stream & DoWork + Buffer bridges
    InitXboxAudioBuffers();
    InstallBridge(0x006E30DD, &Bridge_DirectSoundCreateStream);             // _DirectSoundCreateStream@8
    InstallBridge(0x006E2EAC, &Bridge_IDirectSound_CreateSoundStream);      // _IDirectSound_CreateSoundStream@16
    InstallBridge(0x006E1A58, &Bridge_DirectSoundDoWork);                   // _DirectSoundDoWork@0
    // Initialize CHRTFSource::m_vtable (0x006FE22C) via SetFullHRTF5Channel (0x006E3C2D)
    reinterpret_cast<void (__stdcall *)()>(0x006E3C2D)();

    // 3. IDirectSoundStream / CDirectSoundStream target methods (called by 5-byte thunks at 0x006E1A00..0x006E275B)
    InstallBridge(0x006E14B9, &Stream_SetVolume);                           // CDirectSoundStream::SetVolume
    InstallBridge(0x006E1467, &Stream_SetPitch);                            // CDirectSoundStream::SetPitch
    InstallBridge(0x006E150B, &Stream_SetLFO);                              // CDirectSoundStream::SetLFO
    InstallBridge(0x006E155D, &Stream_SetEG);                               // CDirectSoundStream::SetEG
    InstallBridge(0x006E15AF, &Stream_SetFilter);                           // CDirectSoundStream::SetFilter
    InstallBridge(0x006E1601, &Stream_SetHeadroom);                         // CDirectSoundStream::SetHeadroom
    InstallBridge(0x006E1653, &Stream_SetMixBinVolumes);                    // CDirectSoundStream::SetMixBinVolumes
    InstallBridge(0x006E0D45, &Stream_Pause);                               // CDirectSoundStream::Pause
    InstallBridge(0x006E0D96, &Stream_FlushEx);                             // CDirectSoundStream::FlushEx
    InstallBridge(0x006E16A5, &Stream_GetVoiceProperties);                  // CDirectSoundStream::GetVoiceProperties
    InstallBridge(0x006E16F7, &Stream_Set3DVoiceData);                      // CDirectSoundStream::Set3DVoiceData
    InstallBridge(0x006E1749, &Stream_Use3DVoiceData);                      // CDirectSoundStream::Use3DVoiceData
    InstallBridge(0x006E1F87, &Stream_SetFrequency);                        // CDirectSoundStream::SetFrequency
    InstallBridge(0x006E1FD9, &Stream_SetOutputBuffer);                     // CDirectSoundStream::SetOutputBuffer
    InstallBridge(0x006E202B, &Stream_SetMixBins);                          // CDirectSoundStream::SetMixBins
    InstallBridge(0x006E26CD, &Stream_SetFormat);                           // CDirectSoundStream::SetFormat
}
