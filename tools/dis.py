import struct,rabbitizer,sys
path,base,start,end=sys.argv[1],int(sys.argv[2],0),int(sys.argv[3],0),int(sys.argv[4],0)
b=open(path,"rb").read()
for a in range(start,end,4):
    o=a-base; w=struct.unpack(">I",b[o:o+4])[0]
    print(f"{a:08X} {w:08X} {rabbitizer.Instruction(w,a).disassemble()}")
