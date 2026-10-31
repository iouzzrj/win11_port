#pragma once

#include <stdint.h>

// ============================================================================
// Ghost.exe Virtual Memory Addresses & Constants (ImageBase 0x00400000)
// ============================================================================

// --- Xbox Image Memory Space ---
static constexpr uintptr_t kMinValidXboxAddress                = 0x00400000u;
static constexpr uint32_t  kXboxPeImageBase                    = 0x00400000u;
static constexpr uint32_t  kXboxReservedSize                   = 0x00550000u;
static constexpr uint32_t  kXboxCodeScanStartVa                = 0x00400740u;
static constexpr uint32_t  kXboxCodeScanEndVa                  = 0x0074FE80u;
static constexpr uint32_t  kXboxZeroImageSize                  = 0x005228C0u;
static constexpr uint32_t  kAddrMuStaticCtorSlot               = 0x007C2D9Cu;

// --- Xbox D3D8 Device & State Tables ---
static constexpr uint32_t  ADDR_D3D_PDEVICE                    = 0x006C0888u; // _D3D__pDevice
static constexpr uint32_t  ADDR_D3D_DEVICE                     = 0x006C0890u; // _D3D__Device (CDevice)
static constexpr uint32_t  ADDR_D3D_RENDERSTATE                = 0x006C05F0u; // _D3D__RenderState (166 DWORDs)
static constexpr uint32_t  ADDR_D3D_TEXTURESTATE               = 0x006C03F0u; // _D3D__TextureState (4 stages * 32 DWORDs)
static constexpr uint32_t  ADDR_D3D_INDEXDATA                  = 0x006C03E4u; // _D3D__IndexData
static constexpr uint32_t  ADDR_D3D_DIRTYFLAGS                 = 0x006C03E8u; // _D3D__DirtyFlags
static constexpr uint32_t  ADDR_D3D_VS_CONSTANTS               = 0x006BF7D0u; // CDevice VertexShader constants (192 * vec4)
static constexpr uint32_t  ADDR_D3D_STREAM                     = 0x006C2D48u; // g_Stream (16 * 12 bytes)
static constexpr uint32_t  ADDR_SIMPLE_RS_ENCODE               = 0x007502A0u; // _D3D__SimpleRenderStateEncode (92 DWORDs)

// --- Xbox D3D8 Internal Helpers (XGRPH / PixelJar) ---
static constexpr uint32_t  ADDR_XG_SWIZZLE_RECT                = 0x0071F711u; // _XGSwizzleRect@32
static constexpr uint32_t  ADDR_XG_UNSWIZZLE_RECT              = 0x0071FE62u; // _XGUnswizzleRect@32
static constexpr uint32_t  ADDR_PIXELJAR_LOCK_2D               = 0x006B7150u; // Lock2DSurface@PixelJar@D3D
static constexpr uint32_t  ADDR_PIXELJAR_LOCK_3D               = 0x006B71F0u; // Lock3DSurface@PixelJar@D3D

// --- StarCraft: Ghost Engine Globals & Shader Tables ---
static constexpr uint32_t  ADDR_VERTEX_SHADERS_TABLE           = 0x007D0BE0u; // aVertexShaders[66] (66 * 24 bytes)
static constexpr uint32_t  ADDR_PIXEL_SHADERS_TABLE            = 0x007D1214u; // aPixelShaders[28] (28 * 8 bytes)
static constexpr uint32_t  ADDR_MAIN_BACKBUFFER_PTR            = 0x008AA344u; // Engine main backbuffer XboxPixelContainer*
static constexpr uint32_t  ADDR_CUR_RENDERTARGET_PTR           = 0x008AA350u; // pCurRenderTarget
static constexpr uint32_t  ADDR_LIGHTING_MODE                  = 0x008AA460u; // cRenderState::m_lightingMode
static constexpr uint32_t  ADDR_DYN_VB_PTR                     = 0x008AAA80u; // Engine dynamic skinning vertex buffer
static constexpr uint32_t  ADDR_ACTIVE_CAMERA_PTR              = 0x0088CD6Cu; // Active camera pointer
static constexpr uint32_t  ADDR_CUR_MODEL_INST_PTR             = 0x008ACC30u; // Current cModelInstance*
static constexpr uint32_t  ADDR_CUR_BONE_MATS_PTR              = 0x008ACC34u; // Current bone matrix palette pointer

// --- cDivxVideo (.vid Movie Player in VIDPlayerXB.obj) ---
static constexpr uint32_t  kAddrDivxVideoFrameMove             = 0x0043DE80u; // ?FrameMove@cDivxVideo@@QAEJXZ
static constexpr uint32_t  kAddrDivxVideoRender                = 0x0043DEE0u; // ?Render@cDivxVideo@@QAEJXZ
static constexpr uint32_t  kAddrVidSimplePlayerDecodeNextFrame = 0x0043EB60u;
static constexpr uint32_t  kAddrVidSimplePlayerHandleAudio     = 0x0043EC00u;
static constexpr uint32_t  kAddrVidSimplePlayerStartPlayback   = 0x0043EC10u;
static constexpr uint32_t  kAddrVidRenderToOverlay             = 0x0043E420u;
static constexpr uint32_t  kAddrVidRenderToTexture             = 0x0043E5E0u;
static constexpr uint32_t  kAddrVidRenderToSurface             = 0x0043E670u;

// --- Xbox Audio / DirectSound / XACT Addresses ---
static constexpr uint32_t  ADDR_DSOUND_SINGLETON_PTR           = 0x006FE200u; // m_pDirectSound
static constexpr uint32_t  ADDR_XACT_ENGINE_PTR                = 0x00708F74u; // _g_pXACTEngine
static constexpr uint32_t  ADDR_XACT_CRITICAL_SECTION          = 0x00708F58u; // XACT global critical section
static constexpr uint32_t  ADDR_XACT_LISTENER                  = 0x007091E8u; // Global XACT 3D Listener
