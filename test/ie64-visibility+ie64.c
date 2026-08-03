void visible_function(void) {}
__attribute__((visibility("hidden"))) void hidden_function(void) {}
int visible_object;
__attribute__((visibility("hidden"))) int hidden_object;
__attribute__((section(".fini_array_onexit"), used)) void *retained = hidden_function;
