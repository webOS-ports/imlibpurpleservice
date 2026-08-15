/* Compatibility shim: tidy-html5 5.x renamed buffio.h -> tidybuffio.h, but
 * src/sanitize.cpp includes <buffio.h> as the webOS-era libtidy spelled it.
 * Redirect rather than touching the source the device build also compiles. */
#ifndef IMLIBPURPLE_FUZZ_BUFFIO_COMPAT_H
#define IMLIBPURPLE_FUZZ_BUFFIO_COMPAT_H
#include <tidybuffio.h>
#endif
