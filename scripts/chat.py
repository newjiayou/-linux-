import struct

body = b'{"sender":"u1","target":"broadcast","message":"hello"}'
packet = struct.pack("!I H", 6 + len(body), 1) + body

with open("chat.bin", "wb") as f:
    f.write(packet)