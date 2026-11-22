/* an extern declaration with an asm label can name an internal function */
static int local(void) { return 42; }
extern int via_asm(void) __asm__("local");
int (*entry)(void) = via_asm;
static int dropped(void) { return 1; }
