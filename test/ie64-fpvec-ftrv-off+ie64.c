void __builtin_ie64_ftrv(const float *m, float *v);

void transform(const float *m, float *v) { __builtin_ie64_ftrv(m, v); }
