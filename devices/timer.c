#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "lib/kernel/list.h"

  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

static struct list sleeping_threads;                /* List of sleeping thread, should be ordered through insert */ //---------------------------------------------------------------------------

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");

  list_init(&sleeping_threads);                     /* Setup up the list */ //------------------------------------------------------------------------------------------------------------------
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

//My changes
/* This will have the thread and the time the thread should wake up, the next thread is basically linked list
can also add wakeup_time in thread.h struct*/
struct
sleeping_thread
{
  struct thread *thread;
  int64_t wakeup_time;
  struct sleeping_thread *next_thread;
};

/* For sorting the list, in acending order */
bool
compare (const struct list_elem *a, const struct list_elem *b)
{
    return list_entry(a, struct thread, sleepelem)->wakeup_time < list_entry(b, struct thread, sleepelem)->wakeup_time;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{

  if (ticks <= 0)                                                                       /* 0 ticks are meaningless and so are negatives */ //----------------------------------------------------------------------
    return;

  int64_t start = timer_ticks();

  //Record the time it should wake up //-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  struct thread *sleepy = thread_current ();
  sleepy->wakeup_time = start + ticks;

  //Insert thread into the sleeping thread list in correct order //----------------------------------------------------------------------------------------------------------------------------------------------------
  // if (list_empty(&sleeping_threads) || list_entry(sleeping_threads.head.next, struct thread, sleepelem)->wakeup_time > start + ticks)   /* This thread should be next out, insert to front of linked list */ //----------------------------------------------------
  // {
  //   sleepy->sleepelem.next = &sleeping_threads.head->next;                                       /* Replace the sleepy elem next with the head of list */ //----------------------------------------------------------------
  //   sleeping_threads.head.prev = &sleepy->sleepelem;                                       /* Replace the old head prev with new head */ //---------------------------------------------------------------------------
  //   sleeping_threads.head = sleepy->sleepelem;                                             /* The head is now sleepy */ //--------------------------------------------------------------------------------------------
  // }
  // else
  // {
  //   //Go through each link in list //----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  //   struct list_elem *i = sleeping_threads.head;
  //   while (i.next != NULL)                                                                 /* Iterrate until last elem; after while loop, sleepy should be inserted after i */ //-------------------------------------
  //   {
  //     if (!(list_entry(i.next, struct thread, sleepelem)->wakeup_time <= start + ticks))
  //       break;
  //     i = i.next;
  //   }

  //   //Insert the elem in list //---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  //   sleepy->sleepelem.next = i.next;
  //   sleepy->sleepelem.prev = i;

  //   //Update elem around sleepy //-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  //   i->next->prev = sleepy->sleepelem;
  //   i->next = sleepy->sleepelem;

  //   //Update tail of list iftail is changed //-------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  //   if (i == sleeping_threads->tail)
  //     sleeping_threads->tail = sleepy->sleepelem;
  // }
  list_insert_ordered(&sleeping_threads, &sleepy->sleepelem, compare, NULL);

  //This will lock the thread and schedule the next one //-------------------------------------------------------------------------------------------------------------------------------------------------------------
  enum intr_level old_level = intr_disable ();         /* thread_block wants intr_off */ //-----------------------------------------------------------------------------------------------------------------------------
  thread_block ();                                     /* This will lock the thread and schedule the next one */ //-----------------------------------------------------------------------------------------------------
  intr_set_level (old_level);                          /* Other functions do this, otherwise kernel errors */ //--------------------------------------------------------------------------------------------------------
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  //In tick function, check if there is a sleeping thread, and then check if that thread should wake up //-------------------------------------------------------------------------------------------------------------
  while (!list_empty(&sleeping_threads) && ticks >= list_entry(sleeping_threads.head.next, struct thread, sleepelem)->wakeup_time)   /* Wake up all threads that should be woken up at this time */ //------------------------
  {
    struct thread *wakey = list_entry (sleeping_threads.head.next, struct thread, sleepelem);

    //Update the sleeping thread list to remove the head //------------------------------------------------------------------------------------------------------------------------------------------------------------
    // sleeping_threads->head = sleeping_threads->head->next;
    // sleeping_threads->head->prev = NULL;
    list_pop_front (&sleeping_threads);

    //wakey is removed from list //------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // wakey->sleepelem->prev = NULL;
    // wakey->sleepelem->next = NULL;
    thread_unblock (wakey);
  }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
