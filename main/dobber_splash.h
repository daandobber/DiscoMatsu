#pragma once

#include <stdint.h>

#include "pax_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Draws the complete splash into an existing PAX framebuffer. The caller
// owns presenting the framebuffer, which keeps this component independent
// from a particular board or display driver.
void dobber_splash_render(pax_buf_t *buffer, uint8_t percent);

#ifdef __cplusplus
}
#endif
