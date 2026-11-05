#pragma once

#include <stdint.h>

// ============================================================================
// Ghost.exe Complete PE Section & Memory Layout Reference (ImageBase 0x00400000)
// ============================================================================
// Total PE sections in Ghost.exe: 33 (29 unique linker segment names; BINKYUY2*
// names > 8 chars are truncated to 8 bytes in the PE section header).
//
//  #  PE Name    Full Segment Name   Virtual Address Range      VSize     RawSize   Description
// --- ---------- ------------------- -------------------------- --------- --------- --------------------------------------------------
//  0  .text      .text               0x00400740 .. 0x006AEBDF   0x2AE49A  0x2AE4A0  Game engine C++, Havok 2 physics, xGameScript, CRT
//  1  D3D        D3D                 0x006AEBE0 .. 0x006C309F   0x0144B0  0x0144C0  Xbox D3D8 library & globals (_D3D__Device, State)
//  2  D3DX       D3DX                0x006C30A0 .. 0x006DFDDF   0x01CD28  0x01CD40  Xbox D3DX8 math/texture/shader helpers
//  3  DSOUND     DSOUND              0x006DFDE0 .. 0x006FE3DF   0x01E5FC  0x01E600  Xbox DirectSound8 & MCPX APU voice/stream classes
//  4  XACTENG    XACTENG             0x006FE3E0 .. 0x0070923F   0x00AE58  0x00AE60  Xbox XACT audio engine (CEngine, CSoundCue, etc.)
//  5  BINK       BINK                0x00709240 .. 0x007164DF   0x00D290  0x00D2A0  Bink video core decoder & DirectSound audio glue
//  6  BINK32     BINK32              0x007164E0 .. 0x0071775F   0x001265  0x001280  Bink 32-bit RGB blitter
//  7  BINK32A    BINK32A             0x00717760 .. 0x00718CDF   0x001579  0x001580  Bink 32-bit RGBA alpha blitter
//  8  BINK16     BINK16              0x00718CE0 .. 0x00719FFF   0x00131B  0x001320  Bink 16-bit 565 RGB blitter
//  9  BINK4444   BINK4444            0x0071A000 .. 0x0071B59F   0x00159F  0x0015A0  Bink 16-bit 4444 RGBA blitter
// 10  BINK5551   BINK5551            0x0071B5A0 .. 0x0071C6DF   0x001132  0x001140  Bink 16-bit 5551 RGBA blitter
// 11  BINKYUY2   BINKYUY2            0x0071C6E0 .. 0x0071D39F   0x000CAC  0x000CC0  Bink YUY2 overlay blitter
// 12  BINKYUY2   BINKYUY2M           0x0071D3A0 .. 0x0071D43F   0x00009A  0x0000A0  Bink YUY2M blitter (PE name truncated to 8 chars)
// 13  BINKYUY2   BINKYUY2X2          0x0071D440 .. 0x0071D4FF   0x0000B7  0x0000C0  Bink YUY2 2x zoom blitter (truncated in PE header)
// 14  BINKYUY2   BINKYUY2MX2         0x0071D500 .. 0x0071D59F   0x000087  0x0000A0  Bink YUY2M 2x zoom blitter (truncated in PE header)
// 15  BINK16MX   BINK16MX2           0x0071D5A0 .. 0x0071D6DF   0x00012E  0x000140  Bink 16-bit M 2x blitter (truncated in PE header)
// 16  BINK16X2   BINK16X2            0x0071D6E0 .. 0x0071DC3F   0x000558  0x000560  Bink 16-bit 2x blitter
// 17  BINK16M    BINK16M             0x0071DC40 .. 0x0071DE3F   0x0001F7  0x000200  Bink 16-bit M blitter
// 18  BINK32MX   BINK32MX2           0x0071DE40 .. 0x0071DFFF   0x0001BA  0x0001C0  Bink 32-bit M 2x blitter (truncated in PE header)
// 19  BINK32X2   BINK32X2            0x0071E000 .. 0x0071E59F   0x00058B  0x0005A0  Bink 32-bit 2x blitter
// 20  BINK32M    BINK32M             0x0071E5A0 .. 0x0071E6FF   0x00014C  0x000160  Bink 32-bit M blitter
// 21  XGRPH      XGRPH               0x0071E700 .. 0x00720A1F   0x002318  0x002320  Xbox xgraphics.lib (XGUnswizzleRect, XGSwizzleRect)
// 22  WMADEC     WMADEC              0x00720A20 .. 0x00739A9F   0x01907C  0x019080  Xbox WMA audio decoder XMO
// 23  XNET       XNET                0x00739AA0 .. 0x00747ADF   0x00E040  0x00E040  Xbox network stack (XNetStartup, socket APIs)
// 24  XPP        XPP                 0x00747AE0 .. 0x0074FE7F   0x008388  0x0083A0  Xbox peripheral port & XInput (XInputGetState)
// 25  .rdata     .rdata              0x0074FE80 .. 0x007C2C9F   0x072E10  0x072E20  xboxkrnl.exe IAT (0x0074FE80), vtables, constants
// 26  .data      .data + .bss        0x007C2CA0 .. 0x008B86BF   0x0F5A18  0x0F5A20  Initialized & uninitialized game/engine globals
// 27  DOLBY      DOLBY               0x008B86C0 .. 0x008BF83F   0x007180  0x007180  Dolby surround DSP tables
// 28  BINKDATA   BINKDATA            0x008BF840 .. 0x008CEF9F   0x00F754  0x00F760  Bink codec lookup tables
// 29  INIT       INIT                0x008CEFA0 .. 0x008CF1FF   0x00025E  0x000260  Xbox kernel/XAPI init section
// 30  .tls       .tls                0x008CF200 .. 0x008CF29F   0x000091  0x0000A0  Thread Local Storage template
// 31  .XBLD      .XBLD               0x008CF2A0 .. 0x008CF35F   0x0000B0  0x0000C0  Xbox XDK build version stamp
// 32  .reloc     .reloc              0x008CF360 .. 0x0092227F   0x052F14  0x052F20  PE fixup section

// ============================================================================
// Xbox D3D8 RenderState Enumeration (_D3D__RenderState at 0x006C05F0, 166 DWORDs)
// ============================================================================
// Verified against Ghost.exe:
//   - Simple RenderStates   (  0.. 91): _D3D__SimpleRenderStateEncode at 0x007502A0
//   - Deferred RenderStates ( 92..135): _D3D__DirtyFlags table at 0x00750080 + State*4
//   - Complex RenderStates  (136..165): g_ComplexRenderStateFunctionTable at 0x006BE438
//   - Initial default values( 57..165): g_InitialRenderStates at 0x006BE970

enum XboxRenderState : uint32_t {
    // --- Pixel Shader / Register Combiner Simple RenderStates (0..56) ---
    XBOX_D3DRS_PSALPHAINPUTS0                 =   0, // NV097 0x0260 (init 0x00000203)
    XBOX_D3DRS_PSALPHAINPUTS1                 =   1, // NV097 0x0264 (init 0x00000207)
    XBOX_D3DRS_PSALPHAINPUTS2                 =   2, // NV097 0x0268
    XBOX_D3DRS_PSALPHAINPUTS3                 =   3, // NV097 0x026C
    XBOX_D3DRS_PSALPHAINPUTS4                 =   4, // NV097 0x0270
    XBOX_D3DRS_PSALPHAINPUTS5                 =   5, // NV097 0x0274
    XBOX_D3DRS_PSALPHAINPUTS6                 =   6, // NV097 0x0278
    XBOX_D3DRS_PSALPHAINPUTS7                 =   7, // NV097 0x027C
    XBOX_D3DRS_PSFINALCOMBINERINPUTSABCD      =   8, // NV097 0x0288
    XBOX_D3DRS_PSFINALCOMBINERINPUTSEFG       =   9, // NV097 0x028C
    XBOX_D3DRS_PSCONSTANT0_0                  =  10, // NV097 0x0A60
    XBOX_D3DRS_PSCONSTANT0_1                  =  11, // NV097 0x0A64
    XBOX_D3DRS_PSCONSTANT0_2                  =  12, // NV097 0x0A68
    XBOX_D3DRS_PSCONSTANT0_3                  =  13, // NV097 0x0A6C
    XBOX_D3DRS_PSCONSTANT0_4                  =  14, // NV097 0x0A70
    XBOX_D3DRS_PSCONSTANT0_5                  =  15, // NV097 0x0A74
    XBOX_D3DRS_PSCONSTANT0_6                  =  16, // NV097 0x0A78
    XBOX_D3DRS_PSCONSTANT0_7                  =  17, // NV097 0x0A7C
    XBOX_D3DRS_PSCONSTANT1_0                  =  18, // NV097 0x0A80
    XBOX_D3DRS_PSCONSTANT1_1                  =  19, // NV097 0x0A84
    XBOX_D3DRS_PSCONSTANT1_2                  =  20, // NV097 0x0A88
    XBOX_D3DRS_PSCONSTANT1_3                  =  21, // NV097 0x0A8C
    XBOX_D3DRS_PSCONSTANT1_4                  =  22, // NV097 0x0A90
    XBOX_D3DRS_PSCONSTANT1_5                  =  23, // NV097 0x0A94
    XBOX_D3DRS_PSCONSTANT1_6                  =  24, // NV097 0x0A98
    XBOX_D3DRS_PSCONSTANT1_7                  =  25, // NV097 0x0A9C
    XBOX_D3DRS_PSALPHAOUTPUTS0                =  26, // NV097 0x0AA0
    XBOX_D3DRS_PSALPHAOUTPUTS1                =  27, // NV097 0x0AA4
    XBOX_D3DRS_PSALPHAOUTPUTS2                =  28, // NV097 0x0AA8
    XBOX_D3DRS_PSALPHAOUTPUTS3                =  29, // NV097 0x0AAC
    XBOX_D3DRS_PSALPHAOUTPUTS4                =  30, // NV097 0x0AB0
    XBOX_D3DRS_PSALPHAOUTPUTS5                =  31, // NV097 0x0AB4
    XBOX_D3DRS_PSALPHAOUTPUTS6                =  32, // NV097 0x0AB8
    XBOX_D3DRS_PSALPHAOUTPUTS7                =  33, // NV097 0x0ABC
    XBOX_D3DRS_PSRGBINPUTS0                   =  34, // NV097 0x0AC0
    XBOX_D3DRS_PSRGBINPUTS1                   =  35, // NV097 0x0AC4
    XBOX_D3DRS_PSRGBINPUTS2                   =  36, // NV097 0x0AC8
    XBOX_D3DRS_PSRGBINPUTS3                   =  37, // NV097 0x0ACC
    XBOX_D3DRS_PSRGBINPUTS4                   =  38, // NV097 0x0AD0
    XBOX_D3DRS_PSRGBINPUTS5                   =  39, // NV097 0x0AD4
    XBOX_D3DRS_PSRGBINPUTS6                   =  40, // NV097 0x0AD8
    XBOX_D3DRS_PSRGBINPUTS7                   =  41, // NV097 0x0ADC
    XBOX_D3DRS_PSCOMPAREMODE                  =  42, // NV097 0x17F8
    XBOX_D3DRS_PSFINALCOMBINERCONSTANT0       =  43, // NV097 0x1E20
    XBOX_D3DRS_PSFINALCOMBINERCONSTANT1       =  44, // NV097 0x1E24
    XBOX_D3DRS_PSRGBOUTPUTS0                  =  45, // NV097 0x1E40
    XBOX_D3DRS_PSRGBOUTPUTS1                  =  46, // NV097 0x1E44
    XBOX_D3DRS_PSRGBOUTPUTS2                  =  47, // NV097 0x1E48
    XBOX_D3DRS_PSRGBOUTPUTS3                  =  48, // NV097 0x1E4C
    XBOX_D3DRS_PSRGBOUTPUTS4                  =  49, // NV097 0x1E50
    XBOX_D3DRS_PSRGBOUTPUTS5                  =  50, // NV097 0x1E54
    XBOX_D3DRS_PSRGBOUTPUTS6                  =  51, // NV097 0x1E58
    XBOX_D3DRS_PSRGBOUTPUTS7                  =  52, // NV097 0x1E5C
    XBOX_D3DRS_PSCOMBINERCOUNT                =  53, // NV097 0x1E60
    XBOX_D3DRS_PS_RESERVED                    =  54, // NV097 0x1D90 (unused slot)
    XBOX_D3DRS_PSDOTMAPPING                   =  55, // NV097 0x1E74
    XBOX_D3DRS_PSINPUTTEXTURE                 =  56, // NV097 0x1E78

    // --- General Simple RenderStates (57..91) ---
    XBOX_D3DRS_ZFUNC                          =  57, // NV097 0x0354 (init 0x00000203 = D3DCMP_LESSEQUAL)
    XBOX_D3DRS_ALPHAFUNC                      =  58, // NV097 0x033C (init 0x00000207 = D3DCMP_ALWAYS)
    XBOX_D3DRS_ALPHABLENDENABLE               =  59, // NV097 0x0304 (init 0 = FALSE)
    XBOX_D3DRS_ALPHATESTENABLE                =  60, // NV097 0x0300 (init 0 = FALSE)
    XBOX_D3DRS_ALPHAREF                       =  61, // NV097 0x0340 (init 0)
    XBOX_D3DRS_SRCBLEND                       =  62, // NV097 0x0344 (init 1 = D3DBLEND_ONE)
    XBOX_D3DRS_DESTBLEND                      =  63, // NV097 0x0348 (init 0 = D3DBLEND_ZERO)
    XBOX_D3DRS_ZWRITEENABLE                   =  64, // NV097 0x035C (init 1 = TRUE)
    XBOX_D3DRS_DITHERENABLE                   =  65, // NV097 0x0310 (init 0 = FALSE)
    XBOX_D3DRS_SHADEMODE                      =  66, // NV097 0x037C (init 0x00001D01 = D3DSHADE_GOURAUD)
    XBOX_D3DRS_COLORWRITEENABLE               =  67, // NV097 0x0358 (init 0x01010101 = ALL)
    XBOX_D3DRS_STENCILZFAIL                   =  68, // NV097 0x0374 (init 0x00001E00 = D3DSTENCILOP_KEEP)
    XBOX_D3DRS_STENCILPASS                    =  69, // NV097 0x0378 (init 0x00001E00 = D3DSTENCILOP_KEEP)
    XBOX_D3DRS_STENCILFUNC                    =  70, // NV097 0x0364 (init 0x00000207 = D3DCMP_ALWAYS)
    XBOX_D3DRS_STENCILREF                     =  71, // NV097 0x0368 (init 0)
    XBOX_D3DRS_STENCILMASK                    =  72, // NV097 0x036C (init 0xFFFFFFFF)
    XBOX_D3DRS_STENCILWRITEMASK               =  73, // NV097 0x0360 (init 0xFFFFFFFF)
    XBOX_D3DRS_BLENDOP                        =  74, // NV097 0x0350 (init 0x00008006 = D3DBLENDOP_ADD)
    XBOX_D3DRS_BLENDCOLOR                     =  75, // NV097 0x034C (init 0)
    XBOX_D3DRS_SWATHWIDTH                     =  76, // NV097 0x09F8 (init 4)
    XBOX_D3DRS_POLYGONOFFSETZSLOPESCALE       =  77, // NV097 0x0384 (init 0.0f)
    XBOX_D3DRS_POLYGONOFFSETZOFFSET           =  78, // NV097 0x0388 (init 0.0f)
    XBOX_D3DRS_POINTOFFSETENABLE              =  79, // NV097 0x0330 (init 0 = FALSE)
    XBOX_D3DRS_WIREFRAMEOFFSETENABLE          =  80, // NV097 0x0334 (init 0 = FALSE)
    XBOX_D3DRS_SOLIDOFFSETENABLE              =  81, // NV097 0x0338 (init 0 = FALSE)
    XBOX_D3DRS_DEPTHCLIPCONTROL               =  82, // NV097 0x1D78 (init 1)
    XBOX_D3DRS_STIPPLEENABLE                  =  83, // NV097 0x147C (init 0 = FALSE)
    XBOX_D3DRS_SIMPLE_UNUSED8                 =  84, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED7                 =  85, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED6                 =  86, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED5                 =  87, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED4                 =  88, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED3                 =  89, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED2                 =  90, // NV097 0x1D90
    XBOX_D3DRS_SIMPLE_UNUSED1                 =  91, // NV097 0x1D90

    // --- Deferred RenderStates (92..135) ---
    XBOX_D3DRS_FOGENABLE                      =  92, // dirty 0x2000 (init 0 = FALSE)
    XBOX_D3DRS_FOGTABLEMODE                   =  93, // dirty 0x2000 (init 0 = D3DFOG_NONE)
    XBOX_D3DRS_FOGSTART                       =  94, // dirty 0x2000 (init 0.0f)
    XBOX_D3DRS_FOGEND                         =  95, // dirty 0x2000 (init 1.0f = 0x3F800000)
    XBOX_D3DRS_FOGDENSITY                     =  96, // dirty 0x2000 (init 1.0f = 0x3F800000)
    XBOX_D3DRS_RANGEFOGENABLE                 =  97, // dirty 0x2000 (init 0 = FALSE)
    XBOX_D3DRS_WRAP0                          =  98, // dirty 0x000F (init 0)
    XBOX_D3DRS_WRAP1                          =  99, // dirty 0x000F (init 0)
    XBOX_D3DRS_WRAP2                          = 100, // dirty 0x000F (init 0)
    XBOX_D3DRS_WRAP3                          = 101, // dirty 0x000F (init 0)
    XBOX_D3DRS_LIGHTING                       = 102, // dirty 0x1200 (init 1 = TRUE)
    XBOX_D3DRS_SPECULARENABLE                 = 103, // dirty 0x3000 (init 0 = FALSE)
    XBOX_D3DRS_LOCALVIEWER                    = 104, // dirty 0x1000 (init 1 = TRUE)
    XBOX_D3DRS_COLORVERTEX                    = 105, // dirty 0x1000 (init 1 = TRUE)
    XBOX_D3DRS_BACKSPECULARMATERIALSOURCE     = 106, // dirty 0x1000 (init 2 = D3DMCS_COLOR2)
    XBOX_D3DRS_BACKDIFFUSEMATERIALSOURCE      = 107, // dirty 0x1000 (init 1 = D3DMCS_COLOR1)
    XBOX_D3DRS_BACKAMBIENTMATERIALSOURCE      = 108, // dirty 0x1000 (init 0 = D3DMCS_MATERIAL)
    XBOX_D3DRS_BACKEMISSIVEMATERIALSOURCE     = 109, // dirty 0x1000 (init 0 = D3DMCS_MATERIAL)
    XBOX_D3DRS_SPECULARMATERIALSOURCE         = 110, // dirty 0x1000 (init 2 = D3DMCS_COLOR2)
    XBOX_D3DRS_DIFFUSEMATERIALSOURCE          = 111, // dirty 0x1000 (init 1 = D3DMCS_COLOR1)
    XBOX_D3DRS_AMBIENTMATERIALSOURCE          = 112, // dirty 0x1000 (init 0 = D3DMCS_MATERIAL)
    XBOX_D3DRS_EMISSIVEMATERIALSOURCE         = 113, // dirty 0x1000 (init 0 = D3DMCS_MATERIAL)
    XBOX_D3DRS_BACKAMBIENT                    = 114, // dirty 0x1000 (init 0)
    XBOX_D3DRS_AMBIENT                        = 115, // dirty 0x1000 (init 0)
    XBOX_D3DRS_POINTSIZE                      = 116, // dirty 0x0100 (init 1.0f = 0x3F800000)
    XBOX_D3DRS_POINTSIZE_MIN                  = 117, // dirty 0x0100 (init 0.0f)
    XBOX_D3DRS_POINTSPRITEENABLE              = 118, // dirty 0x0900 (init 0 = FALSE)
    XBOX_D3DRS_POINTSCALEENABLE               = 119, // dirty 0x0100 (init 0 = FALSE)
    XBOX_D3DRS_POINTSCALE_A                   = 120, // dirty 0x0100 (init 1.0f = 0x3F800000)
    XBOX_D3DRS_POINTSCALE_B                   = 121, // dirty 0x0100 (init 0.0f)
    XBOX_D3DRS_POINTSCALE_C                   = 122, // dirty 0x0100 (init 0.0f)
    XBOX_D3DRS_POINTSIZE_MAX                  = 123, // dirty 0x0100 (init 64.0f = 0x42800000)
    XBOX_D3DRS_PATCHEDGESTYLE                 = 124, // dirty 0x0000 (init 0 = D3DPATCHEDGE_DISCRETE)
    XBOX_D3DRS_PATCHSEGMENTS                  = 125, // dirty 0x0000 (init 1.0f = 0x3F800000)
    XBOX_D3DRS_SWAPFILTER                     = 126, // dirty 0x0000 (D3DTEXF_LINEAR etc.)
    XBOX_D3DRS_PRESENTATIONINTERVAL           = 127, // dirty 0x0000
    XBOX_D3DRS_DEFERRED_UNUSED8               = 128,
    XBOX_D3DRS_DEFERRED_UNUSED7               = 129,
    XBOX_D3DRS_DEFERRED_UNUSED6               = 130,
    XBOX_D3DRS_DEFERRED_UNUSED5               = 131,
    XBOX_D3DRS_DEFERRED_UNUSED4               = 132,
    XBOX_D3DRS_DEFERRED_UNUSED3               = 133,
    XBOX_D3DRS_DEFERRED_UNUSED2               = 134,
    XBOX_D3DRS_DEFERRED_UNUSED1               = 135,

    // --- Complex RenderStates (136..165) ---
    XBOX_D3DRS_PSTEXTUREMODES                 = 136, // 0x006AEBE0 (_D3DDevice_SetRenderState_PSTextureModes@4)
    XBOX_D3DRS_VERTEXBLEND                    = 137, // 0x006AF430 (_D3DDevice_SetRenderState_VertexBlend@4)
    XBOX_D3DRS_FOGCOLOR                       = 138, // 0x006AEF90 (_D3DDevice_SetRenderState_FogColor@4)
    XBOX_D3DRS_FILLMODE                       = 139, // 0x006AF300 (_D3DDevice_SetRenderState_FillMode@4, init 0x1B02 = SOLID)
    XBOX_D3DRS_BACKFILLMODE                   = 140, // 0x006AF350 (_D3DDevice_SetRenderState_BackFillMode@4, init 0x1B02)
    XBOX_D3DRS_TWOSIDEDLIGHTING               = 141, // 0x006AF3B0 (_D3DDevice_SetRenderState_TwoSidedLighting@4)
    XBOX_D3DRS_NORMALIZENORMALS               = 142, // 0x006AF090 (_D3DDevice_SetRenderState_NormalizeNormals@4)
    XBOX_D3DRS_ZENABLE                        = 143, // 0x006AFE60 (_D3DDevice_SetRenderState_ZEnable@4)
    XBOX_D3DRS_STENCILENABLE                  = 144, // 0x006AFEF0 (_D3DDevice_SetRenderState_StencilEnable@4)
    XBOX_D3DRS_STENCILFAIL                    = 145, // 0x006AFF80 (_D3DDevice_SetRenderState_StencilFail@4, init 0x1E00 = KEEP)
    XBOX_D3DRS_FRONTFACE                      = 146, // 0x006AF050 (_D3DDevice_SetRenderState_FrontFace@4, init 0x0900 = CW)
    XBOX_D3DRS_CULLMODE                       = 147, // 0x006AEFE0 (_D3DDevice_SetRenderState_CullMode@4, init 0x0901 = CCW)
    XBOX_D3DRS_TEXTUREFACTOR                  = 148, // 0x006AF0D0 (_D3DDevice_SetRenderState_TextureFactor@4, init 0xFFFFFFFF)
    XBOX_D3DRS_ZBIAS                          = 149, // 0x006AF220 (_D3DDevice_SetRenderState_ZBias@4, init 0)
    XBOX_D3DRS_LOGICOP                        = 150, // 0x006AF2A0 (_D3DDevice_SetRenderState_LogicOp@4)
    XBOX_D3DRS_EDGEANTIALIAS                  = 151, // 0x006AEF10 (_D3DDevice_SetRenderState_EdgeAntiAlias@4)
    XBOX_D3DRS_MULTISAMPLEANTIALIAS           = 152, // 0x006B01C0 (_D3DDevice_SetRenderState_MultiSampleAntiAlias@4)
    XBOX_D3DRS_MULTISAMPLEMASK                = 153, // 0x006B0240 (_D3DDevice_SetRenderState_MultiSampleMask@4)
    XBOX_D3DRS_MULTISAMPLEMODE                = 154, // 0x006B0160 (_D3DDevice_SetRenderState_MultiSampleMode@4)
    XBOX_D3DRS_MULTISAMPLERENDERTARGETMODE    = 155, // 0x006B0190 (_D3DDevice_SetRenderState_MultiSampleRenderTargetMode@4)
    XBOX_D3DRS_SHADOWFUNC                     = 156, // 0x006AEF50 (_D3DDevice_SetRenderState_ShadowFunc@4, init 0x0200 = NEVER)
    XBOX_D3DRS_LINEWIDTH                      = 157, // 0x006AF130 (_D3DDevice_SetRenderState_LineWidth@4, init 1.0f)
    XBOX_D3DRS_SAMPLEALPHA                    = 158, // 0x006B0290 (_D3DDevice_SetRenderState_SampleAlpha@4)
    XBOX_D3DRS_DXT1NOISEENABLE                = 159, // 0x006AF1A0 (_D3DDevice_SetRenderState_Dxt1NoiseEnable@4)
    XBOX_D3DRS_YUVENABLE                      = 160, // 0x006AFFF0 (_D3DDevice_SetRenderState_YuvEnable@4)
    XBOX_D3DRS_OCCLUSIONCULLENABLE            = 161, // 0x006B0020 (_D3DDevice_SetRenderState_OcclusionCullEnable@4)
    XBOX_D3DRS_STENCILCULLENABLE              = 162, // 0x006B0090 (_D3DDevice_SetRenderState_StencilCullEnable@4)
    XBOX_D3DRS_ROPZCMPALWAYSREAD              = 163, // 0x006B0100 (_D3DDevice_SetRenderState_RopZCmpAlwaysRead@4)
    XBOX_D3DRS_ROPZREAD                       = 164, // 0x006B0120 (_D3DDevice_SetRenderState_RopZRead@4)
    XBOX_D3DRS_DONOTCULLUNCOMPRESSED          = 165, // 0x006B0140 (_D3DDevice_SetRenderState_DoNotCullUncompressed@4)

    XBOX_D3DRS_MAX                            = 166
};

// ============================================================================
// Xbox D3D8 TextureStageState Enumeration (_D3D__TextureState at 0x006C03F0)
// ============================================================================
// 4 texture stages * 32 DWORDs per stage.
// Verified against _D3DDevice_SetTextureStageStateNotInline@12 (0x006AF680)
// and g_InitialTextureStates (0x006BE950).

enum XboxTextureStageState : uint32_t {
    // --- Deferred per-stage sampler states (0..11) ---
    XBOX_D3DTSS_ADDRESSU                      =  0, // init 1 (D3DTADDRESS_WRAP)
    XBOX_D3DTSS_ADDRESSV                      =  1, // init 1 (D3DTADDRESS_WRAP)
    XBOX_D3DTSS_ADDRESSW                      =  2, // init 1 (D3DTADDRESS_WRAP)
    XBOX_D3DTSS_MAGFILTER                     =  3, // init 1 (D3DTEXF_POINT)
    XBOX_D3DTSS_MINFILTER                     =  4, // init 1 (D3DTEXF_POINT)
    XBOX_D3DTSS_MIPFILTER                     =  5, // init 0 (D3DTEXF_NONE)
    XBOX_D3DTSS_MIPMAPLODBIAS                 =  6, // init 0.0f
    XBOX_D3DTSS_MAXMIPLEVEL                   =  7, // init 0
    XBOX_D3DTSS_MAXANISOTROPY                 =  8, // init 1
    XBOX_D3DTSS_COLORKEYOP                    =  9, // init 0 (D3DTCOLORKEYOP_DISABLE)
    XBOX_D3DTSS_COLORSIGN                     = 10, // init 0
    XBOX_D3DTSS_ALPHAKILL                     = 11, // init 0 (D3DTALPHAKILL_DISABLE)

    // --- Fixed-function combiner stage operations & arguments (12..21) ---
    XBOX_D3DTSS_COLOROP                       = 12, // init 4 on Stage 0 (D3DTOP_MODULATE), 1 on Stage 1..3 (DISABLE)
    XBOX_D3DTSS_COLORARG0                     = 13, // init 1 (D3DTA_CURRENT)
    XBOX_D3DTSS_COLORARG1                     = 14, // init 2 (D3DTA_TEXTURE)
    XBOX_D3DTSS_COLORARG2                     = 15, // init 1 (D3DTA_CURRENT)
    XBOX_D3DTSS_ALPHAOP                       = 16, // init 2 on Stage 0 (D3DTOP_SELECTARG1), 1 on Stage 1..3 (DISABLE)
    XBOX_D3DTSS_ALPHAARG0                     = 17, // init 1 (D3DTA_CURRENT)
    XBOX_D3DTSS_ALPHAARG1                     = 18, // init 2 (D3DTA_TEXTURE)
    XBOX_D3DTSS_ALPHAARG2                     = 19, // init 1 (D3DTA_CURRENT)
    XBOX_D3DTSS_RESULTARG                     = 20, // init 1 (D3DTA_CURRENT)
    XBOX_D3DTSS_TEXTURETRANSFORMFLAGS         = 21, // init 0 (D3DTTFF_DISABLE)

    // --- Complex per-stage texture states (22..31) ---
    XBOX_D3DTSS_BUMPENVMAT00                  = 22, // 0x006AF590 (_D3DDevice_SetTextureState_BumpEnv@12)
    XBOX_D3DTSS_BUMPENVMAT01                  = 23, // 0x006AF590
    XBOX_D3DTSS_BUMPENVMAT11                  = 24, // 0x006AF590 (Note: Xbox orders MAT11 at 24, MAT10 at 25)
    XBOX_D3DTSS_BUMPENVMAT10                  = 25, // 0x006AF590
    XBOX_D3DTSS_BUMPENVLSCALE                 = 26, // 0x006AF590
    XBOX_D3DTSS_BUMPENVLOFFSET                = 27, // 0x006AF590
    XBOX_D3DTSS_TEXCOORDINDEX                 = 28, // 0x006AF480 (_D3DDevice_SetTextureState_TexCoordIndex@8)
    XBOX_D3DTSS_BORDERCOLOR                   = 29, // 0x006AF600 (_D3DDevice_SetTextureState_BorderColor@8)
    XBOX_D3DTSS_COLORKEYCOLOR                 = 30, // 0x006AF640 (_D3DDevice_SetTextureState_ColorKeyColor@8)
    XBOX_D3DTSS_UNSUPPORTED                   = 31,

    XBOX_D3DTSS_MAX                           = 32
};

// ============================================================================
// Xbox D3D8 Hardware Structures & Unified D3D11 Vertex / Constant Buffer Types
// ============================================================================

static constexpr uint32_t kXboxTexAddressWrap          = 1u;
static constexpr uint32_t kXboxTexAddressMirror        = 2u;
static constexpr uint32_t kXboxTexAddressClamp         = 3u;
static constexpr uint32_t kXboxTexAddressBorder        = 4u;
static constexpr uint32_t kXboxTexAddressClampToEdge   = 5u;
static constexpr uint32_t kXboxTexFilterNone           = 0u;
static constexpr uint32_t kXboxTexFilterPoint          = 1u;
static constexpr uint32_t kXboxTexFilterLinear         = 2u;
static constexpr uint32_t kXboxTexFilterAnisotropic    = 3u;
static constexpr uint32_t kXboxTexFilterQuincunx       = 4u;
static constexpr uint32_t kXboxTexFilterGaussianCubic  = 5u;

#pragma pack(push, 1)
struct XboxPixelContainer {
    uint32_t Common; // +0x00
    uint32_t Data;   // +0x04 (pointer to pixel memory)
    uint32_t Lock;   // +0x08
    uint32_t Format; // +0x0C (D3DFORMAT shift=8, dimensionality, mipmap levels, etc.)
    uint32_t Size;   // +0x10 (0 if power-of-two swizzled; non-zero for linear/rect textures)
};

struct XboxVertexBuffer {
    uint32_t Common; // +0x00
    uint32_t Data;   // +0x04 (pointer to vertex/index data)
    uint32_t Lock;   // +0x08
};

struct XboxPushBuffer {
    uint32_t Common;         // +0x00
    uint32_t Data;           // +0x04
    uint32_t Lock;           // +0x08
    uint32_t Size;           // +0x0C
    uint32_t AllocationSize; // +0x10
};

struct XboxPushBufferFixup {
    uint32_t Common; // +0x00
    uint32_t Data;   // +0x04 (pointer to fixup instruction buffer)
    uint32_t Size;   // +0x08
    uint32_t Run;    // +0x0C (byte offset into Data for current fixup run)
};

struct XboxLockedRect {
    int32_t Pitch;
    void*   pBits;
};

struct XboxLockedBox {
    int32_t RowPitch;
    int32_t SlicePitch;
    void*   pBits;
};

struct XboxBox {
    uint32_t Left;
    uint32_t Top;
    uint32_t Front;
    uint32_t Right;
    uint32_t Bottom;
    uint32_t Back;
};

struct XboxPixelShaderDef {
    uint32_t PSAlphaInputs[8];          // dwords[ 0.. 7] -> _D3D__RenderState[ 0.. 7]
    uint32_t PSFinalCombinerInputsABCD; // dwords[ 8]     -> _D3D__RenderState[ 8]
    uint32_t PSFinalCombinerInputsEFG;  // dwords[ 9]     -> _D3D__RenderState[ 9]
    uint32_t PSConstant0[8];            // dwords[10..17] -> _D3D__RenderState[10..17]
    uint32_t PSConstant1[8];            // dwords[18..25] -> _D3D__RenderState[18..25]
    uint32_t PSAlphaOutputs[8];         // dwords[26..33] -> _D3D__RenderState[26..33]
    uint32_t PSRGBInputs[8];            // dwords[34..41] -> _D3D__RenderState[34..41]
    uint32_t PSCompareMode;             // dwords[42]     -> _D3D__RenderState[42]
    uint32_t PSFinalCombinerConstant0;  // dwords[43]     -> _D3D__RenderState[43]
    uint32_t PSFinalCombinerConstant1;  // dwords[44]     -> _D3D__RenderState[44]
    uint32_t PSRGBOutputs[8];           // dwords[45..52] -> _D3D__RenderState[45..52]
    uint32_t PSCombinerCount;           // dwords[53]     -> _D3D__RenderState[53]
    uint32_t PSTextureModes;            // dwords[54]     -> _D3D__RenderState[136]
    uint32_t PSDotMapping;              // dwords[55]     -> _D3D__RenderState[55]
    uint32_t PSInputTexture;            // dwords[56]     -> _D3D__RenderState[56]
    uint32_t PSC0Mapping;               // dwords[57]
    uint32_t PSC1Mapping;               // dwords[58]
    uint32_t PSFinalCombinerConstants;  // dwords[59]
};
static_assert(sizeof(XboxPixelShaderDef) == 240, "XboxPixelShaderDef must be 240 bytes");

struct XboxViewport {
    uint32_t X;
    uint32_t Y;
    uint32_t Width;
    uint32_t Height;
    float    MinZ;
    float    MaxZ;
};

struct Mat44 {
    float m[4][4];
};

struct XboxMaterial8 {
    float Diffuse[4];
    float Ambient[4];
    float Specular[4];
    float Emissive[4];
    float Power;
};

struct XboxLight8 {
    uint32_t Type; // 1=POINT, 2=SPOT, 3=DIRECTIONAL
    float    Diffuse[4];
    float    Specular[4];
    float    Ambient[4];
    float    Position[3];
    float    Direction[3];
    float    Range;
    float    Falloff;
    float    Attenuation0;
    float    Attenuation1;
    float    Attenuation2;
    float    Theta;
    float    Phi;
};

struct UnifiedVertex {
    float x, y, z, w;
    float nx, ny, nz;
    float r, g, b, a;
    float u0, v0;
    float u1, v1;
    float u2, v2;
    float u3, v3;
    float w0, w1, w2, w3; // 3rd texture coordinate (W / R) for 3D volume & cubemap textures
    float sr, sg, sb, sa; // Specular color (oD1 / D3DFVF_SPECULAR / fixed-function specular)
};

struct alignas(16) PerDrawCBData {
    float    wvpRow0[4];
    float    wvpRow1[4];
    float    wvpRow2[4];
    float    wvpRow3[4];
    float    viewport[4];   // x, y, width, height
    float    tfactor[4];    // r, g, b, a
    uint32_t stage0Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage0Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t stage1Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage1Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t stage2Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage2Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t stage3Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage3Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t flags[4];      // x=isRhw|(useGammaLut<<1)|(scissorExcl<<2)|(scissorCnt<<4), y=tex|cube|proj|vol|akill|resTemp, z=alphaTestEnable, w=psMode
    float    alphaTest[4];  // x=alphaFunc (1..8), y=alphaRef (0..1), z=gamma, w=brightness
    float    uvScale01[4];  // x=uScale0, y=vScale0, z=uScale1, w=vScale1 (for linear textures)
    float    uvScale23[4];  // x=uScale2, y=vScale2, z=uScale3, w=vScale3 (for linear textures)
    float    bumpParams[4]; // x=mat00, y=mat01, z=mat10, w=mat11
    float    fogParams[4];  // x=fogMode(0=off,1=exp,2=exp2,3=linear,4=vertexFog), y=fogStart, z=fogEnd, w=fogDensity
    float    fogColor[4];   // r, g, b, isAdditiveBlend
    float    psConst0[4];   // c0
    float    psConst1[4];   // c1
    float    psConst2[4];   // c2
    float    psConst3[4];   // c3
    float    psConst4[4];   // c4
    float    psConst5[4];   // c5
    float    psConst6[4];   // c6
    float    psConst7[4];   // c7
    float    worldRow0[4];  // g_matWorld row 0
    float    worldRow1[4];  // g_matWorld row 1
    float    worldRow2[4];  // g_matWorld row 2
    float    worldRow3[4];  // g_matWorld row 3
    uint32_t lightCount[4]; // x=numLights (0..8), y=gpuLightEnable (0/1), z=lightFlags(bit0=spec,bit1=localViewer,bit2=twoSided,bit3=normNormals), w=0
    float    lightPosType[8][4];    // xyz=Position (Point/Spot) or -Direction (Dir), w=Type (1=Point,2=Spot,3=Dir)
    float    lightDirRange[8][4];   // xyz=Direction (Spot), w=Range
    float    lightAttenSpot[8][4];  // x=Atten0, y=Atten1, z=Atten2, w=cos(Phi*0.5)
    float    lightSpotParams[8][4]; // x=cos(Theta*0.5), y=Falloff, z=0, w=0
    float    lightDiffuse[8][4];    // rgb=Light.Diffuse * Mat.Diffuse, w=BackLight.Diffuse.r
    float    lightAmbient[8][4];    // rgb=Light.Ambient * Mat.Ambient, w=BackLight.Diffuse.g
    float    lightSpecular[8][4];   // rgb=Light.Specular * Mat.Specular, w=Mat.Power
    float    eyePosWorld[4];        // xyz=Camera world position, w=BackLight.Diffuse.b
    float    matBackDiffAmb[4];     // xyz=Back Material base (Emissive + SceneAmb * BackAmb), w=Back alpha
    float    matBackSpec[4];        // xyz=Back Material Specular, w=Back Material Power
    uint32_t stageColorKeyOp[4];    // Stage 0..3 D3DTSS_COLORKEYOP (0=Disable, 1=Alpha, 2=RGBA, 3=Kill)
    float    stageColorKeyColor[4][4]; // Stage 0..3 unpacked D3DTSS_COLORKEYCOLOR (r, g, b, a)
    uint32_t stageColorSign[4];     // Stage 0..3 D3DTSS_COLORSIGN bitmask (bit5=B, bit6=G, bit7=R, bit8=A)
    float    bumpLuminance[4];      // x=stage0 LScale, y=stage0 LOffset, z=stage1 LScale, w=stage1 LOffset
    float    scissorRects[8][4];    // Up to 8 scissor rects (left, top, right, bottom)
    float    rcStageC0[8][4];       // NV2A Register Combiner per-stage PSConstant0[0..7]
    float    rcStageC1[8][4];       // NV2A Register Combiner per-stage PSConstant1[0..7]
    float    rcFinalConst[2][4];    // NV2A Final Combiner PSFinalCombinerConstant0..1
};
#pragma pack(pop)

struct ID3D11VertexShader;
struct ID3D11InputLayout;

struct XboxShaderDeclInfo {
    bool                hasPos;
    bool                hasRhw;
    bool                hasNorm;
    bool                hasDiff;
    bool                hasSpec;
    bool                hasUv0;
    bool                hasUv1;
    bool                hasUv2;
    bool                hasUv3;
    uint16_t            offPos;
    uint16_t            offRhw;
    uint16_t            offNorm;
    uint16_t            offDiff;
    uint16_t            offSpec;
    uint16_t            offUv0;
    uint16_t            offUv1;
    uint16_t            offUv2;
    uint16_t            offUv3;
    uint8_t             dimUv[4];
    int32_t             vsType; // 0..65 if from aVertexShaders[0..65] at 0x007D0BE0, else -1
    bool                hasStream1Diff;
    uint16_t            offStream1Diff;
    bool                isCompiledXvu;
    bool                writesOFog;
    ID3D11VertexShader* gpuVS;
    ID3D11InputLayout*  gpuInputLayout;
    uint16_t            streamStrides[2];
    bool                usesStream[2];
};

// ============================================================================
// Shared D3D11 / Xbox D3D8 Subsystem State & Cross-Module Declarations
// ============================================================================

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11Texture2D;
struct ID3D11Texture3D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11Buffer;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11SamplerState;

// d3d11_backend.cpp
extern ID3D11Device*             g_d3dDevice;
extern ID3D11DeviceContext*      g_d3dCtx;
extern IDXGISwapChain*           g_swapChain;
extern ID3D11Texture2D*          g_backBufferTex;
extern ID3D11Texture2D*          g_backBufferCopyTex;
extern ID3D11ShaderResourceView* g_backBufferCopySRV;
extern bool                      g_backBufferDirty;
extern ID3D11RenderTargetView*   g_rtv;
extern ID3D11Texture2D*          g_depthTex;
extern ID3D11DepthStencilView*   g_dsv;
extern ID3D11RenderTargetView*   g_curActiveRTV;
extern ID3D11DepthStencilView*   g_curActiveDSV;
extern void*                     g_curOffscreenRT;
extern float                     g_gamma;
extern float                     g_brightness;
extern ID3D11ShaderResourceView* g_gammaLutSRV;
extern bool                      g_hasGammaRamp;
extern bool                      g_scissorEnabled;
extern uint32_t                  g_scissorCount;
extern bool                      g_scissorExclusive;
extern RECT                      g_scissorRects[8];
extern ID3D11VertexShader*       g_vs;
extern ID3D11PixelShader*        g_ps;
extern ID3D11InputLayout*        g_inputLayout;
extern ID3D11Buffer*             g_cbPerDraw;
extern ID3D11Buffer*             g_cbVSConstants;
extern ID3D11Buffer*             g_dynVB;
extern uint32_t                  g_dynVBCapacity;

uint32_t MapXboxCmpFuncInt(uint32_t v);
ID3D11BlendState* GetBlendState(uint32_t alphaBlendEnable, uint32_t srcBlend, uint32_t dstBlend, uint32_t blendOp, uint32_t colorWrite);
ID3D11DepthStencilState* GetDepthState(
    uint32_t zEnable, uint32_t zWriteEnable, uint32_t zFunc,
    uint32_t stencilEnable, uint32_t stencilFail, uint32_t stencilZFail, uint32_t stencilPass,
    uint32_t stencilFunc, uint32_t stencilReadMask, uint32_t stencilWriteMask);
ID3D11RasterizerState* GetRastState(uint32_t cullMode, uint32_t fillMode, uint32_t zBias, bool scissor, uint32_t frontFace = 0x0900u);
ID3D11SamplerState* GetSamplerState(
    uint32_t addrU, uint32_t addrV, uint32_t magFilter, uint32_t borderColor,
    uint32_t minFilter = 0xFFFFFFFFu, uint32_t mipFilter = kXboxTexFilterNone,
    uint32_t maxAniso = 1u, uint32_t maxMipLevel = 0u,
    uint32_t mipmapLodBiasBits = 0u, uint32_t addrW = kXboxTexAddressWrap);
ID3D11SamplerState* GetStageSamplerState(uint32_t stage);
ID3D11PixelShader* GetPixelShaderPermutation(uint32_t psMode);
void DrawOsdOverlayOnBackbuffer();
void FlushXboxOverlayToBackbuffer();

// d3d8_texture.cpp
extern XboxPixelContainer* g_boundTextures[4];
void DecodeXboxTextureFormat(
    const XboxPixelContainer* pTex,
    uint32_t* outWidth, uint32_t* outHeight, uint32_t* outLevels,
    uint32_t* outXboxFmt, bool* outSwizzled, uint32_t* outLinearPitch);
ID3D11ShaderResourceView* GetOrCreateD3D11SRV(
    XboxPixelContainer* pTex, float* outUScale = nullptr, float* outVScale = nullptr,
    bool* outIsCube = nullptr, bool* outIsVolume = nullptr);
void __cdecl Bridge_rlMain_SetRenderTarget(void* pTex, int bClear, int bFast);
void InitXboxD3D8TextureBridges();

// d3d8_shader.cpp
extern uint32_t        g_currentVsHandle;
extern uint32_t        g_currentPsHandle;
extern int32_t         g_currentPsType;
extern float           g_psConstants[8][4];
extern const uint32_t* g_curPrelitColors;
extern uint32_t        g_curPrelitBaseVertex;
extern bool            g_curPrelitUseCustomBase;

bool GetXboxVertexShaderDecl(uint32_t handle, XboxShaderDeclInfo* outDecl);
ID3D11PixelShader* GetActiveRegisterCombinerPS();
void PopulateRegisterCombinerConstants(PerDrawCBData* cb);
void __stdcall Bridge_D3DDevice_SetVertexShader(uint32_t Handle);
void InitXboxD3D8ShaderBridges();

// d3d8_draw.cpp
extern XboxViewport g_viewport;
void UnpackD3DColor(uint32_t c, float* r, float* g, float* b, float* a);
void __stdcall Bridge_D3DDevice_SetStreamSource(uint32_t StreamNumber, XboxVertexBuffer* pStreamData, uint32_t Stride);
void __stdcall Bridge_D3DDevice_SetIndices(XboxVertexBuffer* pIndexData, uint32_t BaseVertexIndex);
void __stdcall Bridge_D3DDevice_SetTransform(uint32_t State, const Mat44* pMatrix);
void __stdcall Bridge_D3DDevice_SetViewport(const XboxViewport* pViewport);
void InitXboxD3D8DrawBridges();
