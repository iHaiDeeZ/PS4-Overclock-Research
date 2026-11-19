# PS4 (base) overclock feasibility — kernel RE + live-console research

Can a base PlayStation 4 be overclocked? This is a hands-on answer: reverse-engineering the
FW 14.02 kernel and reading the **live CPU P-state MSRs** off running consoles — a base PS4,
and a PS4 Pro for comparison — with a purpose-built **read-only** payload.

**Targets:** base PS4, FW 14.02 (build `J02700521`); PS4 Pro, FW 14.00 (build `J02699047`).
Kernel link base `0xFFFFFFFF82200000`.

> **Everything here is read-only.** No `wrmsr` to any clock or voltage register was ever issued.
> The one "overclock write" appears only as commented analysis in the appendix.

---

## TL;DR — no, not realistically or safely

- The **CPU** clock is controlled by standard **AMD P-state MSRs**. We read the live table: it
  **tops out at P0 = 1600 MHz**. There is no higher state defined — you can't *select* a faster
  clock, only *create* one by rewriting a P-state definition MSR.
- That rewrite (the only "knob") is almost certainly **re-asserted by the SMU**, the kernel's
  cpufreq layer caches the old values anyway, and it would mean **overvolting a fixed Vcore rail
  and a cooler built for exactly 1.6 GHz / 800 MHz**. Brick/thermal risk, no proven payoff.
- The **GPU and GDDR5** clocks aren't in the x86 MSR space at all — they live in the
  **SMU / Aeolia (ICC) secure co-processor** below the kernel, reached only by a fixed mailbox
  command set that has no "raise clock" command.
- **PS4 Pro "Boost Mode" is not an overclock.** The Pro's P-state table *defines* higher states
  (1800, 2100 MHz); Boost Mode just lets an unpatched game use them instead of clamping to the
  1600 MHz base-compat state. Base PS4 has no such higher state for anything to unlock.

---

## How this was measured

- **Kernel R/W** via ps4debug confirmed the kernel base and known globals (`hz`, `nswbuf`) and the
  live CPUID signature `0x00710F13` → **AMD family 0x16 (Jaguar), model 0x11, stepping 3** — the
  base-PS4 "Liverpool" APU.
- **Ring-0 MSR reads** via [`pstate-probe`](pstate-probe/) — a small payload that uses the
  ps4-payload-sdk (libPS4) `kexec` primitive to run `rdmsr` in kernel mode on the P-state MSRs,
  `copyout`s the values to userland, and decodes FID/DID/VID → MHz/mV. It reads only MSRs the
  kernel itself is proven to touch (so no `rdmsr` can `#GP`) and issues **no `wrmsr`** — strictly
  read-only. `rdmsr` is privileged, so a userland payload can't do it; `kexec` is what makes the
  ring-0 read possible. The same `.bin` is firmware-portable (it resolves offsets per
  `get_firmware()`), which is why it ran unchanged on both 14.02 and 14.00.

---

## CPU clock mechanism (AMD P-state MSRs)

The kernel ships the full FreeBSD `cpufreq` stack — `hwpstate` (`sys/x86/cpufreq/hwpstate.c`),
`acpi_perf`, `acpi_throttle`, ACPI thermal-zone passive cooling — plus Sony's `icc_thermal`.
CPU frequency is the standard AMD P-state MSR interface:

| MSR | Role |
|-----|------|
| `0xC0010061` | P-state Current Limit (max index in low 3 bits) |
| `0xC0010062` | **P-state Control** — write target index to transition |
| `0xC0010063` | P-state Status — current index |
| `0xC0010064`–`0xC001006B` | **P-state Definitions P0–P7** (FID/DID/VID per state; bit 63 = PstateEn) |
| `0xC0010070`/`71` | COFVID control / status |

Two functions confirm the mechanism (disassembled from the dump):

- **`hwpstate_goto_pstate`** (VA `0xffffffff8242e180`) — reads `0xC0010061`, masks the low 3 bits
  (= max index), clamps the request, then `wrmsr 0xC0010062` to switch. It only selects among
  **already-defined** states (all ≤ 1.6 GHz) — i.e. selecting/downclocking, never overclocking.
- **P-state-enable init** (VA `0xffffffff8236e270`) — gated to a Jaguar CPUID; for each definition
  MSR `0xC0010064`–`6B` it does `rdmsr; or edx,0x80000000; wrmsr`, i.e. sets **bit 63 (PstateEn)**.
  This proves the kernel *writes* the definition MSRs, so they are ring-0-writable on this silicon.

Frequency decode (family 16h): `CoreCOF(MHz) = 100 × (FID + 0x10) / 2^DID`.

---

## Measured: base PS4 stock P-state table

Read live with `pstate-probe` (`kexec ret=0`; `LSTAR` high dword `ffffffff` confirms ring-0):

| P-state | enabled | FID | DID | frequency | raw MSR |
|---------|---------|-----|-----|-----------|---------|
| **P0** | yes (**current**) | 0x10 | 1 | **1600 MHz** | `0x8000012d00007850` |
| P1 | yes | 0x0c | 1 | 1400 MHz | `…784c` |
| P2 | yes | 0x08 | 1 | 1200 MHz | `…7848` |
| P3 | yes | 0x04 | 1 | 1000 MHz | `…7844` |
| P4 | yes | 0x00 | 1 | 800 MHz | `…7840` |
| P5 | yes | 0x00 | 1 | 800 MHz | `…7840` |
| P6 | no | — | 1 | (800) | `0x0000012d0000a840` |
| P7 | no | — | 1 | (800) | `…a840` |

- Clean round clocks validate the decode. Limit MSR `0xC0010061 = 0x50` → 6 usable states. Status
  `0xC0010063 = 0` → running at **P0 (max)** even idle at the dashboard — consistent with the
  known fixed-clock design (no in-menu downclock).
- **Voltage:** the CpuVid field is identical across all enabled states → a **single fixed Vcore
  rail** (no per-state scaling), ≈1.17 V nominal (SVI2; exact mV depends on the family-16h VID
  field offset — treat as approximate).
- **Headroom: none.** The top defined state is 1.6 GHz; P6/P7 are disabled 800 MHz placeholders.

---

## Comparison: PS4 Pro — what Boost Mode actually selects

The identical payload on a PS4 Pro (FW 14.00) returns a very different table:

| P-state | Base PS4 | PS4 Pro |
|---------|----------|---------|
| P0 | **1600 MHz** (current) | **2100 MHz** (current) |
| P1 | 1400 | 1800 |
| P2 | 1200 | **1600** (base-compat) |
| P3 | 1000 | 1400 |
| P4 | 800 | 1200 |
| P5 | 800 | 800 |
| P6 | — (disabled) | 1200 |
| P7 | — (disabled) | 800 |
| enabled | 6 | 8 |
| **top defined** | **1600 MHz** | **2100 MHz** |

- The Pro's table **defines 1800 and 2100 MHz states that do not exist on base PS4**, plus a
  **1600 MHz state (P2)** — the base-compat clock an unpatched game is normally clamped to.
- **Boost Mode = letting an unpatched game climb to the Pro's high P-states (up to P0 / 2100 MHz)
  instead of clamping to P2 / 1600 MHz.** The higher clocks were defined all along; Boost Mode
  stops withholding them. Base PS4 can't do this — its table has nothing above 1600 MHz to grant.
- The Pro also shows **per-state voltage scaling** (VID differs across the P0/P1, P2–P5, P6/P7
  groups) — real DVFS — whereas base PS4 ran one fixed Vcore across all states.

The clock ceiling is the **per-SKU SMU P-state table**. Boost Mode *selects within* it; it is not
an overclock.

---

## GPU / GDDR5 clocks — the SMU / ICC secure domain

No GPU or memory clock lives in the AMD MSR range. Those clocks are owned by the **SMU** (on-die
power microcontroller) and the **Aeolia/Belize/Baikal ICC** southbridge secure processor, reached
from the kernel only through a fixed mailbox (`icc_send` / `icc_query`). The entire ICC command
vocabulary is visible in the binary (`sys/dev/scesb/icc/`): power sequencing, NVS read/write,
thermal, **fan duty**, LED/buzzer, buttons, BD-drive power, signed firmware update. **There is no
"set clock / set pstate / set voltage" command**, and it self-gates (`icc_query_critical is not
allowed`). The on-die SMU runs firmware loaded and signed by the secure boot chain and is not in
the FreeBSD kernel's reach at all. (The one power-domain knob that *is* exposed is the fan.)

---

## Appendix: the >1.6 GHz write (analysis only — NOT performed)

For completeness, this is the exact edit an overclock attempt *would* make. **It was not
executed** — no `wrmsr` to a clock/voltage register was issued anywhere in this research.

The knob is the P0 definition MSR `0xC0010064`. With `CoreCOF = 100 × (FID + 0x10) / 2^DID` and
`DID=1`, base P0 is `FID=0x10 → 1600 MHz`. Raising FID raises the clock:

| target | required FID | note |
|--------|-------------|------|
| 1600 MHz (stock) | 0x10 | current |
| 1800 MHz | 0x14 | = the Pro's P1 |
| 2100 MHz | 0x1a | = the Pro's P0 |

```c
/* ANALYSIS ONLY — never executed. Would require a kernel payload with wrmsr. */
uint64_t d = __readmsr(0xC0010064);   /* read P0 definition            */
d = (d & ~0x3FULL) | 0x14;            /* FID 0x10 -> 0x14 (1600->1800)  */
/* stability would also need a lower CpuVid (= more voltage)            */
/* __wrmsr(0xC0010064, d);   <-- THE OVERCLOCK WRITE (deliberately omitted) */
```

Why it doesn't yield a working overclock, and is a brick/thermal risk:

1. **SMU re-assertion** — the SMU owns P-state management and programs this table; a post-boot MSR
   change is expected to be overwritten or ignored.
2. **Cached settings** — `hwpstate`/`acpi_perf` read the definitions at attach; editing the MSR
   afterward doesn't propagate to the kernel's cpufreq view.
3. **Voltage** — one fixed Vcore, no per-state scaling. Higher FID without more VID → instability;
   more VID → overvolting a rail/cooler sized for 1.6 GHz.
4. **Thermal / VRM** — power delivery and cooling are specced for 1.6 GHz / 800 MHz, no margin.

Even bypassing the kernel, the real arbiter is the SMU, and you'd still need the higher states to
exist in its (signed) firmware and the hardware to physically sustain them — which base PS4 lacks.

---

## Reproduce

[`pstate-probe/`](pstate-probe/) builds with the [ps4-payload-sdk](https://github.com/CelesteBlue-dev/PS4-SDK)
(libPS4). From that SDK root:

```bash
cd pstate-probe
make PS4SDK=/path/to/ps4-payload-sdk
```

Load `pstate-probe.bin` with any GoldHEN/BinLoader payload loader on a jailbroken console and read
its output on klog (port 3232) or the on-screen notification. It prints the full P-state table and
a ring-0 sanity line. It is **read-only** — it never writes an MSR.

## Scope & safety

This is reverse-engineering / measurement of hardware the researcher owns, for educational
purposes. No kernel binaries are redistributed — only RE findings (addresses, tables) and original
code. Nothing here writes a clock or voltage register, and the appendix OC write is deliberately
inert. Overclocking a console by the described route is not endorsed: it risks permanent damage
for no demonstrated benefit.
