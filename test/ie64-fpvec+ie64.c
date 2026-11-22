float __builtin_ie64_fipr(const float *a, const float *b);
void __builtin_ie64_ftrv(const float *m, float *v);

#ifdef __IE64_FPVEC__
int fpvec_macro = __IE64_FPVEC__;
#endif

float dot(const float *a, const float *b) { return __builtin_ie64_fipr(a, b); }
void transform(const float *m, float *v) { __builtin_ie64_ftrv(m, v); }

/* An unused result is still an operation: FIPR sets FPSR flags. */
void flags(float *a) { (void)__builtin_ie64_fipr(a, a); }
