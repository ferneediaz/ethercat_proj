#include "angle.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

bool angle_parse(const char *s, uint16_t *out)
{
   if (s == NULL || *s == '\0')
   {
      return false;
   }
   char *end = NULL;
   errno = 0;
   long v = strtol(s, &end, 10);
   if (errno != 0 || end == s || *end != '\0')
   {
      return false;
   }
   if (v < ANGLE_MIN || v > ANGLE_MAX)
   {
      return false;
   }
   *out = (uint16_t)v;
   return true;
}

uint16_t angle_clamp(int value)
{
   if (value < ANGLE_MIN)
   {
      return ANGLE_MIN;
   }
   if (value > ANGLE_MAX)
   {
      return ANGLE_MAX;
   }
   return (uint16_t)value;
}

uint16_t angle_sweep_position(double t_seconds, double period_s)
{
   if (period_s <= 0.0)
   {
      return ANGLE_MIN;
   }
   double phase = fmod(t_seconds, period_s) / period_s; /* 0..1 */
   double pos;
   if (phase < 0.5)
   {
      pos = phase * 2.0; /* rising half */
   }
   else
   {
      pos = (1.0 - phase) * 2.0; /* falling half */
   }
   return angle_clamp((int)lround(pos * ANGLE_MAX));
}

double angle_to_pulse_ms(uint16_t angle)
{
   uint16_t a = angle_clamp(angle);
   return 0.5 + ((double)a / 180.0) * 2.0;
}
