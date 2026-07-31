#include <signal.h>
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
           "  op               bring the slave to OP and hold (Ctrl-C to stop)\n"
           "  set <angle>      write one target angle (0-180) and hold\n"
           "  sweep [period_s] sweep 0->180->0 (default period 4s, Ctrl-C to stop)\n"
           "Example: sudo %s eth0 scan\n",
           prog, prog);
}

/* Sleep until the next cycle boundary */
static void cycle_sleep(struct timespec *next)
{
   next->tv_nsec += ECAT_CYCLE_US * 1000L;
   while (next->tv_nsec >= 1000000000L)
   {
      next->tv_nsec -= 1000000000L;
      next->tv_sec += 1;
   }
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

      if (cycles % (1000000 / ECAT_CYCLE_US) == 0) /* once a second */
      {
         printf("t=%6.1fs target=%3u echo=%3u wkc=%d/%d low_wkc=%d\n", t,
                target, in ? in->echo_angle : 0, wkc, ecat_expected_wkc(),
                low_wkc_cycles);
         fflush(stdout);
      }
      cycles++;
      cycle_sleep(&next);
   }
   printf("Stopped after %ld cycles (%d low-WKC cycles).\n", cycles,
          low_wkc_cycles);
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
