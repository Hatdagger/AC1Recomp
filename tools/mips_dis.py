"""Minimal MIPS R3000 (PS1) disassembler.
usage: python3 tools/mips_dis.py module.bin load_addr start end   (addresses in hex)
module.bin = an FDAT entry dumped from the disc (menus 206 load at 80050DD8)."""
import sys, struct
R=['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
   's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
def s16(x): return x-0x10000 if x&0x8000 else x
def dis(w,pc):
    op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; rd=(w>>11)&31; sh=(w>>6)&31; fn=w&63; imm=w&0xFFFF; si=s16(imm)
    tgt=((pc+4)&0xF0000000)|((w&0x3FFFFFF)<<2); br=pc+4+(si<<2)
    if w==0: return 'nop'
    if op==0:
        n={0:'sll',2:'srl',3:'sra'}.get(fn)
        if n: return f'{n} {R[rd]},{R[rt]},{sh}'
        n={4:'sllv',6:'srlv',7:'srav'}.get(fn)
        if n: return f'{n} {R[rd]},{R[rt]},{R[rs]}'
        if fn==8: return f'jr {R[rs]}'
        if fn==9: return f'jalr {R[rd]},{R[rs]}'
        if fn in(12,13): return ['syscall','break'][fn-12]
        if fn in(16,18): return f"{'mfhi' if fn==16 else 'mflo'} {R[rd]}"
        if fn in(17,19): return f"{'mthi' if fn==17 else 'mtlo'} {R[rs]}"
        n={24:'mult',25:'multu',26:'div',27:'divu'}.get(fn)
        if n: return f'{n} {R[rs]},{R[rt]}'
        n={32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',42:'slt',43:'sltu'}.get(fn)
        if n: return f'{n} {R[rd]},{R[rs]},{R[rt]}'
        return f'?special {fn}'
    if op==1:
        n={0:'bltz',1:'bgez',16:'bltzal',17:'bgezal'}.get(rt,'?regimm'); return f'{n} {R[rs]},{br:08X}'
    if op==2: return f'j {tgt:08X}'
    if op==3: return f'jal {tgt:08X}'
    if op in(4,5): return f"{'beq' if op==4 else 'bne'} {R[rs]},{R[rt]},{br:08X}"
    if op in(6,7): return f"{'blez' if op==6 else 'bgtz'} {R[rs]},{br:08X}"
    n={8:'addi',9:'addiu',10:'slti',11:'sltiu'}.get(op)
    if n: return f'{n} {R[rt]},{R[rs]},{si}'
    n={12:'andi',13:'ori',14:'xori'}.get(op)
    if n: return f'{n} {R[rt]},{R[rs]},0x{imm:X}'
    if op==15: return f'lui {R[rt]},0x{imm:X}'
    if op in(16,18):
        if rs==4: return f'mtc{op-16} {R[rt]},${rd}'
        if rs==0: return f'mfc{op-16} {R[rt]},${rd}'
        if rs==6: return f'ctc{op-16} {R[rt]},${rd}'
        if rs==2: return f'cfc{op-16} {R[rt]},${rd}'
        return f'cop{op-16} 0x{w&0x1FFFFFF:X}'
    n={32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',40:'sb',41:'sh',42:'swl',43:'sw',46:'swr',50:'lwc2',58:'swc2'}.get(op)
    if n: return f'{n} {R[rt]},{si}({R[rs]})'
    return f'?op {op}'
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read(); base=int(sys.argv[2],16); a=int(sys.argv[3],16); e=int(sys.argv[4],16)
    for pc in range(a,e,4):
        w=struct.unpack('<I',d[pc-base:pc-base+4])[0]; print(f'{pc:08X}: {w:08X}  {dis(w,pc)}')
