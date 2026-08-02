#include "ecat.h"

#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool ecat_to_op(void)
{
   ec_config_map(&IOmap);
   ec_configdc();

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
            printf("  SYNC0 generation looks correct in the ESC. If the slave "
                   "counts no edges,\n  suspect the wire: module pin 3 -> "
                   "ESP32 GPIO 21.\n");
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
      ec_slave[0].state = EC_STATE_INIT;
      ec_writestate(0);
   }
   ec_close();
}
