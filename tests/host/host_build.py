"""Run recovered host checks with GCC, or Zig on Windows (set ZIG_CC).

Windows checks use native driver mocks; ASan/UBSan remain enabled on Linux.
"""
import os, subprocess

def run(args, **kwargs):
    args=list(map(str,args))
    if args and args[0]=='gcc' and os.name=='nt':
        compiler=os.environ.get('ZIG_CC')
        if not compiler: raise RuntimeError('Set ZIG_CC to the installed zig.exe path')
        args=[compiler,'cc']+[a for a in args[1:] if not a.startswith('-fsanitize=')]
        # Zig's optimized C mode defines NDEBUG. Keep test assertions active.
        args+=['-D_CRT_SECURE_NO_WARNINGS','-UNDEBUG']
    return subprocess.run(args,**kwargs)
