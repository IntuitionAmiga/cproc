unsigned long __builtin_ie64_mfcr(unsigned int);
unsigned long f(void) { return __builtin_ie64_mfcr(16); }
