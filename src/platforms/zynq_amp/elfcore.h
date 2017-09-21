#ifndef __ELFCORE_H
#define __ELFCORE_H

#include <stdint.h>

#define E_IDENT_ELF32LSB "\x7f\x45LF\1\1\1\0\0\0\0\0\0\0\0\0"

#define ET_NONE 0
#define ET_EXEC 2
#define ET_CORE 4

#define PT_NULL 0
#define PT_LOAD 1
#define PT_NOTE 4

#define NT_PRSTATUS 1
#define NT_AUXV 6
#define NT_ARM_VFP  0x400

#define AT_HWCAP 16

#define HWCAP_VFP 64
#define HWCAP_NEON 4096

struct Elf32_Ehdr {
    char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct Elf32_Note {
    uint32_t namelen;
    uint32_t datalen;
    uint32_t type;
};

struct ARM_prstatus {
    uint32_t si_signo;
    uint32_t si_code;
    uint32_t si_errno;

    uint16_t pr_cursig; /* current signal */
    uint16_t pr_pad0;

    uint32_t pr_sigpend;
    uint32_t pr_sighold;
    uint32_t pr_pid; /* LWP ID */
    uint32_t pr_ppid;
    uint32_t pr_pgrp;
    uint32_t pr_sid;

    uint64_t pr_utime;
    uint64_t pr_stime;
    uint64_t pr_cutime;
    uint64_t pr_cstime;

    uint32_t regs[19]; /* General purpose registers, CPSR */
} __attribute__((packed));

struct ARM_vfp { /* Note name 'LINUX' */
    uint64_t d[32];
    uint32_t sr;
} __attribute__((packed));

#endif
