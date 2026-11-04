"""Exercise the new D3D12 compute-to-draw path with synthetic geometry only."""
import argparse
import ctypes as c
import os
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll',type=Path,required=True)
    args=parser.parse_args()
    dll=args.dll.resolve();directory=dll.parent
    checkout=Path(__file__).resolve().parents[1]
    if directory == checkout or checkout in directory.parents:
        raise ValueError('Keep the GPU build and shader cache outside the checkout')
    os.environ['PS2X_HWGS_API']='d3d12'
    os.environ['PS2X_HWGS_DIRECT']='0'
    os.chdir(directory)
    with os.add_dll_directory(str(directory)):
        gs=c.CDLL(str(dll))
        gs.tf_gs_open.argtypes=[c.c_int,c.c_int,c.c_char_p,c.c_void_p,c.c_uint32]
        gs.tf_gs_open.restype=c.c_int
        gs.tf_gs_close.argtypes=[];gs.tf_gs_close.restype=None
        gs.tf_gs_gpu_geometry_probe.argtypes=[c.POINTER(c.c_uint32),c.c_void_p,c.c_uint32]
        gs.tf_gs_gpu_geometry_probe.restype=c.c_int
        error=c.create_string_buffer(2048)
        if not gs.tf_gs_open(1,0,str(directory/'resources').encode(),error,len(error)):
            raise RuntimeError(error.value.decode(errors='replace'))
        try:
            painted=c.c_uint32()
            if not gs.tf_gs_gpu_geometry_probe(c.byref(painted),error,len(error)):
                raise RuntimeError(error.value.decode(errors='replace'))
            print('PASS GPU affine transform -> GPU vertex buffer -> ordered GS draws; painted pixels:',painted.value)
            print('Synthetic component validation only; no live-game offload or FPS claim.')
        finally:gs.tf_gs_close()


if __name__=='__main__':main()
