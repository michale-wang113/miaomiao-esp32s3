#!/usr/bin/env python3
"""
assets 资源包拆包 / 重打包工具（喵喵同学机芯盒专用）

包格式（已对照官方 78/xiaozhi-esp32 v2.2.2 main/assets.cc 逐字段核实）：
    u32 file_count                 实测 11（官方代码里这个字段被当作条目数）
    u32 checksum                   = sum(bytes[12:12+stored_len]) & 0xFFFF（无符号累加）
    u32 stored_len                 = 整包长度 - 12
    entry[file_count]              每项 44 字节（mmap_assets_table）：
        char name[32]              文件名，\\0 补齐
        u32  size                  blob 长度
        u32  offset                相对数据区起点的偏移
        u16  width                 GIF 为 240，其余 0
        u16  height                GIF 为 240，其余 0
    2 字节填充
    data                           各 blob 顺序排列，每个 blob 之后跟 2 字节填充
                                   （最后一个 blob 后无填充）

注意：官方代码的数据区起点公式是 12 + 44*file_count（无 +2），
而本机芯盒的包实测有 +2 偏移（GIF89a / index.json 落点均已验证），
说明厂商 fork 在官方生成器之上加了 2 字节对齐。本工具按厂商实测布局打包。

用法：
    python3 repack_assets.py extract firmware/assets_pack_full.bin work/
    python3 repack_assets.py build   work/ firmware/assets_new.bin
    python3 repack_assets.py verify  firmware/assets_new.bin
"""
import json
import os
import struct
import sys

HEADER_SIZE = 12
ENTRY_SIZE = 44
GIF_DIM = (240, 240)


def parse(pack: bytes):
    file_count, checksum, stored_len = struct.unpack("<III", pack[:HEADER_SIZE])
    entries = []
    off = HEADER_SIZE
    for _ in range(file_count):
        name = pack[off:off + 32].split(b"\x00")[0].decode()
        size, data_off = struct.unpack("<II", pack[off + 32:off + 40])
        w, h = struct.unpack("<HH", pack[off + 40:off + 44])
        entries.append({"name": name, "size": size, "offset": data_off,
                        "width": w, "height": h})
        off += ENTRY_SIZE
    data_base = off + 2          # 厂商布局：表尾 +2
    return {"file_count": file_count, "checksum": checksum,
            "stored_len": stored_len, "entries": entries, "data_base": data_base}


def cmd_extract(pack_path, out_dir):
    pack = open(pack_path, "rb").read()
    info = parse(pack)
    os.makedirs(out_dir, exist_ok=True)
    print(f"file_count={info['file_count']}  checksum=0x{info['checksum']:04x}  "
          f"stored_len={info['stored_len']}  data_base=0x{info['data_base']:x}")
    for e in info["entries"]:
        start = info["data_base"] + e["offset"]
        blob = pack[start:start + e["size"]]
        path = os.path.join(out_dir, e["name"])
        os.makedirs(os.path.dirname(path) or out_dir, exist_ok=True)
        open(path, "wb").write(blob)
        print(f"  {e['name']:<34} {e['size']:>9} B  {e['width']}x{e['height']}  {blob[:6]!r}")
    print(f"已拆到 {out_dir}/")


def cmd_build(src_dir, out_path):
    names = [n for n in os.listdir(src_dir)
             if os.path.isfile(os.path.join(src_dir, n))]
    # 排序规则（与原厂包一致）：index.json 最后，非 GIF 资源在前，GIF 按名排序
    names.sort(key=lambda n: (n == "index.json", n.lower().endswith(".gif"), n))

    entries, blobs = [], []
    data_off = 0
    for n in names:
        blob = open(os.path.join(src_dir, n), "rb").read()
        w, h = GIF_DIM if n.lower().endswith(".gif") else (0, 0)
        entries.append({"name": n, "size": len(blob), "offset": data_off,
                        "width": w, "height": h})
        blobs.append(blob)
        data_off += len(blob) + 2          # 每个 blob 后跟 2 字节填充

    # 最后一个 blob 后不加填充
    total = HEADER_SIZE + ENTRY_SIZE * len(entries) + 2 + data_off - 2
    out = bytearray()
    out += struct.pack("<III", len(entries), 0, total - HEADER_SIZE)
    for e in entries:
        nm = e["name"].encode()
        if len(nm) > 31:
            raise ValueError(f"文件名过长: {e['name']}")
        out += nm + b"\x00" * (32 - len(nm))
        out += struct.pack("<IIHH", e["size"], e["offset"], e["width"], e["height"])
    out += b"\x5a\x5a"
    for i, b in enumerate(blobs):
        out += b
        if i < len(blobs) - 1:
            out += b"\x5a\x5a"
    assert len(out) == total, (len(out), total)

    # 官方算法：无符号字节累加，范围 [12, 12+stored_len)，取低 16 位
    checksum = sum(out[HEADER_SIZE:]) & 0xFFFF
    out[4:8] = struct.pack("<I", checksum)

    open(out_path, "wb").write(bytes(out))
    print(f"已生成 {out_path}: {total} 字节, {len(entries)} 项, "
          f"stored_len={total-HEADER_SIZE}, checksum=0x{checksum:04x}")


def cmd_verify(pack_path):
    pack = open(pack_path, "rb").read()
    info = parse(pack)
    calc = sum(pack[HEADER_SIZE:HEADER_SIZE + info["stored_len"]]) & 0xFFFF
    ok = calc == info["checksum"] and info["stored_len"] == len(pack) - HEADER_SIZE
    print(f"file_count={info['file_count']}  stored_len={info['stored_len']}  "
          f"实际={len(pack)-HEADER_SIZE}  checksum 存储=0x{info['checksum']:04x} "
          f"计算=0x{calc:04x}  =>  {'✓ 校验通过' if ok else '✗ 不一致'}")
    # 抽查 GIF magic 落点
    for e in info["entries"]:
        start = info["data_base"] + e["offset"]
        blob = pack[start:start + 6]
        mark = ""
        if e["name"].endswith(".gif"):
            mark = "✓" if blob.startswith(b"GIF8") else "✗ GIF magic 错"
        elif e["name"] == "index.json":
            mark = "✓" if blob.startswith(b"{") else "✗ JSON 起点错"
        print(f"  {e['name']:<34} {blob!r} {mark}")
    return ok


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, a = sys.argv[1], sys.argv[2]
    if cmd == "extract":
        cmd_extract(a, sys.argv[3])
    elif cmd == "build":
        cmd_build(a, sys.argv[3])
    elif cmd == "verify":
        cmd_verify(a)
    else:
        print(__doc__)
