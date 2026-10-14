#pragma once
#include <stddef.h>
#include <stdint.h>

int hades_ipc_initialize(uint64_t process_id);
int hades_ipc_shutdown(void);
int hades_ipc_set_suspended(int suspended);
/* 1: active foreground; 2: inactive foreground. Background needs thread suspension. */
int hades_ipc_set_focus_state(unsigned state);
int hades_ipc_set_display_size(uint32_t width, uint32_t height);
int hades_ipc_report_gpu_error(void);
int hades_ipc_set_display_version(const char *version);
int hades_ipc_configure_input_frontend(void);
int hades_ipc_publish_input(uint64_t buttons, int32_t lx, int32_t ly);
/* 1: dispatched; 0: unsupported semantics; -1: invalid transport/host failure. */
int hades_ipc_svc(unsigned svc, uint64_t *registers, void *message, size_t capacity);
