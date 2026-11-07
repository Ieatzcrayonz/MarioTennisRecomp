"""Compact disassembly of functions by address: python fdis.py bin base addr[:size] ..."""
import sys,struct,rabbitizer
sys.path.insert(0,__file__.rsplit("\\",1)[0].rsplit("/",1)[0])
path,base=sys.argv[1],int(sys.argv[2],0); b=open(path,"rb").read()
for spec in sys.argv[3:]:
    a,_,sz=spec.partition(":"); a=int(a,16); sz=int(sz,16) if sz else None
    o=a-base; out=[]
    i=0; prev=0
    while True:
        w=struct.unpack(">I",b[o+i:o+i+4])[0]; ins=rabbitizer.Instruction(w,a+i)
        out.append(" ".join(ins.disassemble().split()))
        i+=4
        if sz is not None and i>=sz: break
        if sz is None and (prev==0x03E00008 or i>=0x200): break
        prev=w
    print(f"== {a:08X}: "+" ; ".join(out))
