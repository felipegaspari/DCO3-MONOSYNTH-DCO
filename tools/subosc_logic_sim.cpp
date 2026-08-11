// Host-side check of the subosc_logic boolean combiner (ENABLE_SUBOSC_ENGINE2).
//
// Decodes and executes the eight *hex-encoded* instruction words exactly as they appear in
// pico-dco.pio.h, so this validates the hand encoding and not just the intent. For each
// operator it holds the two input pins at each of the four combinations, runs the program
// until it comes back around, and reads the output off the side-set. What it is looking for:
//
//   * encoding:        the words decode to the instructions the .pio source says they are
//   * truth tables:    the output matches XOR / AND / OR / XNOR / NAND / NOR, and the two
//                      pass-throughs follow the sub they name. Those two are the asymmetric
//                      ones, so they are also what pins down that sub 1 is the low bit of the
//                      table index (it sits on the IN base, the lower of the two pads).
//   * shift direction: the ISR -> OSR -> PC path only reaches all four table entries with IN
//                      shifting left and OUT shifting right. Running the same program with
//                      the SDK's defaults (both right) must fail, which is what makes the
//                      sm_config_set_in_shift / set_out_shift calls in subosc_logic_init()
//                      load-bearing rather than decoration.
//   * timing:          five cycles per sample
//
//   g++ -O2 -o /tmp/subosc_logic_sim tools/subosc_logic_sim.cpp && /tmp/subosc_logic_sim

#include <cstdint>
#include <cstdio>

// pico-dco.pio.h, subosc_logic_program_instructions[]. Entries 0..3 are rewritten at runtime
// by subosc_logic_write_table(); the defaults below are XOR.
static uint16_t prog[8] = {
    0x1004, 0x1804, 0x1804, 0x1004, 0xa0c3, 0x4002, 0xa0e6, 0x60a2,
};

// subosc_logic_entry_instr(): jmp entry, side-setting the output to `high`.
static uint16_t entry_instr(bool high) {
  return (uint16_t)(0x1004u | (high ? 0x0800u : 0x0000u));
}

// SUBOSC_LOGIC_MASKS[], bit i = output for table entry i, index = (sub2<<1)|sub1. Each mask is
// paired with the operation it is supposed to be, spelled out independently, so a wrong mask
// fails here instead of quietly defining its own truth table.
struct Op {
  const char* name;
  uint8_t mask;
  bool (*fn)(bool sub1, bool sub2);
};
static const Op OPS[] = {
    { "XOR", 0x6, [](bool a, bool b) { return a != b; } },
    { "AND", 0x8, [](bool a, bool b) { return a && b; } },
    { "OR", 0xE, [](bool a, bool b) { return a || b; } },
    { "XNOR", 0x9, [](bool a, bool b) { return a == b; } },
    { "NAND", 0x7, [](bool a, bool b) { return !(a && b); } },
    { "NOR", 0x1, [](bool a, bool b) { return !(a || b); } },
    { "sub1", 0xA, [](bool a, bool b) { (void)b; return a; } },
    { "sub2", 0xC, [](bool a, bool b) { (void)a; return b; } },
};

struct Result {
  bool out;
  int cycles;
  bool ok;
};

// Just enough PIO to run this program: the five instruction forms it uses, decoded from the
// word rather than assumed. Anything else is an encoding error and fails the run.
//
// One sample, starting at `entry` (the steady state, which is where a table entry's jmp lands)
// and ending on the table entry that jump dispatches to, since that entry is what drives the
// output. Leaving the ISR and OSR dirty between samples would be more faithful, but `mov isr,
// null` is the first thing the program does, so it cannot matter - and starting them at zero
// would hide a missing clear.
static Result run(uint32_t pins, bool in_shift_right, bool out_shift_right) {
  uint32_t isr = 0xFFFFFFFFu, osr = 0xFFFFFFFFu, pc = 4;
  bool out = false;
  int cycles = 0;

  for (int step = 0; step < 32; step++) {
    const uint16_t w = prog[pc];
    const uint16_t op = (uint16_t)(w >> 13);
    const uint16_t operand = (uint16_t)(w & 0x00FFu);
    uint32_t next = (pc + 1) & 7u;
    cycles++;

    if (op == 0) {  // JMP
      if ((w & 0x1000u) != 0) {  // side-set enabled
        out = (w & 0x0800u) != 0;
      }
      if ((operand >> 5) != 0) {
        printf("  FAIL: conditional jmp at %u (0x%04x)\n", pc, w);
        return { out, cycles, false };
      }
      next = operand & 0x1Fu;
      // The only jmp in the program is a table entry, and it is what sets the output, so
      // reaching one ends the sample. It must send control back to `entry`.
      if (next != 4) {
        printf("  FAIL: table entry at %u jumps to %u, expected 4\n", pc, next);
        return { out, cycles, false };
      }
      return { out, cycles, true };
    } else if (op == 2) {  // IN
      const uint32_t src = (uint32_t)((operand >> 5) & 7u);
      const uint32_t count = (uint32_t)(operand & 0x1Fu);
      if (src != 0) {
        printf("  FAIL: in from source %u, expected pins\n", src);
        return { out, cycles, false };
      }
      const uint32_t data = pins & ((1u << count) - 1u);
      isr = in_shift_right ? ((isr >> count) | (data << (32 - count)))
                           : ((isr << count) | data);
    } else if (op == 3) {  // OUT
      const uint32_t dest = (uint32_t)((operand >> 5) & 7u);
      const uint32_t count = (uint32_t)(operand & 0x1Fu);
      const uint32_t bits = out_shift_right ? (osr & ((1u << count) - 1u))
                                            : (osr >> (32 - count));
      osr = out_shift_right ? (osr >> count) : (osr << count);
      if (dest != 5) {
        printf("  FAIL: out to dest %u, expected pc\n", dest);
        return { out, cycles, false };
      }
      next = bits;
    } else if (op == 5) {  // MOV
      const uint32_t dest = (uint32_t)((operand >> 5) & 7u);
      const uint32_t mop = (uint32_t)((operand >> 3) & 3u);
      const uint32_t src = (uint32_t)(operand & 7u);
      if (mop != 0) {
        printf("  FAIL: mov with operation %u, expected none\n", mop);
        return { out, cycles, false };
      }
      uint32_t v = 0;
      if (src == 3) v = 0;            // null
      else if (src == 6) v = isr;     // isr
      else {
        printf("  FAIL: mov from source %u\n", src);
        return { out, cycles, false };
      }
      if (dest == 6) isr = v;         // isr (also clears the input shift counter)
      else if (dest == 7) osr = v;    // osr (marks it full, so no pull is needed)
      else {
        printf("  FAIL: mov to dest %u\n", dest);
        return { out, cycles, false };
      }
    } else {
      printf("  FAIL: unexpected opcode %u (0x%04x) at %u\n", op, w, pc);
      return { out, cycles, false };
    }

    pc = next;
  }

  printf("  FAIL: program never reached a table entry\n");
  return { out, cycles, false };
}

static bool expected(uint8_t mask, uint32_t pins) {
  return ((mask >> (pins & 3u)) & 1u) != 0;
}

int main() {
  int failures = 0;

  printf("operator truth tables (IN left, OUT right):\n");
  for (const Op& o : OPS) {
    for (uint32_t i = 0; i < 4; i++) {
      prog[i] = entry_instr(((o.mask >> i) & 1u) != 0);
    }
    printf("  %-4s ", o.name);
    for (uint32_t pins = 0; pins < 4; pins++) {
      const Result r = run(pins, /*in_right=*/false, /*out_right=*/true);
      const bool sub1 = (pins & 1u) != 0;
      const bool sub2 = ((pins >> 1) & 1u) != 0;
      const bool want = o.fn(sub1, sub2);
      const bool bad = !r.ok || r.out != want || r.cycles != 5 ||
                       expected(o.mask, pins) != want;
      printf("s1=%u s2=%u -> %u%s  ", sub1 ? 1u : 0u, sub2 ? 1u : 0u, r.out ? 1u : 0u,
             bad ? " BAD" : "");
      if (bad) failures++;
    }
    printf("\n");
  }

  // The default shift pairing has to be shown to fail, or the explicit configuration in
  // subosc_logic_init() looks optional. With IN right the two bits land at the top of the ISR
  // and OUT right reads zeros, so every combination dispatches to entry 0.
  printf("\nsame program with the SDK default shifts (both right), XOR table:\n");
  for (uint32_t i = 0; i < 4; i++) {
    prog[i] = entry_instr(((0x6 >> i) & 1u) != 0);
  }
  bool all_entry_zero = true;
  for (uint32_t pins = 0; pins < 4; pins++) {
    const Result r = run(pins, /*in_right=*/true, /*out_right=*/true);
    printf("  s1=%u s2=%u -> %u (want %u)\n", pins & 1u, (pins >> 1) & 1u, r.out ? 1u : 0u,
           expected(0x6, pins) ? 1u : 0u);
    if (r.out != expected(0x6, 0)) all_entry_zero = false;
  }
  if (!all_entry_zero) {
    printf("  FAIL: expected every combination to collapse to entry 0\n");
    failures++;
  } else {
    printf("  collapses to entry 0 as predicted, so the explicit shift config is required\n");
  }

  printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
