"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: double fp_stack[8] with fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

from .disasm import Instruction, Operand
from .config import is_code_address, is_data_address, KERNEL_THUNK_ADDR


# ── Operand formatting ──────────────────────────────────────

def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size):
    """Return the MEM macro name for a given operand size."""
    return {1: "MEM8", 2: "MEM16", 4: "MEM32"}.get(size, "MEM32")


def _smem_accessor(size):
    """Return the signed MEM macro for a given operand size."""
    return {1: "SMEM8", 2: "SMEM16", 4: "SMEM32"}.get(size, "SMEM32")


def _fmt_mem(op):
    """Format a memory operand as a C expression (the address computation)."""
    parts = []
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    if op.mem_disp:
        if op.mem_disp < 0:
            # Negative displacement - but we stored unsigned, check sign
            if op.mem_disp > 0x80000000:
                # Actually negative (two's complement)
                signed_disp = op.mem_disp - 0x100000000
                if parts:
                    parts.append(f"- {_fmt_imm(-signed_disp)}")
                else:
                    parts.append(_fmt_imm(op.mem_disp))
            else:
                parts.append(_fmt_imm(op.mem_disp))
        else:
            parts.append(_fmt_imm(op.mem_disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


def _fmt_operand_read(op):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op)
    return "/* unknown operand */"


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
}


# ── Pattern matching for cmp/test + jcc ──────────────────────

def try_match_cmp_jcc(insns, idx):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    lhs = _fmt_operand_read(first.operands[0])
    rhs = _fmt_operand_read(first.operands[1])
    jcc = second.mnemonic

    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None

    cmp_macro, test_macro, desc = cond_info

    if first.mnemonic == "cmp" and cmp_macro:
        cond_expr = f"{cmp_macro}({lhs}, {rhs})"
    elif first.mnemonic == "test" and test_macro:
        cond_expr = f"{test_macro}({lhs}, {rhs})"
    elif first.mnemonic == "test" and cmp_macro:
        # Fallback: test a, a with cmp-style macro
        cond_expr = f"{cmp_macro}({lhs} & {rhs}, 0)"
    elif first.mnemonic == "cmp" and not cmp_macro:
        # Rare jcc types (jo, jno, js, jns) after cmp
        return None
    else:
        return None

    target = second.jump_target
    if target:
        stmt = f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"
    else:
        stmt = f"if ({cond_expr}) goto /* indirect */; /* {jcc}: {desc} */"

    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self._fp_top = 0  # FPU stack top index

    def _call_target_name(self, addr):
        """Get the name for a call target address."""
        if addr in self.label_db:
            return self.label_db[addr]
        if addr in self.func_db:
            info = self.func_db[addr]
            name = info.get("name", f"sub_{addr:08X}")
            return name
        return f"sub_{addr:08X}"

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m in ("rep movsb", "rep movsd", "rep stosb", "rep stosd"):
            return self._lift_rep_string(insn, m)
        if m in ("movsb", "movsd", "stosb", "stosd", "lodsb", "lodsd"):
            return self._lift_string_op(insn, m)

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        if m == "int3":
            return ["__debugbreak(); /* int3 */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            return [f"/* {m} - direction flag */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Unhandled ──
        return [f"/* TODO: {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(ops[1].mem_size)
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "0") + " /* xor self */"]
        expr = f"{dst} {c_op} {src}"
        return [_fmt_operand_write(ops[0], expr)]

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        op = "++" if m == "inc" else "--"
        if ops[0].type == "reg":
            return [f"{val}{op};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op[0]} 1")]

    def _lift_neg(self, insn, ops):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"(uint32_t)(-(int32_t){val})")]

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        return [f"/* {m}: unhandled */"]

    def _lift_shift(self, insn, ops, c_op):
        if len(ops) < 2:
            return [f"/* shift: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], f"{dst} {c_op} {cnt}")]

    def _lift_sar(self, insn, ops):
        if len(ops) < 2:
            return ["/* sar: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} >> {cnt})")]

    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        func = "ROL32" if m == "rol" else "ROR32"
        return [_fmt_operand_write(ops[0], f"{func}({dst}, {cnt})")]

    # ── Compare / Test (standalone) ──

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        return [f"/* cmp {lhs}, {rhs} - flags set for next jcc */"]

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        return [f"/* test {lhs}, {rhs} - flags set for next jcc */"]

    # ── Control flow ──

    def _lift_call(self, insn, ops):
        if insn.call_target:
            name = self._call_target_name(insn.call_target)
            return [f"{name}(); /* call 0x{insn.call_target:08X} */"]
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            return [f"((void (*)(void)){target})(); /* indirect call */"]
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # ret N pops N extra bytes from stack
        if len(ops) >= 1 and ops[0].type == "imm":
            return [f"return; /* ret {ops[0].imm} */"]
        return ["return;"]

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            return [f"goto *{target}; /* indirect jmp - needs jump table */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (not matched as cmp+jcc pattern)."""
        target = insn.jump_target
        jcc = insn.mnemonic
        if target:
            return [f"if (/* {jcc} condition */) goto loc_{target:08X}; /* {jcc} */"]
        return [f"/* {jcc}: no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"/* {m} condition */ 0")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (/* {m} condition */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    def _lift_rep_string(self, insn, m):
        if "movsb" in m:
            return ["memcpy((void*)(uintptr_t)edi, (void*)(uintptr_t)esi, ecx);",
                    "esi += ecx; edi += ecx; ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            return ["memcpy((void*)(uintptr_t)edi, (void*)(uintptr_t)esi, ecx * 4);",
                    "esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */"]
        if "stosb" in m:
            return ["memset((void*)(uintptr_t)edi, (uint8_t)eax, ecx);",
                    "edi += ecx; ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }",
                "edi += ecx * 4; ecx = 0; /* rep stosd */"
            ]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi++; edi++; /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += 4; edi += 4; /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi++; /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += 4; /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi++; /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += 4; /* lodsd */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    def _lift_fpu(self, insn, m, ops):
        """Basic FPU instruction translation using double locals."""
        # FPU is complex. We translate common patterns to double operations.
        # Full accuracy would require an x87 stack emulator.

        if m == "fld":
            if len(ops) >= 1:
                if ops[0].type == "mem":
                    if ops[0].mem_size == 4:
                        return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                    elif ops[0].mem_size == 8:
                        return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld */"]
            return [f"/* fld {insn.op_str} */"]

        if m in ("fst", "fstp"):
            pop = "p" if m == "fstp" else ""
            if len(ops) >= 1 and ops[0].type == "mem":
                if ops[0].mem_size == 4:
                    return [f"MEMF({_fmt_mem(ops[0])}) = (float)fp_top(); fp_pop{pop}(); /* {m} */"]
                elif ops[0].mem_size == 8:
                    return [f"MEMD({_fmt_mem(ops[0])}) = fp_top(); fp_pop{pop}(); /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fild":
            if len(ops) >= 1 and ops[0].type == "mem":
                smem = _smem_accessor(ops[0].mem_size)
                return [f"fp_push((double){smem}({_fmt_mem(ops[0])})); /* fild */"]
            return [f"/* fild {insn.op_str} */"]

        if m in ("fist", "fistp"):
            if len(ops) >= 1 and ops[0].type == "mem":
                mem_acc = _mem_accessor(ops[0].mem_size)
                return [f"{mem_acc}({_fmt_mem(ops[0])}) = (int32_t)fp_top(); /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fadd":
            return [f"fp_st1() += fp_top(); fp_pop(); /* fadd */"]
        if m == "faddp":
            return [f"fp_st1() += fp_top(); fp_pop(); /* faddp */"]
        if m == "fsub":
            return [f"fp_st1() -= fp_top(); fp_pop(); /* fsub */"]
        if m == "fsubp":
            return [f"fp_st1() -= fp_top(); fp_pop(); /* fsubp */"]
        if m == "fmul":
            return [f"fp_st1() *= fp_top(); fp_pop(); /* fmul */"]
        if m == "fmulp":
            return [f"fp_st1() *= fp_top(); fp_pop(); /* fmulp */"]
        if m == "fdiv":
            return [f"fp_st1() /= fp_top(); fp_pop(); /* fdiv */"]
        if m == "fdivp":
            return [f"fp_st1() /= fp_top(); fp_pop(); /* fdivp */"]
        if m == "fchs":
            return [f"fp_top() = -fp_top(); /* fchs */"]
        if m == "fabs":
            return [f"fp_top() = fabs(fp_top()); /* fabs */"]
        if m == "fsqrt":
            return [f"fp_top() = sqrt(fp_top()); /* fsqrt */"]
        if m == "fxch":
            return [f"{{ double _t = fp_top(); fp_top() = fp_st1(); fp_st1() = _t; }} /* fxch */"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            return [f"/* {m} {insn.op_str} - FPU compare, sets FPU flags */"]
        if m == "fnstsw":
            return [f"/* fnstsw {insn.op_str} - store FPU status word */"]
        if m == "fnstcw":
            return [f"/* fnstcw {insn.op_str} - store FPU control word */"]
        if m == "fldcw":
            return [f"/* fldcw {insn.op_str} - load FPU control word */"]
        if m == "fldz":
            return [f"fp_push(0.0); /* fldz */"]
        if m == "fld1":
            return [f"fp_push(1.0); /* fld1 */"]

        return [f"/* FPU: {m} {insn.op_str} */"]


def lift_basic_block(lifter, bb):
    """
    Lift a basic block to C statements.
    Uses pattern matching for cmp/test+jcc pairs.
    Returns list of C statement strings.
    """
    stmts = []
    insns = bb.instructions
    i = 0
    while i < len(insns):
        # Try cmp/test + jcc pattern first
        match = try_match_cmp_jcc(insns, i)
        if match:
            stmt, consumed = match
            stmts.append(stmt)
            i += consumed
            continue

        # Single instruction
        results = lifter.lift_instruction(insns[i])
        stmts.extend(results)
        i += 1

    return stmts
