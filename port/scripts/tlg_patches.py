#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Last Guardian frame-rate and render-resolution patches, written to out/patches.bin.

Each patch is a list of {image offset, expected bytes, new bytes} for eboot 1.03 only (sha256
below, or a SELF dump of it): the expected bytes are checked in eboot.bin before anything is
written, so another build never gets byte writes in the wrong place. The loader (probe.c
apply_patches) applies the file after relocation. Image offsets are eboot virtual addresses;
the Ghidra addresses in the comments (base 0x1000000) are offset + 0x1000000. Research:
docs/60fps.md.

30 FPS keeps the game's frame rate and applies the checked cloth/material null guard.
Native 1080p does not need a resolution patch.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import sys

EBOOT_103_SHA256 = '91dce685be81f8f6bc88097d4fc7a62e97ca08d109621c57fe9371ae125feb3c'
SELF_MAGIC = b'O\x15=\x1d'
TEXT_FILE_OFFSET = 0x4000      # PT_LOAD 0 (R E): file offset of vaddr 0
TEXT_SIZE = 0x16b2d5c
GHIDRA_BASE = 0x1000000
# ELF program-header types whose file bytes are part of the loaded eboot image.
LOADED_SEGMENTS = (1, 0x61000010)  # PT_LOAD, PT_SCE_RELRO


def file_offset(eboot, image_offset, length):
    """Map an image-relative virtual address to its file offset through a loaded ELF segment."""
    if (eboot[:4] != b'\x7fELF' or len(eboot) < 0x40 or
            eboot[4] != 2 or eboot[5] != 1 or image_offset < 0 or length < 0):
        return None
    header_size = struct.calcsize('<IIQQQQQQ')
    try:
        phoff, = struct.unpack_from('<Q', eboot, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', eboot, 0x36)
        if phentsize < header_size or phnum == 0 or phoff + phentsize * phnum > len(eboot):
            return None
        image_end = image_offset + length
        for i in range(phnum):
            pos = phoff + i * phentsize
            kind, _, p_offset, vaddr, _, filesz, _, _ = struct.unpack_from(
                '<IIQQQQQQ', eboot, pos)
            if (kind in LOADED_SEGMENTS and vaddr <= image_offset and
                    image_end <= vaddr + filesz):
                start = p_offset + image_offset - vaddr
                end = start + length
                if start >= 0 and end <= len(eboot) and end <= p_offset + filesz:
                    return start
    except struct.error:
        return None
    return None


def checked_offset(eboot, address, expected):
    """Validate bytes at a Ghidra address and return its image-relative patch offset."""
    offset = address - GHIDRA_BASE
    start = file_offset(eboot, offset, len(expected))
    if start is None:
        raise ValueError(f'{address:#x} is not in a loaded eboot segment')
    found = eboot[start:start + len(expected)]
    if found != expected:
        raise ValueError(f'{address:#x} has {found.hex()}, expected {expected.hex()}')
    return offset
# Game GPU heap (Gomalloc), shared by render targets and stage allocations. 4064 MiB leaves
# 32 MiB below the 4 GiB cap for allocator rounding.
GARLIC_SIZE = 0x1dd3a46
GARLIC_SIZE_EXPECTED = bytes.fromhex('41bf000000b0')
GARLIC_EXTRA_MB = 1248


def f32(value):
    return struct.pack('<f', value)


def rip_disp(instruction_end, target):
    """disp32 of a rip-relative operand: target - address of the next instruction (Ghidra addresses)."""
    return struct.pack('<i', target - instruction_end)


def res_bytes(width, height):
    """Four resolution fields and zero flags in one 0x18-byte mode-table entry."""
    return struct.pack('<4HQ', width, height, width, height, 0)


RES_PRO_ENTRY = bytes.fromhex('90066207200d62070100000000000000')
RES_NOP_ENTRY = bytes.fromhex('0f856701000081bc2424010000380400000f8656010000')


# Patches by name: [(ghidra address, expected bytes, new bytes, what it does)].
PATCHES = {
    # JsDevice/JsSwapChain init (0x1c4f9f0): sceVideoOutSetFlipRate(handle, 1) -> 0.
    # Rate 1 shows a frame every second vblank (30 Hz on the 60 Hz vblank); 0 every vblank.
    'flip_rate_60': [
        (0x1c5151a, bytes.fromhex('be01000000'), bytes.fromhex('be00000000'),
         'sceVideoOutSetFlipRate(h, 1) -> (h, 0)'),
    ],
    # Lua SetMaxFps(fps) binding (0x12091a0): game+0x180 = 1.0/fps, the engine's minimum frame
    # time. The scripts call SetMaxFps(30) (states/main.lua, common/gamesetuptbl.lua).
    # 60: load 0.5 (0x24f17e8) instead of 1.0 (0x24eaf84) -> 0.5/fps: SetMaxFps(30) means 60.
    'max_fps_double': [
        (0x12091db, bytes.fromhex('c5fa100d') + rip_disp(0x12091e3, 0x24eaf84),
         bytes.fromhex('c5fa100d') + rip_disp(0x12091e3, 0x24f17e8),
         'SetMaxFps(n): minimum frame time 1/n -> 0.5/n'),
    ],
    # Unlocked: vdivss xmm1,xmm1,xmm0 -> vxorps xmm1,xmm1,xmm1: minimum frame time 0 (no limit),
    # as SetMaxFps(0) does.
    'max_fps_off': [
        (0x12091e3, bytes.fromhex('c5f25ec8'), bytes.fromhex('c5f057c9'),
         'SetMaxFps(n): minimum frame time 1/n -> 0'),
    ],
    # SmoothDeltaTime (0x1d16480): a frame time in (0.031667, 0.035) s becomes exactly 1/30 s
    # before the 2nd-order filter and the 1/360 s rounding. These three constants are read only
    # here. 60: the same snap one octave up, (0.0158333, 0.0175) -> 1/60.
    'dt_snap_60': [
        (0x24f17f8, f32(0.035), f32(0.0175), 'dt snap upper bound 0.035 -> 0.0175'),
        (0x24f17fc, f32(1 / 30), f32(1 / 60), 'dt snap target 1/30 -> 1/60'),
        (0x24f1800, f32(0.0316666663), f32(0.0158333331), 'dt snap lower bound 0.031667 -> 0.015833'),
    ],
    # Material/cloth update null pointer guard (0x128c100, loop header at 0x128c161..0x128c1a4):
    # At 60+ FPS on fast multi-core CPUs, worker threads can run 0x128c100 before streaming
    # populates the pointer table at [r12 + 0x28] (fault at 0x28c193) or an element entry at
    # [rcx + rax*8] (fault at 0x28c197 reading [rax + 0x28] right before test r15, r15).
    # A null element leaves r15 = 0 (mov r15, rax before the test), as an element without a
    # [rax + 0x28] does: the loop tests r15 again at 0x28c3aa and reads [r15 + 0x210] otherwise.
    'cloth_null_guard': [
        (0x128c161,
         bytes.fromhex(
             '6666666666662e0f1f840000000000'
             '4d8b7424484863c7498b4c242841bb00000000488d1c4048c1e306'
             '4963841ea0000000488b04c14c8b7828488945904d85ff7479'
         ),
         bytes.fromhex(
             '49837c2428000f84f90400000f1f00'
             '4d8b742448498b4c24284531db69dfc0000000'
             '4963841ea0000000488b04c148894590'
             '4989c74885c074074c8b78284d85ff7479'
         ),
         'cloth update null table/element guard at 0x28c161..0x28c1a4'),
    ],
    # Mode table (0x18 bytes per entry) at 0x26ec3e8: use WxH, WxH and no checkerboard.
    # Disable the output-height >1080 branch so the same Pro list works at each render size.
    'res_nop': [
        (0x1c4fa6a, RES_NOP_ENTRY, b'\x90' * 23,
         'CreateDevice: always use the Pro mode list'),
    ],
    'res_360p': [
        (0x26ec3e8, RES_PRO_ENTRY, res_bytes(640, 360), 'default mode -> 640x360'),
    ],
    'res_720p': [
        (0x26ec3e8, RES_PRO_ENTRY, res_bytes(1280, 720), 'default mode -> 1280x720'),
    ],
    'res_1440p': [
        (0x26ec3e8, RES_PRO_ENTRY, res_bytes(2560, 1440), 'default mode -> 2560x1440'),
    ],
    'res_4k': [
        (0x26ec3e8, RES_PRO_ENTRY, res_bytes(3840, 2160), 'default mode -> 3840x2160'),
    ],
}

MODES = {
    '30': ['cloth_null_guard'],
    # max_fps_double/max_fps_off are left out: 60 and unlocked were reached without them
    # (game+0x180 is no frame limiter in the tested scenes; its reader is not found yet).
    # TLG_FPS_PATCHES=flip_rate_60,max_fps_off tries them.
    '60': ['flip_rate_60', 'dt_snap_60', 'cloth_null_guard'],
    'unlocked': ['flip_rate_60', 'cloth_null_guard'],
}
RESOLUTIONS = {
    '1080p': [],
    '360p': ['res_nop', 'res_360p'],
    '720p': ['res_nop', 'res_720p'],
    '1440p': ['res_nop', 'res_1440p'],
    '4k': ['res_nop', 'res_4k'],
}
# The engine rate each mode presents at most (TLG_ENGINE_FPS: the overlay's frame limiter and
# tlg_graphics.h PresentLimit). Unlocked is paced by the present thread (TLG_VBLANK_HZ=0).
ENGINE_FPS = {'30': 30, '60': 60, 'unlocked': 240}


def normalize_mode(value):
    value = (value or '30').strip().lower()
    return {'unlimited': 'unlocked', 'uncapped': 'unlocked', 'free': 'unlocked', '0': 'unlocked'}.get(value, value)


def mode_writes(eboot, names):
    """[(image offset, new bytes, description)] after checking every expected byte in eboot."""
    writes = []
    for name in names:
        for address, expected, new, what in PATCHES[name]:
            offset = address - GHIDRA_BASE
            # Patches may target code or mapped read-only data (the resolution table).
            # checked_offset() validates the actual ELF mapping and exact original bytes.
            if offset < 0 or len(expected) != len(new):
                raise ValueError(f'{name}: bad patch entry at {address:#x}')
            try:
                checked_offset(eboot, address, expected)
            except ValueError as error:
                raise ValueError(f'{name}: {error}') from error
            writes.append((offset, new, f'{name}: {address:#x} {what}'))
    return writes


def garlic_writes(eboot):
    """A checked 4064 MiB game GPU heap patch for every resolution and FPS mode."""
    offset = checked_offset(eboot, GARLIC_SIZE, GARLIC_SIZE_EXPECTED)
    size_mb = 2816 + GARLIC_EXTRA_MB
    data = GARLIC_SIZE_EXPECTED[:2] + struct.pack('<I', size_mb << 20)
    return [(offset, data, f'GPU heap {2816} -> {size_mb} MB')]


def write_patch_file(path, writes):
    """G3PATCH2 (probe.c apply_patches): base 0 (no pointer slots), then (offset, length, bytes)."""
    blob = struct.pack('<8sQQ', b'G3PATCH2', 0, len(writes))
    for offset, data, _ in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    Path(path).write_bytes(blob)


def prepare(game_dir, out_dir, mode, resolution='1080p'):
    """Writes out/patches.bin; unsupported EBOOTs fall back to 30 FPS / native 1080p."""
    mode = normalize_mode(mode)
    resolution = (resolution or '1080p').strip().lower()
    lines = []
    if mode not in MODES:
        lines.append(f'Frame rate: unknown TLG_FPS={mode!r}; using 30')
        mode = '30'
    if resolution not in RESOLUTIONS:
        lines.append(f'Resolution: unknown TLG_RESOLUTION={resolution!r}; using 1080p')
        resolution = '1080p'

    fps_names = [n for n in os.environ.get('TLG_FPS_PATCHES', '').split(',') if n in PATCHES]
    if not fps_names:
        fps_names = MODES[mode]
    resolution_names = RESOLUTIONS[resolution]

    try:
        raw_eboot = (Path(game_dir) / 'eboot.bin').read_bytes()
    except OSError as error:
        raw_eboot = b''
        lines.append(f'Patches: cannot read eboot.bin ({error})')

    digest = hashlib.sha256(raw_eboot).hexdigest()
    eboot = raw_eboot
    patchable = digest == EBOOT_103_SHA256
    if not patchable and raw_eboot[:4] == SELF_MAGIC:
        # A SELF dump wraps the same ELF at different file offsets. Actual writes still check
        # their expected bytes and require a PT_LOAD or PT_SCE_RELRO mapping.
        try:
            from prepare import parse_self
            eboot = bytes(parse_self(raw_eboot)[0])
            patchable = True
            lines.append(f'Patches: eboot.bin is a SELF ({digest[:16]}…), checking the ELF inside')
        except ValueError as error:
            lines.append(f'Patches: cannot read the SELF eboot.bin ({error})')

    fps_writes = []
    if fps_names:
        if not patchable:
            lines.append(f'Frame rate: eboot.bin is not v1.03 ({digest[:16]}…): {mode} FPS patches not applied, 30 FPS')
            mode = '30'
        else:
            try:
                fps_writes = mode_writes(eboot, fps_names)
            except ValueError as error:
                lines.append(f'Frame rate: {error}: patches not applied, 30 FPS')
                mode, fps_writes = '30', []

    resolution_writes = []
    if resolution_names:
        if not patchable:
            lines.append(f'Resolution: eboot.bin is not v1.03 ({digest[:16]}…): patches not applied, 1080p')
            resolution = '1080p'
        else:
            try:
                resolution_writes = mode_writes(eboot, resolution_names)
            except ValueError as error:
                lines.append(f'Resolution: {error}: patches not applied, 1080p')
                resolution, resolution_writes = '1080p', []

    # Keep the existing checked Garlic heap extension independent of FPS/resolution selection.
    heap_writes = []
    try:
        heap_eboot = eboot
        if heap_eboot[:4] == SELF_MAGIC:
            from prepare import parse_self
            heap_eboot = bytes(parse_self(heap_eboot)[0])
        heap_writes = garlic_writes(heap_eboot)
    except (OSError, ValueError) as error:
        lines.append(f'GPU heap: {error}: not applied, 2816 MB')

    write_patch_file(Path(out_dir) / 'patches.bin', fps_writes + resolution_writes + heap_writes)
    for _, data, what in fps_writes:
        lines.append(f'Frame rate patch {what} ({data.hex()})')
    lines.append(f'Frame rate: {mode} FPS mode, {len(fps_writes)} patch writes')
    for _, data, what in resolution_writes:
        lines.append(f'Resolution patch {what} ({data.hex()})')
    lines.append(f'Resolution: {resolution}, {len(resolution_writes)} patch writes')
    for _, data, what in heap_writes:
        lines.append(f'Patch garlic: {what} ({data.hex()})')
    return mode, lines


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game_dir')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--mode', default='30', help='30, 60 or unlocked')
    p.add_argument('--resolution', default='1080p', help='360p, 720p, 1080p, 1440p or 4k')
    a = p.parse_args()
    _, lines = prepare(a.game_dir, a.out, a.mode, a.resolution)
    print('\n'.join(lines))


if __name__ == '__main__':
    sys.exit(main())
