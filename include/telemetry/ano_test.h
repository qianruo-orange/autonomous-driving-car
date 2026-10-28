/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ANO_TEST_H
#define ANO_TEST_H

/* Link-test signals only: CH0 sine, CH1 cosine, CH2 triangle, CH3 square.
 * Four-second period, normalized amplitude [-1, 1]. No controller interaction. */
void ano_test_sample(unsigned long long elapsed_ms, float channels[4]);

#endif
