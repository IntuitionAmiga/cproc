void __builtin_ie64_ftrv(float *m, float *v);

void transform(float *m, float *v) { __builtin_ie64_ftrv(m, v); }
