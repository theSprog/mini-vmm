#!/usr/bin/env python3
"""tests/fuzz/mkseeds.py — 生成 fuzz 的种子语料

种子的作用是让 libFuzzer 从“已经能走到深处”的输入开始变异，而不是
先花几百万次执行去猜 ELF 魔数和 HdrS。这里给两类：

  1. 手写的最小合法镜像。不依赖机器上有没有真内核，CI 里也能跑。
  2. 真 vmlinux / bzImage 的头部切片（如果找得到）。真镜像的 header
     字段组合是手写不出来的，切片保留了它们，又不会让语料变成几十 MB。

用法：mkseeds.py <输出目录>          可选 VMLINUX= / BZIMAGE= 环境变量
"""
import os
import struct
import sys

ELFMAG = b"\x7fELF"


def elf64(phdrs, entry=0x100000, phoff=64, phnum=None, machine=0x3E,
          klass=2, data_enc=1, phentsize=56, body_at=0x1000, body_len=0x100):
    """拼一个 ELF64。phdrs 是 (type, flags, off, vaddr, paddr, filesz, memsz, align) 列表。"""
    if phnum is None:
        phnum = len(phdrs)
    eh = bytearray(64)
    eh[0:4] = ELFMAG
    eh[4] = klass          # EI_CLASS
    eh[5] = data_enc       # EI_DATA
    eh[6] = 1              # EI_VERSION
    struct.pack_into("<HHIQQQIHHHHHH", eh, 16,
                     2,            # e_type = ET_EXEC
                     machine,      # e_machine
                     1,            # e_version
                     entry,        # e_entry
                     phoff,        # e_phoff
                     0,            # e_shoff
                     0,            # e_flags
                     64,           # e_ehsize
                     phentsize,    # e_phentsize
                     phnum,        # e_phnum
                     64,           # e_shentsize
                     0,            # e_shnum
                     0)            # e_shstrndx
    out = bytearray(eh)
    for ph in phdrs:
        out += struct.pack("<IIQQQQQQ", *ph)
    if len(out) < body_at + body_len:
        out += b"\x00" * (body_at + body_len - len(out))
    for i in range(body_len):
        out[body_at + i] = (i * 7 + 3) & 0xFF
    return bytes(out)


def bzimage(setup_sects=1, version=0x020F, xloadflags=0x01, loadflags=0x01,
            pref_address=0x1000000, kernel_alignment=0x200000,
            relocatable=1, init_size=0x100000, syssize=0, jump_len=0x6A,
            total=0x1000, kernel_version=0):
    """拼一个最小的 bzImage：只有 bzimage_load() 会读的那些字段是有意义的。"""
    b = bytearray(total)
    b[0x1F1] = setup_sects & 0xFF
    struct.pack_into("<I", b, 0x1F4, syssize)
    struct.pack_into("<H", b, 0x1FE, 0xAA55)          # boot_flag
    b[0x200] = 0xEB                                    # jmp
    b[0x201] = jump_len                                # header 长度的来源
    b[0x202:0x206] = b"HdrS"
    struct.pack_into("<H", b, 0x206, version)
    struct.pack_into("<H", b, 0x20E, kernel_version)
    b[0x211] = loadflags & 0xFF                        # LOADED_HIGH
    struct.pack_into("<I", b, 0x230, kernel_alignment)
    b[0x234] = relocatable & 0xFF
    struct.pack_into("<H", b, 0x236, xloadflags)       # XLF_KERNEL_64
    struct.pack_into("<Q", b, 0x258, pref_address)
    struct.pack_into("<I", b, 0x260, init_size)
    # 保护模式部分：填点非零内容，让变异有东西可改
    for i in range((setup_sects + 1) * 512, total):
        b[i] = (i * 13 + 7) & 0xFF
    return bytes(b)


def head_of(path, nbytes):
    try:
        with open(path, "rb") as f:
            return f.read(nbytes)
    except OSError:
        return None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    elf_dir = os.path.join(root, "elf")
    bz_dir = os.path.join(root, "bzimage")
    os.makedirs(elf_dir, exist_ok=True)
    os.makedirs(bz_dir, exist_ok=True)

    seeds = {}

    # --- ELF ---
    one_load = (1, 7, 0x1000, 0xFFFFFFFF81000000, 0x100000, 0x100, 0x200, 0x1000)
    seeds[os.path.join(elf_dir, "min_one_load")] = elf64([one_load])
    seeds[os.path.join(elf_dir, "two_loads")] = elf64([
        one_load,
        (1, 6, 0x1100, 0xFFFFFFFF81200000, 0x300000, 0x80, 0x1000, 0x1000),
    ], body_at=0x1180, body_len=0x80)
    # bss 比 filesz 大得多的段：memsz 清零路径
    seeds[os.path.join(elf_dir, "big_bss")] = elf64([
        (1, 6, 0x1000, 0xFFFFFFFF81000000, 0x200000, 0x10, 0x400000, 0x1000),
    ])
    # PT_NOTE + PT_LOAD：非 LOAD 段必须被跳过
    seeds[os.path.join(elf_dir, "note_and_load")] = elf64([
        (4, 4, 0x1000, 0, 0, 0x20, 0x20, 8),
        one_load,
    ])
    # 入口用虚拟地址表示：走 p_vaddr -> p_paddr 换算分支
    seeds[os.path.join(elf_dir, "virt_entry")] = elf64(
        [one_load], entry=0xFFFFFFFF81000000)

    vmlinux = os.environ.get("VMLINUX", "")
    if vmlinux:
        blob = head_of(vmlinux, 16 * 1024)
        if blob:
            seeds[os.path.join(elf_dir, "real_vmlinux_head")] = blob

    # --- bzImage ---
    seeds[os.path.join(bz_dir, "min")] = bzimage()
    seeds[os.path.join(bz_dir, "setup_sects_0")] = bzimage(setup_sects=0,
                                                           total=0x1000)
    seeds[os.path.join(bz_dir, "big_init_size")] = bzimage(init_size=0x2000000)
    seeds[os.path.join(bz_dir, "no_reloc")] = bzimage(relocatable=0,
                                                      kernel_alignment=0)
    seeds[os.path.join(bz_dir, "with_version_str")] = bzimage(
        kernel_version=0x400, total=0x2000)

    bzpath = os.environ.get("BZIMAGE", "")
    if bzpath:
        blob = head_of(bzpath, 64 * 1024)
        if blob:
            seeds[os.path.join(bz_dir, "real_bzimage_head")] = blob

    # 回归输入：真实找到过的缺陷的最小复现，让后续 fuzz 从这些点继续变异
    here = os.path.dirname(os.path.abspath(__file__))
    regress = os.path.join(here, "regress")
    n_regress = 0
    if os.path.isdir(regress):
        for fn in sorted(os.listdir(regress)):
            if not fn.endswith(".bin"):
                continue
            blob = head_of(os.path.join(regress, fn), 1 << 20)
            if blob is None:
                continue
            sub = elf_dir if fn.startswith("elf-") else bz_dir
            seeds[os.path.join(sub, "regress_" + fn)] = blob
            n_regress += 1

    for path, blob in seeds.items():
        with open(path, "wb") as f:
            f.write(blob)

    print("seeds: %d in %s (elf=%d bzimage=%d, regress=%d)" % (
        len(seeds), root,
        len(os.listdir(elf_dir)), len(os.listdir(bz_dir)), n_regress))
    return 0


if __name__ == "__main__":
    sys.exit(main())
