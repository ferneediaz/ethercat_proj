#include "ecat.h"

#include <stdio.h>
#include <string.h>

#include "ethercat.h"

static char IOmap[256];
static int expected_wkc;

int ecat_open(const char *ifname)
{
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
   bool vendor_ok = (uint32_t)ec_slave[1].eep_man == SERVO_VENDOR_ID;
   bool product_ok = (uint32_t)ec_slave[1].eep_id == SERVO_PRODUCT_CODE;
   if (!vendor_ok)
   {
      printf("WARNING: slave 1 vendor is not ASIX (0x%8.8x). Wrong device?\n",
             SERVO_VENDOR_ID);
   }
   else if (!product_ok)
   {
      printf("Vendor is ASIX but product code differs from the ESI "
             "(expected 0x%8.8x). EEPROM likely not written yet — see "
             "docs/bringup-checklist.md Phase 3.\n",
             SERVO_PRODUCT_CODE);
   }
   else
   {
      printf("Slave 1 matches the expected ASIX identity. PASS\n");
   }
   return vendor_ok && product_ok;
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
