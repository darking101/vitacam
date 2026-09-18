#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int webserver_init(void);
void webserver_term(void);
const char *webserver_get_ip(void);
int webserver_get_port(void);
int webserver_is_active(void);
void webserver_set_enabled(int enabled);
const char *webserver_get_password(void);
void webserver_set_password(const char *pwd);
int webserver_is_enabled(void);
int webserver_check_and_clear_files_changed(void);

#ifdef __cplusplus
}
#endif

#endif
