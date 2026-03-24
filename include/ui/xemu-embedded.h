#ifndef XEMU_EMBEDDED_H
#define XEMU_EMBEDDED_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool xemu_embedded_boot(const char *config_path, const char **error_out);
void xemu_embedded_pump_frame(void);
void xemu_embedded_request_shutdown(void);
bool xemu_embedded_is_active(void);
const char *xemu_embedded_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
