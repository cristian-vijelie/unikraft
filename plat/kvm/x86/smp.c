/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Cristian Vijelie <cristianvijelie@gmail.com>
 *
 * Copyright (c) 2021, University Politehnica of Bucharest. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <uk/plat/lcpu.h>
#include <uk/arch/types.h>
#include <x86/acpi/madt.h>
#include <uk/print.h>
#include <stddef.h>
#include <string.h>
#include <x86/apic_defs.h>
#include <x86/cpu.h>
#include <kvm-x86/smp.h>
#include <kvm-x86/delay.h>
#include <uk/plat/time.h>

__u8 bspdone;
__u64 smp_aprunning, cr3_val;
volatile int aps_halted;

static __lcpuid bspid;
static __u32 smp_numcores;
static __u32 apic_ids[256] = {0};
static struct MADT *madt;

struct ukplat_cpu cpus[CONFIG_MAX_CPUS];

const __uptr cpusptr = (__uptr)(&cpus);
const size_t cpustruct_size = sizeof(struct ukplat_cpu);

__lcpuid ukplat_lcpu_count(void)
{
	__u8 *ptr = madt->Entries, numcores = 0;
	unsigned int i;
	struct MADTEntryHeader *mh;
	struct MADTType0Entry *madt_entry;

	if (smp_numcores > 0)
		return smp_numcores;

	UK_ASSERT(madt);

	for (i = 0; i < madt->h.Length; i += mh->Length) {
		mh = (struct MADTEntryHeader *)(ptr + i);
		switch (mh->Type) {
		case 0:
			madt_entry = (struct MADTType0Entry *)mh;
			if (madt_entry->Flags & 1 || madt_entry->Flags & 2)
				numcores++;
			break;
			/*
			 * TODO: search for Type 9 entries, as well, if a CPU
			 * with more than 256 cores is used
			 */
		default:
			break;
		}
	}

	smp_numcores = numcores;

	return numcores;
}

int ukplat_lcpu_start(__lcpuid lcpuid[], void *sp[],
		      ukplat_lcpu_entry_t entry[], int num)
{
	int i, j;
	__u32 eax, edx;
	__lcpuid id;

	smp_aprunning = 0;
	if (smp_numcores == 0)
		ukplat_lcpu_count();

	memcpy((void *)0x8000, &_lcpu_start16, 4096);
	uk_pr_debug("Copied AP boot code to 0x8000\n");

	/* Store the CR3 register */
	__asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3_val));

	for (i = 0; i < num; i++) {
		if (lcpuid != NULL)
			id = lcpuid[i];
		else
			id = i;

		if (id == bspid)
			continue;

		if (apic_ids[id] == 0)
			continue;

		/* Store the entry address and stack pointer */
		cpus[id].entry = entry[i];
		cpus[id].stackp = sp[i];

		/* clear APIC errors */
		wrmsr(x2APIC_ESR, 0, 0);

		/* select AP and trigger INIT IPI */
		eax = APIC_ICR_TRIGGER_LEVEL | APIC_ICR_LEVEL_ASSERT
		      | APIC_ICR_DESTMODE_PHYSICAL | APIC_ICR_DMODE_INIT;
		edx = apic_ids[id];
		wrmsr(x2APIC_ICR, eax, edx);

		/* deassert */
		eax = APIC_ICR_TRIGGER_LEVEL | APIC_ICR_DESTMODE_PHYSICAL
		      | APIC_ICR_DMODE_INIT;
		edx = apic_ids[id];
		wrmsr(x2APIC_ICR, eax, edx);
	}

	/* wait 10 msec */
	mdelay(10);

	for (i = 0; i < num; i++) {
		if (lcpuid != NULL)
			id = lcpuid[i];
		else
			id = i;

		if (id == bspid)
			continue;

		if (apic_ids[id] == 0)
			continue;

		for (j = 0; j < 2; j++) {
			/* clear APIC errors */
			wrmsr(x2APIC_ESR, 0, 0);

			/* select AP and trigger STARTUP IPI for 0x8000 */
			eax = APIC_ICR_TRIGGER_LEVEL | APIC_ICR_LEVEL_ASSERT
			      | APIC_ICR_DESTMODE_PHYSICAL | APIC_ICR_DMODE_SUP
			      | 0x08;
			edx = apic_ids[id];
			wrmsr(x2APIC_ICR, eax, edx);

			/* wait 200 usec */
			udelay(200);
		}
	}
	/* wait 10 msec */
	mdelay(10);

	/* Signal the APs that the BSP has finished */
	/* NOTE: without scheduling implemented, everything works fine without
	 * this part (from here, until return), but everyone recommends that the
	 * APs wait for the BSP to finish.
	 */
	bspdone = 1;

	/* Wait for all the APs to write their ID in smp_aprunning */
	/* NOTE: It's enough for a 12-core CPU, but it may not be enough
	 * for a higher amount of cores - this system must be changed, if it's
	 * expected to return the corect mask everytime.
	 */
	mdelay(10);

	return smp_aprunning;
}

__lcpuid ukplat_lcpu_id(void)
{
	__lcpuid id;

	__asm__ __volatile__("mov $1, %%eax; cpuid; shrl $24, %%ebx;"
			     : "=b"(id));

	return id;
}

int ukplat_lcpu_is_bsp(void)
{
	return (bspid == ukplat_lcpu_id());
}

int __attribute__((optimize("O0")))
ukplat_lcpu_wait(__lcpuid lcpuid[], int num, __nsec timeout)
{
	int i;
	__lcpuid id;
	__nsec end;

	/* Ignore the num parameter */
	if (lcpuid == NULL)
		num = smp_numcores;

	if (timeout > 0)
		end = ukplat_monotonic_clock() + timeout;

	for (i = 0; i < num; i++) {
		if (lcpuid != NULL)
			id = lcpuid[i];
		else
			id = i;

		/* Don't wait for the current CPU */
		if (id == ukplat_lcpu_id())
			continue;

		while (cpus[id].state != idle) {
			if (timeout && (ukplat_monotonic_clock() > end))
				return 1;
		}
	}

	return 0;
}

static void get_lapicid(void)
{
	__u8 *ptr;
	unsigned int i, coreid = 0;
	struct MADTEntryHeader *mh;
	struct MADTType0Entry *madt_entry0;
	struct MADTType9Entry *madt_entry9;

	UK_ASSERT(madt);

	ptr = madt->Entries;

	for (i = 0; i < madt->h.Length; i += mh->Length) {
		mh = (struct MADTEntryHeader *)(ptr + i);
		switch (mh->Type) {
		case 0:
			madt_entry0 = (struct MADTType0Entry *)mh;
			if (madt_entry0->Flags & 1 || madt_entry0->Flags & 2) {
				apic_ids[madt_entry0->ACPIProcessorID] =
				    madt_entry0->APICID;
			} else
				uk_pr_info("Core %d is not available\n",
					   madt_entry0->ACPIProcessorID);
			break;
		/* Unless more than 256 cores are used, no Type 9 entry will be
		 * found. But they are searched, just in case.
		 */
		case 9:
			uk_pr_debug("Found type 9 MADT entry\n");
			madt_entry9 = (struct MADTType9Entry *)mh;
			if (madt_entry9->Flags & 1 || madt_entry9->Flags & 2)
				apic_ids[madt_entry9->ACPIProcessorUID] =
				    madt_entry9->X2APICID;
			else {
				uk_pr_info("Core %d is not available\n",
					   coreid);
			}
			break;
		default:
			break;
		}
	}
}

static int enable_x2apic(void)
{
	__u32 eax, ebx, ecx, edx;

	cpuid(1, 0, &eax, &ebx, &ecx, &edx);
	if (ecx & (1 << x2APIC_CPUID_BIT))
		uk_pr_info("x2APIC is supported; enabling\n");
	else {
		uk_pr_info("x2APIC is not supported\n");
		return 1;
	}

	rdmsr(IA32_APIC_BASE, &eax, &edx);
	uk_pr_debug(
	    "IA32_APIC_BASE has the value %x; EN bit: %d, EXTD bit: %d\n", eax,
	    (eax & (1 << APIC_BASE_EN)) != 0,
	    (eax & (1 << APIC_BASE_EXTD)) != 0);

	/* set the x2APIC enable bit */
	eax |= (1 << APIC_BASE_EXTD);
	wrmsr(IA32_APIC_BASE, eax, edx);
	uk_pr_debug("x2APIC is enabled\n");

	return 0;
}

int smp_init(void)
{
	__u32 eax, edx;

	bspid = ukplat_lcpu_id();
	uk_pr_info("Bootstrapping processor has the ID %d\n", bspid);

	cpus[bspid].state = running;

	if (enable_x2apic() != 0) {
		uk_pr_err("x2APIC could not be enabled!\n");
		return 1;

		/* TODO: Use xAPIC mode */
	}

	rdmsr(x2APIC_SPUR, &eax, &edx);
	uk_pr_debug(
	    "Spurious Interrupt Register has the values %x; EN bit: %d\n", eax,
	    (eax & (1 << APIC_SPUR_EN)) != 0);

	if ((eax & (1 << APIC_SPUR_EN)) == 0) {
		eax |= (1 << APIC_SPUR_EN);
		wrmsr(x2APIC_SPUR, eax, edx);
		uk_pr_debug("Spurious interrupt enabled\n");
	}

	madt = acpi_get_madt();
	UK_ASSERT(madt);

	get_lapicid();

	return 0;
}
