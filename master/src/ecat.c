#include "ecat.h"

#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ethercat.h"

static char IOmap[256];
static int expected_wkc;

/*
 * Distributed clock timing.
 *
 * The DC cycle matches the master's process-data cycle: one SYNC0 pulse per
 * frame is the only arrangement where every frame is acted on exactly once.
 *
 * DC_SYNC0_SHIFT_NS places the SYNC0 edge that far after the DC cycle
 * boundary. The master aims to put its frame on the wire at the boundary
 * itself, so the shift is the margin available for the frame to reach the
 * slave and for the ESC to write SM2 before the slave reads it.
 *
 * Real transit on this one-node bus is tens of microseconds. 2 ms is
 * deliberately generous: the failure mode of too small a shift is that the
 * slave acts on the previous cycle's data every time, which presents as a
 * constant one-cycle lag rather than as an obvious fault, and is therefore
 * expensive to notice. Too large a shift only costs latency.
 */
#define DC_CYCLE_NS ((uint32)ECAT_CYCLE_US * 1000u)
#define DC_SYNC0_SHIFT_NS 2000000

/*
 * The master aims to wake this far after the DC cycle boundary. Waking
 * exactly on it would leave the phase error straddling zero and the sign of
 * the correction flapping, so a small positive offset keeps the controller on
 * one side of the boundary. Taken from SOEM's own red_test example.
 */
#define DC_MASTER_LEAD_NS 50000

static bool dc_active;
static int64_t dc_delta_ns;
static int64_t dc_start_time;

int ecat_open(const char *ifname)
{
   /* SOEM's ec_init can succeed on a name that isn't a real interface,
    * which then looks identical to "no hardware attached". Check first
    * so a typo reports as a typo. */
   if (if_nametoindex(ifname) == 0)
   {
      fprintf(stderr,
              "No network interface named '%s' on this machine.\n"
              "List the available ones with: ip -brief link\n"
              "(The EtherCAT segment is normally eth0.)\n",
              ifname);
      return -1;
   }
   if (!ec_init(ifname))
   {
      fprintf(stderr,
              "ec_init on %s failed. Are you running as root (sudo), and is "
              "the interface name right? (check with: ip link)\n",
              ifname);
      return -1;
   }
   if (ec_config_init(FALSE) <= 0)
   {
      printf("No slaves found on %s.\n", ifname);
      printf("(Expected until the AX58100 board is connected to the IN port "
             "and powered.)\n");
      return 0;
   }
   return ec_slavecount;
}

bool ecat_print_slaves(void)
{
   for (int i = 1; i <= ec_slavecount; i++)
   {
      printf("Slave %d: name='%s' vendor=0x%8.8x product=0x%8.8x rev=0x%8.8x\n",
             i, ec_slave[i].name, (unsigned)ec_slave[i].eep_man,
             (unsigned)ec_slave[i].eep_id, (unsigned)ec_slave[i].eep_rev);
   }
   if (ec_slavecount < 1)
   {
      return false;
   }
   uint32_t man = (uint32_t)ec_slave[1].eep_man;
   uint32_t id = (uint32_t)ec_slave[1].eep_id;

   /* Identity comes from the ESC's EEPROM, not the silicon, so a
    * mismatch means "EEPROM holds a different ESI", not "wrong chip". */
   if (man == SERVO_VENDOR_ID && id == SERVO_PRODUCT_CODE)
   {
      printf("Identity matches the ASIX ESI. PASS\n");
      return true;
   }
   if (man == SSC_DEFAULT_VENDOR_ID && id == SSC_DEFAULT_PRODUCT_CODE)
   {
      printf("Identity is the stock Beckhoff SSC demo EEPROM the module "
             "ships with (vendor 0x%8.8x). The ESC is alive and readable; "
             "this is expected until the EEPROM is rewritten.\n",
             SSC_DEFAULT_VENDOR_ID);
      return true;
   }
   printf("Unrecognised identity (vendor 0x%8.8x product 0x%8.8x). The ESC "
          "responds, but its EEPROM holds an ESI we don't know about.\n",
          man, id);
   return false;
}

static const char *pdi_name(uint8_t pdi)
{
   /* AX58100 datasheet, EEPROM PDI Control (0x00) / register 0x0140 */
   switch (pdi)
   {
      case 0x00: return "interface deactivated (no PDI)";
      case 0x04: return "Digital I/O";
      case 0x05: return "SPI Slave";
      case 0x08: return "16-bit Asynchronous Local Bus";
      case 0x09: return "8-bit Asynchronous Local Bus";
      default: return "reserved/unknown";
   }
}

void ecat_dump_regs(void)
{
   uint16 adr = ec_slave[1].configadr;
   uint8_t type = 0, rev = 0, pdi = 0, esc_cfg = 0, pdi_cfg = 0, sync_cfg = 0;
   uint16_t build = 0, al_ctrl = 0, al_stat = 0;

   ec_FPRD(adr, 0x0000, sizeof(type), &type, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0001, sizeof(rev), &rev, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0002, sizeof(build), &build, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0140, sizeof(pdi), &pdi, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0141, sizeof(esc_cfg), &esc_cfg, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0150, sizeof(pdi_cfg), &pdi_cfg, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0151, sizeof(sync_cfg), &sync_cfg, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0120, sizeof(al_ctrl), &al_ctrl, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0130, sizeof(al_stat), &al_stat, EC_TIMEOUTRET);

   printf("ESC registers (slave 1 @ 0x%4.4x)\n", adr);
   printf("  0x0000 Type              0x%2.2x\n", type);
   printf("  0x0001 Revision          0x%2.2x\n", rev);
   printf("  0x0002 Build             0x%4.4x\n", build);
   printf("  0x0140 PDI Control       0x%2.2x  -> %s\n", pdi, pdi_name(pdi));
   printf("  0x0141 ESC Config        0x%2.2x  -> device emulation %s\n",
          esc_cfg, (esc_cfg & 0x01) ? "ON (ESC drives AL status itself)"
                                    : "OFF (host MCU must drive AL status)");
   if (pdi == 0x05)
   {
      printf("  0x0150 PDI Config       0x%2.2x  -> SPI mode %d, "
             "SPI_SEL active %s\n",
             pdi_cfg, pdi_cfg & 0x03,
             (pdi_cfg & 0x10) ? "high" : "low");
   }
   else
   {
      printf("  0x0150 PDI Config       0x%2.2x\n", pdi_cfg);
   }
   /*
    * 0x0151 decides whether the SYNC0 pad is an output at all. It is loaded
    * from the EEPROM, so on a module whose EEPROM we refuse to rewrite this
    * is a hard constraint rather than something to configure — and it is the
    * single register that determines whether DC synchronisation is even
    * possible here. Bit layout per ETG.1000.4 / ET1100.
    */
   printf("  0x0151 SYNC/LATCH PDI    0x%2.2x  -> SYNC0 %s, %s, active %s\n",
          sync_cfg,
          (sync_cfg & 0x04) ? "OUTPUT (DC sync usable)"
                            : "NOT an output (LATCH0 input)",
          (sync_cfg & 0x01) ? "open drain" : "push-pull",
          (sync_cfg & 0x02) ? "low" : "high");
   printf("  0x0120 AL Control        0x%4.4x\n", al_ctrl);
   printf("  0x0130 AL Status         0x%4.4x\n", al_stat);

   printf("\n");
   if (!(sync_cfg & 0x04))
   {
      printf("SYNC0 is not configured as an output in this EEPROM. The pin\n"
             "will stay silent no matter what the master requests, so the\n"
             "slave will fall back to polling. Fixing it means rewriting the\n"
             "EEPROM, which this project deliberately does not do.\n\n");
   }
   if (pdi == 0x05)
   {
      printf("PDI is SPI Slave: the module expects a host MCU as SPI\n"
             "master on SCK/MISO/MOSI/NSS. SCS_FUNC must be pulled up\n"
             "on-board for this to be active, and this register reading\n"
             "0x05 is that confirmation.\n");
   }
   else if (pdi == 0x00)
   {
      printf("PDI is DEACTIVATED. An MCU on SPI will NOT work until the\n"
             "EEPROM is rewritten with PDI Control = 0x05.\n");
   }
   else
   {
      printf("PDI is not SPI. An SPI host MCU needs EEPROM PDI Control\n"
             "set to 0x05 (see docs/bringup-checklist.md).\n");
   }
}

void ecat_gpio_probe(void)
{
   uint16 a = ec_slave[1].configadr;
   uint8_t al = 0;
   uint16_t ext_pdi = 0;
   uint32_t out = 0, in = 0, in0 = 0;

   ec_FPRD(a, 0x0130, sizeof(al), &al, EC_TIMEOUTRET);
   /* 0x0152-0x0153: Extended PDI configuration. In Digital I/O / GPIO terms
    * this is where the EEPROM says which pins are inputs and which are
    * outputs, configured in pairs. We cannot change it without writing the
    * EEPROM, so it decides whether driving the LEDs is possible at all. */
   ec_FPRD(a, 0x0152, sizeof(ext_pdi), &ext_pdi, EC_TIMEOUTRET);
   ec_FPRD(a, 0x0F10, sizeof(out), &out, EC_TIMEOUTRET);
   ec_FPRD(a, 0x0F18, sizeof(in0), &in0, EC_TIMEOUTRET);

   printf("AL Status        0x%2.2x  (RUN LED shows this: 0x01 INIT=off, "
          "0x02 PREOP=blinking,\n                       0x04 SAFEOP=single "
          "flash, 0x08 OP=solid)\n", al);
   printf("0x0152 Ext PDI   0x%4.4x\n", ext_pdi);
   printf("0x0F10 GP output 0x%8.8x\n", (unsigned)out);
   printf("0x0F18 GP input  0x%8.8x\n\n", (unsigned)in0);

   printf("Walking one bit across General Purpose Outputs 0x0F10.\n");
   printf("Watch L1..L8 along the top edge of the board.\n\n");
   for (int bit = 0; bit < 8; bit++)
   {
      uint32_t v = 1u << bit;

      ec_FPWR(a, 0x0F10, sizeof(v), &v, EC_TIMEOUTRET);
      ec_FPRD(a, 0x0F10, sizeof(out), &out, EC_TIMEOUTRET);
      printf("  wrote bit %d (0x%8.8x), reads back 0x%8.8x\n", bit,
             (unsigned)v, (unsigned)out);
      fflush(stdout);
      osal_usleep(600000);
   }

   uint32_t all = 0x000000ffu;
   ec_FPWR(a, 0x0F10, sizeof(all), &all, EC_TIMEOUTRET);
   printf("\nAll eight bits set. L1..L8 stay dark while 0x0152 is 0x0000 —\n"
          "every pin is an input, so none of them can drive.\n");
   fflush(stdout);
   osal_usleep(1500000);

   uint32_t zero = 0;
   ec_FPWR(a, 0x0F10, sizeof(zero), &zero, EC_TIMEOUTRET);
   printf("Cleared.\n\n");

   printf("Now reading General Purpose Inputs 0x0F18 for 15 s.\n");
   printf("Press SW1 and SW2 — any bit that changes is that button.\n\n");
   uint32_t last = in0;
   for (int i = 0; i < 150; i++)
   {
      ec_FPRD(a, 0x0F18, sizeof(in), &in, EC_TIMEOUTRET);
      if (in != last)
      {
         printf("  inputs 0x%8.8x -> 0x%8.8x   (changed bits: 0x%8.8x)\n",
                (unsigned)last, (unsigned)in, (unsigned)(in ^ last));
         fflush(stdout);
         last = in;
      }
      osal_usleep(100000);
   }
   printf("\nExpect bits 16 and 17: SW1 is IO[16], SW2 is IO[17], both active "
          "low.\nUse `buttons [secs]` for a longer window than this.\n");
}

void ecat_button_watch(int seconds)
{
   uint16 a = ec_slave[1].configadr;
   uint32_t in = 0, last_in = 0;
   uint16_t dl = 0, last_dl = 0;
   uint8_t al = 0, last_al = 0;
   int ticks;
   int hits = 0;

   /*
    * Clamp before doing anything. A bad argument used to fall through as
    * zero ticks, which printed "0 changes seen" and then the conclusion that
    * the buttons are not wired to the chip — a wrong answer produced by never
    * having looked. Refuse instead.
    */
   if (seconds < 1 || seconds > 600)
   {
      fprintf(stderr, "buttons: watch time must be 1..600 seconds (got %d).\n",
              seconds);
      return;
   }
   ticks = seconds * 50; /* 20 ms per tick */

   ec_FPRD(a, 0x0F18, sizeof(last_in), &last_in, EC_TIMEOUTRET);
   ec_FPRD(a, 0x0110, sizeof(last_dl), &last_dl, EC_TIMEOUTRET);
   ec_FPRD(a, 0x0130, sizeof(last_al), &last_al, EC_TIMEOUTRET);

   printf("Baseline: 0x0F18 inputs 0x%8.8x, 0x0110 DL status 0x%4.4x, "
          "AL 0x%2.2x\n", (unsigned)last_in, last_dl, last_al);
   printf("Watching for %d s at 20 ms. Press SW1, then SW2, then both.\n\n",
          seconds);
   fflush(stdout);

   for (int i = 0; i < ticks; i++)
   {
      if (ec_FPRD(a, 0x0F18, sizeof(in), &in, EC_TIMEOUTRET) > 0 &&
          in != last_in)
      {
         printf("[%5.1fs] GPIO IN  0x%8.8x -> 0x%8.8x   bit(s) 0x%8.8x\n",
                i * 0.02, (unsigned)last_in, (unsigned)in,
                (unsigned)(in ^ last_in));
         last_in = in;
         hits++;
      }
      if (ec_FPRD(a, 0x0110, sizeof(dl), &dl, EC_TIMEOUTRET) > 0 &&
          dl != last_dl)
      {
         printf("[%5.1fs] DL STAT  0x%4.4x -> 0x%4.4x   (port/link change)\n",
                i * 0.02, last_dl, dl);
         last_dl = dl;
         hits++;
      }
      if (ec_FPRD(a, 0x0130, sizeof(al), &al, EC_TIMEOUTRET) > 0 &&
          al != last_al)
      {
         printf("[%5.1fs] AL STAT  0x%2.2x -> 0x%2.2x   (state change/reset)\n",
                i * 0.02, last_al, al);
         last_al = al;
         hits++;
      }
      osal_usleep(20000);
   }

   printf("\n%d change(s) seen.\n", hits);
   if (hits == 0)
   {
      /* Measured on this board 2026-08-02: SW1 is IO[16] (bit 16) and SW2 is
       * IO[17] (bit 17), both active low. So silence here means the buttons
       * were not pressed, or the run was too short — not that they are
       * unwired. Do not let this print a conclusion the hardware refutes. */
      printf("No input moved. On this board SW1 is IO[16] and SW2 is IO[17],\n"
             "so expect bits 16 and 17 of 0x0F18 to drop to 0 while held.\n"
             "Nothing changing most likely means nothing was pressed.\n");
   }
   fflush(stdout);
}

/* Defined below, next to the other watchdog register handling. */
static void ecat_arm_watchdog(void);

bool ecat_to_op(void)
{
   ec_config_map(&IOmap);
   ec_configdc();

   /* After ec_config_map, because that is what programs the SyncManagers the
    * watchdog guards; before SAFE_OP, so the slave is protected for its whole
    * time carrying process data. */
   ecat_arm_watchdog();

   printf("Waiting for all slaves to reach SAFE_OP...\n");
   ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   if (ec_slave[0].state != EC_STATE_SAFE_OP)
   {
      fprintf(stderr, "Not all slaves reached SAFE_OP (state 0x%2.2x).\n",
              ec_slave[0].state);
      ec_readstate();
      for (int i = 1; i <= ec_slavecount; i++)
      {
         fprintf(stderr, "  Slave %d state=0x%2.2x AL status=0x%4.4x (%s)\n",
                 i, ec_slave[i].state, ec_slave[i].ALstatuscode,
                 ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
      }
      return false;
   }

   expected_wkc = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;

   /*
    * Activate SYNC0 before requesting OP.
    *
    * The slave arms its SYNC0 interrupt at startup but only starts following
    * the edges once they appear, so activating here means the very first OP
    * cycle is already DC-paced. Doing it after OP would leave a window where
    * the slave is applying outputs on its fallback poll timer.
    */
   dc_active = false;
   if (!ec_slave[1].hasdc)
   {
      printf("Slave 1 reports no distributed-clock support; the loop will "
             "free-run.\n");
   }
   else if (getenv("SERVO_DC") != NULL && atoi(getenv("SERVO_DC")) == 0)
   {
      printf("Distributed clocks disabled by SERVO_DC=0. The loop will "
             "free-run — useful for comparing jitter against DC mode.\n");
   }
   else
   {
      ec_dcsync0(1, TRUE, DC_CYCLE_NS, DC_SYNC0_SHIFT_NS);
      dc_active = true;
      printf("Distributed clocks: SYNC0 active on slave 1, %u us cycle, "
             "%d us shift.\n",
             (unsigned)(DC_CYCLE_NS / 1000u), DC_SYNC0_SHIFT_NS / 1000);

      /*
       * Read the DC registers back rather than trusting that the write took.
       * ec_dcsync0() returns void, so without this there is no way to tell a
       * working clock from a silent pin — and "the slave counted zero edges"
       * has at least three quite different causes.
       */
      {
         uint16 a = ec_slave[1].configadr;
         uint8_t act = 0, cyc_unit = 0;
         uint32_t cyc0 = 0;
         int64_t start = 0, systime = 0;

         ec_FPRD(a, 0x0981, sizeof(act), &act, EC_TIMEOUTRET);
         ec_FPRD(a, 0x0980, sizeof(cyc_unit), &cyc_unit, EC_TIMEOUTRET);
         ec_FPRD(a, 0x09A0, sizeof(cyc0), &cyc0, EC_TIMEOUTRET);
         ec_FPRD(a, 0x0990, sizeof(start), &start, EC_TIMEOUTRET);
         dc_start_time = start;
         ec_FPRD(a, 0x0910, sizeof(systime), &systime, EC_TIMEOUTRET);

         /*
          * Pulse length is EEPROM-loaded and in units of 10 ns. Zero does
          * not mean "no pulse": it means the ESC drives SYNC0 active and
          * holds it there until the application acknowledges by reading
          * 0x098E. A host that never acknowledges therefore sees exactly
          * one edge ever — or none at all, if the pin latched high during
          * an earlier run and the ESC was never power-cycled since.
          */
         uint16_t pulse = 0;
         ec_FPRD(a, 0x0982, sizeof(pulse), &pulse, EC_TIMEOUTRET);
         printf("  0x0982 SYNC0 pulse len %u (x10ns) = %u ns%s\n", pulse,
                (unsigned)pulse * 10u,
                (pulse == 0)
                    ? "  <-- 0: level-latched, needs ack via 0x098E"
                    : "");

         printf("  0x0980 cyclic unit ctl 0x%2.2x\n", cyc_unit);
         printf("  0x0981 DC activation  0x%2.2x  -> cyclic %s, SYNC0 %s\n",
                act, (act & 0x01) ? "ON" : "off",
                (act & 0x02) ? "ON" : "off");
         printf("  0x09A0 SYNC0 cycle    %u ns\n", (unsigned)cyc0);
         printf("  0x0990 start time     %lld\n", (long long)start);
         printf("  0x0910 system time    %lld\n", (long long)systime);
         /*
          * 0x0984 is the ESC's own verdict on whether cyclic operation
          * actually started. Bit 2 is the start-time plausibility check: if
          * the start time was already in the past, or too far ahead, the ESC
          * refuses to begin and every other register still reads as if it
          * had. That combination — perfect configuration, no pulses — is
          * otherwise indistinguishable from a broken wire.
          *
          * 0x092C is how far this slave's clock is from the reference. It
          * must be small and stable, or DC is not actually synchronised.
          */
         uint8_t actstat = 0;
         uint32_t tdiff = 0;
         int64_t systime2 = 0;

         ec_FPRD(a, 0x0984, sizeof(actstat), &actstat, EC_TIMEOUTRET);
         ec_FPRD(a, 0x092C, sizeof(tdiff), &tdiff, EC_TIMEOUTRET);
         osal_usleep(20000);
         ec_FPRD(a, 0x0910, sizeof(systime2), &systime2, EC_TIMEOUTRET);

         printf("  0x0984 activation stat 0x%2.2x  -> SYNC0 pending %s, "
                "start-time check %s\n",
                actstat, (actstat & 0x01) ? "yes" : "no",
                (actstat & 0x04) ? "FAILED (start time out of range)" : "ok");
         printf("  0x092C time deviation  %u ns\n", (unsigned)tdiff);
         printf("  0x0910 system time     %lld -> %lld after 20 ms (%s)\n",
                (long long)systime, (long long)systime2,
                (systime2 > systime) ? "clock IS running"
                                     : "CLOCK IS NOT RUNNING");

         if ((act & 0x03) != 0x03)
         {
            printf("  !! SYNC0 generation is NOT enabled in the ESC. The pin "
                   "will not pulse.\n");
         }
         else if (cyc0 == 0)
         {
            printf("  !! SYNC0 cycle time is 0 — the ESC will fire once, not "
                   "cyclically.\n");
         }
         else
         {
            /* Names the pin the firmware actually uses. This said GPIO 21
             * for a long time after SYNC0 had been moved to GPIO 2, which is
             * worse than saying nothing: it sends whoever is debugging to
             * inspect a wire that is not there. GPIO 21 does not work as an
             * input on this board — see firmware/overlays/nema17.overlay. */
            printf("  SYNC0 generation looks correct in the ESC. If the slave "
                   "counts no edges,\n  suspect the wire: module header pin 3 "
                   "-> ESP32 GPIO 2.\n");
         }
      }
      printf("The master's cycle will phase-lock to the bus clock; watch the "
             "dc= figure converge.\n");
   }

   /* Request OP; one valid process-data cycle is required first */
   ec_slave[0].state = EC_STATE_OPERATIONAL;
   ec_send_processdata();
   ec_receive_processdata(EC_TIMEOUTRET);
   ec_writestate(0);

   int tries = 40;
   do
   {
      ec_send_processdata();
      ec_receive_processdata(EC_TIMEOUTRET);
      ec_statecheck(0, EC_STATE_OPERATIONAL, 50000);
   } while (tries-- && ec_slave[0].state != EC_STATE_OPERATIONAL);

   if (ec_slave[0].state != EC_STATE_OPERATIONAL)
   {
      fprintf(stderr, "Failed to reach OP (state 0x%2.2x).\n",
              ec_slave[0].state);
      return false;
   }
   printf("All slaves reached OP. Expected working counter: %d\n",
          expected_wkc);
   return true;
}

servo_outputs_t *ecat_outputs(void)
{
   if (ec_slave[1].Obytes < sizeof(servo_outputs_t) || !ec_slave[1].outputs)
   {
      return NULL;
   }
   return (servo_outputs_t *)ec_slave[1].outputs;
}

servo_inputs_t *ecat_inputs(void)
{
   if (ec_slave[1].Ibytes < sizeof(servo_inputs_t) || !ec_slave[1].inputs)
   {
      return NULL;
   }
   return (servo_inputs_t *)ec_slave[1].inputs;
}

int ecat_cycle(void)
{
   ec_send_processdata();
   return ec_receive_processdata(EC_TIMEOUTRET);
}

int ecat_expected_wkc(void)
{
   return expected_wkc;
}

bool ecat_dc_active(void)
{
   return dc_active;
}

void ecat_dc_report(void)
{
   uint16 a;
   uint8_t actstat = 0, act = 0;
   int64_t systime = 0;

   if (!dc_active)
   {
      return;
   }
   a = ec_slave[1].configadr;
   ec_FPRD(a, 0x0984, sizeof(actstat), &actstat, EC_TIMEOUTRET);
   ec_FPRD(a, 0x0981, sizeof(act), &act, EC_TIMEOUTRET);
   ec_FPRD(a, 0x0910, sizeof(systime), &systime, EC_TIMEOUTRET);

   printf("DC status now: 0x0981=0x%2.2x 0x0984=0x%2.2x  system time %lld\n",
          act, actstat, (long long)systime);
   printf("  start time was %lld (%lld ms ago)\n", (long long)dc_start_time,
          (long long)((systime - dc_start_time) / 1000000));
   if (actstat & 0x01)
   {
      printf("  !! The FIRST SYNC0 pulse is still pending, long after its "
             "start time.\n     Cyclic operation never began — this is an ESC "
             "problem, not a wiring one.\n");
   }
   else
   {
      printf("  SYNC0 has started: the first pulse fired and the ESC is "
             "generating them.\n     If the host still sees no edges, the "
             "signal is not reaching its pin.\n");
   }
}

int64_t ecat_dc_delta(void)
{
   return dc_delta_ns;
}

/*
 * PI controller aligning the master's cycle to the slave's DC clock.
 *
 * ec_DCtime is the bus clock as of the last received frame. Taking it modulo
 * the cycle time gives where in the DC cycle this frame landed; the job is to
 * drive that towards DC_MASTER_LEAD_NS by nudging the sleep length.
 *
 * The proportional term corrects the current error and the integral term
 * removes the standing offset caused by the Pi's clock running at a slightly
 * different rate from the ESC's. The divisors are SOEM's, from red_test.c —
 * deliberately sluggish, because this is correcting a drift of parts per
 * million and an aggressive loop would just amplify scheduling noise.
 */
int64_t ecat_dc_correction(void)
{
   static int64_t integral;
   const int64_t cycle = (int64_t)ECAT_CYCLE_US * 1000;
   int64_t delta;

   if (!dc_active)
   {
      return 0;
   }

   delta = (ec_DCtime - DC_MASTER_LEAD_NS) % cycle;
   /* Fold the error into +/- half a cycle so the loop takes the short way
    * round instead of chasing a whole cycle of apparent error. */
   if (delta > (cycle / 2))
   {
      delta -= cycle;
   }
   if (delta > 0)
   {
      integral++;
   }
   if (delta < 0)
   {
      integral--;
   }
   dc_delta_ns = delta;
   return -(delta / 100) - (integral / 20);
}


/*
 * ESC watchdog registers, ETG.1000.4. Not defined by SOEM: ethercattype.h has
 * ECT_REG_WDCNT (0x0442) and nothing else, and it is never referenced.
 */
#define ESC_REG_WD_DIVIDER 0x0400 /* 16-bit, in 40 ns units, minus 2 */
#define ESC_REG_WD_TIME_SM 0x0420 /* 16-bit, in watchdog increments */
#define ESC_REG_WD_STATUS 0x0440  /* 16-bit, bit 0: 1 = SM watchdog OK */

/* One watchdog increment. The register counts 25 MHz ticks and the hardware
 * adds 2, so the value written is (target / 40 ns) - 2. */
#define WD_INCREMENT_NS 100000 /* 100 us */

/* How many cycles the slave may miss before the watchdog trips. Ten is long
 * enough that a single scheduling hiccup on a non-realtime kernel does not
 * fault the bus, and short enough that a dead master is noticed inside a
 * tenth of a second. */
#define WD_MISSED_CYCLES 10

/*
 * Arm the ESC's SyncManager watchdog.
 *
 * Without this the slave has no way to notice the master vanishing: it goes on
 * reading the last values left in DPRAM forever, and an axis mid-move holds
 * its last target indefinitely. The EEPROM already enables the watchdog
 * trigger bit on SM2 (F:00010064 in slaveinfo, see firmware/soes/
 * ecat_options.h), so the mechanism is present and merely unconfigured.
 *
 * Both values are derived from ECAT_CYCLE_US rather than written as constants,
 * so changing the cycle time cannot leave a watchdog tuned for the old one.
 */
static void ecat_arm_watchdog(void)
{
   uint16 adr = ec_slave[1].configadr;
   uint16 divider = (uint16)((WD_INCREMENT_NS / 40) - 2);
   uint32 timeout_ns = (uint32)ECAT_CYCLE_US * 1000u * WD_MISSED_CYCLES;
   uint16 count = (uint16)(timeout_ns / WD_INCREMENT_NS);
   uint16 status = 0;

   ec_FPWR(adr, ESC_REG_WD_DIVIDER, sizeof(divider), &divider, EC_TIMEOUTRET);
   ec_FPWR(adr, ESC_REG_WD_TIME_SM, sizeof(count), &count, EC_TIMEOUTRET);

   /* Deliberately no status read here. The watchdog only starts running once
    * process data is flowing, so 0x0440 reads 0 at this point whether the
    * configuration took or not — the same trap as reading DC register 0x0984
    * before the first SYNC0 pulse was due. See ecat_watchdog_report(). */
   (void)status;
   printf("SM watchdog armed: %u increments of %u ns = %u ms (%d missed "
          "cycles at %d us)\n",
          count, (unsigned)WD_INCREMENT_NS, (unsigned)(timeout_ns / 1000000u),
          WD_MISSED_CYCLES, ECAT_CYCLE_US);
}

void ecat_watchdog_report(void)
{
   uint16 status = 0;

   if (ec_slavecount < 1)
   {
      return;
   }
   if (ec_FPRD(ec_slave[1].configadr, ESC_REG_WD_STATUS, sizeof(status),
               &status, EC_TIMEOUTRET) <= 0)
   {
      fprintf(stderr, "SM watchdog: could not read 0x0440.\n");
      return;
   }
   printf("  0x0440 watchdog status 0x%4.4x -> %s\n", status,
          (status & 0x0001)
              ? "being fed, the slave is seeing our process data"
              : "EXPIRED — the slave is NOT receiving process data");
}

/*
 * Walk the bus down to a requested state, one transition at a time.
 *
 * ec_writestate() is fire-and-forget. The previous shutdown wrote INIT and
 * closed the socket in the next statement, so whether the slave ever saw the
 * request was down to timing — which is why a killed master left the slave
 * stuck in OP, and the next ESP32 boot logged AL Status 0x11 with code 0x001d
 * as it tried an illegal INIT->OP jump.
 *
 * Two things make this reliable. Process data keeps being exchanged while
 * waiting, because a slave in OP that stops receiving frames faults on its
 * watchdog instead of transitioning cleanly. And the descent goes one step at
 * a time: OP->INIT in a single write is legal in the spec but leaves less to
 * diagnose when a slave refuses, since there is no way to tell which of the
 * three transitions it objected to.
 */
bool ecat_request_state(uint16 state)
{
   static const uint16 ladder[] = {EC_STATE_SAFE_OP, EC_STATE_PRE_OP,
                                   EC_STATE_INIT};
   bool ok = true;

   if (ec_slavecount < 1)
   {
      return true;
   }

   /* Where are we actually? The cached value can be stale, or zero if
    * nothing has read it yet — and acting on zero made this walk try to
    * CLIMB to SAFE-OP from PRE-OP on any command that never reached OP,
    * which of course failed. */
   ec_readstate();

   for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++)
   {
      uint16 step = ladder[i];
      uint16 cur = ec_slave[0].state & 0x000f;
      bool in_error = (ec_slave[0].state & EC_STATE_ERROR) != 0;

      /* Only ever descend. Already at or below this rung means nothing to
       * do here — but keep going, because a lower rung may still apply. */
      if (cur <= step)
      {
         continue;
      }

      uint16 before = ec_slave[0].state;

      /* A slave sitting in a state with the error bit latched ignores a
       * plain state request; the acknowledge bit is what clears it. */
      ec_slave[0].state = in_error ? (step | EC_STATE_ACK) : step;
      ec_writestate(0);

      for (int t = 0; t < 50; t++)
      {
         ec_send_processdata();
         ec_receive_processdata(EC_TIMEOUTRET);
         if (ec_statecheck(0, step, 10000) == step)
         {
            break;
         }
      }
      printf("  0x%2.2x -> 0x%2.2x: now 0x%2.2x\n", before, step,
             ec_slave[0].state);

      if ((ec_slave[0].state & 0x000f) != step)
      {
         ec_readstate();
         fprintf(stderr,
                 "Slave would not leave 0x%2.2x for 0x%2.2x (AL status code "
                 "0x%4.4x %s).\n",
                 before, step, ec_slave[1].ALstatuscode,
                 ec_ALstatuscode2string(ec_slave[1].ALstatuscode));
         ok = false;
         break;
      }
      if (step == state)
      {
         break;
      }
   }
   return ok;
}


/*
 * Read AL Status without configuring anything.
 *
 * Every other command here goes through ecat_open(), which calls
 * ec_config_init() — and that transitions every slave to PRE-OP as part of
 * enumerating them. So the obvious way to check "did the slave end up in
 * INIT?" cannot work: looking moves it to PRE-OP before you can see.
 *
 * This opens the raw socket and issues one broadcast read of 0x0130, which
 * changes nothing. It is the only honest way to observe the state a previous
 * master run left behind.
 */
int ecat_probe_al_state(const char *ifname)
{
   uint16 al = 0;
   int wkc;

   if (if_nametoindex(ifname) == 0)
   {
      fprintf(stderr, "No network interface named '%s'.\n", ifname);
      return -1;
   }
   if (!ec_init(ifname))
   {
      fprintf(stderr, "ec_init on %s failed (need sudo?).\n", ifname);
      return -1;
   }

   wkc = ec_BRD(0, ECT_REG_ALSTAT, sizeof(al), &al, EC_TIMEOUTRET);
   ec_close();

   if (wkc <= 0)
   {
      fprintf(stderr, "No slave answered the broadcast read.\n");
      return -1;
   }
   al = etohs(al);
   static const char *const names[] = {"?",      "INIT", "PREOP", "BOOT",
                                       "SAFEOP", "?",    "?",     "?",
                                       "OP"};
   uint16 base = al & 0x000f;

   printf("AL Status 0x%4.4x -> %s%s  [%d slave(s) answered]\n", al,
          (base <= 8) ? names[base] : "?", (al & 0x0010) ? " +ERROR" : "",
          wkc);
   return (int)al;
}


/*
 * Measure the SyncManager watchdog end to end.
 *
 * Brings the bus to OP, runs process data normally for a couple of seconds so
 * the watchdog is being fed, then simply STOPS sending — without exiting, so
 * the socket stays open and the slave can still be read. Polls AL Status and
 * 0x0440 until the watchdog trips, and reports how long that took.
 *
 * Done in one process on purpose. The obvious alternative — kill the master
 * and start a second tool to watch — puts two raw sockets on the same
 * interface, each receiving the other's frames, which makes both unreliable.
 * And polling with a broadcast read is safe here precisely because a BRD of
 * 0x0130 does not write SM2, so watching does not feed the thing being watched.
 */
bool ecat_watchdog_test(void)
{
   const int feed_ms = 2000;
   const int poll_us = 2000;
   const int limit_ms = 5000;
   uint16 adr;
   uint16 al = 0, alcode = 0, wd = 0;
   struct timespec t0, now;
   double elapsed_ms = 0.0;

   if (!ecat_to_op())
   {
      return false;
   }
   adr = ec_slave[1].configadr;

   for (int i = 0; i < feed_ms / (ECAT_CYCLE_US / 1000); i++)
   {
      ecat_cycle();
      osal_usleep(ECAT_CYCLE_US);
   }

   ec_FPRD(adr, ESC_REG_WD_STATUS, sizeof(wd), &wd, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0130, sizeof(al), &al, EC_TIMEOUTRET);
   printf("Fed for %d ms: AL 0x%4.4x, watchdog 0x%4.4x (%s)\n", feed_ms, al,
          wd, (wd & 0x0001) ? "being fed" : "NOT running");

   if (!(wd & 0x0001))
   {
      fprintf(stderr,
              "Watchdog is not running even while process data flows — it "
              "cannot be tested. Check that SM2 has the watchdog trigger bit "
              "set (F:00010064 in slaveinfo).\n");
      return false;
   }

   printf("Stopping process data now; waiting for the watchdog...\n");
   clock_gettime(CLOCK_MONOTONIC, &t0);

   bool tripped = false;

   while (elapsed_ms < limit_ms)
   {
      osal_usleep(poll_us);
      clock_gettime(CLOCK_MONOTONIC, &now);
      elapsed_ms = (double)(now.tv_sec - t0.tv_sec) * 1000.0 +
                   (double)(now.tv_nsec - t0.tv_nsec) / 1e6;

      ec_FPRD(adr, ESC_REG_WD_STATUS, sizeof(wd), &wd, EC_TIMEOUTRET);
      if (!(wd & 0x0001))
      {
         tripped = true;
         break;
      }
   }

   if (!tripped)
   {
      fprintf(stderr, "Watchdog did not trip within %d ms.\n", limit_ms);
      return false;
   }
   printf("Watchdog tripped after %.1f ms (programmed %u ms).\n", elapsed_ms,
          (unsigned)((uint32)ECAT_CYCLE_US * WD_MISSED_CYCLES / 1000u));

   /*
    * The AL state machine reacts much later than the ESC register, and the gap
    * is the interesting part.
    *
    * The ESC's watchdog is hardware and time-based: it trips at the programmed
    * interval. SOES's own watchdog counts POLL ITERATIONS (watchdog_cnt in
    * soes_app.c), so what it means in seconds depends entirely on how fast the
    * slave's loop happens to be running — which changes the moment SYNC0
    * starts pacing it. Both are reported rather than only whichever fires
    * first.
    */
   struct timespec tal;
   double al_ms = 0.0;

   for (int i = 0; i < 1200; i++)
   {
      osal_usleep(5000);
      ec_FPRD(adr, 0x0130, sizeof(al), &al, EC_TIMEOUTRET);
      if (al & 0x0010)
      {
         break;
      }
   }
   clock_gettime(CLOCK_MONOTONIC, &tal);
   al_ms = (double)(tal.tv_sec - t0.tv_sec) * 1000.0 +
           (double)(tal.tv_nsec - t0.tv_nsec) / 1e6;
   printf("Slave AL state reacted after %.0f ms (SOES counts poll iterations, "
          "not time).\n", al_ms);
   ec_FPRD(adr, 0x0134, sizeof(alcode), &alcode, EC_TIMEOUTRET);
   printf("AL Status 0x%4.4x, AL Status Code 0x%4.4x (%s)\n", al, alcode,
          ec_ALstatuscode2string(alcode));
   return true;
}


/*
 * SDO access over the CoE mailbox.
 *
 * Different path from everything else here: process data rides the cyclic
 * frame and is limited to what the PDO mapping declares, while SDOs go through
 * the mailbox SyncManagers (SM0/SM1 at 0x1000/0x1080, which this module's
 * EEPROM already declares) and can reach any object in the dictionary. That is
 * how real EtherCAT devices are configured — process data moves, SDOs set up.
 *
 * Works from PRE-OP upwards, so parameters can be read and set before the
 * axis is allowed to move.
 */
bool ecat_sdo_read(uint16_t index, uint8_t sub, int64_t *out)
{
   uint8 buf[8] = {0};
   int size = (int)sizeof(buf);
   int wkc;

   wkc = ec_SDOread(1, index, sub, FALSE, &size, buf, EC_TIMEOUTRXM);
   if (wkc <= 0)
   {
      fprintf(stderr, "SDO read 0x%4.4x:%2.2x failed (wkc %d)%s%s\n", index,
              sub, wkc, ec_iserror() ? ": " : "",
              ec_iserror() ? ec_elist2string() : "");
      return false;
   }

   /* The dictionary decides the width; report what came back rather than
    * assuming, so a 1-byte object does not read as a huge number. */
   int64_t v = 0;

   for (int i = 0; i < size && i < 8; i++)
   {
      v |= (int64_t)buf[i] << (8 * i);
   }
   printf("0x%4.4x:%2.2x = %lld (0x%llx, %d byte%s)\n", index, sub,
          (long long)v, (unsigned long long)v, size, size == 1 ? "" : "s");
   if (out != NULL)
   {
      *out = v;
   }
   return true;
}

bool ecat_sdo_write(uint16_t index, uint8_t sub, int size, int64_t value)
{
   uint8 buf[8] = {0};
   int wkc;

   if (size < 1 || size > 8)
   {
      fprintf(stderr, "SDO write size must be 1..8 bytes.\n");
      return false;
   }
   for (int i = 0; i < size; i++)
   {
      buf[i] = (uint8)((uint64_t)value >> (8 * i));
   }

   wkc = ec_SDOwrite(1, index, sub, FALSE, size, buf, EC_TIMEOUTRXM);
   if (wkc <= 0)
   {
      /* A refused write is a result, not a crash: the slave is telling us the
       * value is out of range, and the abort code says why. */
      fprintf(stderr, "SDO write 0x%4.4x:%2.2x = %lld REFUSED (wkc %d)%s%s\n",
              index, sub, (long long)value, wkc, ec_iserror() ? ": " : "",
              ec_iserror() ? ec_elist2string() : "");
      return false;
   }
   printf("0x%4.4x:%2.2x <- %lld (%d byte%s) OK\n", index, sub,
          (long long)value, size, size == 1 ? "" : "s");
   return true;
}



/* Local copy of main.c's cycle sleep. Duplicated rather than exported because
 * the alternative is a header dependency from ecat.c back up into the CLI, and
 * this is six lines of absolute-deadline arithmetic. */
static void cycle_sleep_ns(struct timespec *next, int64_t correction_ns)
{
   int64_t nsec = (int64_t)next->tv_nsec + (int64_t)ECAT_CYCLE_US * 1000 +
                  correction_ns;

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

/*
 * Does mailbox traffic disturb process data?
 *
 * Everything else here does its SDO work in PRE-OP and exits, which leaves the
 * interesting question unanswered: SDOs are blocking mailbox transactions, and
 * this loop has a 10 ms budget and a 100 ms SyncManager watchdog. If an SDO
 * takes longer than the budget the cycle is missed; if it takes longer than the
 * watchdog the slave faults out of OP and the axis stops.
 *
 * So run the cyclic loop for real and inject SDOs into it, counting what that
 * costs. One process, because a second tool on the same interface would put two
 * raw sockets on one NIC and neither would be trustworthy.
 *
 * Reports the worst cycle overrun and whether the slave stayed in OP. A pass
 * here means mailbox access during operation is safe on this hardware; a fail
 * is worth knowing before relying on it.
 */
bool ecat_sdo_during_op(int seconds, int sdo_every_cycles)
{
   servo_outputs_t *out;
   struct timespec next, t0, t1;
   int cycles = 0, low_wkc = 0, sdos = 0, sdo_fail = 0;
   double worst_sdo_ms = 0.0, worst_cycle_ms = 0.0;
   int64_t worst_dc_ns = 0;
   int64_t value = 0;

   if (!ecat_to_op())
   {
      return false;
   }
   out = ecat_outputs();
   if (out == NULL)
   {
      fprintf(stderr, "PDO mapping mismatch.\n");
      return false;
   }

   printf("Running process data with an SDO every %d cycles for %d s...\n",
          sdo_every_cycles, seconds);

   clock_gettime(CLOCK_MONOTONIC, &next);
   int total = seconds * (1000000 / ECAT_CYCLE_US);

   for (int i = 0; i < total; i++)
   {
      struct timespec c0, c1;

      clock_gettime(CLOCK_MONOTONIC, &c0);
      out->target_angle = 90;
      if (ecat_cycle() < ecat_expected_wkc())
      {
         low_wkc++;
      }
      cycles++;

      if (sdo_every_cycles > 0 && (i % sdo_every_cycles) == 0 && i > 0)
      {
         int size = 8;
         uint8 buf[8] = {0};
         int wkc;

         clock_gettime(CLOCK_MONOTONIC, &t0);
         /* 0x8000:01 is the step interval — read only here, so a failure
          * cannot leave the axis configured differently than it started. */
         wkc = ec_SDOread(1, 0x8000, 0x01, FALSE, &size, buf, EC_TIMEOUTRXM);
         clock_gettime(CLOCK_MONOTONIC, &t1);

         double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

         if (ms > worst_sdo_ms)
         {
            worst_sdo_ms = ms;
         }
         sdos++;
         if (wkc <= 0)
         {
            sdo_fail++;
         }
         else
         {
            value = 0;
            for (int b = 0; b < size && b < 8; b++)
            {
               value |= (int64_t)buf[b] << (8 * b);
            }
         }
      }

      clock_gettime(CLOCK_MONOTONIC, &c1);
      double cms = (double)(c1.tv_sec - c0.tv_sec) * 1000.0 +
                   (double)(c1.tv_nsec - c0.tv_nsec) / 1e6;

      if (cms > worst_cycle_ms)
      {
         worst_cycle_ms = cms;
      }
      cycle_sleep_ns(&next, ecat_dc_correction());

      /*
       * The phase error is the number that actually matters for a
       * DC-synchronised node. A cycle that overruns its budget shifts when the
       * frame lands relative to SYNC0, and unlike a missed deadline that is
       * something the slave can feel. Ignore the first two seconds while the
       * phase-locked loop settles.
       */
      if (ecat_dc_active() && i > 2 * (1000000 / ECAT_CYCLE_US))
      {
         int64_t d = ecat_dc_delta();
         int64_t mag = (d < 0) ? -d : d;

         if (mag > worst_dc_ns)
         {
            worst_dc_ns = mag;
         }
      }
   }

   uint16 al = 0;

   ec_FPRD(ec_slave[1].configadr, 0x0130, sizeof(al), &al, EC_TIMEOUTRET);

   printf("cycles=%d low_wkc=%d sdos=%d sdo_failed=%d\n", cycles, low_wkc,
          sdos, sdo_fail);
   printf("worst SDO %.2f ms, worst cycle %.2f ms (budget %d ms), "
          "last value %lld\n",
          worst_sdo_ms, worst_cycle_ms, ECAT_CYCLE_US / 1000, (long long)value);
   printf("worst DC phase error after settling: %lld ns\n",
          (long long)worst_dc_ns);
   printf("AL Status after the run: 0x%4.4x (%s)\n", al,
          (al & 0x000f) == 0x08 ? "still OP" : "LEFT OP");

   return (low_wkc == 0) && (sdo_fail == 0) && ((al & 0x000f) == 0x08);
}

void ecat_close(void)
{
   if (ec_slavecount > 0)
   {
      /* Stop SYNC0 before dropping the bus. Leaving cyclic operation running
       * on a slave the master has abandoned means the pin keeps pulsing with
       * nothing feeding it data, which is exactly the confusing state this
       * project already lost time to once. */
      if (dc_active)
      {
         ec_dcsync0(1, FALSE, 0, 0);
         dc_active = false;
      }
      /* Walk down properly and confirm each rung, rather than firing one
       * write at INIT and closing the socket before the slave can act. */
      (void)ecat_request_state(EC_STATE_INIT);
   }
   ec_close();
}
