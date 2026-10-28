/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "telemetry/ano_simulation.h"
#include <math.h>

void ano_simulation_sample(unsigned long long elapsed_ms, float channels[4])
{
    double phase = (double)(elapsed_ms % 4000) / 4000.0;
    double angle = 6.28318530717958647693 * phase;
    channels[0] = (float)sin(angle);
    channels[1] = (float)cos(angle);
    channels[2] = (float)(1.0 - 4.0 * fabs(phase - 0.5));
    channels[3] = phase < 0.5 ? 1.0f : -1.0f;
}
