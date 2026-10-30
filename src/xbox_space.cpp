// ============================================================================
// Reserved Virtual Address Space for Original Xbox Ghost.exe (0x00401000..0x00950FFF)
// ============================================================================
// By placing this 0x00550000-byte '.xbox' code section in the first translation unit
// linked into GhostWin11.exe (with /BASE:0x00400000), the MSVC linker reserves
// virtual addresses 0x00401000..0x00950FFF for Ghost.exe's 19 PE sections and pushes
// all host compatibility runtime code (.text, .rdata, .data) above 0x00951000.

#pragma code_seg(".xbox")
extern "C" __declspec(allocate(".xbox")) unsigned char g_XboxReservedSpace[0x00550000] = { 0x90 };
