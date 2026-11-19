/* pstate-probe - PS4 FW 14.02 (J02700521) CPU P-state MSR reader (READ-ONLY).
 *
 * Measures the stock AMD Jaguar P-state table on a live console by executing
 * rdmsr in ring 0 (via the ps4-payload-sdk kexec primitive) on the P-state
 * MSRs, copying the values back to userland, and decoding FID/DID/VID into
 * MHz / mV. It reads ONLY MSRs that the 14.02 kernel itself is proven to
 * access (verified by disassembly of kernel1402.bin + the SDK), so no rdmsr
 * can #GP on a missing MSR. It performs NO wrmsr and writes NOTHING to any
 * clock/voltage/control register. Pure measurement.
 *
 * MSRs read (all confirmed present on this silicon):
 *   0xC0010061 P-state Current Limit   (kernel rdmsr @ffffffff8242e195)
 *   0xC0010062 P-state Control         (kernel wrmsr @ffffffff8242e1ca)
 *   0xC0010063 P-state Status          (kernel rdmsr @ffffffff8242e104)
 *   0xC0010064..6B P-state Def 0..7     (kernel rd/wrmsr @ffffffff8236e287)
 *   0xC0010070 COFVID Control          (kernel wrmsr @ffffffff828e587c)
 *   0xC0010071 COFVID Status           (kernel rdmsr @ffffffff8242e514)
 *   0xC0000082 LSTAR                   (SDK reads it; ring-0 sanity)
 *
 * AMD family 16h (Jaguar) P-state def encoding (MSRC001_006[B:4]):
 *   bit63 PstateEn | VID[21:14] | DID[8:6] | FID[5:0]
 *   CoreCOF(MHz) = 100 * (FID + 16) / 2^DID
 *   Vcore(mV)    = 1550 - 6.25 * VID        (SVI2 nominal)
 */
#include "ps4.h"

#define NMSR 14

struct kpayload_rdmsr_info {
  uint16_t fw_version;
  uint64_t uaddr; /* userland buffer, NMSR * 8 bytes */
};
struct kpayload_rdmsr_args {
  void *syscall_handler;
  struct kpayload_rdmsr_info *kpayload_rdmsr_info;
};

/* runs in ring 0 via kexec - keep minimal, no userland calls except copyout */
int kpayload_rdmsr(struct thread *td, struct kpayload_rdmsr_args *args) {
  UNUSED(td);
  void *kernel_base;
  int (*copyout)(const void *kaddr, void *uaddr, size_t len);

  uint16_t fw_version = args->kpayload_rdmsr_info->fw_version;
  build_kpayload(fw_version, copyout_macro); /* sets kernel_base + copyout */

  uint64_t v[NMSR];
  v[0]  = __readmsr(0xC0010061);
  v[1]  = __readmsr(0xC0010062);
  v[2]  = __readmsr(0xC0010063);
  v[3]  = __readmsr(0xC0010064);
  v[4]  = __readmsr(0xC0010065);
  v[5]  = __readmsr(0xC0010066);
  v[6]  = __readmsr(0xC0010067);
  v[7]  = __readmsr(0xC0010068);
  v[8]  = __readmsr(0xC0010069);
  v[9]  = __readmsr(0xC001006A);
  v[10] = __readmsr(0xC001006B);
  v[11] = __readmsr(0xC0010070);
  v[12] = __readmsr(0xC0010071);
  v[13] = __readmsr(0xC0000082);

  copyout(v, (void *)args->kpayload_rdmsr_info->uaddr, sizeof(v));
  return 0;
}

static int read_pstate_msrs(uint64_t *out /* NMSR */) {
  uint64_t *buf = mmap(NULL, NMSR * 8, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (buf == (void *)-1 || !buf) return -1;
  struct kpayload_rdmsr_info info;
  info.fw_version = get_firmware();
  info.uaddr = (uint64_t)buf;
  int ret = kexec(&kpayload_rdmsr, &info);
  if (ret >= 0) memcpy(out, buf, NMSR * 8);
  munmap(buf, NMSR * 8);
  return ret;
}

static uint32_t cof_mhz(uint64_t v) {
  uint32_t fid = (uint32_t)(v & 0x3F);
  uint32_t did = (uint32_t)((v >> 6) & 0x7);
  return (100u * (fid + 16u)) >> did;
}
static int vid_mv(uint64_t v) {
  uint32_t vid = (uint32_t)((v >> 14) & 0xFF);
  return 1550 - (int)((vid * 625u) / 100u); /* 6.25mV * VID */
}

int _main(struct thread *td) {
  UNUSED(td);
  initKernel();
  initLibc();
  initPthread();
  initSysUtil();
  jailbreak();

  char fw[8] = {0};
  get_firmware_string(fw);
  uint64_t kb = get_kernel_base();
  if ((kb >> 32) != 0xFFFFFFFFull) {
    printf_notification("pstate-probe: kernel base invalid (0x%lx) on %s - aborting.", kb, fw);
    return 0;
  }

  uint64_t v[NMSR] = {0};
  int ret = read_pstate_msrs(v);
  printf_debug("================ pstate-probe (FW %s) ================\n", fw);
  printf_debug("kernel_base=0x%lx  kexec ret=%d\n", kb, ret);
  printf_debug("LSTAR(0xC0000082)=0x%016lx  (ring-0 rdmsr sanity: hi should be ffffffff)\n", v[13]);

  if (ret < 0) {
    printf_notification("pstate-probe: kexec failed (%d) on %s.", ret, fw);
    return 0;
  }

  uint32_t limit   = (uint32_t)v[0];
  uint32_t control = (uint32_t)v[1];
  uint32_t status  = (uint32_t)v[2];
  uint32_t cur     = status & 0x7;         /* CurPstate */
  uint32_t maxval  = (limit >> 4) & 0x7;   /* PstateMaxVal (numerically highest = slowest) */
  uint32_t curlim  = limit & 0x7;          /* CurPstateLimit */
  uint32_t cmd     = control & 0x7;        /* PstateCmd (last requested) */

  printf_debug("[reg] limit=0x%08x (MaxVal=%u CurLimit=%u)  control=0x%08x (Cmd=%u)  status=0x%08x (CurPstate=%u)\n",
               limit, maxval, curlim, control, cmd, status, cur);
  printf_debug("[reg] COFVID_ctl(0xC0010070)=0x%016lx  COFVID_sts(0xC0010071)=0x%016lx\n", v[11], v[12]);

  int enabled = 0, maxmhz = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t d = v[3 + i];
    int en = (int)((d >> 63) & 1);
    uint32_t fid = (uint32_t)(d & 0x3F);
    uint32_t did = (uint32_t)((d >> 6) & 0x7);
    uint32_t vid = (uint32_t)((d >> 14) & 0xFF);
    uint32_t mhz = cof_mhz(d);
    int mv = vid_mv(d);
    printf_debug("  P%d %s FID=0x%02x DID=%u VID=0x%02x -> %4u MHz, ~%d mV  [raw 0x%016lx]%s\n",
                 i, en ? "EN " : "dis", fid, did, vid, mhz, mv, d,
                 ((uint32_t)i == cur) ? "  <== CURRENT" : "");
    if (en) { enabled++; if ((int)mhz > maxmhz) maxmhz = (int)mhz; }
  }

  uint64_t dcur = v[3 + (cur & 0x7)];
  printf_debug("[now] CurPstate P%u -> %u MHz, ~%d mV\n", cur, cof_mhz(dcur), vid_mv(dcur));
  printf_debug("[sum] %d P-states enabled; highest defined = %d MHz\n", enabled, maxmhz);
  printf_debug("=====================================================\n");

  printf_notification("pstate-probe (FW %s)\nCurrent: P%u = %u MHz (~%d mV)\n%d P-states enabled, top defined = %d MHz\nLSTAR hi=0x%08x (ring0 OK)",
                      fw, cur, cof_mhz(dcur), vid_mv(dcur), enabled, maxmhz, (uint32_t)(v[13] >> 32));
  return 0;
}
