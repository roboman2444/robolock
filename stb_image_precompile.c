//this file's sole pourpose is to make successive recompiles faster by not recompiling stb stuff
#include <stdlib.h>
#include <stdio.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
