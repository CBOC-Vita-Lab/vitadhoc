# Copyright CBOC-Vita. MIT licence.
"""A very small 6502 assembler, enough for the synthetic test cartridges of the NES twin.

Syntax, one statement per line, ';' starts a comment:
    NAME = $10            constant (value below 256 means zero page when used as an operand)
    label:                code label (always absolute)
    lda #$10 / lda $2002 / lda tab,x / lda (ptr),y / asl a / jmp label / bne label
    .org $8000            set the address (the output starts at the first .org)
    .byte 1,2,$ff,"txt"   bytes
    .word label           16 bit little endian
    .fill n,value         n bytes of value
Two passes; branches are checked to be in range.
"""
import re

# opcode tables: mnemonic -> {mode: opcode}
_OPS = {
 'adc': {'imm':0x69,'zp':0x65,'zpx':0x75,'abs':0x6D,'abx':0x7D,'aby':0x79,'izy':0x71},
 'and': {'imm':0x29,'zp':0x25,'zpx':0x35,'abs':0x2D,'abx':0x3D,'aby':0x39,'izy':0x31},
 'asl': {'acc':0x0A,'zp':0x06,'zpx':0x16,'abs':0x0E,'abx':0x1E},
 'bit': {'zp':0x24,'abs':0x2C},
 'cmp': {'imm':0xC9,'zp':0xC5,'zpx':0xD5,'abs':0xCD,'abx':0xDD,'aby':0xD9,'izy':0xD1},
 'cpx': {'imm':0xE0,'zp':0xE4,'abs':0xEC},
 'cpy': {'imm':0xC0,'zp':0xC4,'abs':0xCC},
 'dec': {'zp':0xC6,'zpx':0xD6,'abs':0xCE,'abx':0xDE},
 'eor': {'imm':0x49,'zp':0x45,'zpx':0x55,'abs':0x4D,'abx':0x5D,'aby':0x59,'izy':0x51},
 'inc': {'zp':0xE6,'zpx':0xF6,'abs':0xEE,'abx':0xFE},
 'jmp': {'abs':0x4C},
 'jsr': {'abs':0x20},
 'lda': {'imm':0xA9,'zp':0xA5,'zpx':0xB5,'abs':0xAD,'abx':0xBD,'aby':0xB9,'izy':0xB1},
 'ldx': {'imm':0xA2,'zp':0xA6,'zpy':0xB6,'abs':0xAE,'aby':0xBE},
 'ldy': {'imm':0xA0,'zp':0xA4,'zpx':0xB4,'abs':0xAC,'abx':0xBC},
 'lsr': {'acc':0x4A,'zp':0x46,'zpx':0x56,'abs':0x4E,'abx':0x5E},
 'ora': {'imm':0x09,'zp':0x05,'zpx':0x15,'abs':0x0D,'abx':0x1D,'aby':0x19,'izy':0x11},
 'rol': {'acc':0x2A,'zp':0x26,'zpx':0x36,'abs':0x2E,'abx':0x3E},
 'ror': {'acc':0x6A,'zp':0x66,'zpx':0x76,'abs':0x6E,'abx':0x7E},
 'sbc': {'imm':0xE9,'zp':0xE5,'zpx':0xF5,'abs':0xED,'abx':0xFD,'aby':0xF9,'izy':0xF1},
 'sta': {'zp':0x85,'zpx':0x95,'abs':0x8D,'abx':0x9D,'aby':0x99,'izy':0x91},
 'stx': {'zp':0x86,'zpy':0x96,'abs':0x8E},
 'sty': {'zp':0x84,'zpx':0x94,'abs':0x8C},
}
_IMPL = {'clc':0x18,'cld':0xD8,'cli':0x58,'clv':0xB8,'dex':0xCA,'dey':0x88,'inx':0xE8,'iny':0xC8,'nop':0xEA,
         'pha':0x48,'php':0x08,'pla':0x68,'plp':0x28,'rti':0x40,'rts':0x60,'sec':0x38,'sed':0xF8,'sei':0x78,
         'tax':0xAA,'tay':0xA8,'tsx':0xBA,'txa':0x8A,'txs':0x9A,'tya':0x98}
_BRANCH = {'bcc':0x90,'bcs':0xB0,'beq':0xF0,'bmi':0x30,'bne':0xD0,'bpl':0x10,'bvc':0x50,'bvs':0x70}


class AsmError(Exception):
    pass


def _num(tok, syms, line):
    tok = tok.strip()
    if tok.startswith('<'):
        return _num(tok[1:], syms, line) & 0xFF
    if tok.startswith('>'):
        return (_num(tok[1:], syms, line) >> 8) & 0xFF
    m = re.match(r'^(\w+)\s*([+-])\s*(\w+)$', tok)
    if m and not re.match(r'^\$?[0-9a-fA-F]+$', tok):
        a = _num(m.group(1), syms, line); b = _num(m.group(3), syms, line)
        return a + b if m.group(2) == '+' else a - b
    if tok.startswith('$'):
        return int(tok[1:], 16)
    if tok.startswith('%'):
        return int(tok[1:], 2)
    if re.match(r'^\d+$', tok):
        return int(tok)
    if tok in syms:
        return syms[tok]
    raise AsmError('unknown symbol %r in %r' % (tok, line))


def assemble(src):
    lines = []
    for raw in src.split('\n'):
        l = raw.split(';')[0].strip()
        if l:
            lines.append(l)
    syms = {}
    zp_syms = set()
    out_start = None
    for pas in (1, 2):
        pc = 0
        out = bytearray()
        for line in lines:
            m = re.match(r'^(\w+)\s*=\s*(.+)$', line)
            if m:
                if pas == 1:
                    v = _num(m.group(2), syms, line)
                    syms[m.group(1)] = v
                    if v < 256:
                        zp_syms.add(m.group(1))
                continue
            m = re.match(r'^(\w+):\s*(.*)$', line)
            if m:
                if pas == 1:
                    syms[m.group(1)] = pc
                line = m.group(2).strip()
                if not line:
                    continue
            if line.startswith('.org'):
                pc = _num(line.split(None, 1)[1], syms, line)
                if out_start is None:
                    out_start = pc
                elif pas == 2:
                    if pc < out_start + len(out):
                        raise AsmError('.org moves backwards: ' + line)
                    out.extend(b'\x00' * (pc - out_start - len(out)))
                continue
            if out_start is None:
                raise AsmError('code before .org: ' + line)
            if line.startswith('.byte'):
                vals = []
                for t in re.findall(r'"[^"]*"|[^,]+', line[5:]):
                    t = t.strip()
                    if t.startswith('"'):
                        vals.extend(t[1:-1].encode('latin-1'))
                    else:
                        vals.append(_num(t, syms, line) & 0xFF if pas == 2 or re.match(r'^[$%]?\w+$', t) and t in syms or re.match(r'^[$%\d]', t) else 0)
                out.extend(vals); pc += len(vals)
                continue
            if line.startswith('.word'):
                for t in line[5:].split(','):
                    v = _num(t, syms, line) if pas == 2 else 0
                    out.extend([v & 0xFF, v >> 8]); pc += 2
                continue
            if line.startswith('.fill'):
                a = line[5:].split(',')
                n = _num(a[0], syms, line); v = _num(a[1], syms, line) if len(a) > 1 else 0
                out.extend([v & 0xFF] * n); pc += n
                continue
            parts = line.split(None, 1)
            mn = parts[0].lower()
            arg = parts[1].strip() if len(parts) > 1 else ''
            if mn in _IMPL:
                out.append(_IMPL[mn]); pc += 1
                continue
            if mn in _BRANCH:
                tgt = _num(arg, syms, line) if pas == 2 else pc
                off = tgt - (pc + 2)
                if pas == 2 and not -128 <= off <= 127:
                    raise AsmError('branch out of range: ' + line)
                out.extend([_BRANCH[mn], off & 0xFF]); pc += 2
                continue
            if mn not in _OPS:
                raise AsmError('unknown mnemonic: ' + line)
            tab = _OPS[mn]
            a = arg
            mode = None; val = 0
            if a.lower() == 'a' and 'acc' in tab:
                mode = 'acc'
            elif a.startswith('#'):
                mode = 'imm'; val = _num(a[1:], syms, line) if pas == 2 else 0
            elif re.match(r'^\((.+)\)\s*,\s*[yY]$', a):
                mode = 'izy'; val = _num(re.match(r'^\((.+)\)', a).group(1), syms, line) if pas == 2 else 0
            else:
                idx = ''
                m2 = re.match(r'^(.+),\s*([xXyY])$', a)
                if m2:
                    a, idx = m2.group(1), m2.group(2).lower()
                base = a.strip()
                is_zp = False
                if re.match(r'^\$[0-9a-fA-F]{1,2}$', base):
                    is_zp = True
                elif base in zp_syms:
                    is_zp = True
                elif re.match(r'^(\w+)\s*[+-]\s*\w+$', base) and re.match(r'^(\w+)', base).group(1) in zp_syms:
                    is_zp = True
                v = _num(base, syms, line) if pas == 2 or is_zp else 0
                val = v
                if mn in ('jmp', 'jsr'):
                    mode = 'abs'
                else:
                    mode = ('zp' if is_zp else 'abs') + {'': '', 'x': 'x', 'y': 'y'}[idx]
                    if mode in ('absx', 'absy'):
                        mode = 'abx' if idx == 'x' else 'aby'
                    if mode == 'zpy' and 'zpy' not in tab:
                        mode = 'aby'
                    if mode == 'zpx' and 'zpx' not in tab:
                        mode = 'abx'
            if mode not in tab:
                if mode == 'zp' and 'abs' in tab:
                    mode = 'abs'
                elif mode in ('zpx',) and 'abx' in tab:
                    mode = 'abx'
                elif mode in ('zpy',) and 'aby' in tab:
                    mode = 'aby'
                else:
                    raise AsmError('bad addressing mode %s for %s' % (mode, line))
            op = tab[mode]
            if mode in ('acc',):
                out.append(op); pc += 1
            elif mode in ('imm', 'zp', 'zpx', 'zpy', 'izy'):
                out.extend([op, val & 0xFF]); pc += 2
            else:
                out.extend([op, val & 0xFF, (val >> 8) & 0xFF]); pc += 3
    return out_start, bytes(out), syms
