/*
 * Host tests for the degrees <-> microsteps conversion in
 * firmware/src/step_math.h.
 *
 * Runs on the Mac with no hardware. Worth having because this is the one part
 * of the stepper path where a mistake is silent: an off-by-one puts the axis
 * a step away from where the master thinks it is, and with no encoder nothing
 * downstream can tell.
 *
 * The assertions are properties rather than a table of expected answers. A
 * table would only restate the formula, so it would agree with any bug that
 * was present when the table was written. Properties like "converting back
 * never moves more than one step" hold for the correct implementation and fail
 * for the plausible wrong ones — truncating instead of rounding, or losing the
 * clamp.
 */
#include <stdio.h>

#include "../../firmware/src/step_math.h"

static int failures;

#define CHECK(cond)                                                            \
   do                                                                          \
   {                                                                           \
      if (!(cond))                                                             \
      {                                                                        \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
         failures++;                                                           \
      }                                                                        \
   } while (0)

int main(void)
{
   /* Configurations this project could plausibly be set to: the current
    * 200 x1, and the microstep settings the board's DIP switches offer. */
   const uint32_t configs[] = {200, 400, 800, 1600, 3200, 6400};

   for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); c++)
   {
      uint32_t spr = configs[c];
      const uint16_t max_angle = 180;

      /* Endpoints are exact in both directions. */
      CHECK(step_math_angle_to_steps(0, spr) == 0);
      CHECK(step_math_steps_to_angle(0, spr, max_angle) == 0);
      CHECK(step_math_angle_to_steps(360, spr) == (int32_t)spr);

      int32_t prev = -1;

      for (uint16_t deg = 0; deg <= max_angle; deg++)
      {
         int32_t steps = step_math_angle_to_steps(deg, spr);

         /* Monotonic: a larger angle never maps to fewer steps. */
         CHECK(steps >= prev);
         prev = steps;

         /* Never asks for more steps than a full revolution holds. */
         CHECK(steps <= (int32_t)spr);

         /*
          * Round trip. One step of loss is inherent — the axis genuinely
          * cannot stop between steps — but more than that means the rounding
          * is wrong. At 200 steps/rev one step is 1.8 degrees, so allow 2.
          */
         uint16_t back = step_math_steps_to_angle(steps, spr, max_angle);
         int diff = (int)back - (int)deg;

         if (diff < 0)
         {
            diff = -diff;
         }
         CHECK(diff <= (int)(360 / spr) + 1);
      }

      /* Clamp holds above the limit rather than wrapping. */
      CHECK(step_math_steps_to_angle((int32_t)spr, spr, max_angle) ==
            max_angle);

      /* Negative positions report zero, not a wrapped unsigned angle. */
      CHECK(step_math_steps_to_angle(-1, spr, max_angle) == 0);
      CHECK(step_math_steps_to_angle(-100000, spr, max_angle) == 0);
   }

   /* A zero steps_per_rev must not divide by zero. */
   CHECK(step_math_steps_to_angle(50, 0, 180) == 0);

   /*
    * Rounding is to NEAREST, not down. At 200 steps/rev one step is 1.8
    * degrees, so 1 degree is closer to step 1 (1.8) than to step 0. A
    * truncating implementation returns 0 here and passes every test above.
    */
   CHECK(step_math_angle_to_steps(1, 200) == 1);
   CHECK(step_math_angle_to_steps(90, 200) == 50);

   if (failures == 0)
   {
      printf("test_step_math: all tests passed\n");
      return 0;
   }
   printf("test_step_math: %d failure(s)\n", failures);
   return 1;
}
