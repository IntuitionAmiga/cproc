/* SPDX-FileCopyrightText: 2026 Zayn Otley
 * SPDX-License-Identifier: ISC */

unsigned long __builtin_ie64_aesks1i(unsigned long, unsigned int);

unsigned long f(unsigned long x, unsigned int y) { return __builtin_ie64_aesks1i(x, y); }
