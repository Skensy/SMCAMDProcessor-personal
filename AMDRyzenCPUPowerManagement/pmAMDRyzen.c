//
//  pmAMDRyzen.c
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach, modified by Droga (2026) on 3/27/20.
//

#include "pmAMDRyzen.h"
#include "PStatePolicy.h"

#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/mach_time.h>



x86_lcpu_t *pmRyzen_cpunum_to_lcpu[XNU_MAX_CPU];

void *pmRyzen_io_service_handle;

pmProcessor_t pmRyzen_cpus[XNU_MAX_CPU];
uint64_t pmRyzen_tsc_freq;
uint64_t pmRyzen_effective_timetsc;

uint32_t pmRyzen_num_phys;
uint32_t pmRyzen_num_logi;
pmRyzen_idle_strategy_t pmRyzen_idle_strategy = PMRYZEN_IDLE_STRATEGY_SIMPLE;

static uint8_t pmRyzen_enabled_pstates;
static bool renoirPolicy;
static uint64_t promotionDelay[2];
static void (*resyncDeadlines)(int);

// Local CPU, interrupts disabled. rtcDeadline is the absolute time last
// requested by XNU's setPop(), in the same units as GetDeadline.
static void set_promotion_deadline(uint32_t cn, uint64_t deadline) {
    pmProcessor_t *cpu = &pmRyzen_cpus[cn];
    uint64_t previous = cpu->promotionDeadline;
    if (previous == deadline) return;
    cpu->promotionDeadline = deadline;
    if (resyncDeadlines && cpu->lcpu &&
        pmRyzen_needs_timer_resync(cpu->lcpu->rtcDeadline, previous, deadline))
        resyncDeadlines(cn);
}

static void arm_promotion(uint32_t cn) {
    if (!renoirPolicy) return;
    pmProcessor_t *cpu = &pmRyzen_cpus[cn];
    uint64_t delay = pmRyzen_promotion_interval(promotionDelay, pmRyzen_get_limit(),
                                                cpu->PState, cpu->cpu_awake != 0);
    set_promotion_deadline(cn, delay ? mach_absolute_time() + delay : 0);
}

void pmRyzen_update_pstate_mask(uint8_t mask) {
    __atomic_store_n(&pmRyzen_enabled_pstates, mask, __ATOMIC_RELAXED);
}

uint32_t pmRyzen_hpcpus = 0;
uint32_t pmRyzen_pstatelimit;

void(*pmRyzen_pmUnRegister)(pmDispatch_t*) = 0;
void(*pmRyzen_cpu_NMI)(int) = 0;
void(*pmRyzen_NMI_enabled)(boolean_t) = 0;
void(*pmRyzen_cpu_IPI)(int) = 0;



pmDispatch_t pmRyzen_cpuFuncs = {
    .pmCPUStateInit = 0,
    .cstateInit = 0,
    .MachineIdle = &pmRyzen_machine_idle,
    .GetDeadline = 0,
    .SetDeadline = 0,
    .Deadline = 0,
    .exitIdle = 0,
    .markCPURunning = 0,
    .pmCPUControl = 0,
    .pmCPUHalt = 0,
    .getMaxSnoop = 0,
    .setMaxBusDelay = 0,
    .getMaxBusDelay = 0,
    .setMaxIntDelay = 0,
    .getMaxIntDelay = 0,
    .pmCPUSafeMode = 0,
    .pmTimerStateSave = 0,
    .pmTimerStateRestore = 0,
    .exitHalt = 0,
    .exitHaltToOff = 0,
    .markAllCPUsOff = 0,
    .pmSetRunCount = 0,
    .pmIsCPUUnAvailable = 0,
    .pmChooseCPU = 0, // Keep XNU's choice of idle core and processor set.
    .pmIPIHandler = 0,
    .pmThreadTellUrgency = 0,
    .pmActiveRTThreads = 0,
    .pmInterruptPrewakeApplicable = 0,
    .pmThreadGoingOffCore = 0,
};

#pragma mark - P-State Initialization

void pmRyzen_init_PState(void){
    // P1 = 80% of P0 by default. Override via boot-arg `amdpp1ratio=0.75` etc.
    static float p1_ratio = 0.0f;
    if (p1_ratio == 0.0f) {
        uint32_t bootRatio = 0;
        PE_parse_boot_argn("amdpp1ratio", &bootRatio, sizeof(bootRatio));
        p1_ratio = (bootRatio > 0 && bootRatio <= 100) ? (float)bootRatio / 100.0f : 0.80f;
    }

    uint64_t p0 = pmRyzen_rdmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0);
    if (!(p0 & (1ULL << 63))) return;
    
    uint64_t p0dfsid = (p0 >> 8) & 0x3f;
    if (p0dfsid == 0) return;
    
    float p0spd = ((float)(p0 & 0xff) / (float)p0dfsid) * 200.0f;
    
    uint64_t p1 = pmRyzen_rdmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0 + 1);
    if (!(p1 & (1ULL << 63)) || ((p1 >> 8) & 0x3f) == 0) return;
    uint32_t fid_raw = (uint32_t)((p0spd * p1_ratio) / 200.0f * (float)((p1 >> 8) & 0x3f));
    if (fid_raw > 0xFF) fid_raw = 0xFF;
    uint64_t p1fid = (uint64_t)fid_raw;
    
    pmRyzen_wrmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0 + 1, (p1 & ~0xFFULL) | p1fid | (1ULL << 63));
}

// Called locally with interrupts disabled; account only successful MSR transitions.
void pmRyzen_record_pstate(uint32_t cn, uint8_t state) {
    pmProcessor_t *cpu = &pmRyzen_cpus[cn];
    if (state == 0 || !pmRyzen_get_limit()) set_promotion_deadline(cn, 0);
    if (cpu->PState == state) return;
    if (state == 0) __atomic_fetch_add(&pmRyzen_hpcpus, 1, __ATOMIC_RELAXED);
    else if (cpu->PState == 0) __atomic_fetch_sub(&pmRyzen_hpcpus, 1, __ATOMIC_RELAXED);
    cpu->PState = state;
}

static void set_PState(uint32_t cn, uint8_t state, bool force) {
    if (!pmRyzen_get_limit() || state >= 8) return;
    if (!(__atomic_load_n(&pmRyzen_enabled_pstates, __ATOMIC_RELAXED) & (1U << state))) return;
    if (!force && pmRyzen_cpus[cn].PState == state) return;
    if (pmRyzen_wrmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_CTL, state))
        pmRyzen_record_pstate(cn, state);
}

static uint64_t renoir_get_deadline(x86_lcpu_t *lcpu) {
    unsigned cn = lcpu->cpu_num;
    if (cn >= XNU_MAX_CPU || !pmRyzen_get_limit()) return 0;
    return pmRyzen_cpus[cn].promotionDeadline;
}

static void renoir_handle_deadline(x86_lcpu_t *lcpu) {
    unsigned cn = lcpu->cpu_num;
    if (cn >= XNU_MAX_CPU) return;
    pmProcessor_t *cpu = &pmRyzen_cpus[cn];
    cpu->promotionDeadline = 0;
    if (cpu->cpu_awake && pmRyzen_get_limit()) {
        set_PState(cn, 0, false);
        cpu->ll_count = 0;
        arm_promotion(cn); // Retry a failed MSR write after another full interval.
    }
}

static void pmRyzen_doPState_reset(void *preserveAllowed) {
    uint32_t cn = cpu_number();
    if (cn >= XNU_MAX_CPU) return;
    pmProcessor_t *cpu = &pmRyzen_cpus[cn];
    cpu->ll_count = 0;
    cpu->eff_timeacc = cpu->eff_idleacc = 0;
    cpu->last_start_tsc = cpu->cpu_awake ? rdtsc64() : 0;
    uint8_t mask = __atomic_load_n(&pmRyzen_enabled_pstates, __ATOMIC_RELAXED);
    uint8_t state = preserveAllowed && renoirPolicy
        ? pmRyzen_reconciled_state(mask, cpu->PState, pmRyzen_get_limit()) : 0;
    set_PState(cn, state, !preserveAllowed);
    arm_promotion(cn);
}

void pmRyzen_PState_reset(void) {
    mp_rendezvous_no_intrs(&pmRyzen_doPState_reset, NULL);
}

void pmRyzen_reconcile_pstates(void) {
    bool preserve = true;
    mp_rendezvous_no_intrs(&pmRyzen_doPState_reset, &preserve);
}

static void refresh_local_deadline(void *unused) {
    uint32_t cn = cpu_number();
    if (cn < XNU_MAX_CPU) arm_promotion(cn);
}

void pmRyzen_refresh_deadlines(void) {
    if (renoirPolicy) mp_rendezvous_no_intrs(refresh_local_deadline, NULL);
}

#pragma mark - Init / Stop

void pmRyzen_init(void *handle, int allowDispatch, bool renoir){
    
    pmRyzen_io_service_handle = handle;
    IOLog("AMDRyzenCPUPowerManagement::pmRyzen_init sizeof(pmProcessor_t) = %lu bytes (cacheline aligned)\n", (unsigned long)sizeof(pmProcessor_t));
    
    
    if (!pmRyzen_symtable._pmDispatch || !pmRyzen_symtable._pmUnRegister ||
        !pmRyzen_symtable._tscFreq) {
        IOLog("pmRyzen_init: critical symbols not resolved, aborting\n");
        return;
    }
    
    void **kernelDisp = pmRyzen_symtable._pmDispatch;
    pmRyzen_pmUnRegister = (void(*)(pmDispatch_t*))pmRyzen_symtable._pmUnRegister;
    pmRyzen_cpu_NMI = pmRyzen_symtable._cpu_NMI_interrupt
        ? (void(*)(int))pmRyzen_symtable._cpu_NMI_interrupt : NULL;
    pmRyzen_NMI_enabled = pmRyzen_symtable._NMIPI_enable
        ? (void(*)(boolean_t))pmRyzen_symtable._NMIPI_enable : NULL;
    pmRyzen_cpu_IPI = pmRyzen_symtable._i386_cpu_IPI
        ? (void(*)(int))pmRyzen_symtable._i386_cpu_IPI : NULL;
    pmRyzen_tsc_freq = *((uint64_t*)pmRyzen_symtable._tscFreq);
    
    
    
    renoirPolicy = renoir && allowDispatch;
    if (renoirPolicy) {
        clock_interval_to_absolutetime_interval(10, NSEC_PER_MSEC, &promotionDelay[0]);
        clock_interval_to_absolutetime_interval(50, NSEC_PER_MSEC, &promotionDelay[1]);
        pmRyzen_cpuFuncs.GetDeadline = renoir_get_deadline;
        pmRyzen_cpuFuncs.Deadline = renoir_handle_deadline;
    }
    pmCallBacks_t cb;
    if (allowDispatch) {
        if(*kernelDisp)(*pmRyzen_pmUnRegister)(*kernelDisp);
        pmKextRegister(PM_DISPATCH_VERSION, &pmRyzen_cpuFuncs, &cb);
        if (renoirPolicy) resyncDeadlines = cb.resyncDeadlines;
    } else {
        pmKextRegister(PM_DISPATCH_VERSION, NULL, &cb);
    }
    
    x86_pkg_t * pkg = cb.GetPkgRoot();
    int pkgCount = 0;
    
    pmRyzen_num_phys = 0;
    pmRyzen_num_logi = 0;
    pmRyzen_hpcpus = 0;
    pmRyzen_set_limit(PSTATE_LIMIT);
    
    while(pkg){
        pkgCount++;
        x86_core_t *core = pkg->cores;
        
        while (core) {
            pmRyzen_num_phys++;
            x86_lcpu_t *lcpu = core->lcpus;
            
            while (lcpu) {
                pmRyzen_num_logi++;
                if (lcpu->cpu_num >= XNU_MAX_CPU) {
                    static boolean_t loggedMaxCpu = FALSE;
                    if (!loggedMaxCpu) {
                        loggedMaxCpu = TRUE;
                        IOLog("AMDRyzenCPUPowerManagement ERR: System has CPU %u beyond limit %u. Cores beyond limit will not be managed.\n", lcpu->cpu_num, XNU_MAX_CPU);
                    }
                    lcpu = lcpu->next_in_core;
                    continue;
                }
                
                pmRyzen_cpunum_to_lcpu[lcpu->cpu_num] = lcpu;
                
                pmProcessor_t *cpu = &pmRyzen_cpus[lcpu->cpu_num];
                cpu->lcpu = lcpu;
                cpu->PState = 8; // No successful request recorded yet.
                cpu->stat_exit_idle = 0;
                cpu->arm_flag = 0;
                cpu->cpu_awake = 1;
                
                lcpu = lcpu->next_in_core;
            }
            
            core = core->next_in_pkg;
        }
        
        pkg = pkg->next;
    }
    // Removed: package count debug log not needed in production

    
    pmRyzen_effective_timetsc = ((double)pmRyzen_tsc_freq * EFF_INTERVAL);
    if (allowDispatch) {
        pmRyzen_init_PState();
        pmRyzen_PState_reset();
    } else {
        pmRyzen_set_limit(0);
    }

    cb.initComplete();
}

void pmRyzen_stop(void){
    
    if (pmRyzen_pmUnRegister) {
        (*pmRyzen_pmUnRegister)(&pmRyzen_cpuFuncs);
    }
    
    // Make sure all managed cores exited idle thread.
    // With the SIMPLE (sti;hlt) strategy, exit is immediate — just set arm_flag.
    for (int i = 0; i < pmRyzen_num_logi && i < XNU_MAX_CPU; i++) {
        if (!pmRyzen_cpunum_to_lcpu[i]) continue;
        pmRyzen_cpus[i].arm_flag = 1;
    }
}



#pragma mark - Load & Idle

float pmRyzen_avgload_pcpu(uint32_t cpu){
    if (cpu >= XNU_MAX_CPU || !pmRyzen_cpunum_to_lcpu[cpu]) return 0.0f;
    float loadacc = 0;
    int num_lcpus = 0;
    
    x86_lcpu_t *lcpu = pmRyzen_cpunum_to_lcpu[cpu]->core->lcpus;
    while (lcpu) {
        if (lcpu->cpu_num < XNU_MAX_CPU) {
            float idle_ratio = (pmRyzen_cpus[lcpu->cpu_num].eff_timeaccd > 0)
                ? (float)pmRyzen_cpus[lcpu->cpu_num].eff_idleaccd / (float)pmRyzen_cpus[lcpu->cpu_num].eff_timeaccd
                : 0.0f;
            loadacc += 1.0f - idle_ratio;
            num_lcpus++;
        }
        lcpu = lcpu->next_in_core;
    }
    
    if (num_lcpus == 0) return 0.0f;
    return loadacc / (float)num_lcpus;
}


// Called on the target CPU by the existing interrupt-disabled telemetry rendezvous.
// An IPI waking a halted CPU arrives while cpu_awake is still false.
void pmRyzen_check_busy_cpu(uint32_t cn) {
    if (cn >= XNU_MAX_CPU || pmRyzen_get_limit() == 0) return;
    pmProcessor_t *cpu = &pmRyzen_cpus[cn];
    if (cpu->cpu_awake && cpu->PState && cpu->PState < 8 &&
        pmRyzen_busy_for_window(rdtsc64(), cpu->last_start_tsc, pmRyzen_effective_timetsc)) {
        set_PState(cn, 0, false);
        cpu->ll_count = 0;
    }
}

uint64_t pmRyzen_machine_idle(uint64_t maxDur){

    __asm__ volatile("cli;");
    
    uint32_t cn = cpu_number();
    if (cn >= XNU_MAX_CPU) {
        __asm__ volatile("sti;hlt;cli" ::: "memory");
        return 0;
    }
    pmProcessor_t *self = &pmRyzen_cpus[cn];
    
    
    set_promotion_deadline(cn, 0);
    self->cpu_awake = 0;
    self->arm_flag = 0;
    
    uint64_t tscnow = rdtsc64();

    self->last_idle_tsc = tscnow;
    // Removed: last_running_time calculation replaced by eff_timeacc tracking
    
    // Only idle strategy: sti; hlt — safe on all AMD CPUs.
    // Intel-style MONITOR/MWAIT was removed (unsafe on AMD).
    // AMD MONITORX/MWAITX may be added as a future enhancement.
    __asm__ volatile("sti;hlt;cli" ::: "memory");

    
    self->cpu_awake = 1;
    
    
    tscnow = rdtsc64();

    if (tscnow < self->last_idle_tsc) {
        self->eff_timeacc = self->eff_idleacc = 0;
        self->last_start_tsc = tscnow;
        arm_promotion(cn);
        return 0;
    }
    uint64_t tscela = tscnow - self->last_idle_tsc;
    uint64_t start = self->last_start_tsc;
    if (!start || start > self->last_idle_tsc) start = self->last_idle_tsc;
    self->eff_timeacc += tscnow - start;
    self->eff_idleacc += tscela;

    if(self->eff_timeacc > pmRyzen_effective_timetsc){
        int direction = pmRyzen_load_direction(self->eff_timeacc, self->eff_idleacc);
        unsigned limit = pmRyzen_get_limit();
        if (direction > 0) {
            set_PState(cn, 0, false);
            self->ll_count = 0;
        } else if (direction < 0 && limit) {
            unsigned windows = renoirPolicy ? pmRyzen_down_windows(limit) :
                1 + PSTATE_STEPDOWN_TIME + __atomic_load_n(&pmRyzen_hpcpus, __ATOMIC_RELAXED) * PSTATE_STEPDOWN_MP_GAIN;
            if (++self->ll_count >= windows) {
                self->ll_count = 0;
                uint8_t mask = __atomic_load_n(&pmRyzen_enabled_pstates, __ATOMIC_RELAXED);
                set_PState(cn, pmRyzen_next_lower(mask, self->PState, limit), false);
            }
        } else if (renoirPolicy) {
            self->ll_count = 0;
        }

        self->eff_idleaccd = self->eff_idleacc;
        self->eff_timeaccd = self->eff_timeacc;
        self->eff_timeacc = 0;
        self->eff_idleacc = 0;

    }

    self->last_start_tsc = tscnow;
    self->last_idle_length = tscela;
    arm_promotion(cn);
    return 0;
}

pmProcessor_t* pmRyzen_get_processor(uint32_t cpu){
    if (cpu >= XNU_MAX_CPU) return NULL;
    return &pmRyzen_cpus[cpu];
}
