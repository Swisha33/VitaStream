import socket, struct, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", port))
def qname(data):
    i, parts = 12, []
    while data[i]:
        l = data[i]; parts.append(data[i+1:i+1+l].decode()); i += l + 1
    return ".".join(parts), i + 5
def name_bytes(n): return b"".join(bytes([len(p)]) + p.encode() for p in n.split(".")) + b"\0"
while True:
    data, addr = s.recvfrom(512)
    name, qend = qname(data)
    rid = data[:2]; question = data[12:qend]
    answers, rcode = [], 0
    if name == "nx.test": rcode = 3
    elif name == "ads.test": answers.append((b"\xc0\x0c", 1, 60, bytes([0,0,0,0])))
    elif name == "cname.test":
        tgt = name_bytes("real.example")
        answers.append((b"\xc0\x0c", 5, 300, tgt))
        answers.append((tgt, 1, 300, bytes([10,0,0,7])))
    elif name == "drop.test": continue
    else: answers.append((b"\xc0\x0c", 1, 120, bytes([93,184,216,34])))
    hdr = rid + struct.pack(">HHHHH", 0x8180 | rcode, 1, len(answers), 0, 0)
    body = question
    for nm, t, ttl, rd in answers: body += nm + struct.pack(">HHIH", t, 1, ttl, len(rd)) + rd
    if name == "wrongid.test":
        s.sendto(b"\xff\xff" + hdr[2:] + body, addr)   # fremde ID zuerst
    s.sendto(hdr + body, addr)
