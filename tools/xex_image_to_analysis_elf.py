#!/usr/bin/env python3
"""Wrap an extracted Xbox 360 PE image in a synthetic ELF for Ghidra/REA.

This does not extract or modify an XEX. The input must already be the loaded
PE image (sections laid out at their virtual addresses). The ELF is only an
analysis carrier: it preserves the image bytes and addresses, but is not a
valid replacement game executable.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ELF_LOAD_OFFSET = 0x1000
ELF32_HEADER_SIZE = 52
ELF32_PROGRAM_HEADER_SIZE = 32
ELF32_SECTION_HEADER_SIZE = 40
IMAGE_FILE_MACHINE_XBOX360 = 0x01F2
IMAGE_NT_OPTIONAL_HDR32_MAGIC = 0x010B


def checked_slice(data: bytes, start: int, size: int, what: str) -> bytes:
    if start < 0 or size < 0 or start + size > len(data):
        raise ValueError(f"{what} lies outside the extracted image")
    return data[start : start + size]


def make_analysis_elf(image: bytes) -> tuple[bytes, int, int, list[tuple[str, int, int]]]:
    checked_slice(image, 0, 0x40, "DOS header")
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if checked_slice(image, pe_offset, 4, "PE signature") != b"PE\0\0":
        raise ValueError("input is not an extracted PE image")

    coff_offset = pe_offset + 4
    coff = checked_slice(image, coff_offset, 20, "COFF header")
    machine, section_count, _, _, _, optional_size, _ = struct.unpack("<HHIIIHH", coff)
    if machine != IMAGE_FILE_MACHINE_XBOX360:
        raise ValueError(f"expected Xbox 360 PowerPC machine 0x01f2, got 0x{machine:04x}")

    optional_offset = coff_offset + 20
    optional = checked_slice(image, optional_offset, optional_size, "optional header")
    if len(optional) < 64 or struct.unpack_from("<H", optional)[0] != IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        raise ValueError("expected a PE32 optional header")
    entry_rva = struct.unpack_from("<I", optional, 16)[0]
    image_base = struct.unpack_from("<I", optional, 28)[0]
    headers_size = struct.unpack_from("<I", optional, 60)[0]
    if not 0 < headers_size <= len(image):
        raise ValueError("invalid PE SizeOfHeaders for the extracted image")

    section_table = optional_offset + optional_size
    checked_slice(image, section_table, section_count * 40, "section table")
    sections: list[tuple[str, int, int, int, int]] = []
    for index in range(section_count):
        offset = section_table + index * 40
        name_bytes = checked_slice(image, offset, 8, "section name").split(b"\0", 1)[0]
        try:
            name = name_bytes.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("PE section names must be ASCII") from exc
        virtual_size, virtual_address = struct.unpack_from("<II", image, offset + 8)
        characteristics = struct.unpack_from("<I", image, offset + 36)[0]
        if virtual_size == 0:
            continue
        checked_slice(image, virtual_address, virtual_size, f"section {name}")
        address = image_base + virtual_address
        if address + virtual_size > 0x1_0000_0000:
            raise ValueError(f"section {name} exceeds the 32-bit PowerPC address space")

        # ELF section flags: SHF_WRITE=1, SHF_ALLOC=2, SHF_EXECINSTR=4.
        sh_flags = 2
        if characteristics & 0x80000000:
            sh_flags |= 1
        if characteristics & 0x20000000:
            sh_flags |= 4
        sections.append((name, address, virtual_address, virtual_size, sh_flags))

    executable_sections = [section for section in sections if section[4] & 4]
    if not executable_sections:
        raise ValueError("no executable PE section was found")
    entry = image_base + entry_rva
    if not any(address <= entry < address + size for _, address, _, size, _ in executable_sections):
        raise ValueError("PE entry point is outside all executable sections")

    # Embed the loaded image verbatim after the ELF headers. Section offsets
    # map directly to their PE RVAs in this preserved image.
    file_bytes = bytearray(ELF_LOAD_OFFSET + len(image))
    file_bytes[ELF_LOAD_OFFSET:] = image
    program_segments: list[tuple[int, int, int, int]] = [
        (image_base, 0, headers_size, 4),  # PT_LOAD headers, read-only
    ]
    for _, address, rva, size, sh_flags in sections:
        p_flags = 4 | (2 if sh_flags & 1 else 0) | (1 if sh_flags & 4 else 0)
        program_segments.append((address, rva, size, p_flags))

    section_names = bytearray(b"\0")
    section_name_offsets: list[int] = []
    for name, *_ in sections:
        section_name_offsets.append(len(section_names))
        section_names.extend(name.encode("ascii") + b"\0")
    shstrtab_name_offset = len(section_names)
    section_names.extend(b".shstrtab\0")

    section_header_count = 1 + len(sections) + 1
    shstrtab_index = section_header_count - 1
    shoff = (len(file_bytes) + 3) & ~3
    file_bytes.extend(bytes(shoff - len(file_bytes)))
    shstrtab_offset = shoff + section_header_count * ELF32_SECTION_HEADER_SIZE

    section_headers = [bytes(ELF32_SECTION_HEADER_SIZE)]
    for (_, address, rva, size, sh_flags), name_offset in zip(sections, section_name_offsets):
        section_headers.append(
            struct.pack(
                ">IIIIIIIIII",
                name_offset,
                1,  # SHT_PROGBITS
                sh_flags,
                address,
                ELF_LOAD_OFFSET + rva,
                size,
                0,
                0,
                16,
                0,
            )
        )
    section_headers.append(
        struct.pack(
            ">IIIIIIIIII",
            shstrtab_name_offset,
            3,  # SHT_STRTAB
            0,
            0,
            shstrtab_offset,
            len(section_names),
            0,
            0,
            1,
            0,
        )
    )
    for header in section_headers:
        file_bytes.extend(header)
    file_bytes.extend(section_names)

    ident = b"\x7fELF" + bytes((1, 2, 1, 0, 0)) + bytes(7)  # ELF32, big-endian
    elf_header = struct.pack(
        ">16sHHIIIIIHHHHHH",
        ident,
        2,  # ET_EXEC
        20,  # EM_PPC
        1,
        entry,
        ELF32_HEADER_SIZE,
        shoff,
        0,  # generic PowerPC e_flags
        ELF32_HEADER_SIZE,
        ELF32_PROGRAM_HEADER_SIZE,
        len(program_segments),
        ELF32_SECTION_HEADER_SIZE,
        section_header_count,
        shstrtab_index,
    )
    file_bytes[:ELF32_HEADER_SIZE] = elf_header
    for index, (address, rva, size, p_flags) in enumerate(program_segments):
        program_header = struct.pack(
            ">IIIIIIII",
            1,  # PT_LOAD
            ELF_LOAD_OFFSET + rva,
            address,
            address,
            size,
            size,
            p_flags,
            0x1000,
        )
        start = ELF32_HEADER_SIZE + index * ELF32_PROGRAM_HEADER_SIZE
        file_bytes[start : start + ELF32_PROGRAM_HEADER_SIZE] = program_header

    summary = [(name, address, size) for name, address, _, size, _ in sections]
    return bytes(file_bytes), image_base, entry, summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="extracted PE image laid out at virtual addresses")
    parser.add_argument("output", type=Path, help="synthetic ELF32 big-endian PowerPC analysis carrier")
    args = parser.parse_args()

    if args.input.resolve() == args.output.resolve():
        parser.error("input and output must be different files")
    image = args.input.read_bytes()
    try:
        elf, image_base, entry, sections = make_analysis_elf(image)
    except ValueError as exc:
        parser.error(str(exc))
    args.output.write_bytes(elf)

    print(f"input={args.input} bytes=0x{len(image):x}")
    print(f"output={args.output} bytes=0x{len(elf):x} synthetic=ELF32-BE EM_PPC e_flags=0")
    print(f"image_base=0x{image_base:08x} entry=0x{entry:08x} sections={len(sections)}")
    for name, address, size in sections:
        print(f"{name:8s} 0x{address:08x}-0x{address + size:08x} size=0x{size:x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
