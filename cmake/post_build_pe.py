#!/usr/bin/env python3
"""
Post-build PE verification and patching for GhostWin11.exe:
1. Verifies that ImageBase is 0x00400000 and the '.xbox' reserved section is
   placed at RVA 0x00001000 (VA 0x00401000..0x00950FFF) before host '.text'.
2. Clears IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG (DataDirectory[10]) in the PE header
   so Windows' user-mode exception dispatcher does not reject Xbox SEH frames.
"""

import struct
import sys


def patch_and_verify_pe(exe_path: str) -> None:
    with open(exe_path, "rb") as f:
        data = bytearray(f.read())

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    pe_sig, machine, _ = struct.unpack_from("<IHH", data, e_lfanew)
    if pe_sig != 0x00004550 or machine != 0x014C:
        raise RuntimeError(f"Invalid 32-bit x86 PE image: {exe_path}")

    size_of_opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt_offset = e_lfanew + 24
    image_base = struct.unpack_from("<I", data, opt_offset + 28)[0]
    if image_base != 0x00400000:
        raise RuntimeError(f"Expected ImageBase 0x00400000, got 0x{image_base:08X}")

    # Verify '.xbox' section placement at RVA 0x00001000
    sec_offset = opt_offset + size_of_opt
    first_sec_name = bytes(data[sec_offset : sec_offset + 8]).rstrip(b"\x00").decode("ascii", errors="replace")
    first_sec_vsize, first_sec_rva = struct.unpack_from("<II", data, sec_offset + 8)
    if first_sec_name != ".xbox" or first_sec_rva != 0x00001000 or first_sec_vsize < 0x00550000:
        raise RuntimeError(
            f"First PE section must be '.xbox' at RVA 0x1000 (size >= 0x550000), "
            f"got '{first_sec_name}' at RVA 0x{first_sec_rva:X} (size 0x{first_sec_vsize:X})"
        )

    # Clear IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG (DataDirectory[10] at opt_offset + 96 + 10 * 8)
    load_config_dir_offset = opt_offset + 96 + 10 * 8
    struct.pack_into("<II", data, load_config_dir_offset, 0, 0)

    with open(exe_path, "wb") as f:
        f.write(data)

    print(
        f"[PostBuild] Verified {exe_path}: ImageBase=0x{image_base:08X}, "
        f".xbox=0x{image_base + first_sec_rva:08X}..0x{image_base + first_sec_rva + first_sec_vsize:08X}, "
        f"LoadConfig cleared."
    )


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: post_build_pe.py <path_to_GhostWin11.exe>", file=sys.stderr)
        sys.exit(1)
    patch_and_verify_pe(sys.argv[1])
