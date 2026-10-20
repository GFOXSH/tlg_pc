#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Last Guardian frame rate (TLG_FPS=30|60|unlocked) and resolution (TLG_RESOLUTION) patches,
written to out/patches.bin.

Each patch is a list of {image offset, expected bytes, new bytes} for eboot 1.03 only (sha256
below, or a SELF dump of it): the expected bytes are checked in eboot.bin before anything is
written, so another build never gets byte writes in the wrong place. The loader (probe.c
apply_patches) applies the file after relocation. Image offsets are eboot virtual addresses;
the Ghidra addresses in the comments (base 0x1000000) are offset + 0x1000000. Research:
docs/60fps.md.

30 (default) and native 1080p write an empty patch file: the game runs exactly as on PS4.
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
# Segment types the loader maps: PT_LOAD and PT_SCE_RELRO (read-only data fixed up at load).
LOADED_SEGMENTS = (1, 0x61000010)


def f32(value):
    return struct.pack('<f', value)


def rip_disp(instruction_end, target):
    """disp32 of a rip-relative operand: target - address of the next instruction (Ghidra addresses)."""
    return struct.pack('<i', target - instruction_end)


def res_bytes(w, h):
    return struct.pack('<4HQ', w, h, w, h, 0)


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
    'cloth_null_guard': [
        (0x128c161,
         bytes.fromhex(
             '6666666666662e0f1f840000000000'
             '4d8b7424484863c7498b4c242841bb00000000488d1c4048c1e306'
             '4963841ea0000000488b04c14c8b7828488945904d85ff7479'
         ),
         bytes.fromhex(
             '49837c2428000f84f90400000f1f00'
             '4d8b74244889f8498b4c24284531db8d1c4048c1e306'
             '4963841ea0000000488b04c1488945904885c074074c8b78284d85ff7479'
         ),
         'cloth update null table/element guard at 0x28c161..0x28c1a4'),
    ],
    # The mode table (0x18 bytes per entry) starts with the Pro list's default entry at 0x26ec3e8:
    # 1680x1890 checkerboard, output 3360x1890; it becomes WxH, WxH, no checkerboard.
    # The 23 bytes at 0x1c4fa6a in JsDevice::CreateDevice (0x1c4f9f0) are output height > 1080 test:
    # NOPed, the Pro list is always used.
    'res_nop': [
        (0x1c4fa6a, RES_NOP_ENTRY, b'\x90' * 23, 'CreateDevice: always use the Pro mode list'),
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
# The engine rate each mode presents at most (TLG_ENGINE_FPS: the overlay's frame limiter and
# tlg_graphics.h PresentLimit). Unlocked is paced by the present thread (TLG_VBLANK_HZ=0).
ENGINE_FPS = {'30': 30, '60': 60, 'unlocked': 240}

RESOLUTIONS = {
    '1080p': [],
    '360p': ['res_nop', 'res_360p'],
    '720p': ['res_nop', 'res_720p'],
    '1440p': ['res_nop', 'res_1440p'],
    '4k': ['res_nop', 'res_4k'],
}


def normalize_mode(value):
    value = (value or '30').strip().lower()
    return {'unlimited': 'unlocked', 'uncapped': 'unlocked', 'free': 'unlocked', '0': 'unlocked'}.get(value, value)


def file_offset(eboot, offset, length):
    """File offset of image bytes [offset, offset + length): text by TEXT_FILE_OFFSET, anything
    after it (the data tables) through the ELF's PT_LOAD and PT_SCE_RELRO headers; None when no segment holds them."""
    if offset + length <= TEXT_SIZE:
        return TEXT_FILE_OFFSET + offset
    if eboot[:4] != b'\x7fELF' or len(eboot) < 0x40:
        return None
    phoff, = struct.unpack_from('<Q', eboot, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', eboot, 0x36)
    for i in range(phnum):
        position = phoff + i * phentsize
        if position + 56 > len(eboot):
            break
        kind, _, p_offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', eboot, position)
        if kind in LOADED_SEGMENTS and vaddr <= offset and offset + length <= vaddr + filesz \
                and p_offset + offset - vaddr + length <= len(eboot):
            return p_offset + offset - vaddr
    return None


def mode_writes(eboot, names):
    """[(image offset, new bytes, description)] after checking every expected byte in eboot."""
    writes = []
    for name in names:
        for address, expected, new, what in PATCHES[name]:
            offset = address - GHIDRA_BASE
            if offset < 0 or len(expected) != len(new):
                raise ValueError(f'{name}: bad patch entry at {address:#x}')
            start = file_offset(eboot, offset, len(new))
            if start is None:
                raise ValueError(f'{name}: {address:#x} is not in the eboot image')
            found = eboot[start:start + len(new)]
            if found != expected:
                raise ValueError(f'{name}: {address:#x} has {found.hex()}, expected {expected.hex()}')
            writes.append((offset, new, f'{name}: {address:#x} {what}'))
    return writes


def write_patch_file(path, writes):
    """G3PATCH2 (probe.c apply_patches): base 0 (no pointer slots), then (offset, length, bytes)."""
    blob = struct.pack('<8sQQ', b'G3PATCH2', 0, len(writes))
    for offset, data, _ in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    Path(path).write_bytes(blob)


def load_eboot(game_dir):
    """(eboot bytes the patches can be checked against, or None; notes; why not) for 1.03: the
    checked plain dump or the ELF inside a SELF dump of it."""
    notes = []
    try:
        eboot = (Path(game_dir) / 'eboot.bin').read_bytes()
    except OSError as error:
        eboot = b''
        notes.append(f'cannot read eboot.bin ({error})')
    digest = hashlib.sha256(eboot).hexdigest()
    usable = digest == EBOOT_103_SHA256
    if not usable and eboot[:4] == SELF_MAGIC:
        # A SELF dump of 1.03 (sha256 b7f61057…, #40) wraps the same ELF as the plain
        # dump the hash is for, at other file offsets: patch the ELF inside (the image
        # prepare.py hands the loader). mode_writes checks every expected byte, so a SELF
        # of another build still falls back to 30.
        try:
            from prepare import parse_self
            eboot = bytes(parse_self(eboot)[0])
            usable = True
            notes.append(f'eboot.bin is a SELF ({digest[:16]}…), checking the ELF inside')
        except ValueError as error:
            notes.append(f'cannot read the SELF eboot.bin ({error})')
    return (eboot if usable else None), notes, f'eboot.bin is not v1.03 ({digest[:16]}…)'


def prepare(game_dir, out_dir, mode='30', resolution='1080p'):
    """Writes out/patches.bin for the mode and resolution; returns (applied mode, log lines).
    Any problem (other eboot build, unexpected bytes) falls back to 30 FPS / 1080p without patches."""
    mode = normalize_mode(mode)
    resolution = (resolution or '1080p').strip().lower()
    lines = []
    if mode not in MODES:
        lines.append(f'Frame rate: unknown TLG_FPS={mode!r}; using 30')
        mode = '30'
    if resolution not in RESOLUTIONS:
        lines.append(f'Resolution: unknown TLG_RESOLUTION={resolution!r}; using 1080p')
        resolution = '1080p'

    eboot, notes, problem = load_eboot(game_dir)
    lines += [f'Patches: {note}' for note in notes]

    # TLG_FPS_PATCHES="a,b" (measurements): these patches instead of the mode's own.
    fps_names = [n for n in os.environ.get('TLG_FPS_PATCHES', '').split(',') if n in PATCHES] or MODES[mode]
    fps_writes = []
    if eboot is None:
        lines.append(f'Frame rate: {problem}: {mode} FPS patches not applied, 30 FPS')
        mode = '30'
    else:
        try:
            fps_writes = mode_writes(eboot, fps_names)
        except ValueError as error:
            lines.append(f'Frame rate: {error}: patches not applied, 30 FPS')
            mode, fps_writes = '30', []

    res_names = RESOLUTIONS[resolution]
    res_writes = []
    if res_names:
        if eboot is None:
            lines.append(f'Resolution: {problem}: patches not applied, 1080p')
            resolution = '1080p'
        else:
            try:
                res_writes = mode_writes(eboot, res_names)
            except ValueError as error:
                lines.append(f'Resolution: {error}: patches not applied, 1080p')
                resolution, res_writes = '1080p', []

    write_patch_file(Path(out_dir) / 'patches.bin', fps_writes + res_writes)
    for _, data, what in fps_writes:
        lines.append(f'Frame rate patch {what} ({data.hex()})')
    lines.append(f'Frame rate: {mode} FPS mode, {len(fps_writes)} patch writes')

    if res_writes:
        for _, data, what in res_writes:
            lines.append(f'Resolution patch {what} ({data.hex()})')
        lines.append(f'Resolution: {resolution}, {len(res_writes)} patch writes')
    else:
        lines.append(f'Resolution: {resolution}, 0 patch writes')

    return mode, lines


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game_dir')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--mode', default='30', help='30, 60 or unlocked')
    p.add_argument('--resolution', default='1080p', help='1080p, 720p, 1440p, 4k, 360p')
    a = p.parse_args()
    _, lines = prepare(a.game_dir, a.out, a.mode, a.resolution)
    print('\n'.join(lines))


if __name__ == '__main__':
    sys.exit(main())