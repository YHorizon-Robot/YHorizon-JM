#ifndef YFOC_MOTOR_H
#define YFOC_MOTOR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ABI v1, cdecl, UTF-8. All commands return 0 on success, -1 on error.
   wait_feedback also returns 1 on timeout. One transaction per handle at a time. */
typedef uint64_t yfoc_handle;
typedef struct { uint32_t board_id, caps, fw_major, fw_minor; } yfoc_device_info;
typedef struct {
    uint32_t state, fault, warn, mode;
    int32_t inner_loop; /* -1 if unknown */
    double vbus, temp;
} yfoc_status_data;
typedef struct { double position, velocity, torque; } yfoc_feedback_data;
typedef struct { double id, iq, iq_ref, vq; } yfoc_current_data;
typedef struct {
    double i_limit, v_limit, kp, ki, kt, gear;
    uint32_t bandwidth_hz, loop_hz;
} yfoc_config_data;
typedef struct { int32_t node, has_second; double target, second; } yfoc_target;
typedef struct {
    uint64_t host_us;
    uint32_t cycle, node;
    double reference, position, velocity;
} yfoc_sample;
/* Samples are valid for this call only. Return nonzero to cancel. */
typedef int32_t (*yfoc_cycle_callback)(void *, const yfoc_sample *, size_t);

uint32_t yfoc_abi_version(void);
const char *yfoc_last_error(void); /* valid until next error on this thread */
int32_t yfoc_open(const char *interface_name, const char *channel, uint32_t bitrate, yfoc_handle *out);
int32_t yfoc_close(yfoc_handle);
int32_t yfoc_info(yfoc_handle, int32_t node, yfoc_device_info *out);
int32_t yfoc_status(yfoc_handle, int32_t node, yfoc_status_data *out);
int32_t yfoc_feedback(yfoc_handle, int32_t node, yfoc_feedback_data *out);
int32_t yfoc_wait_feedback(yfoc_handle, int32_t node, uint32_t timeout_ms, yfoc_feedback_data *out);
int32_t yfoc_current(yfoc_handle, int32_t node, yfoc_current_data *out);
int32_t yfoc_current_config(yfoc_handle, int32_t node, yfoc_config_data *out);
int32_t yfoc_set_mode(yfoc_handle, int32_t node, int32_t mode);
int32_t yfoc_set_inner(yfoc_handle, int32_t node, int32_t inner_loop);
int32_t yfoc_set_position(yfoc_handle, int32_t node, double rad, double max_rad_s);
int32_t yfoc_set_velocity(yfoc_handle, int32_t node, double rad_s);
int32_t yfoc_motion_setup(yfoc_handle, int32_t node, double kp, double kd);
int32_t yfoc_set_motion(yfoc_handle, int32_t node, double rad, double rad_s, double torque_nm);
int32_t yfoc_disable(yfoc_handle, int32_t node);
int32_t yfoc_run_group(yfoc_handle, const yfoc_target *, size_t count, double seconds,
                       yfoc_cycle_callback, void *user);
int32_t yfoc_cancel_group(yfoc_handle);

#ifdef __cplusplus
}
#endif
#endif
