#!/usr/bin/env python3
"""Analyze and safely extract the local World Adventures IPA (stdlib only)."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import struct
import zipfile


class MachO32:
    def __init__(self, data):
        self.data = data
        self.segments, self.sections, self.libraries = [], [], []
        self.cryptid, self.entry = None, None
        self.symbols, self.relocations, self.indirect = [], [], []
        magic, cpu, subtype, kind, count, size, self.flags = self.unpack("7I", 0)
        if magic != 0xFEEDFACE or cpu != 12 or kind != 2:
            raise ValueError("Esperado Mach-O ARM 32-bit MH_EXECUTE.")
        self.subtype = subtype
        self.bounds(28, size)
        at, end = 28, 28 + size
        sym, dysym = None, None
        for _ in range(count):
            command, length = self.unpack("2I", at)
            if length < 8 or at + length > end:
                raise ValueError("Tabela de comandos Mach-O inválida.")
            if command == 1:
                if length < 56:
                    raise ValueError("Segmento incompleto.")
                name, va, vs, off, fs, maxprot, prot, ns, flags = self.unpack("16s8I", at + 8)
                if length != 56 + ns * 68 or fs > vs:
                    raise ValueError("Segmento/seções inconsistentes.")
                self.bounds(off, fs)
                self.segments.append(dict(name=self.name(name), address=va, size=vs,
                                          offset=off, file_size=fs, protection=prot))
                for i in range(ns):
                    sec, seg, addr, sz, fo, al, ro, nr, fl, r1, r2 = self.unpack("16s16s9I", at + 56 + i * 68)
                    self.sections.append(dict(name=self.name(sec), segment=self.name(seg),
                                              address=addr, size=sz, offset=fo, flags=fl,
                                              reserved1=r1, reserved2=r2))
            elif command == 2:
                sym = self.unpack("4I", at + 8)
            elif command == 11:
                dysym = self.unpack("18I", at + 8)
            elif command in (12, 0x80000018):
                nameoff = self.unpack("I", at + 8)[0]
                if nameoff < 24 or nameoff >= length:
                    raise ValueError("Nome de biblioteca inválido.")
                self.libraries.append(self.cstring(at + nameoff, at + length))
            elif command == 0x21:
                off, sz, self.cryptid = self.unpack("3I", at + 8)
                self.bounds(off, sz)
            elif command == 5:
                flavor, words = self.unpack("2I", at + 8)
                if flavor != 1 or words != 17 or length != 84:
                    raise ValueError("Estado inicial ARM desconhecido.")
                self.entry = self.unpack("I", at + 16 + 15 * 4)[0]
            at += length
        if at != end or sym is None or dysym is None or self.entry is None:
            raise ValueError("Mach-O legado sem tabelas essenciais.")
        so, n, strings, stringsize = sym
        self.bounds(so, n * 12)
        self.bounds(strings, stringsize)
        for i in range(n):
            si, ty, sec, desc, value = self.unpack("IBBHI", so + i * 12)
            if si >= stringsize:
                raise ValueError("Índice de string de símbolo inválido.")
            self.symbols.append(dict(name=self.cstring(strings + si, strings + stringsize),
                                     type=ty, section=sec, description=desc, value=value))
        start, num = dysym[4:6]
        if start + num > n:
            raise ValueError("Índices de imports inválidos.")
        self.imports = self.symbols[start:start + num]
        io, ni = dysym[12:14]
        self.bounds(io, ni * 4)
        self.indirect = [self.unpack("I", io + i * 4)[0] for i in range(ni)]
        for group, off, num in (("external", *dysym[14:16]), ("local", *dysym[16:18])):
            self.bounds(off, num * 8)
            for i in range(num):
                addr, bits = self.unpack("2I", off + i * 8)
                if addr & 0x80000000:
                    record = dict(group=group, scattered=True, address=addr & 0xFFFFFF,
                                  type=(addr >> 24) & 15, length=(addr >> 28) & 3,
                                  pcrel=(addr >> 30) & 1, value=bits)
                else:
                    symbol = bits & 0xFFFFFF
                    record = dict(group=group, scattered=False, address=addr,
                                  symbol_index=symbol, pcrel=(bits >> 24) & 1,
                                  length=(bits >> 25) & 3, external=(bits >> 27) & 1,
                                  type=bits >> 28)
                    if record["external"]:
                        if symbol >= n:
                            raise ValueError("Relocação com índice de símbolo inválido.")
                        record["symbol"] = self.symbols[symbol]["name"]
                self.relocations.append(record)

    def bounds(self, off, size):
        if off < 0 or size < 0 or off + size > len(self.data):
            raise ValueError("Leitura fora dos limites do Mach-O.")

    def unpack(self, fmt, off):
        self.bounds(off, struct.calcsize("<" + fmt))
        return struct.unpack_from("<" + fmt, self.data, off)

    @staticmethod
    def name(raw):
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace")

    def cstring(self, off, end=None):
        end = len(self.data) if end is None else end
        stop = self.data.find(b"\0", off, end)
        if stop < 0:
            raise ValueError("String Mach-O sem terminador.")
        return self.data[off:stop].decode("utf-8", "replace")

    def offset(self, address):
        for seg in self.segments:
            delta = address - seg["address"]
            if 0 <= delta < seg["file_size"]:
                return seg["offset"] + delta
        raise ValueError(f"Endereço sem dados no arquivo: {address:#x}")

    def word(self, address):
        return self.unpack("I", self.offset(address))[0]

    def string_at(self, address):
        return self.cstring(self.offset(address)) if address else None

    def pointers(self, section):
        found = next((s for s in self.sections if s["name"] == section), None)
        if found is None:
            return []
        return [self.word(found["address"] + i) for i in range(0, found["size"], 4)]

    def methods(self, address):
        if not address:
            return []
        stride, count = self.unpack("2I", self.offset(address))
        stride &= ~3
        if stride < 12 or count > 10000:
            raise ValueError("Lista de métodos inválida.")
        return [dict(name=self.string_at(self.word(address + 8 + i * stride)),
                     types=self.string_at(self.word(address + 12 + i * stride)),
                     implementation=f"0x{self.word(address + 16 + i * stride):x}")
                for i in range(count)]

    def objc(self):
        classes = []
        for address in self.pointers("__objc_classlist"):
            isa = self.word(address)
            ro = self.word(address + 16) & ~3
            mro = self.word(isa + 16) & ~3
            flags, start, size = self.unpack("3I", self.offset(ro))
            ivars = []
            iv = self.word(ro + 28)
            if iv:
                stride, count = self.unpack("2I", self.offset(iv))
                for i in range(count):
                    ptr, name, ty, align, sz = self.unpack("5I", self.offset(iv + 8 + i * stride))
                    ivars.append(dict(name=self.string_at(name), type=self.string_at(ty),
                                      offset=self.word(ptr), size=sz))
            classes.append(dict(address=f"0x{address:x}", name=self.string_at(self.word(ro + 16)),
                                instance_start=start, instance_size=size, ivars=ivars,
                                methods=self.methods(self.word(ro + 20)),
                                class_methods=self.methods(self.word(mro + 20))))
        return dict(classes=classes, selectors=[self.string_at(p) for p in self.pointers("__objc_selrefs")])


def analyze(ipa, output, extract):
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ipa) as z:
        prefix = "Payload/sims3wa_iphone.app/"
        plist = plistlib.loads(z.read(prefix + "Info.plist"))
        executable = z.read(prefix + plist["CFBundleExecutable"])
        image = MachO32(executable)
        entries = [e for e in z.infolist() if e.filename.startswith(prefix) and not e.is_dir()]
        by_library = Counter(image.libraries[(s["description"] >> 8) - 1] for s in image.imports
                             if 0 < s["description"] >> 8 <= len(image.libraries))
        report = dict(ipa=ipa.name, executable_sha256=hashlib.sha256(executable).hexdigest(),
                      executable_bytes=len(executable), info=plist, cpu="ARM", subtype=image.subtype,
                      bits=32, cryptid=image.cryptid, entry=f"0x{image.entry:x}", flags=f"0x{image.flags:x}",
                      pie=bool(image.flags & 0x200000), segments=image.segments, sections=image.sections,
                      libraries=image.libraries, import_count=len(image.imports), imports_by_library=by_library,
                      imports=image.imports, relocations=image.relocations,
                      relocation_counts=dict(Counter(f'{r["group"]}:type{r["type"]}:len{r["length"]}'
                                                     for r in image.relocations)),
                      bundle_files=len(entries), bundle_bytes=sum(e.file_size for e in entries),
                      extensions=dict(Counter(PurePosixPath(e.filename).suffix.lower() for e in entries)),
                      objc=image.objc())
        (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        (output / "imports.txt").write_text("\n".join(s["name"] for s in image.imports) + "\n")
        (output / "sims3wa_armv6").write_bytes(executable)
        if extract:
            if image.cryptid != 0:
                raise ValueError("Executável ainda criptografado; extração para testes recusada.")
            seen = set()
            for entry in entries:
                path = PurePosixPath(entry.filename)
                if path.is_absolute() or ".." in path.parts or "\\" in entry.filename or entry.filename in seen:
                    raise ValueError(f"Caminho inválido/duplicado: {entry.filename}")
                if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError(f"Link simbólico: {entry.filename}")
                seen.add(entry.filename)
                dest = extract.joinpath(*path.parts)
                if dest.exists():
                    raise ValueError(f"Preservando arquivo existente: {dest}")
                dest.parent.mkdir(parents=True, exist_ok=True)
                with z.open(entry) as src, dest.open("xb") as dst:
                    import shutil
                    shutil.copyfileobj(src, dst)
        print(f'ARMv6 32-bit; cryptid={image.cryptid}; PIE={report["pie"]}; entry={report["entry"]}')
        print(f'{len(image.imports)} imports; {len(image.relocations)} relocations; '
              f'{len(report["objc"]["classes"])} classes; {len(report["objc"]["selectors"])} selector references')
        print(f'{len(entries)} bundle files; {report["bundle_bytes"] / (1 << 20):.1f} MiB extracted')
        print(f'Report: {output / "report.json"}')


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("--output", type=Path, default=Path("analysis"))
    parser.add_argument("--extract", type=Path)
    args = parser.parse_args()
    analyze(args.ipa, args.output, args.extract)
