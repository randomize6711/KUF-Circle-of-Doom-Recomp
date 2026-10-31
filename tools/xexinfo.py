#!/usr/bin/env python3
"""Read-only XEX2 header inspector. Usage: xexinfo.py <default.xex> [out_dir]
Prints optional headers, security info, import libraries. With out_dir, writes the
decrypted (still possibly compressed) payload to out_dir/payload.bin."""
import struct, sys, os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes.fromhex("")  # removed
NAMES = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO", 0x000005FF: "DELTA_PATCH",
    0x00010001: "ORIGINAL_BASE_ADDRESS", 0x00010100: "ENTRY_POINT", 0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES", 0x00018002: "CHECKSUM_TIMESTAMP", 0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP", 0x000183FF: "ORIGINAL_PE_NAME", 0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO", 0x00020200: "DEFAULT_STACK_SIZE", 0x00020301: "DEFAULT_FS_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE", 0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS", 0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO", 0x000401FF: "TITLE_WORKSPACE_SIZE", 0x00040201: "GAME_RATINGS",
    0x00040310: "LAN_KEY", 0x000405FF: "XBOX360_LOGO", 0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS", 0x00040801: "ADDITIONAL_TITLE_MEMORY", 0x00E10402: "EXPORTS_BY_NAME",
}
u32 = lambda b, o: struct.unpack_from(">I", b, o)[0]
u16 = lambda b, o: struct.unpack_from(">H", b, o)[0]

def main():
    data = open(sys.argv[1], "rb").read()
    assert data[:4] == b"XEX2", data[:4]
    flags, pe_off, _, sec_off, n = struct.unpack_from(">5I", data, 4)
    print(f"module_flags={flags:#x} pe_data_offset={pe_off:#x} security_offset={sec_off:#x} opt_headers={n}")
    hdrs = {}
    for i in range(n):
        k, v = struct.unpack_from(">II", data, 24 + i * 8)
        hdrs[k] = v
        print(f"  {k:#010x} {NAMES.get(k, '?'):28s} {v:#x}")
    def blob(k):
        off = hdrs[k]; return data[off:off + u32(data, off)]
    if 0x000183FF in hdrs:
        print("original PE name:", blob(0x000183FF)[4:].split(b"\0")[0].decode())
    if 0x00040006 in hdrs:
        o = hdrs[0x00040006]
        media, ver, basever, tid = struct.unpack_from(">IIII", data, o)
        print(f"execution: media_id={media:#x} version={ver:#x} base_version={basever:#x} title_id={tid:#010x} "
              f"platform={data[o+16]} exec_type={data[o+17]} disc={data[o+18]}/{data[o+19]} savegame_id={u32(data,o+20):#x}")
    if 0x00020104 in hdrs:
        print("tls (slots, raw addr, data size, raw size):", struct.unpack_from(">4I", data, hdrs[0x00020104]))
    for k in (0x00020200, 0x00020401, 0x00030000, 0x00010100, 0x00010201, 0x00020301):
        if k in hdrs: print(f"{NAMES[k]} = {hdrs[k]:#x}")
    if 0x000200FF in hdrs:
        b = blob(0x000200FF); cnt = (len(b) - 4) // 16
        print("static libraries:")
        for i in range(cnt):
            name = b[4 + i*16: 12 + i*16].split(b"\0")[0].decode()
            ma, mi, bu, qf = struct.unpack_from(">HHHH", b, 12 + i*16)
            print(f"  {name:10s} {ma}.{mi}.{bu}.{qf & 0xff} (approval {qf >> 13})")
    if 0x000002FF in hdrs:
        b = blob(0x000002FF)
        for i in range((len(b) - 4) // 16):
            nm = b[4+i*16:12+i*16].split(b"\0")[0].decode(errors="replace")
            print(f"resource {nm!r} addr={u32(b,12+i*16):#x} size={u32(b,16+i*16):#x}")
    # security info
    s = sec_off
    img_size = u32(data, s + 4); flags_img = u32(data, s + 0x10C); load = u32(data, s + 0x110)
    page_cnt = u32(data, s + 0x180)
    print(f"security: image_size={img_size:#x} image_flags={flags_img:#x} load_address={load:#x} page_descriptors={page_cnt}")
    enc_key = data[s + 0x150:s + 0x160]
    region = u32(data, s + 0x178); media = u32(data, s + 0x17C)
    print(f"region={region:#x} allowed_media={media:#x}")
    # sections from page descriptors
    page_size = 0x1000 if flags_img & 0x10000000 else 0x10000
    addr = load; secs = []
    for i in range(page_cnt):
        info = u32(data, s + 0x184 + i * 24)
        cnt, typ = info >> 4, info & 0xF
        if secs and secs[-1][2] == typ: secs[-1][1] += cnt * page_size
        else: secs.append([addr, cnt * page_size, typ])
        addr += cnt * page_size
    tn = {1: "code", 2: "data", 3: "readonly"}
    for a, sz, t in secs: print(f"  section {a:#010x}-{a+sz:#010x} {tn.get(t, t)}")
    ff = data[hdrs[0x000003FF]:]
    enc, comp = u16(ff, 4), u16(ff, 6)
    print(f"file format: encryption={enc} (0=none,1=normal) compression={comp} (1=basic,2=normal/LZX,3=delta)")
    if comp == 2:
        print(f"  lzx window_size={u32(ff, 8):#x}")
    # imports (names only; ordinals need the image)
    if 0x000103FF in hdrs:
        b = blob(0x000103FF)
        strsz, strcnt = u32(b, 4), u32(b, 8)
        names = [x.decode() for x in b[12:12 + strsz].split(b"\0") if x]
        o = 12 + strsz
        libs = []
        while o < len(b):
            lsz = u32(b, o)
            ver, minver = u32(b, o + 28), u32(b, o + 32)
            ni, cnt = u16(b, o + 36), u16(b, o + 38)
            recs = [u32(b, o + 40 + j * 4) for j in range(cnt)]
            fmt = lambda v: f"{v>>28}.{(v>>24)&0xf}.{(v>>8)&0xffff}.{v&0xff}"
            print(f"import lib {names[ni]} v{fmt(ver)} (min {fmt(minver)}): {cnt} records")
            libs.append((names[ni], recs))
            o += lsz
        globals()["LIBS"] = libs
    file_key = Cipher(algorithms.AES(RETAIL_KEY), modes.ECB()).decryptor().update(enc_key)
    payload = data[pe_off:]
    if enc == 1:
        payload = Cipher(algorithms.AES(file_key), modes.CBC(b"\0" * 16)).decryptor().update(payload[:len(payload) & ~15])
    if len(sys.argv) > 2:
        os.makedirs(sys.argv[2], exist_ok=True)
        open(os.path.join(sys.argv[2], "payload.bin"), "wb").write(payload)
        if comp == 2:
            # first block descriptor (size + sha1) lives in the file format header
            open(os.path.join(sys.argv[2], "lzx_first_block.bin"), "wb").write(ff[12:12+24])
        print("wrote payload", len(payload))
    print("payload head:", payload[:16].hex())

main()
