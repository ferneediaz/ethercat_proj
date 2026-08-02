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
            printf("t=%6.1fs target=%3u echo=%3u wkc=%d/%d low_wkc=%d "
                   "dc=%+8lldns peak=%lldns\n",
                   t, target, in ? in->echo_angle : 0, wkc,
                   ecat_expected_wkc(), low_wkc_cycles,
                   (long long)ecat_dc_delta(), (long long)dc_peak_ns);
         }
         else
         {
            printf("t=%6.1fs target=%3u echo=%3u wkc=%d/%d low_wkc=%d\n", t,
                   target, in ? in->echo_angle : 0, wkc, ecat_expected_wkc(),
                   low_wkc_cycles);
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
