#ifndef PE1_PSYQ_BIOS_H
#define PE1_PSYQ_BIOS_H

/* Character-output services reached through the LIBC BIOS veneers. */
int printf(const char *format, ...);
int puts(const char *text);

/* File services used by the memory-card manager. */
int open(char *path, int mode);
int close(int descriptor);
int erase(char *path);

/* Program control and cache services. */
void exit(int code);
void FlushCache(void);

/* Entry-hook and automatic IRQ acknowledgement services used by LIBETC. */
int EnterCriticalSection(void);
void ExitCriticalSection(void);
int ResetEntryInt(void);
void ReturnFromException(void);
void HookEntryInt(void *entry);
void ChangeClearPAD(int mode);
int ChangeClearRCnt(int counter, int enabled);

#define PE1_STRINGIFY_INNER(value) #value
#define PE1_STRINGIFY(value) PE1_STRINGIFY_INNER(value)

/* PSY-Q's assembled BIOS veneers preserve all arguments and tail-call the
 * selected A0/B0/C0 service through the documented t2/t1 register ABI. */
#define PSYQ_BIOS_TRAMPOLINE(name, table, selector) \
    __asm__( \
        ".section .text, \"ax\"\n" \
        ".set noreorder\n" \
        ".globl " #name "\n" \
        ".type " #name ", @function\n" \
        #name ":\n" \
        ".ent " #name "\n" \
        "addiu $10, $0, " PE1_STRINGIFY(table) "\n" \
        "jr $10\n" \
        "addiu $9, $0, " PE1_STRINGIFY(selector) "\n" \
        ".size " #name ", . - " #name "\n" \
        ".end " #name "\n" \
        ".set reorder\n")

/* Critical-section BIOS ABI: selector in a0, result in v0. The caller
 * binds these operands to $4/$2; C owns selector setup and the return.
 * Tied operands use GCC272's syntax instead of its unsupported +r form. */
#define PSYQ_BIOS_SYSCALL(result, selector) \
    __asm__ volatile("syscall" : "=r"(result), "=r"(selector) \
                     : "1"(selector) : "memory")

#if !defined(__psp__) && !defined(ENABLE_PSP)
void bzero(void *destination, int length);
void *memcpy(void *destination, const void *source, unsigned int length);
char *strcpy(char *destination, char *source);
#else
#include <string.h>
#include <strings.h>
#endif

int DeliverEvent(unsigned int event, int spec);

int OpenEvent(unsigned int desc, int spec, int mode, int (*func)(void));
int CloseEvent(int event);
int EnableEvent(int event);
int TestEvent(int event);
int DisableEvent(int event);
int UnDeliverEvent(unsigned int event, int spec);

/* LIBAPI root counters (counter.c). */
int SetRCnt(unsigned int counter, int target, int mode);
int GetRCnt(unsigned int counter);
int StartRCnt(unsigned int counter);
int StopRCnt(unsigned int counter);

/* LIBAPI.H descriptor, event and counter mode constants. */
#define RCntCNT2 0xF2000002
#define RCntMdINTR 0x1000
#define EvSpINT 0x0002
#define EvMdINTR 0x1000

#endif
