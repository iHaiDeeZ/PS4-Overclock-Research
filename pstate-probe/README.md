# pstate-probe

A tiny **read-only** PS4 payload that reads the AMD Jaguar **CPU P-state MSRs** in ring 0 and
decodes them into frequency (MHz) and voltage (mV). It uses the ps4-payload-sdk (libPS4) `kexec`
primitive to run `rdmsr` in kernel mode — `rdmsr` is privileged, so a plain userland payload can't
do it — then `copyout`s the values to userland and prints them.

It reads only MSRs the PS4 kernel itself is proven to access (so no `rdmsr` can `#GP` on a missing
register) and issues **no `wrmsr`**. It writes nothing to any clock, voltage, or control register.

## MSRs read

`0xC0010061` (P-state limit), `0xC0010062` (control), `0xC0010063` (status),
`0xC0010064`–`0xC001006B` (P-state definitions P0–P7), `0xC0010070`/`71` (COFVID), and
`0xC0000082` (LSTAR, as a ring-0 sanity check).

Decode (family 16h): `CoreCOF = 100 × (FID + 0x10) / 2^DID`; bit 63 = PstateEn.

## Build

With the [ps4-payload-sdk](https://github.com/CelesteBlue-dev/PS4-SDK) (libPS4):

```bash
make PS4SDK=/path/to/ps4-payload-sdk
```

Produces `pstate-probe.bin`.

## Run

Load `pstate-probe.bin` with any GoldHEN / BinLoader payload loader on a jailbroken console. Output
goes to klog (TCP port 3232) and an on-screen notification — the full P-state table, the current
state, and a `LSTAR hi=0xffffffff` line confirming the reads executed in ring 0.

The payload is firmware-portable: it resolves kernel offsets via `get_firmware()`, so the same
`.bin` runs across the firmwares libPS4 supports (tested on 14.02 and 14.00).
