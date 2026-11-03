"""Synthetic GPU integration test: no game files; requires the built Windows Vulkan bridge."""
import argparse
import ctypes as c
import os
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, required=True)
    args = parser.parse_args()
    directory = args.dll.resolve().parent
    dll_path = args.dll.resolve()
    dll_directory = os.add_dll_directory(str(directory))
    os.chdir(directory) # PCSX2 delay-loads shaderc; keep test outputs outside the checkout.
    gs = c.CDLL(str(dll_path))
    u = c.c_uint32
    signatures = {
        'open': ([c.c_int, c.c_int, c.c_char_p, c.c_void_p, u], c.c_int),
        'close': ([], None), 'reset': ([], None), 'sync': ([], None),
        'configure': ([c.c_int, c.c_int], None),
        'gif': ([u, c.c_void_p, u], None), 'vsync': ([c.c_void_p], None),
        'frame': ([c.c_void_p, u, c.POINTER(u), c.POINTER(u)], u),
        'read_rect': ([u]*7 + [c.c_void_p, u], u),
        'read_pixel': ([u]*5, u), 'write_pixel': ([u]*6, None),
        'snapshot': ([c.c_void_p, u], None), 'upload_vram': ([c.c_void_p, u], None),
    }
    for name, (inputs, output) in signatures.items():
        fn = getattr(gs, 'tf_gs_' + name)
        fn.argtypes, fn.restype = inputs, output
    error = c.create_string_buffer(2048)
    if not gs.tf_gs_open(1, 0, str(directory/'resources').encode(), error, len(error)):
        raise RuntimeError(error.value.decode(errors='replace'))
    try:
        # Upload and snapshot all VRAM; verify the byte contract before rendering.
        vram = c.create_string_buffer(bytes(range(256)) * (4*1024*1024//256))
        result = c.create_string_buffer(4*1024*1024)
        gs.tf_gs_upload_vram(vram, len(result))
        gs.tf_gs_snapshot(result, len(result))
        assert result.raw == vram.raw[:len(result)], 'VRAM round trip'
        # Exercise transfer formats, nonzero base, stride, and odd rectangle widths.
        for psm, bits in [(0,32),(1,24),(2,16),(0x13,8),(0x14,4)]:
            mask = (1 << bits)-1
            expected = []
            for y in range(3):
                for x in range(5):
                    value = (x + y*5 + 1) & mask
                    expected.append(value)
                    gs.tf_gs_write_pixel(psm, 8192, 2, x+3, y+2, value)
                    assert gs.tf_gs_read_pixel(psm,8192,2,x+3,y+2) == value, ('pixel',psm,x,y)
            count = (len(expected)*bits+7)//8
            buffer = c.create_string_buffer(count)
            assert gs.tf_gs_read_rect(psm,8192,2,3,2,5,3,buffer,count) == count, ('rectangle size',psm)
            packed = int.from_bytes(buffer.raw, 'little')
            actual = [(packed >> (i*bits)) & mask for i in range(len(expected))]
            assert actual == expected, ('rectangle pixels',psm,actual,expected)
            assert gs.tf_gs_read_rect(psm,8192,2,3,2,5,3,buffer,count-1) == 0, 'capacity guard'
        gs.tf_gs_reset()
        registers = bytearray(8192)
        for offset, value in {
            0:1|(1<<2)|(1<<5)|(255<<8), 0x10:4|(32<<3)|(2<<13),
            0x20:1, 0x70:10<<9,
            0x80:636|(50<<12)|(3<<23)|(2559<<32)|(447<<44),
        }.items():
            struct.pack_into('<Q', registers, offset, value)
        q = []
        def ad(reg, value): q.extend((value,reg))
        def xy(x,y): return (x*16)|((y*16)<<16)
        ad(0x4c,10<<16); ad(0x4e,140|(1<<32)); ad(0x18,0)
        ad(0x40,(639<<16)|(447<<48)); ad(0x47,(1<<16)|(1<<17))
        ad(0x1a,1); ad(0x45,0); ad(0x46,1); ad(0x49,0); ad(0x4a,0)
        ad(0,6); ad(1,0x3f80000080000080)
        ad(5,xy(0,0)); ad(5,xy(640,448))
        ad(0,3); ad(1,0x3f8000008000ff00)
        ad(5,xy(100,80)); ad(5,xy(500,97)); ad(5,xy(227,350))
        packet = struct.pack('<'+'Q'*(len(q)+2),len(q)//2|(1<<15)|(1<<60),14,*q)
        sizes = []
        for scale,fxaa in [(1,0),(4,0),(4,1),(1,0)]:
            gs.tf_gs_configure(scale,fxaa)
            gs.tf_gs_gif(3,packet,len(packet))
            gs.tf_gs_vsync(bytes(registers))
            w,h = u(),u()
            count = gs.tf_gs_frame(None,0,c.byref(w),c.byref(h))
            assert count == w.value*h.value*4 and count, ('frame size',w.value,h.value,count)
            pixels = c.create_string_buffer(count)
            gs.tf_gs_frame(pixels,count,None,None)
            sizes.append((w.value,h.value))
            if scale == 1: assert sizes[-1] == (640,448), sizes
            else: assert sizes[-1] == (2560,1792), sizes
            # Both a background and an interior triangle sample survive scaling and FXAA.
            for x,y,rgb in [(20,20,b'\x80\0\0'),(230,180,b'\0\xff\0')]:
                offset = ((y*scale)*w.value+x*scale)*4
                assert pixels.raw[offset:offset+3] == rgb, ('rendered color',scale,fxaa,x,y)
            # Verify actual subpixel geometry, not a resized 640x448 image.
            if scale == 4 and not fxaa:
                p=pixels.raw
                mixed=0
                for y in range(80,350):
                    for x in range(100,500):
                        base=((y*4)*w.value+x*4)*4
                        if any(p[base:base+3] != p[base+dy*w.value*4+dx*4:base+dy*w.value*4+dx*4+3]
                               for dx,dy in [(3,0),(0,3),(3,3)]): mixed+=1
                assert mixed > 100, ('subpixel geometry',mixed)
                print('Subpixel edge blocks:',mixed,flush=True)
            # Download rendered GPU pixels into guest VRAM, independent of presentation.
            rect = c.create_string_buffer(4*4*4)
            assert gs.tf_gs_read_rect(0,0,10,230,180,4,4,rect,64) == 64
            assert all(rect.raw[i:i+3] == b'\0\xff\0' for i in range(0,64,4)), 'GPU-to-guest readback'
            print('PASS scale',scale,'FXAA',fxaa,'frame',sizes[-1],flush=True)
    finally:
        gs.tf_gs_close()
        dll_directory.close()


if __name__ == '__main__':
    main()
