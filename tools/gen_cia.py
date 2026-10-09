"""Phase 5 generator: build/Build rows -> build/cia/ (doomlauncher3ds.rsf, banner.bnr, icon.smdh) for makerom.

The CIA is made by the Makefile from the same ELF as the 3dsx (target build/doomlauncher3ds.cia):
  title/short_title/publisher/product_code/unique_id -> SMDH + RSF BasicInfo/TitleInfo (title ID 00040000 <unique_id>00)
  app_ram_mb -> SystemModeExt, system_mode -> SystemMode, cpu ('804MHz + L2') -> CpuSpeed + EnableL2Cache
  stack_kb -> SystemControlInfo StackSize (the 3dsx gets it from __stacksize__, a CIA from the exheader)
  banner_image / banner_audio -> IWAD lumps ('doom2:TITLEPIC') or files (gen/banner.png, Banner/*.mp3) converted here; smdh_flags / smdh_regions -> bannertool makesmdh
The service/syscall lists are the standard homebrew application set (libctru apps, all ARM11 services we use).
"""
import os, struct, subprocess, sys
import numpy as np
from PIL import Image
from export import rows
from pk3 import ROOT, IWADS
import sprite_patch

OUT = os.path.join(ROOT, 'build', 'cia')

SERVICES = ['APT:U', 'ac:u', 'am:net', 'boss:U', 'cam:u', 'cecd:u', 'cfg:nor', 'cfg:u', 'csnd:SND', 'dsp::DSP', 'frd:u',
            'fs:USER', 'gsp::Gpu', 'gsp::Lcd', 'hid:USER', 'http:C', 'ir:rst', 'ir:u', 'ir:USER', 'mic:u', 'mcu::HWC',
            'ndm:u', 'news:u', 'nwm::EXT', 'nwm::UDS', 'ptm:sysm', 'ptm:u', 'pxi:dev', 'soc:U', 'ssl:C', 'y2r:u']
SYSCALLS = ['ControlMemory', 'QueryMemory', 'ExitProcess', 'GetProcessAffinityMask', 'SetProcessAffinityMask',
            'GetProcessIdealProcessor', 'SetProcessIdealProcessor', 'CreateThread', 'ExitThread', 'SleepThread',
            'GetThreadPriority', 'SetThreadPriority', 'GetThreadAffinityMask', 'SetThreadAffinityMask',
            'GetThreadIdealProcessor', 'SetThreadIdealProcessor', 'GetCurrentProcessorNumber', 'Run', 'CreateMutex',
            'ReleaseMutex', 'CreateSemaphore', 'ReleaseSemaphore', 'CreateEvent', 'SignalEvent', 'ClearEvent',
            'CreateTimer', 'SetTimer', 'CancelTimer', 'ClearTimer', 'CreateMemoryBlock', 'MapMemoryBlock',
            'UnmapMemoryBlock', 'CreateAddressArbiter', 'ArbitrateAddress', 'CloseHandle', 'WaitSynchronization1',
            'WaitSynchronizationN', 'SignalAndWait', 'DuplicateHandle', 'GetSystemTick', 'GetHandleInfo',
            'GetSystemInfo', 'GetProcessInfo', 'GetThreadInfo', 'ConnectToPort', 'SendSyncRequest1',
            'SendSyncRequest2', 'SendSyncRequest3', 'SendSyncRequest4', 'SendSyncRequest', 'OpenProcess',
            'OpenThread', 'GetProcessId', 'GetProcessIdOfThread', 'GetThreadId', 'GetResourceLimit',
            'GetResourceLimitLimitValues', 'GetResourceLimitCurrentValues', 'GetThreadContext', 'Break',
            'OutputDebugString', 'ControlPerformanceCounter', 'CreatePort', 'CreateSessionToPort', 'CreateSession',
            'AcceptSession', 'ReplyAndReceive1', 'ReplyAndReceive2', 'ReplyAndReceive3', 'ReplyAndReceive4',
            'ReplyAndReceive', 'BindInterrupt', 'UnbindInterrupt', 'InvalidateProcessDataCache',
            'StoreProcessDataCache', 'FlushProcessDataCache', 'StartInterProcessDma', 'StopDma', 'GetDmaState',
            'RestartDma', 'DebugActiveProcess', 'BreakDebugProcess', 'TerminateDebugProcess', 'GetProcessDebugEvent',
            'ContinueDebugEvent', 'GetProcessList', 'GetThreadList', 'GetDebugThreadContext', 'SetDebugThreadContext',
            'QueryDebugProcessMemory', 'ReadProcessMemory', 'WriteProcessMemory', 'SetHardwareBreakPoint',
            'GetDebugThreadParam', 'ControlProcessMemory', 'MapProcessMemory', 'UnmapProcessMemory', 'CreateCodeSet',
            'CreateProcess', 'TerminateProcess', 'SetProcessResourceLimits', 'CreateResourceLimit',
            'SetResourceLimitValues', 'AddCodeSegment', 'Backdoor', 'KernelSetState', 'QueryProcessMemory']
SYSCALL_IDS = list(range(1, 63)) + list(range(71, 89)) + list(range(96, 110)) + list(range(112, 116)) + list(range(117, 126))
DEPENDENCIES = [('ac', '2402'), ('act', '3802'), ('am', '1502'), ('boss', '3402'), ('camera', '1602'), ('cecd', '2602'),
                ('cfg', '1702'), ('codec', '1802'), ('csnd', '2702'), ('dlp', '2802'), ('dsp', '1a02'),
                ('friends', '3202'), ('gpio', '1b02'), ('gsp', '1c02'), ('hid', '1d02'), ('http', '2902'),
                ('i2c', '1e02'), ('ir', '3302'), ('mcu', '1f02'), ('mic', '2002'), ('ndm', '2b02'), ('news', '3502'),
                ('nim', '2c02'), ('nwm', '2d02'), ('pdn', '2102'), ('ps', '3102'), ('ptm', '2202'), ('ro', '3702'),
                ('socket', '2e02'), ('spi', '2302'), ('ssl', '2f02')]


def wad_lump(path, name):
    data = open(path, 'rb').read()
    n, off = struct.unpack('<4xii', data[:12])
    for k in range(n):
        pos, size, nm = struct.unpack('<ii8s', data[off + 16 * k:off + 16 * k + 16])
        if nm.split(b'\0')[0].decode('latin1').upper() == name.upper():
            return data[pos:pos + size]
    raise KeyError('%s not in %s' % (name, path))


def lump_ref(ref):
    iwad, name = ref.split(':')
    return IWADS[iwad], name


def picture(data, pal):
    """Doom picture (patch) format -> RGB array on black."""
    w, h, _, _ = struct.unpack('<hhhh', data[:8])
    img = np.zeros((h, w, 3), np.uint8)
    offs = struct.unpack('<%dI' % w, data[8:8 + 4 * w])
    for x, p in enumerate(offs):
        while data[p] != 0xFF:
            top, n = data[p], data[p + 1]
            img[top:top + n, x] = pal[np.frombuffer(data[p + 3:p + 3 + n], np.uint8)]
            p += n + 4
    return img


def source_file(ref):
    """A Build sheet file reference: relative to the project, else to the folder above it (gzdoom/, Banner/)."""
    for base in (ROOT, os.path.dirname(ROOT)):
        if os.path.exists(os.path.join(base, ref)):
            return os.path.join(base, ref)
    return None


def banner_png(ref, dst):
    if source_file(ref):	# a picture file (e.g. gen/banner.png from tools/gen_banner.py)
        Image.open(source_file(ref)).convert('RGB').resize((256, 128), Image.LANCZOS).save(dst)
        return
    wad, name = lump_ref(ref)
    pal = sprite_patch.read_playpal(wad).astype(np.uint8)
    pic = Image.fromarray(picture(wad_lump(wad, name), pal))
    # Doom pictures are drawn for 4:3 (320x200 shown at 320x240): keep that aspect, letterbox to 256x128
    h = 128
    w = round(pic.width * (h / pic.height) * (200 / 240))
    out = Image.new('RGB', (256, 128))
    out.paste(pic.resize((w, h), Image.LANCZOS), ((256 - w) // 2, 0))
    out.save(dst)


def write_wav(dst, pcm, rate):
    channels = pcm.shape[1] if pcm.ndim == 2 else 1	# frames x channels, written interleaved
    body = pcm.astype('<i2').tobytes()
    open(dst, 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVE' +
                          b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * 2 * channels, 2 * channels, 16) +
                          b'data' + struct.pack('<I', len(body)) + body)


def resample(pcm, rate, out_rate):
    if out_rate == rate:
        return pcm
    if rate % out_rate == 0:	# integer factor: average groups (box low-pass + decimate)
        f = rate // out_rate
        return pcm[: len(pcm) // f * f].reshape(-1, f).mean(axis=1)
    n = int(len(pcm) * out_rate / rate)
    return np.interp(np.arange(n) * rate / out_rate, np.arange(len(pcm)), pcm).astype(np.float32)


def banner_wav(ref, dst, out_rate=None, seconds=3.0, channels=1):
    if source_file(ref):	# an audio file (mp3/ogg/wav): resampled, trimmed, 0.3 s fade-out
        import soundfile as sf
        pcm, rate = sf.read(source_file(ref), dtype='float32', always_2d=True)
        if channels == 2 and pcm.shape[1] >= 2:
            pcm = pcm[:, :2]
        else:	# mono source or mono banner: mix down, then copy to every output channel
            pcm = np.repeat(pcm.mean(axis=1, keepdims=True), channels, axis=1)
        out_rate = int(out_rate or rate)
        pcm = np.stack([resample(pcm[:, c], rate, out_rate) for c in range(channels)], axis=1)
        pcm = pcm[: int(out_rate * seconds)].copy()
        fade = min(len(pcm), int(out_rate * 0.3))
        pcm[len(pcm) - fade:] *= np.linspace(1, 0, fade)[:, None]
        write_wav(dst, np.clip(pcm * 32767, -32768, 32767).astype(np.int16), out_rate)
        return
    wad, name = lump_ref(ref)
    d = wad_lump(wad, name)
    fmt, rate, n = struct.unpack('<HHI', d[:8])
    assert fmt == 3, 'not a DMX sound: ' + ref
    pcm = np.frombuffer(d[8 + 16:8 + n - 16], np.uint8).astype(np.int16)  # 16 pad bytes each side
    pcm = ((pcm - 128) << 8)[: rate * 3]  # banner tunes are limited to 3 s
    body = pcm.astype('<i2').tobytes()
    open(dst, 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVE' +
                          b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16) +
                          b'data' + struct.pack('<I', len(body)) + body)


def rsf(B):
    ram = '%dMB' % int(B['app_ram_mb'])
    cpu = str(B['cpu'])
    lines = ['# GENERATED by tools/gen_cia.py from sheets/build.xlsx (Build) -- edit the sheet',
             'BasicInfo:',
             '  Title: "%s"' % B['short_title'][:8].replace(' ', ''),
             '  ProductCode: "%s"' % B['product_code'],
             '  Logo: Homebrew',
             '', 'RomFs:', '  RootPath: build/romfs',
             '', 'TitleInfo:', '  Category: Application', '  UniqueId: %s' % B['unique_id'],
             '', 'Option:', '  UseOnSD: true', '  FreeProductCode: true', '  MediaFootPadding: false',
             '  EnableCrypt: false', '  EnableCompress: true',
             '', 'AccessControlInfo:',
             '  CoreVersion: 2', '  DescVersion: 2', '  ReleaseKernelMajor: "02"', '  ReleaseKernelMinor: "33"',
             '  UseExtSaveData: false',
             '  FileSystemAccess:'] + ['    - ' + a for a in B['fs_access'].split()] + [
             # kernel memory maps: NDSP writes DSP shared memory (ndspInitialize data-aborted at 0x1FF57FFE on
             # hardware without it; a 3dsx inherits these from the Homebrew Launcher, Azahar does not enforce them)
             '  MemoryMapping:', '    - 1f000000-1f5fffff:r  # VRAM',
             '  IORegisterMapping:', '    - 1ff00000-1ff7ffff  # DSP memory',
             '  MemoryType: Application',
             '  SystemMode: %s' % B['system_mode'],
             '  SystemModeExt: %s' % ram,
             '  CpuSpeed: %s' % ('804MHz' if '804' in cpu else '268MHz'),
             '  EnableL2Cache: %s' % ('true' if 'L2' in cpu else 'false'),
             '  CanAccessCore2: true',
             '  IdealProcessor: 0', '  AffinityMask: 1', '  Priority: 16', '  MaxCpu: 0x9E',
             '  HandleTableSize: 0x200', '  DisableDebug: false', '  EnableForceDebug: false',
             '  CanWriteSharedPage: true', '  CanUsePrivilegedPriority: false', '  CanUseNonAlphabetAndNumber: true',
             '  PermitMainFunctionArgument: true', '  CanShareDeviceMemory: true', '  RunnableOnSleep: false',
             '  SpecialMemoryArrange: true',
             '  ServiceAccessControl:'] + ['    - %s' % s for s in SERVICES] + \
            ['  SystemCallAccess:'] + ['    %s: %d' % kv for kv in zip(SYSCALLS, SYSCALL_IDS)] + \
            ['', 'SystemControlInfo:', '  SaveDataSize: 0KB', '  RemasterVersion: 0',
             '  StackSize: 0x%X' % (int(B['stack_kb']) * 1024),
             '  Dependency:'] + ['    %s: 0x000401300000%s' % d for d in DEPENDENCIES]
    return '\n'.join(lines) + '\n'


def main():
    assert len(SYSCALLS) == len(SYSCALL_IDS)
    B = {r['key']: r.get('value', '~') for r in rows('build', 'Build')}
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, 'doomlauncher3ds.rsf'), 'w', newline='\n').write(rsf(B))
    banner_png(B['banner_image'], os.path.join(OUT, 'banner.png'))
    banner_wav(B['banner_audio'], os.path.join(OUT, 'banner.wav'), B.get('banner_audio_rate'), float(B.get('banner_audio_s', 3)),
               int(B.get('banner_audio_channels', 1)))
    bt = B['bannertool_exe']
    run = lambda *a: subprocess.run([bt] + list(a), check=True, capture_output=True, text=True)
    if B.get('banner_mode') == '3d':	# spinning voxel model from tools/gen_banner3d.py
        picture = ['-ci', os.path.join(ROOT, 'gen', 'banner.cgfx')]
    else:
        picture = ['-i', os.path.join(OUT, 'banner.png')]
    run('makebanner', *picture, '-a', os.path.join(OUT, 'banner.wav'), '-o', os.path.join(OUT, 'banner.bnr'))
    run('makesmdh', '-s', B['short_title'], '-l', B['title'], '-p', B['publisher'],
        '-i', os.path.join(ROOT, 'platform', '3ds', 'icon.png'), '-f', B['smdh_flags'], '-r', B['smdh_regions'],
        '-o', os.path.join(OUT, 'icon.smdh'))
    tid = '00040000%06X00' % int(B['unique_id'], 16)
    print(OUT, 'rsf + banner.bnr + icon.smdh, title ID', tid)


if __name__ == '__main__':
    main()
