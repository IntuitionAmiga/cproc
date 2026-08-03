int weak_object __attribute__((weak));
int hidden_object __attribute__((visibility("hidden")));
int section_object __attribute__((section(".special"))) = 1;
int weak_first __attribute__((weak)), weak_second;
int hidden_first __attribute__((visibility("hidden"))), hidden_second;
int section_first __attribute__((section(".first"))) = 2, section_second = 3;
extern __attribute__((visibility("hidden"))) int hidden_interleaved = 4;

void constructor(void) __attribute__((constructor(101)));
void constructor(void) {}

void destructor(void) __attribute__((destructor(202)));
void destructor(void) {}
