float __builtin_ie64_fipr(const float *a, const float *b);

#ifdef __IE64_FPVEC__
#error "the macro must not be defined without -m fpvec"
#endif

float dot(const float *a, const float *b) { return __builtin_ie64_fipr(a, b); }
