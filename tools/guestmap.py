"""Map lines of the generated C++ back to guest instructions (shared by the symbolizer tools).

In the generated code each guest instruction is preceded by a `// <mnemonic> <operands>` comment; the active
guest instruction at a line is the last such comment above it. Its address is rebuilt from the function
start (PPCFuncMappings in *_init.cpp) and the `loc_XXXXXXXX:` labels.
"""
import glob
import os
import re


class GuestMap:
    def __init__(self, gen_dir):
        self.gen_dir = gen_dir
        self.func_addr = {}
        init = glob.glob(os.path.join(gen_dir, "*_init.cpp"))
        if init:
            for m in re.finditer(r"\{ 0x([0-9A-Fa-f]{8}), (\w+) \}", open(init[0]).read()):
                self.func_addr[m.group(2)] = int(m.group(1), 16)
        self._maps = {}

    def lookup(self, path, lineno):
        """(guest address, instruction text, function name) for a generated file line, or None."""
        if not os.path.exists(path):
            path = os.path.join(self.gen_dir, os.path.basename(path))
            if not os.path.exists(path):
                return None
        if path not in self._maps:
            result = {}
            addr = func = current = None
            for n, line in enumerate(open(path), start=1):
                m = re.match(r"^DEFINE_REX_FUNC\((\w+)\)", line)
                if m:
                    func = m.group(1)
                    addr = self.func_addr.get(func)
                    current = None
                else:
                    lm = re.match(r"^loc_([0-9A-F]{8}):", line)
                    if lm:
                        addr = int(lm.group(1), 16)
                    else:
                        cm = re.match(r"^\t// (\S.*)$", line)
                        if cm and addr is not None:
                            current = (addr, cm.group(1).strip(), func)
                            addr += 4
                if current:
                    result[n] = current
            self._maps[path] = result
        return self._maps[path].get(lineno)
