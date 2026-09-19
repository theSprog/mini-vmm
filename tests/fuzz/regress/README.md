# tests/fuzz/regress — 曾经让加载器崩掉的输入

每个文件都是 fuzz 真实找到过的缺陷的最小复现，修好之后留在这里当回归输入。
两条路径都会用到它们：

- `tests/offline/unit.sh` 每次都会把它们逐个喂给加载器（毫秒级，进默认集合）；
- `tests/fuzz/mkseeds.py` 把它们拷进语料，让后续 fuzz 从这些点继续变异。

| 文件 | 缺陷 | 机制 |
|---|---|---|
| `elf-phoff-wraparound.bin` | `elf_loader.c` 的截断检查被 64 位回绕绕过，越界读 | `e_phoff = 0xFFFFFFFFFFFFFFC0`、`e_phnum = 2`，`e_phoff + 2*56` 回绕成 `0x30` 通过检查，随后 `img + e_phoff` 指到映射区之前 64 字节 |
| `elf-unaligned-phdr.bin` | 以 `Elf64_Phdr*` 解引用文件里任意偏移，未对齐即未定义行为 | `e_phoff` 不是 8 的倍数 |

加新文件时同时在上表加一行，写清楚"当初错在哪"——否则过半年没人知道
这个二进制块为什么在仓库里。
