#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../src/angle.h"

static int failures = 0;

#define CHECK(cond)                                                     \
   do                                                                   \
   {                                                                    \
      if (!(cond))                                                      \
      {                                                                 \
         printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
         failures++;                                                    \
      }                                                                 \
   } while (0)

int main(void)
{
   uint16_t a;

   /* angle_parse: valid values */
   CHECK(angle_parse("0", &a) && a == 0);
   CHECK(angle_parse("90", &a) && a == 90);
   CHECK(angle_parse("180", &a) && a == 180);

   /* angle_parse: invalid values */
   CHECK(!angle_parse("-1", &a));
   CHECK(!angle_parse("181", &a));
   CHECK(!angle_parse("abc", &a));
   CHECK(!angle_parse("90x", &a));
   CHECK(!angle_parse("", &a));
   CHECK(!angle_parse(NULL, &a));

   /* angle_clamp */
   CHECK(angle_clamp(-50) == 0);
   CHECK(angle_clamp(0) == 0);
   CHECK(angle_clamp(90) == 90);
   CHECK(angle_clamp(180) == 180);
   CHECK(angle_clamp(999) == 180);

   /* angle_sweep_position: triangle wave, period 4s */
   CHECK(angle_sweep_position(0.0, 4.0) == 0);
   CHECK(angle_sweep_position(1.0, 4.0) == 90);  /* quarter period: half up */
   CHECK(angle_sweep_position(2.0, 4.0) == 180); /* half period: top */
   CHECK(angle_sweep_position(3.0, 4.0) == 90);  /* three quarters: half down */
   CHECK(angle_sweep_position(4.0, 4.0) == 0);   /* full period: back to 0 */
   CHECK(angle_sweep_position(6.0, 4.0) == 180); /* wraps into next period */
   CHECK(angle_sweep_position(1.0, 0.0) == 0);   /* bad period is safe */

   /* angle_to_pulse_ms: SG90 datasheet values */
   CHECK(fabs(angle_to_pulse_ms(0) - 0.5) < 1e-9);
   CHECK(fabs(angle_to_pulse_ms(90) - 1.5) < 1e-9);
   CHECK(fabs(angle_to_pulse_ms(180) - 2.5) < 1e-9);

   if (failures == 0)
   {
      printf("test_angle: all tests passed\n");
      return 0;
   }
   printf("test_angle: %d failure(s)\n", failures);
   return 1;
}
