#include "ecat.h"

#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "ethercat.h"

static char IOmap[256];
static int expected_wkc;

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
   uint8_t type = 0, rev = 0, pdi = 0, esc_cfg = 0, pdi_cfg = 0;
   uint16_t build = 0, al_ctrl = 0, al_stat = 0;

   ec_FPRD(adr, 0x0000, sizeof(type), &type, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0001, sizeof(rev), &rev, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0002, sizeof(build), &build, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0140, sizeof(pdi), &pdi, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0141, sizeof(esc_cfg), &esc_cfg, EC_TIMEOUTRET);
   ec_FPRD(adr, 0x0150, sizeof(pdi_cfg), &pdi_cfg, EC_TIMEOUTRET);
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
   printf("  0x0120 AL Control        0x%4.4x\n", al_ctrl);
   printf("  0x0130 AL Status         0x%4.4x\n", al_stat);

   printf("\n");
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

void ecat_close(void)
{
   if (ec_slavecount > 0)
   {
      ec_slave[0].state = EC_STATE_INIT;
      ec_writestate(0);
   }
   ec_close();
}
