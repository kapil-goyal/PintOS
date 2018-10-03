#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <fixed-point.h>
#include "threads/flags.h"
#include "devices/timer.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h" 
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Next wake up time for a thread to unblock*/
static int64_t next_wakeup_time;

/* Load Average Value */
static int load_avg;

/* List of threads that will be in blocked state */
static struct list sleepers;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Manager thread*/
static struct thread *manager_thread;

/* BSD Scheduler thread*/
static struct thread *bsd_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
bool fourth_tick;
bool one_second;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
void timer_wakeup (void);
static void manager (void *aux UNUSED);
static void bsd_scheduler (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */

/* Stores the original priority of the current thread and makes the its priority equal to PRI_MAX till
 it wakes up again. */
void thread_set_priority_temporarily_up() {
  thread_current()->original_priority = thread_current()->priority;
  thread_current()->priority = PRI_MAX;
}

/* Restores the original priority of the thread whose priority was increased sometime before */
void thread_priority_restore() {
  thread_current()->priority = thread_current()->original_priority;
}

void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleepers);
  next_wakeup_time = INT64_MAX;
  load_avg = 0;
  fourth_tick = false;
  one_second = false;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);

  struct semaphore manager_started;
  sema_init (&manager_started, 0);
  thread_create ("manager", PRI_MAX, manager, &manager_started);
  sema_down (&manager_started);

  struct semaphore bsd_started;
  sema_init (&bsd_started, 0);
  thread_create ("bsd_scheduler", PRI_MAX, bsd_scheduler, &bsd_started);
  sema_down (&bsd_started);
}



/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();
  t->recent_cpu = _ADD_INT(t->recent_cpu,1);

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (next_wakeup_time != INT64_MAX && timer_ticks() >= next_wakeup_time && manager_thread->status == THREAD_BLOCKED) {
    thread_unblock(manager_thread);
  }

  if (timer_ticks() % TIMER_FREQ == 0) {
    one_second = true;
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE) {
    fourth_tick = true;
    intr_yield_on_return ();
  }
    
  if (fourth_tick || one_second) {
    if (bsd_thread->status == THREAD_BLOCKED)
    {  
      thread_unblock(bsd_thread);
    }
  }
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);
  // thread_yield(); // important
  if(t->priority > thread_current()->priority){
    thread_yield();
  }
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}


// Comparator function to sort ready queue threads according to their original priority.
static bool comp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *ta = list_entry(a, struct thread, elem);
  struct thread *tb = list_entry(b, struct thread, elem);
  return (ta->priority > tb->priority);
}


/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
      list_insert_ordered (&ready_list, &t->elem, comp, NULL);

  t->status = THREAD_READY;
  intr_set_level (old_level);

}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it call schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread && cur != manager_thread && cur != bsd_thread) 
    list_insert_ordered (&ready_list, &cur->elem, comp, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. Also preemption is taken care of
   Donation of priority is also considered. Now thread->priority is considered as donated priority */
void
thread_set_priority (int new_priority) 
{
  if(thread_current()->before_donate_priority == thread_current()->priority){
    thread_current ()->priority = new_priority;
  } 
  else
  {
    thread_current ()->priority = new_priority>thread_current ()->priority ? new_priority:thread_current ()->priority;
  }
  thread_current()->before_donate_priority = new_priority;
  
  if(list_empty(&ready_list)){
    return;
  }
  struct thread *next_wanna_run = list_entry(list_front(&ready_list), struct thread, elem);

  if( next_wanna_run->priority >= new_priority){
    thread_yield();
  }
}
/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

void
thread_update_priority(struct thread* t) {
  int aux = _ADD_INT (_DIVIDE_INT (t->recent_cpu, 4), 2*t->nice_value);
  t->priority = _TO_INT_ZERO (_INT_SUB (PRI_MAX, aux));
}

void
thread_update_recent_cpu (struct thread* t) {
  int double_load_avg = _MULTIPLY_INT (load_avg, 2);
  int alpha = _DIVIDE (double_load_avg, _ADD_INT (double_load_avg, 1));
  int aux = _MULTIPLY (alpha, t->recent_cpu);
  t->recent_cpu = _ADD_INT (aux, t->nice_value);
}

void
thread_update_load_avg (void) {
  int ready_threads = list_size(&ready_list);
  int64_t num = _ADD_INT (_MULTIPLY_INT (load_avg, 59), ready_threads);
  load_avg = _DIVIDE_INT (num, 60);
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  thread_current()->nice_value = nice;
  thread_update_priority(thread_current());

    thread_yield();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current()->nice_value;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return _TO_INT_NEAREST (_MULTIPLY_INT (load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return _TO_INT_NEAREST (_MULTIPLY_INT (thread_current ()->recent_cpu, 100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

// This function will set next_wakeup_time value acc to sleepers queue value.
void thread_set_next_wakeup() {
  if (list_empty(&sleepers)) {
    next_wakeup_time = INT64_MAX;
    return;
  }
  struct thread *front_thread = list_entry(list_front(&sleepers), struct thread, sleepers_elem);
  next_wakeup_time = front_thread->wakeup_at;
}

void
timer_wakeup (void) {
  enum intr_level intr_old_level = intr_get_level();
  intr_disable();

  while (!list_empty(&sleepers)) {
    struct thread *t = list_entry(list_front(&sleepers), struct thread, sleepers_elem);
    if (t->wakeup_at > next_wakeup_time)
      break;
    list_pop_front(&sleepers);
    thread_unblock(t);
  }
  thread_set_next_wakeup();
  intr_set_level(intr_old_level);
}

static void
manager (void *manager_started_ UNUSED) 
{
  struct semaphore *manager_started = manager_started_;
  manager_thread = thread_current ();
  sema_up (manager_started);

  while (true) {
    intr_disable();
    thread_block();
    intr_enable();

    timer_wakeup();
  }
}

static void 
bsd_scheduler (void *bsd_started_ UNUSED)
{
  struct semaphore *bsd_started = bsd_started_;
  bsd_thread = thread_current ();
  sema_up (bsd_started);

  struct list_elem *e;

  while (true)
  {
    intr_disable ();
    thread_block ();
    intr_enable ();

    if(thread_mlfqs){
      
      if (one_second)
      {
        thread_update_load_avg ();
        for (e = list_begin (&all_list); e != list_end (&all_list); e = list_next (e))
        {
          struct thread *t = list_entry (e, struct thread, allelem);
          if (t != manager_thread &&
              t != bsd_thread &&
              t != idle_thread)
          {
            thread_update_recent_cpu (t);
          }
        }
        one_second = false;
      }

      if (fourth_tick)
      {
        for (e = list_begin (&all_list); e != list_end (&all_list);
             e = list_next (e))
        {
          struct thread *t = list_entry (e, struct thread, allelem);
          if (t != manager_thread &&
              t != bsd_thread &&
              t != idle_thread)
          {
            thread_update_priority (t);
          }
        }
        fourth_tick = false;
      }
    }
    
  }
}


/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->wakeup_at = -1;
  t->priority = priority;
  if (t == initial_thread)
    t->nice_value = 0;
  else
    t->nice_value = thread_current()->nice_value;
  t->recent_cpu = 0;
  t->before_donate_priority=priority;
  t->original_priority = priority;
  t->magic = THREAD_MAGIC;
  t->seeking = NULL;
  t->seeking_sema= NULL;
  list_init(&t->holding_locks);
  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
  {
    struct list_elem *e = list_min (&ready_list, comp, NULL);
    list_remove (e);
    return list_entry (e, struct thread, elem);
  }
    // return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
schedule_tail (struct thread *prev) 
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until schedule_tail() has
   completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  schedule_tail (prev); 
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

// Comparator function  to sort sleepers queue threads acc to their wakeup time.
static bool 
before (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *thread_a = list_entry(a, struct thread, sleepers_elem);
  struct thread *thread_b = list_entry(b, struct thread, sleepers_elem);

  if (thread_a->wakeup_at == thread_b->wakeup_at) {
    return thread_a->original_priority > thread_b->original_priority;
  }
  return thread_a->wakeup_at < thread_b->wakeup_at;
}

// This function will block the current thread for time = "wakeup_at" ticks and will insert it into 
// sleepers queue. Also next wakeup time is set if wakeup_at time is less than next_wakeup_time.
void
thread_block_till(int64_t wakeup_at) {
  struct thread *cur = thread_current();
  enum intr_level previous_intr;
  previous_intr = intr_disable();

  cur->wakeup_at = wakeup_at;
  if (next_wakeup_time >= wakeup_at)
    next_wakeup_time = wakeup_at;
  list_insert_ordered(&sleepers, &cur->sleepers_elem, before, NULL);
  thread_block();
  intr_set_level(previous_intr);
}

bool check_child_status(tid_t child_tid){
  struct list_elem *e;

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
  {
    struct thread *t = list_entry (e, struct thread, allelem);
    if(t->tid == child_tid){
      if(t->status!=THREAD_DYING){
        return true;
      }
    }
  }
  return false;
}


