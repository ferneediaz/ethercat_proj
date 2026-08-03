#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "angle.h"
#include "ecat.h"

static volatile sig_atomic_t stop_requested = 0;

static void on_sigint(int sig)
{
   (void)sig;
   stop_requested = 1;
}

static void usage(const char *prog)
{
   fprintf(stderr,
           "Usage: %s <ifname> <command> [args]\n"
           "Commands:\n"
           "  scan             list slaves and check the ASIX identity\n"
           "  regs             dump and decode the ESC configuration registers\n"
           "  gpio             identify the L1-L8 LEDs and SW1/SW2 buttons\n"
           "  buttons [secs]   watch every ESC input for SW1/SW2 (default 45s)\n"
           "  op               bring the slave to OP and hold (Ctrl-C to stop)\n"
           "  set <angle>      write one target angle (0-180) and hold\n"
           "  move <from> <to> time one move, sampling actual every cycle\n"
           "  sweep [period_s] sweep 0->180->0 (default period 4s, Ctrl-C to stop)\n"
           "Example: sudo %s eth0 scan\n",
           prog, prog);
}

/*
 * Sleep until the next cycle boundary.
 *
 * correction_ns comes from the DC phase-locked loop and is zero when
 * distributed clocks are inactive, in which case this is a plain fixed-period
 * loop against an absolute deadline. When DC is active the correction
 * stretches or shortens individual cycles by a few hundred nanoseconds until
 * the master's wakeup sits at a fixed offset from the bus clock.
 */
static void cycle_sleep(struct timespec *next, int64_t correction_ns)
{
   int64_t nsec = (int64_t)next->tv_nsec + (int64_t)ECAT_CYCLE_US * 1000 +
                  correction_ns;

   /* The correction can be negative and larger than tv_nsec, so normalise in
    * both directions rather than only carrying upwards. */
   while (nsec >= 1000000000LL)
   {
      nsec -= 1000000000LL;
      next->tv_sec += 1;
   }
   while (nsec < 0)
   {
      nsec += 1000000000LL;
      next->tv_sec -= 1;
   }
   next->tv_nsec = (long)nsec;
   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

/* Run the cyclic loop. If sweep_period > 0, sweep; otherwise hold
 * fixed_angle. Prints a status line once a second. */
static int run_loop(double sweep_period, uint16_t fixed_angle)
{
   servo_outputs_t *out = ecat_outputs();
   servo_inputs_t *in = ecat_inputs();
   if (out == NULL)
   {
      fprintf(stderr,
              "Slave output image is smaller than servo_outputs_t — PDO "
              "mapping mismatch with pdo_layout.h.\n");
      return EXIT_FAILURE;
   }
   /* Symmetric with the check above, and not merely tidiness: the status
    * line used to fall back to printing 0 when this was NULL, so an input
    * mapping that had grown past the SyncManager showed up as "the slave is
    * echoing zero" rather than as the mapping error it is. */
   if (in == NULL)
   {
      fprintf(stderr,
              "Slave input image is smaller than servo_inputs_t — PDO "
              "mapping mismatch with pdo_layout.h.\n");
      return EXIT_FAILURE;
   }

   struct timespec next, start;
   clock_gettime(CLOCK_MONOTONIC, &start);
   next = start;
   long cycles = 0;
   int low_wkc_cycles = 0;
   int64_t dc_peak_ns = 0;
   bool dc_reported = false;

   while (!stop_requested)
   {
      double t = 0.0;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      t = (double)(now.tv_sec - start.tv_sec) +
          (double)(now.tv_nsec - start.tv_nsec) / 1e9;

      uint16_t target = (sweep_period > 0.0)
                            ? angle_sweep_position(t, sweep_period)
                            : fixed_angle;
      out->target_angle = target;

      int wkc = ecat_cycle();
      if (wkc < ecat_expected_wkc())
      {
         low_wkc_cycles++;
      }

      int64_t correction = ecat_dc_correction();

      /* One deferred DC status read, once the first SYNC0 pulse is long
       * overdue. Reading it at activation time proves nothing. */
      if (ecat_dc_active() && !dc_reported && t > 5.0)
      {
         dc_reported = true;
         ecat_dc_report();
      }

      /* Ignore the first two seconds: the phase-locked loop starts wherever
       * the master happened to boot relative to the bus clock, and that
       * initial error is not a jitter measurement. */
      if (ecat_dc_active() && t > 2.0)
      {
         int64_t d = ecat_dc_delta();
         int64_t mag = (d < 0) ? -d : d;

         if (mag > dc_peak_ns)
         {
            dc_peak_ns = mag;
         }
      }

      if (cycles % (1000000 / ECAT_CYCLE_US) == 0) /* once a second */
      {
         if (ecat_dc_active())
         {
            printf("t=%6.1fs target=%3u echo=%3u actual=%3u wkc=%d/%d "
                   "low_wkc=%d dc=%+8lldns peak=%lldns\n",
                   t, target, in->echo_angle, in->actual_angle, wkc,
                   ecat_expected_wkc(), low_wkc_cycles,
                   (long long)ecat_dc_delta(), (long long)dc_peak_ns);
         }
         else
         {
            printf("t=%6.1fs target=%3u echo=%3u actual=%3u wkc=%d/%d "
                   "low_wkc=%d\n",
                   t, target, in->echo_angle, in->actual_angle, wkc,
                   ecat_expected_wkc(), low_wkc_cycles);
         }
         fflush(stdout);
      }
      cycles++;
      cycle_sleep(&next, correction);
   }
   printf("Stopped after %ld cycles (%d low-WKC cycles).\n", cycles,
          low_wkc_cycles);
   if (dc_peak_ns > 0)
   {
      printf("Peak DC phase error after settling: %lld ns.\n",
             (long long)dc_peak_ns);
   }
   return EXIT_SUCCESS;
}


/*
 * Command one step change and trace the axis to it, sampling every cycle.
 *
 * The 1 Hz status line in run_loop() is far too coarse to see an axis move:
 * a 180-degree move at the stepper's default rate takes about 250 ms, so a
 * once-a-second sample almost always lands after arrival and actual looks
 * identical to the command. That is precisely the case where a field wired to
 * the target and a field wired to a position counter are indistinguishable.
 *
 * Sampling every cycle separates them. A real position counter passes through
 * intermediate values on the way; an echo jumps in one step. The count of
 * distinct intermediate values printed below is what makes that testable
 * rather than a matter of opinion.
 *
 * The elapsed time is also the measurement Phase 3 needs: change the step
 * interval over CoE and this number must change proportionally.
 */
static int run_move(uint16_t from, uint16_t to)
{
   servo_outputs_t *out = ecat_outputs();
   servo_inputs_t *in = ecat_inputs();

   if (out == NULL || in == NULL)
   {
      fprintf(stderr, "PDO mapping mismatch with pdo_layout.h.\n");
      return EXIT_FAILURE;
   }

   struct timespec next;
   clock_gettime(CLOCK_MONOTONIC, &next);

   /* Park at the starting angle and let it settle, so the move being timed
    * starts from a known standstill rather than mid-travel. */
   out->target_angle = from;
   for (int i = 0; i < 200 && !stop_requested; i++)
   {
      ecat_cycle();
      cycle_sleep(&next, ecat_dc_correction());
   }
   printf("parked at %u (actual %u), now commanding %u\n", from,
          in->actual_angle, to);

   /* Distinct values of actual seen strictly between the endpoints. An echo
    * of the command produces none of these. */
   uint16_t seen[512];
   int nseen = 0;
   uint16_t prev = in->actual_angle;
   int cycles = 0;
   int arrived_at = -1;
   const int limit = 2000; /* 20 s at a 10 ms cycle */

   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   out->target_angle = to;

   while (cycles < limit && !stop_requested)
   {
      ecat_cycle();
      uint16_t a = in->actual_angle;

      if (a != prev)
      {
         if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
         {
            seen[nseen++] = a;
         }
         prev = a;
      }
      if (arrived_at < 0 && in->echo_angle == to && a == in->echo_angle)
      {
         arrived_at = cycles;
         break;
      }
      cycles++;
      cycle_sleep(&next, ecat_dc_correction());
   }

   struct timespec t1;
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
               (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

   /* Intermediates are the transitions that were not the final value. */
   int intermediates = nseen;
   if (intermediates > 0 && seen[nseen - 1] == in->actual_angle)
   {
      intermediates--;
   }

   printf("move %u -> %u: arrived=%s elapsed_ms=%.1f cycles=%d "
          "transitions=%d intermediates=%d final_actual=%u final_echo=%u\n",
          from, to, arrived_at >= 0 ? "yes" : "NO", ms, cycles, nseen,
          intermediates, in->actual_angle, in->echo_angle);

   if (arrived_at < 0)
   {
      fprintf(stderr, "Axis never reported reaching %u.\n", to);
      return EXIT_FAILURE;
   }
   return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
   if (argc < 3)
   {
      usage(argv[0]);
      return EXIT_FAILURE;
   }
   const char *ifname = argv[1];
   const char *cmd = argv[2];

   signal(SIGINT, on_sigint);

   int slaves = ecat_open(ifname);
   if (slaves < 0)
   {
      return EXIT_FAILURE;
   }

   int rc = EXIT_SUCCESS;

   if (strcmp(cmd, "scan") == 0)
   {
      if (slaves > 0)
      {
         ecat_print_slaves();
      }
      /* 0 slaves already reported by ecat_open; still a clean run */
   }
   else if (slaves == 0)
   {
      fflush(stdout); /* keep the message after ecat_open's output */
      fprintf(stderr, "Command '%s' needs at least one slave on the bus.\n",
              cmd);
      rc = EXIT_FAILURE;
   }
   else if (strcmp(cmd, "regs") == 0)
   {
      ecat_print_slaves();
      printf("\n");
      ecat_dump_regs();
   }
   else if (strcmp(cmd, "gpio") == 0)
   {
      /* Identify the board's L1-L8 LEDs and SW1/SW2 buttons. Works in INIT,
       * so it needs no host firmware and no process data. */
      ecat_gpio_probe();
   }
   else if (strcmp(cmd, "buttons") == 0)
   {
      /* Just the input half of the gpio probe, with a long enough window
       * that a human can be told to press something and still be in time. */
      ecat_button_watch(argc >= 4 ? atoi(argv[3]) : 45);
   }
   else if (strcmp(cmd, "op") == 0)
   {
      ecat_print_slaves();
      if (ecat_to_op())
      {
         printf("Holding OP; Ctrl-C to stop.\n");
         rc = run_loop(0.0, 0);
      }
      else
      {
         rc = EXIT_FAILURE;
      }
   }
   else if (strcmp(cmd, "move") == 0)
   {
      uint16_t from, to;
      if (argc < 5 || !angle_parse(argv[3], &from) || !angle_parse(argv[4], &to))
      {
         fprintf(stderr, "move needs <from> <to>, both 0-180.\n");
         rc = EXIT_FAILURE;
      }
      else if (ecat_to_op())
      {
         rc = run_move(from, to);
      }
      else
      {
         rc = EXIT_FAILURE;
      }
   }
   else if (strcmp(cmd, "set") == 0)
   {
      uint16_t angle;
      if (argc < 4 || !angle_parse(argv[3], &angle))
      {
         fprintf(stderr, "set needs an angle between 0 and 180.\n");
         rc = EXIT_FAILURE;
      }
      else if (ecat_to_op())
      {
         printf("Holding angle %u; Ctrl-C to stop.\n", angle);
         rc = run_loop(0.0, angle);
      }
      else
      {
         rc = EXIT_FAILURE;
      }
   }
   else if (strcmp(cmd, "sweep") == 0)
   {
      double period = 4.0;
      if (argc >= 4)
      {
         period = atof(argv[3]);
         if (period <= 0.0)
         {
            fprintf(stderr, "sweep period must be a positive number.\n");
            ecat_close();
            return EXIT_FAILURE;
         }
      }
      if (ecat_to_op())
      {
         printf("Sweeping 0->180->0 with period %.1fs; Ctrl-C to stop.\n",
                period);
         rc = run_loop(period, 0);
      }
      else
      {
         rc = EXIT_FAILURE;
      }
   }
   else
   {
      usage(argv[0]);
      rc = EXIT_FAILURE;
   }

   ecat_close();
   return rc;
}
