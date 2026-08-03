#define ANGLE_HEADER <preprocess-include-macro.h>
#include ANGLE_HEADER
#define HEADER_NAME "preprocess-include-macro.h"
#define INDIRECT_HEADER HEADER_NAME
#include INDIRECT_HEADER
#define INCLUDE(argument) argument
#include INCLUDE(HEADER_NAME)
MACRO_HEADER_VALUE
