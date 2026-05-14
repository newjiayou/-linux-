import struct

packet = struct.pack("!I H", 6, 2)
with open("hb.bin", "wb") as f:
    f.write(packet)