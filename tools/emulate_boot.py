#!/usr/bin/env python3
"""Execute real ARM32 game startup on a 64-bit PC using Unicorn.

Research-only CPU/guest-memory proof: runs the 28 original constructors and
optionally _main up to the UIApplicationMain boundary. No graphics or gameplay.
Unknown imports/selectors fail instead of silently claiming a successful boot.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import platform
import struct
import time

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn import UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                              UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
                              UC_ARM_REG_FPEXC, UC_ARM_REG_C1_C0_2)
from analyze_ipa import MachO32

REGISTERS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
IMPORT_BASE = 0x01000000
STOP = 0x02000000
HEAP = 0x10000000
HEAP_SIZE = 32 << 20
STACK = 0x70000000
STACK_SIZE = 2 << 20


def signed(value, bits=32):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Guest:
    def __init__(self, binary):
        self.image = MachO32(binary)
        if self.image.cryptid != 0:
            raise ValueError("FairPlay encrypted image rejected")
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.used_imports = Counter()
        self.used_selectors = Counter()
        self.handlers, self.thunks, self.objects, self.classes = {}, {}, {}, {}
        self.allocations = {}
        self.heap_at = HEAP
        self.returned = False
        self.ui_application = None
        self.failure = None
        self.initializers = []
        self.class_names = {}
        self.wide_results = set()
        self.started = time.monotonic()
        for segment in self.image.segments:
            if segment["name"] == "__PAGEZERO" or not segment["size"]:
                continue
            start = segment["address"] & ~4095
            end = (segment["address"] + segment["size"] + 4095) & ~4095
            self.uc.mem_map(start, end - start)
            self.uc.mem_write(segment["address"], binary[segment["offset"]:segment["offset"] + segment["file_size"]])
        self.uc.mem_map(IMPORT_BASE, 0x10000)
        self.uc.mem_map(STOP, 4096)
        self.uc.mem_map(HEAP, HEAP_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        self.uc.mem_map(STACK, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.uc.mem_write(STOP, struct.pack("<I", 0xe12fff1e))  # BX LR; hook stops first.
        self.setup_handlers()
        self.bind()
        self.uc.mem_protect(IMPORT_BASE, 0x10000, UC_PROT_READ | UC_PROT_EXEC)
        self.uc.mem_protect(STOP, 4096, UC_PROT_READ | UC_PROT_EXEC)
        self.category_methods = {}
        for category in self.image.pointers("__objc_catlist"):
            cls = self.class_names[self.word(category + 4)]
            for method in self.image.methods(self.word(category + 8)):
                self.category_methods[(cls, method["name"])] = int(method["implementation"], 16)
        self.uc.hook_add(UC_HOOK_CODE, self.import_call, begin=IMPORT_BASE, end=IMPORT_BASE + 0xffff)
        self.uc.hook_add(UC_HOOK_CODE, self.return_hook, begin=STOP, end=STOP)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self.memory_fault)
        for segment in self.image.segments:
            if segment["name"] != "__PAGEZERO" and segment["size"]:
                self.uc.mem_protect(segment["address"], segment["size"], segment["protection"])

    def reg(self, number):
        return self.uc.reg_read(REGISTERS[number])

    def word(self, address):
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def put_word(self, address, value):
        self.uc.mem_write(address, struct.pack("<I", value & 0xffffffff))

    def cstring(self, address, limit=1 << 20):
        if not address:
            return ""
        out = bytearray()
        for offset in range(limit):
            byte = bytes(self.uc.mem_read(address + offset, 1))
            if byte == b"\0":
                return out.decode("utf-8", "replace")
            out += byte
        raise ValueError(f"Unterminated guest string at {address:#x}")

    def alloc(self, size):
        size = max(size, 1)
        start = (self.heap_at + 15) & ~15
        end = start + size
        if end > HEAP + HEAP_SIZE:
            raise MemoryError("Guest heap exhausted")
        self.heap_at = end
        self.allocations[start] = size
        return start

    def data(self, content):
        address = self.alloc(len(content))
        self.uc.mem_write(address, content)
        return address

    def cfstring(self, text):
        raw = text.encode("utf-16-le")
        obj = self.alloc(16)
        self.objects[obj] = ("string", text)
        self.uc.mem_write(obj, struct.pack("<4I", 0, 0x7d0, self.data(raw + b"\0\0"), len(raw) // 2))
        return obj

    def string(self, address):
        if not address:
            return ""
        if address in self.objects and self.objects[address][0] == "string":
            return self.objects[address][1]
        flags, data, length = (self.word(address + i) for i in (4, 8, 12))
        if length > 1 << 20:
            raise ValueError("Invalid guest CFString length")
        raw = bytes(self.uc.mem_read(data, length * (2 if flags & 0x10 else 1)))
        return raw.decode("utf-16-le" if flags & 0x10 else "utf-8", "replace")

    def array(self, values):
        obj = self.alloc(16)
        self.objects[obj] = ("array", values)
        return obj

    def class_object(self, name):
        if name not in self.classes:
            obj = self.alloc(20)
            self.classes[name] = obj
            self.class_names[obj] = name
            self.put_word(obj, obj)
        return self.classes[name]

    def thunk(self, name):
        if name not in self.thunks:
            address = IMPORT_BASE + len(self.thunks) * 16
            self.uc.mem_write(address, struct.pack("<I", 0xe12fff1e))
            self.thunks[name] = address
        return self.thunks[name]

    def resolve(self, symbol):
        if (symbol["type"] & 0xe0) == 0 and (symbol["type"] & 0xe) == 0xe:
            return symbol["value"] | (1 if symbol["description"] & 8 else 0)
        name = symbol["name"]
        for prefix in ("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_"):
            if name.startswith(prefix):
                return self.class_object(name[len(prefix):])
        if name in ("__objc_empty_cache", "__objc_empty_vtable"):
            return self.alloc(16)
        if name == "___CFConstantStringClassReference":
            return self.class_object("__NSCFConstantString")
        # Only function thunks have callbacks. Unused imported data gets distinct
        # guest memory, recorded as a placeholder in the result, never host pointers.
        lazy_names = {self.image.symbols[idx]["name"] for section in self.image.sections
                      if section["flags"] & 0xff == 7
                      for idx in self.image.indirect[section["reserved1"]:section["reserved1"] + section["size"] // 4]
                      if idx < len(self.image.symbols)}
        if name in lazy_names or name in self.handlers:
            return self.thunk(name)
        if not hasattr(self, "data_imports"):
            self.data_imports = {}
        if name not in self.data_imports:
            self.data_imports[name] = self.alloc(512)
        return self.data_imports[name]

    def bind(self):
        for section in self.image.sections:
            if section["flags"] & 0xff not in (6, 7):
                continue
            for index in range(section["size"] // 4):
                symbol = self.image.indirect[section["reserved1"] + index]
                if symbol & 0xc0000000:
                    continue
                self.put_word(section["address"] + index * 4, self.resolve(self.image.symbols[symbol]))
        for relocation in self.image.relocations:
            if relocation["type"] != 0 or relocation["length"] != 2 or relocation["pcrel"]:
                raise ValueError("Unsupported guest relocation")
            address = relocation["address"]
            addend = self.word(address)
            self.put_word(address, self.resolve(self.image.symbols[relocation["symbol_index"]]) + addend)

    def add(self, name, handler):
        self.handlers[name] = handler

    def setup_handlers(self):
        for name in ("_malloc", "__Znwm", "__Znam", "_valloc"):
            self.add(name, lambda: self.alloc(self.reg(0)))
        for name in ("_free", "__ZdlPv", "__ZdaPv"):
            self.add(name, lambda: 0)  # Arena retained for this short-lived startup probe.
        def memset():
            self.uc.mem_write(self.reg(0), bytes([self.reg(1) & 255]) * self.reg(2))
            return self.reg(0)
        def memcpy():
            self.uc.mem_write(self.reg(0), bytes(self.uc.mem_read(self.reg(1), self.reg(2))))
            return self.reg(0)
        self.add("_memset", memset)
        self.add("_memcpy", memcpy)
        self.add("_memmove", memcpy)
        def atomic_add():
            value = (self.word(self.reg(1)) + self.reg(0)) & 0xffffffff
            self.put_word(self.reg(1), value)
            return value
        self.add("_OSAtomicAdd32", atomic_add)
        def sysctlbyname():
            key = self.cstring(self.reg(0))
            values = {"hw.machine": b"iPhone2,1\0", "hw.model": b"N88AP\0"}
            if key not in values:
                raise NotImplementedError(f"sysctlbyname {key}")
            raw = values[key]
            if self.reg(1) and self.word(self.reg(2)) < len(raw):
                return -1
            if self.reg(1):
                self.uc.mem_write(self.reg(1), raw)
            self.put_word(self.reg(2), len(raw))
            return 0
        self.add("_sysctlbyname", sysctlbyname)
        self.add("_strlen", lambda: len(self.cstring(self.reg(0)).encode()))
        self.add("_strcmp", lambda: (self.cstring(self.reg(0)) > self.cstring(self.reg(1))) -
                                    (self.cstring(self.reg(0)) < self.cstring(self.reg(1))))
        for name in ("___cxa_atexit", "__ZNSt8ios_base4InitC1Ev", "__ZNSt8ios_base4InitD1Ev",
                     "__Unwind_SjLj_Register", "__Unwind_SjLj_Unregister"):
            self.add(name, lambda: 0)
        self.add("___cxa_guard_acquire", lambda: 0 if self.uc.mem_read(self.reg(0), 1)[0] else 1)
        self.add("___cxa_guard_release", lambda: self.uc.mem_write(self.reg(0), b"\1") or 0)
        self.add("_CFBundleGetMainBundle", lambda: self.class_object("ProbeBundle"))
        self.add("_CFBundleCopyBundleLocalizations", lambda: self.array([self.cfstring(s) for s in ("en", "fr_FR", "it", "de", "es_ES", "ja")]))
        self.add("_CFLocaleCopyPreferredLanguages", lambda: self.array([self.cfstring("en")]))
        self.add("_CFArrayGetCount", lambda: len(self.objects[self.reg(0)][1]) if self.reg(0) else 0)
        self.add("_CFArrayGetValueAtIndex", lambda: self.objects[self.reg(0)][1][self.reg(1)])
        self.add("_CFRelease", lambda: 0)
        self.add("_CFRetain", lambda: self.reg(0))
        self.add("_CFStringGetLength", lambda: len(self.string(self.reg(0)).encode("utf-16-le")) // 2)
        self.add("_CFStringCompare", lambda: (self.string(self.reg(0)) > self.string(self.reg(1))) -
                                            (self.string(self.reg(0)) < self.string(self.reg(1))))
        def compare_options():
            first = self.string(self.reg(0))[self.reg(2):self.reg(2) + self.reg(3)]
            second = self.string(self.reg(1))
            options = self.word(self.uc.reg_read(UC_ARM_REG_SP))
            if options & ~1:
                raise NotImplementedError(f"CFString comparison flags {options:#x}")
            if options & 1:
                first, second = first.casefold(), second.casefold()
            return (first > second) - (first < second)
        self.add("_CFStringCompareWithOptions", compare_options)
        def get_cstring():
            raw = self.string(self.reg(0)).encode("utf-8") + b"\0"
            if len(raw) > self.reg(2):
                return 0
            self.uc.mem_write(self.reg(1), raw)
            return 1
        self.add("_CFStringGetCString", get_cstring)
        def get_chars():
            raw = self.string(self.reg(0)).encode("utf-16-le")
            # ARM32 CFRange follows the string as r1=location, r2=length, r3=buffer.
            self.uc.mem_write(self.reg(3), raw[self.reg(1) * 2:(self.reg(1) + self.reg(2)) * 2])
            return 0
        self.add("_CFStringGetCharacters", get_chars)
        self.add("_CFStringGetCharacterAtIndex", lambda: ord(self.string(self.reg(0))[self.reg(1)]))
        self.add("_CFStringCreateCopy", lambda: self.cfstring(self.string(self.reg(1))))
        self.add("_CFStringCreateWithCString", lambda: self.cfstring(self.cstring(self.reg(1))))
        self.add("_CFStringCreateWithCStringNoCopy", lambda: self.cfstring(self.cstring(self.reg(1))))
        self.add("_CFStringCreateMutable", lambda: self.cfstring(""))
        self.add("_objc_msgSend", self.objc_send)
        self.add("_UIApplicationMain", self.application_main)
        self.numeric_helpers()

    def float_value(self, reg, kind):
        raw = struct.pack("<I", self.reg(reg))
        if kind == "d":
            raw += struct.pack("<I", self.reg(reg + 1))
        return struct.unpack("<" + kind, raw)[0]

    def float_result(self, value, kind):
        try:
            raw = struct.pack("<" + kind, value)
        except OverflowError:
            raw = struct.pack("<" + kind, math.copysign(math.inf, value))
        return struct.unpack("<" + ("I" if kind == "f" else "Q"), raw)[0]

    def numeric_helpers(self):
        operations = {"add": lambda a, b: a + b, "sub": lambda a, b: a - b,
                      "mul": lambda a, b: a * b, "div": lambda a, b: a / b}
        for suffix, kind, stride in (("sf", "f", 1), ("df", "d", 2)):
            for op, fn in operations.items():
                name = f"___{op}{suffix}3vfp"
                self.add(name, lambda k=kind, s=stride, f=fn:
                         self.float_result(f(self.float_value(0, k), self.float_value(s, k)), k))
                if kind == "d":
                    self.wide_results.add(name)
            comparisons = {"eq": lambda a, b: a == b, "ne": lambda a, b: a != b,
                           "ge": lambda a, b: a >= b, "gt": lambda a, b: a > b,
                           "le": lambda a, b: a <= b, "lt": lambda a, b: a < b,
                           "unord": lambda a, b: math.isnan(a) or math.isnan(b)}
            for op, fn in comparisons.items():
                self.add(f"___{op}{suffix}2vfp", lambda k=kind, s=stride, f=fn:
                         int(f(self.float_value(0, k), self.float_value(s, k))))
        self.add("___extendsfdf2vfp", lambda: self.float_result(self.float_value(0, "f"), "d"))
        self.add("___truncdfsf2vfp", lambda: self.float_result(self.float_value(0, "d"), "f"))
        for name, kind in (("___floatsidfvfp", "d"), ("___floatsisfvfp", "f")):
            self.add(name, lambda k=kind: self.float_result(signed(self.reg(0)), k))
        self.add("___floatunssidfvfp", lambda: self.float_result(self.reg(0), "d"))
        for name, kind, bits in (("___fixdfsivfp", "d", 32), ("___fixsfsivfp", "f", 32),
                                  ("___fixdfdi", "d", 64), ("___fixsfdi", "f", 64)):
            self.add(name, lambda k=kind, b=bits: int(self.float_value(0, k)) & ((1 << b) - 1))
        self.add("___fixunsdfsivfp", lambda: int(self.float_value(0, "d")) & 0xffffffff)
        self.add("___fixunssfsivfp", lambda: int(self.float_value(0, "f")) & 0xffffffff)
        self.add("___divsi3", lambda: math.trunc(signed(self.reg(0)) / signed(self.reg(1))))
        self.add("___modsi3", lambda: signed(self.reg(0)) - math.trunc(signed(self.reg(0)) / signed(self.reg(1))) * signed(self.reg(1)))
        self.add("___udivsi3", lambda: self.reg(0) // self.reg(1))
        self.add("___umodsi3", lambda: self.reg(0) % self.reg(1))
        self.add("___floatdidf", lambda: self.float_result(signed(self.reg(0) | (self.reg(1) << 32), 64), "d"))
        self.add("___floatundisf", lambda: self.float_result(self.reg(0) | (self.reg(1) << 32), "f"))
        self.wide_results.update(("___extendsfdf2vfp", "___floatsidfvfp", "___floatunssidfvfp",
                                  "___floatdidf", "___fixdfdi", "___fixsfdi"))

    def objc_send(self):
        receiver, selector = self.reg(0), self.cstring(self.reg(1))
        if not receiver:
            return 0
        name = self.class_names.get(receiver)
        self.used_selectors[f"{name or self.objects.get(receiver, ('unknown',))[0]} {selector}"] += 1
        instance_name = self.objects.get(receiver, (None,))[0]
        if (instance_name, selector) in self.category_methods:
            return ("guest_imp", self.category_methods[(instance_name, selector)])
        if name in ("NSString", "NSMutableString") and selector in ("stringWithCString:", "stringWithCString:encoding:", "stringWithUTF8String:"):
            return self.cfstring(self.cstring(self.reg(2)))
        startup_classes = ("NSAutoreleasePool", "NSRecursiveLock", "NSLock")
        if name in startup_classes and selector == "alloc":
            obj = self.alloc(4)
            self.objects[obj] = (name, None)
            self.put_word(obj, receiver)
            return obj
        if name == "UIDevice" and selector == "currentDevice":
            if not hasattr(self, "device"):
                self.device = self.alloc(4)
                self.objects[self.device] = ("UIDevice", None)
                self.put_word(self.device, receiver)
            return self.device
        if receiver in self.objects and self.objects[receiver][0] == "UIDevice":
            if selector == "systemVersion":
                return self.cfstring("3.0")
            if selector == "userInterfaceIdiom":
                return 0
            if selector == "model":
                return self.cfstring("iPhone")
        is_string = (receiver in self.objects and self.objects[receiver][0] == "string") or \
                    self.class_names.get(self.word(receiver)) == "__NSCFConstantString"
        if is_string:
            if selector == "floatValue":
                return self.float_result(float(self.string(receiver)), "f")
            if selector == "UTF8String":
                return self.data(self.string(receiver).encode() + b"\0")
            if selector == "length":
                return len(self.string(receiver).encode("utf-16-le")) // 2
            if selector == "isEqualToString:":
                return int(self.string(receiver) == self.string(self.reg(2)))
            if selector == "hasPrefix:":
                return int(self.string(receiver).startswith(self.string(self.reg(2))))
            if selector in ("retain", "autorelease", "copy"):
                return receiver
            if selector == "release":
                return 0
        if receiver in self.objects and self.objects[receiver][0] in startup_classes:
            if selector == "init":
                return receiver
            if selector in ("release", "drain"):
                return 0
            if self.objects[receiver][0] in ("NSLock", "NSRecursiveLock") and selector in ("lock", "unlock"):
                return 0  # Serialized, single-thread startup probe only.
        raise NotImplementedError(f"Probe ObjC {name or self.objects.get(receiver)} {selector}")

    def application_main(self):
        self.ui_application = dict(argc=self.reg(0), delegate=self.string(self.reg(3)),
                                   pc=f"0x{self.uc.reg_read(UC_ARM_REG_PC):x}")
        self.uc.emu_stop()
        return 0

    def import_call(self, uc, address, size, user_data):
        name = next((n for n, a in self.thunks.items() if a == address), None)
        if not name or name not in self.handlers:
            self.failure = f"Unimplemented guest import {name or hex(address)}"
            uc.emu_stop()
            return
        self.used_imports[name] += 1
        try:
            result = self.handlers[name]()
            if isinstance(result, tuple) and result[0] == "guest_imp":
                # Dispatch the game's original ObjC category implementation,
                # preserving guest registers/stack and the caller's LR.
                uc.reg_write(UC_ARM_REG_PC, result[1])
                return
            if result is not None:
                uc.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
                # 64-bit/double helpers return r0:r1, others only change r0.
                if name in self.wide_results:
                    uc.reg_write(UC_ARM_REG_R1, (result >> 32) & 0xffffffff)
        except Exception as error:
            self.failure = f"{name}: {error}"
            uc.emu_stop()

    def return_hook(self, uc, address, size, user_data):
        self.returned = True
        uc.emu_stop()

    def memory_fault(self, uc, access, address, size, value, user_data):
        self.failure = f"Guest memory fault access={access} address={address:#x} pc={uc.reg_read(UC_ARM_REG_PC):#x}"
        return False

    def call(self, address, args, instruction_limit):
        self.returned = False
        # Switch banked CPU mode before writing SP/LR, otherwise user SP/LR stay zero.
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x10 | (0x20 if address & 1 else 0))
        self.uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SIZE - 256)
        self.uc.reg_write(UC_ARM_REG_LR, STOP)
        for index, value in enumerate(args[:4]):
            self.uc.reg_write(REGISTERS[index], value)
        try:
            self.uc.emu_start(address, 0xffffffff, count=instruction_limit)
        except UcError as error:
            self.failure = self.failure or str(error)
        if self.failure:
            raise RuntimeError(self.failure)
        if not self.returned and self.ui_application is None:
            raise RuntimeError(f"Instruction limit reached at {self.uc.reg_read(UC_ARM_REG_PC):#x}")

    def run(self, main, instruction_limit):
        argv = self.data(struct.pack("<2I", self.data(b"sims3wa_iphone\0"), 0))
        env = self.data(b"\0" * 4)
        initializers = self.image.pointers("__mod_init_func")
        for index, address in enumerate(initializers):
            self.call(address, [1, argv, env, env], instruction_limit)
            self.initializers.append(f"0x{address:x}")
            print(f"Original constructor {index + 1}/{len(initializers)} returned", flush=True)
        if main:
            symbol = next(s for s in self.image.symbols if s["name"] == "_main" and s["type"] == 14)
            self.call(symbol["value"], [1, argv, 0, 0], instruction_limit)
        return dict(host=platform.machine(), host_pointer_bits=struct.calcsize("P") * 8,
                    guest="ARM32 ARMv6 + Thumb", cpu_backend="Unicorn 2.1.4", guest_base="0x1000",
                    native_arm32_execution=False, original_constructors=len(self.initializers),
                    expected_constructors=len(initializers), ui_application_main=self.ui_application,
                    used_imports=dict(self.used_imports), data_placeholders=getattr(self, "data_imports", {}),
                    used_selectors=dict(self.used_selectors),
                    guest_heap_bytes=self.heap_at - HEAP, elapsed_seconds=time.monotonic() - self.started,
                    limitation="Startup CPU proof only; no UIKit implementation, graphics, Android APK or gameplay.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--main", action="store_true", help="Run _main until the UIApplicationMain boundary")
    parser.add_argument("--instruction-limit", type=int, default=5_000_000)
    parser.add_argument("--output", type=Path, default=Path("analysis/emulated-boot.json"))
    args = parser.parse_args()
    binary = args.binary.read_bytes()
    guest = Guest(binary)
    try:
        result = guest.run(args.main, args.instruction_limit)
    except RuntimeError as error:
        result = dict(success=False, error=str(error), pc=f"0x{guest.uc.reg_read(UC_ARM_REG_PC):x}",
                      original_constructors=len(guest.initializers), used_imports=dict(guest.used_imports))
    else:
        result["success"] = True
    result["executable_sha256"] = hashlib.sha256(binary).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
