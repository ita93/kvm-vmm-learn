#include <asm/bootparam.h>
#include <asm/kvm.h>
#include <asm/kvm_para.h>
#include <bits/types.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>

#include <asm/e820.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "err.h"
#include "vm.h"

// Registers initialization
static int vm_init_regs(vm_t *g) {
  struct kvm_sregs sregs;

  if (ioctl(g->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
    return throw_err("Failed to get registers");

// all segment selector are the same
#define X(R) sregs.R.base = 0, sregs.R.limit = ~0, sregs.R.g = 1
  X(cs), X(ds), X(fs), X(gs), X(es), X(ss);
#undef X

  sregs.cs.db = 1;
  sregs.ss.db = 1;
  sregs.cr0 |= 1; /* enable protected mode */

  if (ioctl(g->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
    return throw_err("Failed to set special registers");

  struct kvm_regs regs;
  if (ioctl(g->vcpu_fd, KVM_GET_REGS, &regs) < 0)
    return throw_err("Failed to get registers");

  regs.rflags = 2;
  // similar to qemu?
  regs.rip = 0x100000, regs.rsi = 0x10000;
  if (ioctl(g->vcpu_fd, KVM_SET_REGS, &regs) < 0)
    return throw_err("Failed to set registers");

  return 0;
}

#define N_ENTRIES 100
static void vm_init_cpu_id(vm_t *g) {
  struct {
    uint32_t nent;
    uint32_t padding;
    struct kvm_cpuid_entry2 entries[N_ENTRIES];
  } kvm_cpuid = {.nent = N_ENTRIES};
  ioctl(g->kvm_fd, KVM_GET_SUPPORTED_CPUID, &kvm_cpuid);

  for (unsigned int i = 0; i < N_ENTRIES; i++) {
    struct kvm_cpuid_entry2 *entry = &kvm_cpuid.entries[i];
    if (entry->function == KVM_CPUID_SIGNATURE) {
      entry->eax = KVM_CPUID_FEATURES;
      entry->ebx = 0x4b4d564b; // KVMK
      entry->ecx = 0x564b4d56; // VMKV
      entry->edx = 0x4d;       // M
    }
  }

  ioctl(g->vcpu_fd, KVM_SET_CPUID2, &kvm_cpuid);
}

int vm_init(vm_t *g) {
  printf("Initializing VM\n");

  if ((g->kvm_fd = open("/dev/kvm", O_RDWR)) < 0)
    return throw_err("Failed to open /dev/kvm");

  if ((g->vm_fd = ioctl(g->kvm_fd, KVM_CREATE_VM, 0)) < 0)
    return throw_err("Failed to create VM");

  if (ioctl(g->vm_fd, KVM_SET_TSS_ADDR, 0xffffd000) < 0)
    return throw_err("Failed to set TSS addres");

  __u64 map_addr = 0xffffc000;
  // This mapping allow intel to switch between different CPU Mode
  if (ioctl(g->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &map_addr) < 0)
    return throw_err("Failed to set identity map address");

  if (ioctl(g->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
    return throw_err("Failed to create interrupt controller model");
  }

  struct kvm_pit_config pit = {.flags = 0};
  if (ioctl(g->vm_fd, KVM_CREATE_PIT2, &pit) < 0)
    return throw_err("Failed to create i8254 interval timer");

  // Create memory for the VM
  g->mem = mmap(NULL, RAM_SIZE, PROT_WRITE | PROT_READ,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!g->mem)
    return throw_err("Failed to mmap vm memory");

  struct kvm_userspace_memory_region region = {
      .slot = 0,
      .flags = 0,
      .guest_phys_addr = 0,
      .memory_size = RAM_SIZE,
      .userspace_addr = (__u64)g->mem,
  };

  // Because the memory is smaller than 4G, it won't overlap with the MMIO
  if (ioctl(g->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    return throw_err("Failed to setup user memory region");

  if ((g->vcpu_fd = ioctl(g->vm_fd, KVM_CREATE_VCPU, 0)) < 0)
    return throw_err("Failed to create vcpu");

  vm_init_regs(g);
  vm_init_cpu_id(g);

  return 0;
}

int vm_load_image(vm_t *g, const char *image_path) {
  int fd = open(image_path, O_RDONLY);
  if (fd < 0)
    return 1;

  struct stat st;
  fstat(fd, &st);
  size_t datasz = st.st_size;
  void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);

  struct boot_params *boot =
      (struct boot_params *)((uint8_t *)g->mem + 0x10000);
  void *cmdline = ((uint8_t *)g->mem) + 0x20000;
  void *kernel = ((uint8_t *)g->mem) + 0x100000;

  memset(boot, 0, sizeof(struct boot_params));
  memmove(boot, data, sizeof(struct boot_params));

  size_t setup_sectors = boot->hdr.setup_sects;
  size_t setupsz = (setup_sectors + 1) * 512; // ech sector is 512 bytes

  boot->hdr.vid_mode = 0xFFFF; // VGA
  boot->hdr.type_of_loader = 0xFF;
  boot->hdr.loadflags |= CAN_USE_HEAP | 0x01 | KEEP_SEGMENTS;
  boot->hdr.heap_end_ptr = 0xFE00;
  boot->hdr.ext_loader_ver = 0x0;
  boot->hdr.cmd_line_ptr = 0x20000;
  memset(cmdline, 0, boot->hdr.cmdline_size);
  memcpy(cmdline, "console=ttyS0", 14);
  memmove(kernel, (char *)data + setupsz, datasz - setupsz);

  // Setup E820 memory table to send the memory address information to initrd

  unsigned int idx = 0;
  boot->e820_table[idx] = (struct boot_e820_entry){
      .addr = 0x0,
      .size = ISA_START_ADDRESS - 1,
      .type = E820_RAM,
  };

  boot->e820_table[idx++] = (struct boot_e820_entry){
      .addr = ISA_END_ADDRESS,
      .size = RAM_SIZE - ISA_END_ADDRESS,
      .type = E820_RAM,
  };

  boot->e820_entries = idx; // number of idx, (at this point it should be 2)

  return 0;
}

int vm_load_initrd(vm_t *v, const char *initrd_path) {
  int fd = open(initrd_path, O_RDONLY);
  if (fd < 0)
    return 1;

  struct stat st;
  fstat(fd, &st);
  size_t datasz = st.st_size;
  void *data = mmap(0, datasz, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);

  struct boot_params *boot = (struct boot_params*)((uint8_t*)v->mem + 0x10000);
  unsigned long addr = boot->hdr.initrd_addr_max & ~0xfffff;

  // Understand this code:
  // We loaded kernel image to address 0x100000.
  // This for loop is to find the address that we can copy initrd to.
  // Start from the highest address, we continue to grown down until we find a slot that can fit the whole initrd image
  // but it can not overlap with the kernel image
  for (;;) {
    if (addr < 0x100000)
        return throw_err("Not enough memroy for initrd");
    else if (addr < (RAM_SIZE - datasz))
        break;
    addr -= 0x100000;
  }

  void *initrd = ((uint8_t *)v->mem) + addr;
  memset(initrd, 0, datasz);
  memmove(initrd, data, datasz);

  boot->hdr.ramdisk_image = addr;
  boot->hdr.ramdisk_size = datasz;

  return 0;
}

void vm_exit(vm_t *g) {
  close(g->kvm_fd);
  close(g->vm_fd);
  close(g->vcpu_fd);
  munmap(g->mem, RAM_SIZE);
}

int vm_run(vm_t *g) {
  int run_size = ioctl(g->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run =
      mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, g->vcpu_fd, 0);

  while (1) {
    if (ioctl(g->vcpu_fd, KVM_RUN, 0) < 0)
      return throw_err("Failed to execute kvm_run");

    switch (run->exit_reason) {
    case KVM_EXIT_IO:
      if (run->io.port == 0x3f8 && run->io.direction == KVM_EXIT_IO_OUT) {
        uint32_t size = run->io.size;
        uint64_t offset = run->io.data_offset;
        printf("%.*s", size * run->io.count, (char *)run + offset);
      } else if ((run->io.port == 0x3f8 + 5) &&
                 (run->io.direction == KVM_EXIT_IO_IN)) {
        *((char *)run + run->io.data_offset) = 0x20;
      }
      break;
    case KVM_EXIT_SHUTDOWN:
      printf("shutdown \n");
      return 0;
    default:
      printf("reason: %d\n", run->exit_reason);
      return -1;
    }
  }
}
