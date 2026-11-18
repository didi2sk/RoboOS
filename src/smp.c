/*
 * More cores. Core 0 runs the shell, programs, the SD card, the screen and the
 * sound. Core 1 is the I/O core: it polls the USB controller all the time
 * (keyboard, hot-plug, Ethernet) and runs the network (RoboOS messages, lwIP
 * with its timers), so the network answers and keys arrive even while a
 * program computes. Cores 2 and 3 are workers: core 0 hands them a function
 * to run (smp_run / smp_wait), e.g. turning video frames into pixels.
 *
 * Shared things are guarded by locks:
 *   io lock      - the USB controller, Ethernet, lwIP, sockets, RoboOS messages
 *   console lock - the screen and the UART output (whole kprintf calls)
 *   mbox lock    - the mailbox buffer to the GPU firmware
 * The locks are recursive (the same core may take one again). Lock order:
 * io before console.
 *
 * If core 1 does not start, or crashes, everything falls back to the old way:
 * core 0 polls USB and the network from con_trygetc().
 */
#include "kernel.h"
#include "usb.h"

struct rlock { volatile u32 lock; volatile int owner, depth; };
static struct rlock io_l = { 0, -1, 0 }, con_l = { 0, -1, 0 }, mbox_l = { 0, -1, 0 };
static volatile int io_running;

/* worker cores 2 and 3 */
struct job { void (*fn)(void *); void *arg; volatile int busy, alive; };
static struct job jobs[4];
static volatile u64 io_loops;

int smp_core(void)
{
    u64 m;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    return (int)(m & 3);
}

static void rl_take(struct rlock *l)
{
    if (!mmu_enabled()) return;                     /* exclusives need the MMU + caches (boot) */
    int me = smp_core();
    if (l->owner == me) { l->depth++; return; }
    while (__atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->lock, __ATOMIC_RELAXED)) __asm__ volatile("yield");
    l->owner = me;
    l->depth = 1;
}

static void rl_give(struct rlock *l)
{
    if (!mmu_enabled()) return;
    if (l->owner != smp_core()) return;
    if (--l->depth) return;
    l->owner = -1;
    __atomic_store_n(&l->lock, 0, __ATOMIC_RELEASE);
}

/* give the lock up completely (to wait for the other core); returns the depth */
static int rl_release(struct rlock *l)
{
    if (!mmu_enabled() || l->owner != smp_core()) return 0;
    int d = l->depth;
    l->depth = 1;
    rl_give(l);
    return d;
}

static void rl_reacquire(struct rlock *l, int d)
{
    if (!d) return;
    rl_take(l);
    l->depth = d;
}

void io_lock(void)     { rl_take(&io_l); }
void io_unlock(void)   { rl_give(&io_l); }
int  io_release(void)  { return rl_release(&io_l); }
void io_reacquire(int d) { rl_reacquire(&io_l, d); }
int  io_held(void)     { return io_l.owner == smp_core(); }
void con_lock(void)    { rl_take(&con_l); }
void con_unlock(void)  { rl_give(&con_l); }
void mbox_lock(void)   { rl_take(&mbox_l); }
void mbox_unlock(void) { rl_give(&mbox_l); }

/* is core 1 doing the USB and network polling? */
int io_core_active(void) { return io_running; }

/* the I/O core's work, also used by core 0 when there is no I/O core */
void io_poll_all(void)
{
    kbd_poll();
    usb_hotplug();
    net_poll();
}

static void worker(int core)
{
    struct job *j = &jobs[core];
    __atomic_store_n(&j->alive, 1, __ATOMIC_RELEASE);
    for (;;) {
        while (!__atomic_load_n(&j->busy, __ATOMIC_ACQUIRE)) __asm__ volatile("wfe");
        j->fn(j->arg);
        __atomic_store_n(&j->busy, 0, __ATOMIC_RELEASE);
        __asm__ volatile("dsb sy\n sev");
    }
}

/* run fn(arg) on worker core 2 or 3; -1 = that core is not there or busy */
int smp_run(int core, void (*fn)(void *), void *arg)
{
    if (core < 2 || core > 3 || !jobs[core].alive || jobs[core].busy) return -1;
    jobs[core].fn = fn;
    jobs[core].arg = arg;
    __atomic_store_n(&jobs[core].busy, 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy\n sev");
    return 0;
}

/* wait until the core has finished; -1 = it did not within ms (it is given up) */
int smp_wait(int core, u32 ms)
{
    if (core < 2 || core > 3) return -1;
    u64 end = timer_us() + (u64)ms * 1000;
    while (__atomic_load_n(&jobs[core].busy, __ATOMIC_ACQUIRE))
        if (timer_us() > end) { jobs[core].alive = 0; return -1; }
    return 0;
}

int smp_workers(void) { return (jobs[2].alive ? 1 : 0) + (jobs[3].alive ? 1 : 0); }
int smp_worker_ok(int core) { return core >= 2 && core <= 3 && jobs[core].alive; }

void secondary_main(int core)
{
    mmu_enable();
    if (core >= 2) worker(core);
    io_running = 1;
    for (;;) {
        io_lock();
        io_poll_all();
        io_unlock();
        io_loops++;
        delay_us(200);                                /* give core 0 a chance at the lock */
    }
}

/* a core crashed: core 1 gives its work back to core 0 */
void smp_crashed(int core)
{
    if (core == 1) {
        io_running = 0;
        if (io_l.owner == 1) { io_l.owner = -1; io_l.depth = 0; __atomic_store_n(&io_l.lock, 0, __ATOMIC_RELEASE); }
        if (con_l.owner == 1) { con_l.owner = -1; con_l.depth = 0; __atomic_store_n(&con_l.lock, 0, __ATOMIC_RELEASE); }
        if (mbox_l.owner == 1) { mbox_l.owner = -1; mbox_l.depth = 0; __atomic_store_n(&mbox_l.lock, 0, __ATOMIC_RELEASE); }
    }
    if (core >= 2) { jobs[core].alive = 0; __atomic_store_n(&jobs[core].busy, 0, __ATOMIC_RELEASE); }
    for (int c = 2; c <= 3; c++)                       /* a lock left behind by a worker */
        if (con_l.owner == c) { con_l.owner = -1; con_l.depth = 0; __atomic_store_n(&con_l.lock, 0, __ATOMIC_RELEASE); }
}

/* wake core 1 through the firmware's spin table; 1 = it runs */
int smp_start_io(void)
{
    extern char secondary_entry[];
    volatile u64 *spin = (volatile u64 *)0xE0;      /* core 1 (core 2: 0xE8, core 3: 0xF0) */
    *spin = (u64)(uintptr_t)secondary_entry;
    __asm__ volatile("dc civac, %0\n dsb sy\n sev" :: "r"(spin) : "memory");
    u64 end = timer_us() + 200000;
    while (!io_running && timer_us() < end) ;
    return io_running;
}

u64 smp_io_loops(void) { return io_loops; }

/* wake cores 2 and 3 as workers; returns how many run */
int smp_start_workers(void)
{
    extern char secondary_entry[];
    for (int c = 2; c <= 3; c++) {
        volatile u64 *spin = (volatile u64 *)(uintptr_t)(0xE0 + (c - 1) * 8);   /* 0xE8, 0xF0 */
        *spin = (u64)(uintptr_t)secondary_entry;
        __asm__ volatile("dc civac, %0\n dsb sy\n sev" :: "r"(spin) : "memory");
    }
    u64 end = timer_us() + 200000;
    while (smp_workers() < 2 && timer_us() < end) ;
    return smp_workers();
}
