# PS4-Overclock-Research

**Can a base PlayStation 4 be overclocked?** A hands-on answer from reverse-engineering the
FW 14.02 kernel and reading the **live CPU P-state MSRs** off running consoles with a small
**read-only** payload.

**Short answer: no — not realistically or safely.** The CPU P-state table tops out at 1.6 GHz with
no higher state defined; the GPU/memory clocks live in the SMU/Aeolia secure co-processor the
kernel can't reach; and the only writable "knob" is overridden by the SMU and would overvolt a rail
and cooler built for exactly 1.6 GHz.

Measured live on a base PS4 vs a PS4 Pro (same payload, `kexec ret=0`, ring-0 confirmed):

| P-state | Base PS4 | PS4 Pro |
|---------|----------|---------|
| P0 | **1600 MHz** (current) | **2100 MHz** (current) |
| P1 | 1400 | 1800 |
| P2 | 1200 | **1600** (base-compat) |
| P3 | 1000 | 1400 |
| P4 / P5 | 800 | 1200 / 800 |
| P6 / P7 | — (disabled) | 1200 / 800 |
| **top defined** | **1600 MHz** | **2100 MHz** |

The Pro *defines* states above 1.6 GHz; the base PS4 does not. **PS4 Pro "Boost Mode" is therefore
not an overclock** — it just lets an unpatched game use the Pro's already-defined higher states
instead of clamping to the 1600 MHz base-compat state. There is nothing equivalent to unlock on
base hardware.

## Contents

- **[OVERCLOCK-RESEARCH.md](OVERCLOCK-RESEARCH.md)** — full write-up: the AMD P-state MSR mechanism,
  the measured base/Pro tables, the GPU/SMU/ICC analysis, and (as inert commented analysis) exactly
  what a >1.6 GHz write would be and why it fails.
- **[pstate-probe/](pstate-probe/)** — the read-only measurement payload (source + Makefile + prebuilt
  `.bin`). It runs `rdmsr` in ring 0 via the ps4-payload-sdk `kexec` primitive and decodes the
  P-state table. It never writes an MSR.

## Read-only / scope

Reverse-engineering and measurement of hardware the researcher owns, for educational purposes. No
kernel binaries are redistributed — only RE findings and original code. Nothing here writes a clock
or voltage register. Overclocking a base PS4 by the route described is not endorsed: it risks
permanent damage for no demonstrated benefit.

## License

MIT — see [LICENSE](LICENSE).
