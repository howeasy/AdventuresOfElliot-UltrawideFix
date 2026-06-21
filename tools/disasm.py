import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r'G:/SteamLibrary/steamapps/common/The Adventures of Elliot_The Millennium Tales/Elliot/Binaries/Win64/Elliot-Win64-Shipping.exe'
raw = open(EXE, 'rb').read()
e = struct.unpack_from('<I', raw, 0x3c)[0]
ns = struct.unpack_from('<H', raw, e+6)[0]
so = struct.unpack_from('<H', raw, e+20)[0]
secs = e+24+so

def rva2off(rva):
    for i in range(ns):
        o = secs+i*40
        va = struct.unpack_from('<I', raw, o+12)[0]
        vs = struct.unpack_from('<I', raw, o+8)[0]
        ra = struct.unpack_from('<I', raw, o+20)[0]
        if va <= rva < va+max(vs, 1):
            return ra+(rva-va)
    return None

def f32(rva):
    o = rva2off(rva)
    if o is None: return None
    return struct.unpack_from('<f', raw, o)[0]

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def disasm(start, length, label=''):
    print(f'\n===== {label} @ +{start:x} (len {length}) =====')
    off = rva2off(start)
    code = raw[off:off+length]
    for ins in md.disasm(code, start):
        note = ''
        m, op = ins.mnemonic, ins.op_str
        if m == 'call' and op.startswith('0x'):
            note = '   --> +%x' % int(op, 16)
        # resolve rip-relative float loads
        if m in ('movss', 'movsd', 'movaps', 'movups', 'movdqa', 'comiss', 'ucomiss', 'addss', 'mulss', 'divss', 'subss') and 'rip' in op:
            try:
                disp = ins.disp
                tgt = ins.address + ins.size + disp
                v = f32(tgt)
                if v is not None:
                    note += '   ; [+%x]=%g (0x%08x)' % (tgt, v, struct.unpack('<I', struct.pack('<f', v))[0])
            except Exception:
                pass
        if m == 'lea' and 'rip' in op:
            try:
                tgt = ins.address + ins.size + ins.disp
                note += '   ; ->+%x' % tgt
            except Exception:
                pass
        print(f'+{ins.address:7x}  {ins.bytes.hex():<20s} {m:7s} {op}{note}')

if __name__ == '__main__':
    start = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x35fdbd0
    length = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xad4
    disasm(start, length, sys.argv[3] if len(sys.argv) > 3 else 'func')
