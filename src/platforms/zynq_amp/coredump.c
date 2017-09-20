#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "elfcore.h"

#define MAX_SEGMENTS 10

struct corefile {
    struct Elf32_Ehdr ehdr;
    struct Elf32_Phdr phdr[MAX_SEGMENTS];
    const void *segments[MAX_SEGMENTS];
    void *note;
    size_t note_size;
};

struct corefile *core_new(uint32_t machine)
{
    struct corefile *cf = calloc(1, sizeof(*cf));
    memcpy(cf->ehdr.e_ident, E_IDENT_ELF32LSB, sizeof(cf->ehdr.e_ident));
    cf->ehdr.e_type = ET_CORE;
    cf->ehdr.e_version = 1;
    cf->ehdr.e_ehsize = sizeof(struct Elf32_Ehdr);
    cf->ehdr.e_phoff = sizeof(struct Elf32_Ehdr);
    cf->ehdr.e_phentsize = sizeof(struct Elf32_Phdr);
    cf->ehdr.e_machine = machine;
    return cf;
}

void core_add_ph(struct corefile *cf, uint32_t p_type, uint32_t p_vaddr,
                 const void *data, uint32_t p_filesz)
{
    struct Elf32_Phdr *phdr = &cf->phdr[cf->ehdr.e_phnum];
    phdr->p_type = p_type;
    phdr->p_vaddr = p_vaddr;
    phdr->p_memsz = p_filesz;
    phdr->p_filesz = p_filesz;
    cf->segments[cf->ehdr.e_phnum] = data;
    cf->ehdr.e_phnum++;
}

static inline size_t pad(size_t x) { return ((x + 3) / 4)*4; }

void core_note_add(struct corefile *cf,
                   char *name, uint32_t type, const void *data, size_t len)
{
    struct Elf32_Note note = {
        .namelen = strlen(name) + 1,
        .datalen = len,
        .type = type,
    };
    size_t this_size = sizeof(note) + pad(strlen(name) + 1) + pad(len);
    cf->note = realloc(cf->note, cf->note_size + this_size);
    void *p = cf->note + cf->note_size;
    memset(p, 0, this_size);
    memcpy(p, &note, sizeof(note)); p += sizeof(note);
    memcpy(p, name, strlen(name) + 1); p += pad(strlen(name) + 1);
    memcpy(p, data, len);
    cf->note_size += this_size;
}

void core_note_add_prstatus(struct corefile *cf,
                            uint16_t signal, const uint32_t regs[18])
{
    struct ARM_prstatus prstatus = {
        .pr_cursig = signal,
    };
    memcpy(&prstatus.regs, regs, sizeof(prstatus.regs));
    core_note_add(cf, "CORE", NT_PRSTATUS, &prstatus, sizeof(prstatus));
}

void core_note_add_arm_vfp(struct corefile *cf, const struct ARM_vfp *regs)
{
    core_note_add(cf, "LINUX", NT_ARM_VFP, regs, sizeof(*regs));
}

void core_dump(FILE *f, struct corefile *cf)
{
    core_add_ph(cf, PT_NOTE, 0, cf->note, cf->note_size);

    /* Write the ELF header */
    fwrite(&cf->ehdr, sizeof(cf->ehdr), 1, f);
    uint32_t offset = cf->ehdr.e_ehsize + cf->ehdr.e_phentsize * cf->ehdr.e_phnum;
    for (int i = 0; i < cf->ehdr.e_phnum; i++) {
        /* Update program header and write to file */
        cf->phdr[i].p_offset = offset;
        offset += cf->phdr[i].p_filesz;
        fwrite(&cf->phdr[i], sizeof(cf->phdr[i]), 1, f);
    }
    for (int i = 0; i < cf->ehdr.e_phnum; i++) {
        /* Write actual segment data to file */
        fwrite(cf->segments[i], cf->phdr[i].p_filesz, 1, f);
    }
}

void core_free(struct corefile *cf)
{
    if (cf->note != NULL)
        free(cf->note);
    free(cf);
}

#if 0
int main(void)
{
    uint32_t regs[18] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0x1df};
    struct ARM_vfp vfp;
    for (int i = 0; i < 32; i++)
        vfp.d[i] = i;
    vfp.sr = 0xbeef;
    struct corefile *cf = core_new(0x28); /* ARM */
    core_note_add_prstatus(cf, SIGSEGV, regs);
    core_note_add_arm_vfp(cf, &vfp);
    core_add_ph(cf, PT_LOAD, 0x100000, "some shit", 12);
    core_add_ph(cf, PT_LOAD, 0x200000, "some other shit", 16);
    FILE *f = fopen("core", "w");
    core_dump(f, cf);
    fclose(f);
}
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "general.h"
#include "target.h"
extern void cortexa_cache_clean(target *t, target_addr src, size_t len);

void zynq_amp_core_dump(target *t)
{
/*  From Piksi LD script:
    flash : org = 0x1c000000, len = 0x02000000
    vring : org = 0x1e000000, len = 0x00800000
    ram0  : org = 0x1e800000, len = 0x01800000
*/
    const struct section {
        uint32_t base;
        uint32_t size;
    } sections[] = {
        {0x1c000000, 0x02000000},
        {0x1e000000, 0x00800000},
        {0x1e800000, 0x01800000},
        {0, 0}
    };
    uint8_t treg[target_regs_size(t)];
    uint32_t regs[18] = {0};
    target_regs_read(t, treg);
    memcpy(regs, treg, 17 * sizeof(uint32_t));

    struct corefile *cf = core_new(0x28); /* ARM */
    core_note_add_prstatus(cf, SIGSEGV, regs);
    int pmem = open("/dev/mem", O_RDWR | O_SYNC);
    for (const struct section *s = sections; s->size; s++) {
        cortexa_cache_clean(t, s->base, s->size);
        volatile void *ptr = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                  pmem, s->base);
        core_add_ph(cf, PT_LOAD, s->base, (void*)ptr, s->size);
    }

    time_t tim;
    char fn[80];
    time(&tim);
    strftime(fn, sizeof(fn), "/tmp/cores/zynq_amp_core-%Y%m%d-%H%M%S", gmtime(&tim));
    FILE *f = fopen(fn, "w");
    core_dump(f, cf);
    fclose(f);
    close(pmem);

    piksi_log("Firmware core dumped: %s", fn);
}
