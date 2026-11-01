// ============================================================================
// Post-build PE verification and patching tool for GhostWin11.exe
// ============================================================================
// Uses official Windows SDK PE structures (IMAGE_DOS_HEADER, IMAGE_NT_HEADERS32,
// IMAGE_SECTION_HEADER, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG) with zero magic offsets:
// 1. Verifies that ImageBase is 0x00400000 and the first PE section is '.xbox'
//    at RVA 0x00001000 (VA 0x00401000..0x00950FFF) before host '.text'.
// 2. Clears DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG] in the PE header
//    so Windows' user-mode exception dispatcher does not reject Xbox SEH frames.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr DWORD kExpectedImageBase = 0x00400000u;
static constexpr DWORD kExpectedXboxRva   = 0x00001000u;
static constexpr DWORD kExpectedXboxVSize = 0x00550000u;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: post_build_pe <path_to_GhostWin11.exe>\n");
        return 1;
    }

    const char* exePath = argv[1];
    FILE* fp = std::fopen(exePath, "rb");
    if (!fp) {
        std::fprintf(stderr, "[PostBuild] Failed to open file for reading: %s\n", exePath);
        return 1;
    }

    std::fseek(fp, 0, SEEK_END);
    long fileSize = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (fileSize < static_cast<long>(sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS32))) {
        std::fclose(fp);
        std::fprintf(stderr, "[PostBuild] File too small to be a valid PE image: %s\n", exePath);
        return 1;
    }

    std::vector<BYTE> data(static_cast<size_t>(fileSize));
    if (std::fread(data.data(), 1, data.size(), fp) != data.size()) {
        std::fclose(fp);
        std::fprintf(stderr, "[PostBuild] Failed to read PE image: %s\n", exePath);
        return 1;
    }
    std::fclose(fp);

    auto* dosHdr = reinterpret_cast<IMAGE_DOS_HEADER*>(data.data());
    if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE ||
        dosHdr->e_lfanew <= 0 ||
        static_cast<size_t>(dosHdr->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > data.size()) {
        std::fprintf(stderr, "[PostBuild] Invalid DOS/PE header offset in: %s\n", exePath);
        return 1;
    }

    auto* ntHdr = reinterpret_cast<IMAGE_NT_HEADERS32*>(data.data() + dosHdr->e_lfanew);
    if (ntHdr->Signature != IMAGE_NT_SIGNATURE ||
        ntHdr->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        ntHdr->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        std::fprintf(stderr, "[PostBuild] Invalid 32-bit x86 PE image: %s\n", exePath);
        return 1;
    }

    DWORD imageBase = ntHdr->OptionalHeader.ImageBase;
    if (imageBase != kExpectedImageBase) {
        std::fprintf(stderr, "[PostBuild] Expected ImageBase 0x%08lX, got 0x%08lX\n",
            static_cast<unsigned long>(kExpectedImageBase),
            static_cast<unsigned long>(imageBase));
        return 1;
    }

    if (ntHdr->FileHeader.NumberOfSections < 1) {
        std::fprintf(stderr, "[PostBuild] PE image has no sections: %s\n", exePath);
        return 1;
    }

    IMAGE_SECTION_HEADER* firstSec = IMAGE_FIRST_SECTION(ntHdr);
    char secName[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
    std::memcpy(secName, firstSec->Name, IMAGE_SIZEOF_SHORT_NAME);

    DWORD firstSecRva   = firstSec->VirtualAddress;
    DWORD firstSecVSize = firstSec->Misc.VirtualSize;
    if (std::strcmp(secName, ".xbox") != 0 ||
        firstSecRva != kExpectedXboxRva ||
        firstSecVSize < kExpectedXboxVSize) {
        std::fprintf(stderr,
            "[PostBuild] First PE section must be '.xbox' at RVA 0x%lX (size >= 0x%lX), "
            "got '%s' at RVA 0x%lX (size 0x%lX)\n",
            static_cast<unsigned long>(kExpectedXboxRva),
            static_cast<unsigned long>(kExpectedXboxVSize),
            secName,
            static_cast<unsigned long>(firstSecRva),
            static_cast<unsigned long>(firstSecVSize));
        return 1;
    }

    // Clear IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG in OptionalHeader.DataDirectory
    if (ntHdr->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG) {
        ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress = 0;
        ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size           = 0;
    }

    fp = std::fopen(exePath, "wb");
    if (!fp) {
        std::fprintf(stderr, "[PostBuild] Failed to open file for writing: %s\n", exePath);
        return 1;
    }
    if (std::fwrite(data.data(), 1, data.size(), fp) != data.size()) {
        std::fclose(fp);
        std::fprintf(stderr, "[PostBuild] Failed to write patched PE image: %s\n", exePath);
        return 1;
    }
    std::fclose(fp);

    std::printf(
        "[PostBuild] Verified %s: ImageBase=0x%08lX, .xbox=0x%08lX..0x%08lX, LoadConfig cleared.\n",
        exePath,
        static_cast<unsigned long>(imageBase),
        static_cast<unsigned long>(imageBase + firstSecRva),
        static_cast<unsigned long>(imageBase + firstSecRva + firstSecVSize));
    return 0;
}
