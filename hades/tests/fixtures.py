# 合成格式测试数据；指令来自 ARM64 编码定义，不使用游戏文件。
# Synthetic format fixtures; instructions use ARM64 encoding definitions, not game files.
import hashlib
import struct


def synthetic_nso(build_id=bytes(32)):
    text, ro, data = bytearray(0x1000), bytearray(0x1000), bytearray(16)
    struct.pack_into("<I", text, 4, 0x1000)
    words = [
        0x9100025F,
        0x10000000 | (0x5C >> 2) << 5 | 18,
        0x386E69E6,
        0x8B060A52,
        *[0x4EA01C00 | register for register in range(2, 8)],
        0xD61F0240,
        0xAA0103F2,
    ]
    for index, word in enumerate(words):
        struct.pack_into("<I", text, 0x20 + index * 4, word)
    for offset in range(0x80, 0x480, 4):
        struct.pack_into("<I", text, offset, 0xD503201F)
    ro[:4] = b"MOD0"
    struct.pack_into("<i", ro, 4, 0x80)
    struct.pack_into("<i", ro, 16, 0x40)
    ro[0x40:0x44] = bytes((1, 3, 3, 3))
    struct.pack_into("<IIII", ro, 0x44, 0, 1, 0x20, 0)
    strings = b"\0provider\0external\0libgame.so\0"
    ro[0x240:0x240 + len(strings)] = strings
    tags = [(4, 0x1200), (5, 0x1240), (6, 0x1280), (10, len(strings)),
            (11, 24), (1, strings.index(b"libgame")), (7, 0x1300), (8, 24), (9, 24), (0, 0)]
    for index, (tag, value) in enumerate(tags):
        struct.pack_into("<qQ", ro, 0x80 + index * 16, tag, value)
    struct.pack_into("<IIIIII", ro, 0x200, 1, 3, 1, 0, 0, 0)
    struct.pack_into("<IBBHQQ", ro, 0x298, 1, 0x12, 0, 1, 0x80, 4)
    struct.pack_into("<IBBHQQ", ro, 0x2B0, 10, 0x12, 0, 0, 0, 0)
    struct.pack_into("<QQq", ro, 0x300, 0x2000, (2 << 32) | 1026, 0)
    header = bytearray(256)
    header[:4] = b"NSO0"
    struct.pack_into("<I", header, 12, 0x38)
    header[0x40:0x60] = build_id
    struct.pack_into("<II", header, 0x98, 0x280, 72)
    file_offset = 256
    for index, segment in enumerate((text, ro, data)):
        struct.pack_into("<III", header, 0x10 + 16 * index, file_offset, index * 0x1000, len(segment))
        struct.pack_into("<I", header, 0x60 + 4 * index, len(segment))
        header[0xA0 + 32 * index:0xC0 + 32 * index] = hashlib.sha256(segment).digest()
        file_offset += len(segment)
    return bytes(header + text + ro + data)
