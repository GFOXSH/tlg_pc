#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare and run The Last Guardian (CUSA03627 v1.03) through the native loader.

Shared by the command line (run_tlg.sh -> `tlg_run.py [seconds]`) and the launcher window
(tlg_launcher.py), so both start the game the same way:
  1. prepare.py / link_modules.py / content_profile.py build out/boot-linked.bin from the game
     folder (eboot 1.03 + the game's libc/Fios2; audio uses the bundled libSceNgs2 V5 image);
     isa_rewrite.py handles unsupported PS4 instructions and Windows red-zone protection;
  2. the TLG defaults below become TLG_* environment variables (each one validated in play);
  3. tlg-probe runs with its output in out/logs/run-<date>.log (last_run.log points to it).
"""
import copy
import datetime
import glob
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

import shader_cache

# A packaged build (PyInstaller) keeps its folders next to the executable: scripts/, out/, user/.
FROZEN = bool(getattr(sys, 'frozen', False))
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
REPO_DIR = PORT_DIR.parent
OUT_DIR = PORT_DIR / 'out'
USER_DIR = PORT_DIR / 'user'
LOG_DIR = OUT_DIR / 'logs'
PROBE = OUT_DIR / ('tlg-probe.exe' if os.name == 'nt' else 'tlg-probe')
KEEP_LOGS = 30


# VRAM budget policy. This target uses a small Tk launcher rather than the upstream
# backports-table UI, so keep the budget calculation/detection here without Qt dependencies.
BUDGET_DEFAULT = 7000
BUDGET_MIN, BUDGET_MAX = 2000, 24000


def suggested_budget_mb(vram_gb):
    """Texture-cache budget in MB: 7000 for 8 GB+, scaled down for smaller cards."""
    value = min(BUDGET_DEFAULT, 5600 * float(vram_gb) / 6)
    return int(min(max(round(value / 100) * 100, BUDGET_MIN), BUDGET_MAX))


def _parse_nvidia_vram(text):
    values = [int(value) for value in re.findall(r'^\s*(\d+)\s*$', text or '', re.M)]
    return max(values) if values else None


def _read_amdgpu_vram():
    sizes = []
    for path in glob.glob('/sys/class/drm/card*/device/mem_info_vram_total'):
        try:
            sizes.append(int(Path(path).read_text(encoding='ascii').strip()))
        except (OSError, ValueError):
            pass
    return max(sizes) // (1024 * 1024) if sizes else None


def _parse_vulkan_vram(text):
    best = {}
    name = ''
    size = None
    local = False
    in_heap = False

    def finish_heap():
        if in_heap and size and local:
            best[name] = max(best.get(name, 0), size)

    for line in (text or '').splitlines():
        device = re.match(r'\s*deviceName\s*=\s*(.+?)\s*$', line)
        if device:
            finish_heap()
            name, size, local, in_heap = device.group(1), None, False, False
            continue
        if re.match(r'\s*memoryHeaps\[\d+\]:', line):
            finish_heap()
            size, local, in_heap = None, False, True
            continue
        if not in_heap:
            continue
        match = re.match(r'\s*size\s*=\s*(\d+)', line)
        if match and size is None:
            size = int(match.group(1))
        elif 'MEMORY_HEAP_DEVICE_LOCAL_BIT' in line:
            local = True
        elif re.match(r'\s*memoryTypes\[', line) or re.match(r'\s*memoryHeapCount', line):
            finish_heap()
            size, local, in_heap = None, False, False
    finish_heap()
    return max(best.values()) // (1024 * 1024) if best else None


def detect_vram_mb(timeout=15):
    """Detect VRAM via nvidia-smi, AMD sysfs, then Vulkan device-local heaps."""
    flags = {
        'stdin': subprocess.DEVNULL,
        'capture_output': True,
        'text': True,
        'timeout': timeout,
        'creationflags': getattr(subprocess, 'CREATE_NO_WINDOW', 0),
    }
    try:
        result = subprocess.run(
            ['nvidia-smi', '--query-gpu=memory.total', '--format=csv,noheader,nounits'],
            **flags)
        value = _parse_nvidia_vram(result.stdout)
        if value:
            return value
    except (OSError, subprocess.SubprocessError):
        pass
    value = _read_amdgpu_vram()
    if value:
        return value
    try:
        result = subprocess.run([os.environ.get('TLG_VULKANINFO', 'vulkaninfo')], **flags)
        return _parse_vulkan_vram(result.stdout)
    except (OSError, subprocess.SubprocessError):
        return None


# Game language: the engine's own ids (NOT the PS4 system ones), looked up through
# sceSystemServiceParamGetInt(LANG). Each CUSA ships a subset of them; an id it does not ship
# falls back to English (1). Tables swept from the loaded trico_<lang>.psarc.
LANGUAGES_US = {'en': 1, 'pt_BR': 17, 'es_LA': 20, 'fr_CA': 22}
LANGUAGES_EU = {'en': 1, 'fr': 2, 'es_ES': 3, 'de': 4, 'it': 5, 'nl': 6, 'pt_PT': 7, 'ru': 8,
                'fi': 12, 'sv': 13, 'da': 14, 'no': 15, 'pl': 16, 'tr': 19, 'ar': 21}
LANGUAGES_BY_TITLE = {'CUSA03627': LANGUAGES_US, 'CUSA03745': LANGUAGES_EU}
DEFAULT_LANGUAGE_BY_TITLE = {'CUSA03627': 'es_LA', 'CUSA03745': 'es_ES'}


def game_languages(game_dir=''):
    """The language table (engine ids) the CUSA in game_dir ships; the US one when it is unknown."""
    title = shader_cache.title_id(game_dir) if game_dir else None
    return LANGUAGES_BY_TITLE.get(title, LANGUAGES_US)


def default_language(game_dir=''):
    """The game language the launcher starts with for the CUSA in game_dir (US by default)."""
    title = shader_cache.title_id(game_dir) if game_dir else None
    return DEFAULT_LANGUAGE_BY_TITLE.get(title, 'es_LA')

# TLG defaults (see README "Opções" and the reasons in each comment).
DEFAULT_ENV = {
    'TLG_PIPELINE_BINARIES': '0',  # VK_KHR_pipeline_binary crashes on the NVIDIA 595 driver
    'TLG_READBACKS': '2',          # the game reads GPU compute results on the CPU (AsyncMap)
    'TLG_READBACK_LINEAR': '1',    # small/linear render targets back to the CPU: auto exposure (#38, #40)
    'TLG_GC_BUDGET_MB': '7000',    # current upstream default; smaller cards get a lower detected budget
    'TLG_UPSCALER': 'off',         # native image: GoW3's temporal upscalers made it shake
    'TLG_JITTER': '0',
    'TLG_OBJECT_MOTION': '0',
    'TLG_FRAME_STATS': '1',        # FPS in the log every 5 s (scripts/watch_session.sh heartbeat)
}

# Supported Advanced GPU readback switches (setting key, environment variable, default).
# Keep this list in sync with the controls and implementations in the GPU code.
READBACK_FLAGS = (
    ('rb_hot', 'TLG_RB_HOT', True),
    ('deferred_readback', 'TLG_DEFERRED_READBACK', True),
    ('fast_read_fault', 'TLG_FAST_READ_FAULT', True),
    ('readback_linear', 'TLG_READBACK_LINEAR', True),
    ('stale_readback', 'TLG_STALE_READBACK', False),
    ('gc_batch_download', 'TLG_GC_BATCH_DOWNLOAD', False),
    ('async_fences', 'TLG_ASYNC_FENCES', False),
)

DEFAULT_STICK_KEYS = {
    'walk_up': 'W', 'walk_left': 'A', 'walk_down': 'S', 'walk_right': 'D',
    'camera_up': 'I', 'camera_left': 'J', 'camera_down': 'K', 'camera_right': 'L',
}

DEFAULT_SETTINGS = {
    # Source tree: the merged game view. Package: the player picks the game folder.
    'game_dir': '' if FROZEN else str(REPO_DIR / 'game' / 'CUSA03627'),
    'game_language': 'pt_BR',
    'ui_language': 'auto',         # auto | pt_BR | en_US | ru_RU
    'show_fps': False,
    'display_mode': 'Windowed',    # Windowed | Borderless | Fullscreen (tlg.ini)
    'vsync': True,
    'vram_budget_mb': 7000,
    'keep_gpu_textures': True,
    # Mouse and keyboard (tlg.ini; the in-game menu's Controls tab changes the mouse ones too).
    'mouse_enabled': True,
    'mouse_sensitivity': 1.0,
    'mouse_invert_y': False,
    'key_map': {},                 # PS4 button -> 'Key|Mouse Left' (empty: the default)
    'stick_keys': dict(DEFAULT_STICK_KEYS),
    'frame_rate': '30',            # 30 | 60 | unlocked (scripts/tlg_patches.py; TLG_FPS wins)
    'resolution': '1080p',         # 360p | 720p | 1080p | 1440p | 4k (checked EBOOT patch)
    'draw_pipe': 'auto',           # auto | on | off (TLG_DRAW_PIPE; Advanced)
}
FRAME_RATES = ['30', '60', 'unlocked']
RESOLUTIONS = ['360p', '720p', '1080p', '1440p', '4k']
RESOLUTION_SIZES = {
    '360p': (640, 360), '720p': (1280, 720), '1080p': (1920, 1080),
    '1440p': (2560, 1440), '4k': (3840, 2160),
}
# The modes really applied (unsupported EBOOTs stay at 30 FPS / native 1080p).
applied_frame_rate = '30'
applied_resolution = '1080p'
# MiB added to the game's Garlic GPU heap by scripts/tlg_patches.py.
applied_garlic_extra = 0

# Keyboard defaults of src/runtime_pad.c, in its key_map syntax ('|': several).
DEFAULT_KEYS = {
    'cross': 'Left Ctrl', 'circle': 'E', 'square': 'Left Shift', 'triangle': 'Space',
    'l1': 'Q', 'r1': 'F', 'l2': '', 'r2': '', 'l3': 'Tab', 'r3': '', 'options': 'Return',
    'touchpad_left': '', 'touchpad_right': '',
    'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right',
}
# Synced with tlg.ini both ways: the in-game menu saves them there.
INI_KEYS = ('mouse_enabled', 'mouse_sensitivity', 'mouse_invert_y', 'key_map')


def key_map_spec(mapping):
    """The tlg.ini key_map value: only the buttons that differ from the defaults."""
    entries = []
    for button, value in keyboard_key_map(mapping).items():
        if value != DEFAULT_KEYS[button]:
            entries.append(f'{button}={value}')
    return ','.join(entries)

def keyboard_key_map(mapping):
    """Keep only configurable keyboard bindings; mouse bindings are fixed in the runtime."""
    clean = {}
    for button, value in mapping.items():
        if button not in DEFAULT_KEYS or not isinstance(value, str):
            continue
        bindings = [part.strip() for part in value.split('|') if part.strip()]
        keyboard = '|'.join(part for part in bindings
                            if not part.lower().startswith(('mouse ', 'wheel ')))
        if keyboard:
            clean[button] = keyboard
    return clean


def parse_key_map(spec):
    mapping = {}
    for entry in (spec or '').split(','):
        button, eq, value = entry.partition('=')
        if eq and button.strip().lower() in DEFAULT_KEYS:
            mapping[button.strip().lower()] = value.strip()
    return keyboard_key_map(mapping)


def _as_bool(value, default=False):
    """Parse persisted booleans without treating the string '0' as true."""
    if value is None:
        return default
    if isinstance(value, str):
        return value.strip().lower() not in ('', '0', 'false', 'no', 'off')
    return bool(value)


def normalize_settings(values):
    """Return a validated, independent settings snapshot with all public defaults applied."""
    settings = copy.deepcopy(DEFAULT_SETTINGS)
    if isinstance(values, dict):
        settings.update(values)

    for key, default in (('show_fps', False), ('vsync', True), ('keep_gpu_textures', True),
                         ('mouse_enabled', True), ('mouse_invert_y', False)):
        settings[key] = _as_bool(settings.get(key), default)

    settings['game_dir'] = str(settings.get('game_dir') or '')
    settings['game_language'] = str(settings.get('game_language') or 'pt_BR')
    if settings.get('ui_language') not in ('auto', 'pt_BR', 'en_US', 'ru_RU'):
        settings['ui_language'] = 'auto'
    if settings.get('display_mode') not in ('Windowed', 'Borderless', 'Fullscreen'):
        settings['display_mode'] = 'Windowed'
    if str(settings.get('frame_rate')) not in FRAME_RATES:
        settings['frame_rate'] = '30'
    else:
        settings['frame_rate'] = str(settings['frame_rate'])
    if settings.get('resolution') not in RESOLUTIONS:
        settings['resolution'] = '1080p'
    if settings.get('draw_pipe') not in ('auto', 'on', 'off'):
        settings['draw_pipe'] = 'auto'

    try:
        settings['vram_budget_mb'] = min(BUDGET_MAX, max(BUDGET_MIN, int(settings['vram_budget_mb'])))
    except (TypeError, ValueError, OverflowError):
        settings['vram_budget_mb'] = DEFAULT_SETTINGS['vram_budget_mb']
    try:
        settings['mouse_sensitivity'] = min(5.0, max(0.1, float(settings.get('mouse_sensitivity', 1.0))))
    except (TypeError, ValueError, OverflowError):
        settings['mouse_sensitivity'] = DEFAULT_SETTINGS['mouse_sensitivity']

    mode = str(settings.get('gpu_readbacks', '2'))
    if mode not in ('0', '1', '2') or mode == '2':
        settings.pop('gpu_readbacks', None)
    else:
        settings['gpu_readbacks'] = mode

    for key, _env_key, default in READBACK_FLAGS:
        if key in settings:
            settings[key] = _as_bool(settings[key], default)

    raw_key_map = settings.get('key_map')
    settings['key_map'] = keyboard_key_map(raw_key_map if isinstance(raw_key_map, dict) else {})
    stick_keys = dict(DEFAULT_STICK_KEYS)
    raw_stick_keys = settings.get('stick_keys')
    if isinstance(raw_stick_keys, dict):
        for key, value in raw_stick_keys.items():
            if key in DEFAULT_STICK_KEYS and isinstance(value, str) and value.strip():
                stick_keys[key] = value.strip()
    settings['stick_keys'] = stick_keys

    for key in ('readback_defaults_2026_10', 'budget_default_2026_10', 'budget_default_2026_11'):
        settings.pop(key, None)
    settings['settings_schema_version'] = 2
    return settings


def config_path():
    base = Path(os.environ.get('APPDATA') or os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')
    return base / 'tlg-launcher' / 'settings.json'


def load_settings():
    """Load, migrate and validate launcher preferences, then merge in-game-owned controls."""
    raw = {}
    try:
        loaded = json.loads(config_path().read_text(encoding='utf-8'))
        if isinstance(loaded, dict):
            raw = loaded
    except (OSError, ValueError, TypeError):
        pass

    settings = copy.deepcopy(DEFAULT_SETTINGS)
    settings.update(raw)
    schema = raw.get('settings_schema_version', 0)
    try:
        schema = int(schema)
    except (TypeError, ValueError):
        schema = 0
    original_schema = schema

    if raw and schema < 1:
        if not raw.get('readback_defaults_2026_10'):
            settings.pop('deferred_readback', None)
        if not raw.get('budget_default_2026_10'):
            try:
                settings['vram_budget_mb'] = min(int(settings['vram_budget_mb']),
                                                 DEFAULT_SETTINGS['vram_budget_mb'])
            except (TypeError, ValueError, OverflowError):
                settings['vram_budget_mb'] = DEFAULT_SETTINGS['vram_budget_mb']
        schema = 1
    if raw and schema < 2:
        if not raw.get('budget_default_2026_11'):
            try:
                legacy_budget = int(settings['vram_budget_mb'])
            except (TypeError, ValueError, OverflowError):
                legacy_budget = DEFAULT_SETTINGS['vram_budget_mb']
            if legacy_budget in (5000, DEFAULT_SETTINGS['vram_budget_mb']):
                vram = detect_vram_mb()
                settings['vram_budget_mb'] = (suggested_budget_mb(vram / 1024) if vram
                                              else DEFAULT_SETTINGS['vram_budget_mb'])
        schema = 2

    settings = normalize_settings(settings)
    ini = read_ini(USER_DIR / 'tlg.ini')
    display_mode = ini.get('display_mode', '').strip().lower()
    if display_mode in ('windowed', 'borderless', 'fullscreen'):
        settings['display_mode'] = display_mode.title()
    for key in ('vsync', 'show_fps', 'keep_gpu_textures', 'mouse_enabled', 'mouse_invert_y'):
        if key in ini:
            settings[key] = _as_bool(ini[key], settings[key])
    if 'mouse_sensitivity' in ini:
        try:
            settings['mouse_sensitivity'] = min(5.0, max(0.1, float(ini['mouse_sensitivity'])))
        except (TypeError, ValueError, OverflowError):
            pass
    if 'key_map' in ini:
        settings['key_map'] = parse_key_map(ini['key_map'])

    if raw and original_schema < 2:
        save_settings(settings)
    return settings

def save_settings(settings):
    """Atomically persist normalized preferences and synchronize their tlg.ini counterparts."""
    normalized = normalize_settings(settings)
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                mode='w', encoding='utf-8', dir=path.parent,
                prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(normalized, stream, indent=2, ensure_ascii=False)
            stream.write('\n')
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    sync_settings_to_ini(normalized)
    return normalized

def read_ini(path):
    """The key=value pairs of tlg.ini (empty when the file does not exist yet)."""
    values = {}
    try:
        for line in path.read_text(encoding='utf-8').splitlines():
            if '=' in line and not line.lstrip().startswith(('#', ';')):
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def _atomic_write_text(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                mode='w', encoding='utf-8', dir=path.parent,
                prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def update_ini(path, values):
    """Update selected tlg.ini keys, preserving comments and unrelated settings."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# tlg settings']

    output = []
    written = set()
    for line in lines:
        stripped = line.lstrip()
        if not stripped or stripped.startswith(('#', ';')) or '=' not in line:
            output.append(line)
            continue
        key = line.split('=', 1)[0].strip()
        if key not in values:
            output.append(line)
        elif key not in written:
            output.append(f'{key}={values[key]}')
            written.add(key)

    output.extend(f'{key}={value}' for key, value in values.items() if key not in written)
    _atomic_write_text(path, '\n'.join(output) + '\n')


def sync_settings_to_ini(settings):
    """Persist launcher-owned live controls in the same file used by the in-game menu."""
    settings = normalize_settings(settings)
    update_ini(USER_DIR / 'tlg.ini', {
        'display_mode': settings['display_mode'].lower(),
        'vsync': int(settings['vsync']),
        'show_fps': int(settings['show_fps']),
        'keep_gpu_textures': int(settings['keep_gpu_textures']),
        'mouse_enabled': int(settings['mouse_enabled']),
        'mouse_sensitivity': f"{settings['mouse_sensitivity']:.2f}",
        'mouse_invert_y': int(settings['mouse_invert_y']),
        'key_map': key_map_spec(settings['key_map']),
    })


def reset_advanced_settings(settings):
    """Reset every Advanced-dialog option, including its tlg.ini-backed GPU settings."""
    reset = dict(settings)
    reset.pop('gpu_readbacks', None)
    reset.pop('draw_pipe', None)
    reset.pop('keep_gpu_textures', None)
    for key, _env_key, _default in READBACK_FLAGS:
        reset.pop(key, None)
    normalized = normalize_settings(reset)
    save_settings(normalized)
    update_ini(USER_DIR / 'tlg.ini', {'readback_batch': '0'})
    return normalized

# The serial the GPU backend names this game's shader cache after (another region's copy: its
# param.sfo TITLE_ID, read from the game folder).
SHADER_CACHE_SERIALS = ('CUSA03627',)


def shader_cache_serials(settings):
    serial = shader_cache.title_id(settings['game_dir']) if settings.get('game_dir') else None
    return sorted({*SHADER_CACHE_SERIALS, *([serial] if serial else [])})


def gpu_user_dir():
    """Where the GPU backend keeps its caches (shaders, pipelines): the saves folder, on every OS.
    TLG_GPU_USER_DIR moves it."""
    return Path(os.environ.get('TLG_GPU_USER_DIR') or USER_DIR)


def shader_cache_size(settings):
    """Bytes of compiled shaders this game has on disk (launcher/shader_cache.py has the paths)."""
    return shader_cache.size(shader_cache_serials(settings), gpu_user_dir())


def clear_shader_cache(settings):
    """Deletes this game's compiled shaders: the next start rebuilds them (longer load, some
    stutter at first). Saves, settings and logs stay. Returns (bytes freed, [(path, error)])."""
    return shader_cache.clear(shader_cache_serials(settings), gpu_user_dir())


def python_command(script):
    """How to run one of scripts/*.py: the packaged executable runs them itself (--script)."""
    if FROZEN:
        return [sys.executable, '--script', str(script)]
    return [sys.executable, str(script)]


def attach_stdio():
    """A windowed Windows executable starts without sys.stdout; inherited pipes still exist."""
    if os.name != 'nt':
        return
    import ctypes
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)  # no console flashing for the scripts


def run_script(argv):
    """--script <path> [args]: runs a preparation script inside the packaged executable."""
    import runpy
    attach_stdio()
    script = Path(argv[0])
    sys.argv = [str(script), *argv[1:]]
    sys.path.insert(0, str(script.parent))
    try:
        runpy.run_path(str(script), run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def frame_rate(settings):
    """The requested frame rate mode: TLG_FPS, else the launcher's setting."""
    return os.environ.get('TLG_FPS') or str(settings.get('frame_rate', '30'))


def resolution(settings):
    """The requested render resolution: TLG_RESOLUTION, else the launcher's setting."""
    return os.environ.get('TLG_RESOLUTION') or str(settings.get('resolution', '1080p'))


def prepare(settings, log=print):
    """Builds out/boot-linked.bin, out/content.bin and out/patches.bin (frame rate patches);
    returns the module list that was linked."""
    global applied_frame_rate, applied_resolution, applied_garlic_extra
    applied_frame_rate = '30'
    applied_resolution = '1080p'
    applied_garlic_extra = 0
    game = settings['game_dir']
    scripts = PORT_DIR / 'scripts'
    modules = ['libc.prx', 'libSceFios2.prx']
    steps = [
        [*python_command(scripts / 'prepare.py'), game, '--out', str(OUT_DIR)],
        [*python_command(scripts / 'link_modules.py'), game, '--out', str(OUT_DIR), '--modules', *modules],
        [*python_command(scripts / 'content_profile.py'), game, '--out', str(OUT_DIR)],
        [*python_command(scripts / 'tlg_patches.py'), game, '--out', str(OUT_DIR),
         '--mode', frame_rate(settings), '--resolution', resolution(settings)],
    ]
    for step in steps:
        result = subprocess.run(step, cwd=str(scripts), capture_output=True, text=True,
                                creationflags=NO_WINDOW)
        if result.returncode:
            name = next(Path(arg).name for arg in step if arg.endswith('.py'))
            raise RuntimeError(f'{name}: {result.stderr.strip() or result.stdout.strip()}')
        for line in result.stdout.splitlines():
            log(line)
            if line.startswith('Frame rate: ') and line.endswith(' patch writes'):
                applied_frame_rate = line.split()[2]
            if line.startswith('Resolution: ') and line.endswith(' patch writes'):
                applied_resolution = line.split()[1].rstrip(',')
            if line.startswith('Patch garlic: '):
                words = line.split()
                applied_garlic_extra = int(words[-3]) - int(words[-5])
    isa_table(log)
    return modules


ISA_TABLE = OUT_DIR / 'isa-rewrite.bin'


def cpu_missing_sets(log=print):
    """Instruction sets `tlg-probe --cpu-features` reports missing ([] when complete); None when the
    probe could not tell (it did not start: the table is then built anyway, see isa_table). A CPU
    that lacks a set nothing can rewrite (AVX and older) stops here (exit 20) with the probe's reason.
    The probe gets the same library path as the game (out/lib in the Linux package)."""
    env = os.environ.copy()
    bundled = OUT_DIR / 'lib'
    if os.name != 'nt' and bundled.is_dir():
        env['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, [str(bundled), os.environ.get('LD_LIBRARY_PATH')]))
    try:
        result = subprocess.run([str(PROBE), '--cpu-features'], stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, timeout=30, creationflags=NO_WINDOW, env=env)
    except (OSError, subprocess.SubprocessError):
        return []  # no probe yet: check_game reports it
    if result.returncode == 20:
        raise RuntimeError(result.stdout.strip() or 'this CPU cannot run the game')
    for line in result.stdout.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
        if line.startswith('CPU: complete'):
            return []
    detail = (result.stderr or result.stdout).strip().splitlines()
    log(f'CPU check failed (exit {result.returncode}): {detail[-1] if detail else "no output"}; '
        'building the instruction rewrite table anyway')
    return None


def red_zone_wanted(env=None):
    """Windows red-zone protection (scripts/red_zone.py, from iss_pc #29/#41): on by default on
    Windows, TLG_RED_ZONE_REWRITE=0 turns it off; on Linux only TLG_RED_ZONE_REWRITE=1 (tests)
    turns it on."""
    env = os.environ if env is None else env
    policy = env.get('TLG_RED_ZONE_REWRITE') or ''
    return policy != '0' if os.name == 'nt' else policy == '1'


def isa_table(log=print):
    """ps4_core ISA rewrite (upstream gow3_pc #129): on a CPU without MOVBE, BMI1, LZCNT or SSE4A
    the probe replaces those instructions in the image, from the table scripts/isa_rewrite.py
    writes (iced-x86). TLG_ISA_REWRITE=1 (every set) or a list such as TLG_ISA_REWRITE=sse4a builds
    it on any CPU (testing), =0 never. The same table carries the red-zone protection
    (red_zone_wanted), so on Windows it is always built; the script keeps a table that already
    matches the image. A failed table is reported and the probe then explains what is missing."""
    policy = os.environ.get('TLG_ISA_REWRITE') or ''
    red_zone = red_zone_wanted()
    if not red_zone:
        ISA_TABLE.unlink(missing_ok=True)  # always matches the boot-linked.bin just written
    missing = cpu_missing_sets(log)
    # None (unknown) builds it too: the probe only rewrites the sets this CPU really lacks.
    if not ((policy and policy != '0') or (policy != '0' and (missing is None or missing)) or red_zone):
        return
    scripts = PORT_DIR / 'scripts'
    result = subprocess.run([*python_command(scripts / 'isa_rewrite.py'), str(OUT_DIR / 'boot-linked.bin'),
                             '--out', str(OUT_DIR), *(['--red-zone'] if red_zone else [])], cwd=str(scripts),
                            capture_output=True, text=True, creationflags=NO_WINDOW)
    if result.returncode:
        ISA_TABLE.unlink(missing_ok=True)
    for line in (result.stdout + result.stderr).splitlines():
        log(line)


def window_icon():
    """The port's icon (launcher/tlg.png) as BMP, SDL's built-in format, in out/; None if absent."""
    source = PORT_DIR / 'launcher' / 'tlg.png'
    target = OUT_DIR / 'window_icon.bmp'
    try:
        if source.is_file() and (not target.is_file() or target.stat().st_mtime < source.stat().st_mtime):
            from PIL import Image  # optional: without Pillow the window keeps the default icon
            OUT_DIR.mkdir(parents=True, exist_ok=True)
            Image.open(source).convert('RGB').resize((128, 128)).save(target)
        return target if target.is_file() else None
    except Exception:
        return None


def build_env(settings, extra=None):
    env = dict(os.environ)
    for key, value in DEFAULT_ENV.items():
        env.setdefault(key, value)  # explicit TLG_* in the environment always win
    # Advanced GPU-readback controls. Values equal to the code defaults are deliberately omitted;
    # explicit TLG_* environment variables always take precedence over launcher settings.
    readback_mode = str(settings.get('gpu_readbacks', '2'))
    if readback_mode not in ('0', '1', '2'):
        readback_mode = '2'
    if readback_mode != '2' and 'TLG_READBACKS' not in os.environ:
        env['TLG_READBACKS'] = readback_mode
    for setting_key, env_key, default in READBACK_FLAGS:
        if setting_key in settings and bool(settings[setting_key]) != default and env_key not in os.environ:
            env[env_key] = '1' if settings[setting_key] else '0'
    env['TLG_GC_BUDGET_MB'] = os.environ.get('TLG_GC_BUDGET_MB', str(int(settings['vram_budget_mb'])))
    env['TLG_KEEP_GPU_TEXTURES'] = os.environ.get('TLG_KEEP_GPU_TEXTURES', '1' if settings.get('keep_gpu_textures', True) else '0')
    stick_keys = dict(DEFAULT_STICK_KEYS)
    configured_stick_keys = settings.get('stick_keys')
    if isinstance(configured_stick_keys, dict):
        stick_keys.update({key: value for key, value in configured_stick_keys.items()
                           if key in DEFAULT_STICK_KEYS and isinstance(value, str) and value.strip()})
    for action, key in stick_keys.items():
        env.setdefault(f'TLG_{action.upper()}', key)
    env['TLG_LANGUAGE'] = os.environ.get('TLG_LANGUAGE', str(
        game_languages(settings.get('game_dir')).get(settings['game_language'], 1)))
    # The in-game overlay currently supports only Portuguese and English. A Russian
    # launcher must not pass ru_RU here: the runtime treats every non-English value as Portuguese.
    if settings.get('ui_language') in ('pt_BR', 'en_US'):
        env['TLG_UI_LANG'] = settings['ui_language']
    env['TLG_CONFIG'] = str(USER_DIR / 'tlg.ini')
    # Window name and app id (matches tlg-pc.desktop for Alt+Tab and the dock) and icon.
    env.setdefault('TLG_APP_NAME', 'The Last Guardian')
    env.setdefault('TLG_APP_ID', 'tlg-pc')
    icon = window_icon()
    if icon:
        env.setdefault('TLG_WINDOW_ICON', str(icon))
    # The present limiter never lets more frames through than the patched engine rate; unlocked
    # flips as soon as a frame is ready (480 Hz vblank) up to the display refresh or the menu limit.
    env['TLG_ENGINE_FPS'] = {'30': '30', '60': '60'}.get(applied_frame_rate, '240')
    render_width, render_height = RESOLUTION_SIZES.get(applied_resolution, RESOLUTION_SIZES['1080p'])
    env.setdefault('TLG_INTERNAL_WIDTH', str(render_width))
    env.setdefault('TLG_INTERNAL_HEIGHT', str(render_height))
    # Auto leaves the runtime's available-thread heuristic intact; On/Off are explicit overrides.
    draw_pipe = str(settings.get('draw_pipe', 'auto')).strip().lower()
    if 'TLG_DRAW_PIPE' not in os.environ and draw_pipe in ('on', 'off'):
        env['TLG_DRAW_PIPE'] = '1' if draw_pipe == 'on' else '0'
    # 30/60: a 240 Hz vblank shows a late frame ~4 ms late instead of a full 60 Hz vblank later.
    # The present limiter keeps flips at the patched engine rate (driver.cpp PresentThread).
    env.setdefault('TLG_VBLANK_HZ', '0' if applied_frame_rate == 'unlocked' else '240')
    if applied_frame_rate in ('60', 'unlocked'):
        env.setdefault('TLG_BUFFER_READBACK_STATS', '1')
    # Grow direct memory with the enlarged Garlic heap so render targets and guest allocations fit.
    if applied_garlic_extra:
        env.setdefault('TLG_DMEM_MB', str(5056 + applied_garlic_extra))
    restart = f'"{sys.executable}" --play' if FROZEN else f'"{sys.executable}" "{Path(__file__).resolve()}"'
    env.setdefault('TLG_RESTART_COMMAND', restart)
    # Linux package: the libraries built with the port (SDL3, FFmpeg, ...) are in out/lib.
    bundled = OUT_DIR / 'lib'
    if os.name != 'nt' and bundled.is_dir():
        env['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, [str(bundled), os.environ.get('LD_LIBRARY_PATH')]))
    # Shaders and pipelines beside the saves (the GPU reads it when it starts; before, Linux used
    # shadPS4's ~/.local/share/shadPS4 and Windows <saves>/user).
    env['TLG_GPU_USER_DIR'] = str(gpu_user_dir())
    if extra:
        env.update(extra)
    return env


def new_log():
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log = LOG_DIR / f'run-{datetime.datetime.now():%Y%m%d-%H%M%S}.log'
    latest = OUT_DIR / 'last_run.log'
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(log)
    except OSError:
        pass
    for old in sorted(LOG_DIR.glob('run-*.log'), reverse=True)[KEEP_LOGS:]:
        old.unlink(missing_ok=True)
    return log


def launch(settings, seconds=0, extra_env=None):
    """Writes the in-game settings, starts the game; returns (process, log path).
    The process's combined output is a text pipe; the caller copies it to the log."""
    USER_DIR.mkdir(parents=True, exist_ok=True)
    settings = normalize_settings(settings)
    save_settings(settings)
    log = new_log()
    cmd = [str(PROBE), str(OUT_DIR / 'boot-linked.bin'), '--app0', settings['game_dir'],
           '--user', str(USER_DIR), '--content-profile', str(OUT_DIR / 'content.bin'),
           '--patches', str(OUT_DIR / 'patches.bin'), '--timeout', str(int(seconds))]
    if ISA_TABLE.is_file():
        cmd += ['--isa-rewrite', str(ISA_TABLE)]
    process = subprocess.Popen(cmd, cwd=str(USER_DIR), env=build_env(settings, extra_env),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               encoding='utf-8', errors='replace', bufsize=1)
    return process, log


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    attach_stdio()
    seconds = int(argv[0]) if argv and argv[0].isdigit() else 0
    settings = load_settings()
    if os.environ.get('TLG_GAME_DIR'):
        settings['game_dir'] = os.environ['TLG_GAME_DIR']
    if argv[:1] == ['--clear-shader-cache']:
        freed, failures = clear_shader_cache(settings)
        print(f'Cache de shaders: {shader_cache.human_size(freed)} liberados', flush=True)
        for path, error in failures:
            print(f'Não apagado (em uso? feche o jogo): {path}: {error}', file=sys.stderr)
        return 1 if failures else 0
    problems = check_game(settings['game_dir'])
    if problems:
        for kind, detail in problems:
            print(f'{"Compile primeiro" if kind == "not_built" else "Faltando"}: {detail}', file=sys.stderr)
        return 1
    notes = []
    prepare(settings, log=lambda line: notes.append(line) if line.startswith('Frame rate') else None)
    process, log = launch(settings, seconds)
    print(f'Rodando (log: {log})', flush=True)
    with open(log, 'w', encoding='utf-8') as out:
        for line in notes:
            print(line, flush=True)
            out.write(line + '\n')
        for line in process.stdout:
            sys.stdout.write(line)
            out.write(line)
            out.flush()
    return process.wait()


if __name__ == '__main__':
    sys.exit(main())
