#pragma once
#include "yfoc_motor.h"
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

namespace yfoc {
enum class Mode : int32_t { motion, velocity, position };
enum class InnerLoop : int32_t { voltage, current };
struct Axis {
    int32_t node;
    double target;
    std::optional<double> second = std::nullopt;
};
using CycleCallback = std::function<bool(const yfoc_sample *, size_t)>;

class Bus {
public:
    Bus(const char *interface_name, const char *channel, uint32_t bitrate = 1000000) {
        if (yfoc_abi_version() != 1) throw std::runtime_error("YFOC SDK ABI mismatch");
        check(yfoc_open(interface_name, channel, bitrate, &handle_));
    }
    Bus(const Bus &) = delete;
    Bus &operator=(const Bus &) = delete;
    ~Bus() { if (handle_) yfoc_close(handle_); }
    void close() {
        if (handle_) { check(yfoc_close(handle_)); handle_ = 0; }
    }
    yfoc_device_info info(int32_t node) const { return read(node, yfoc_info); }
    yfoc_status_data status(int32_t node) const { return read(node, yfoc_status); }
    yfoc_feedback_data feedback(int32_t node) const { return read(node, yfoc_feedback); }
    yfoc_current_data current(int32_t node) const { return read(node, yfoc_current); }
    yfoc_config_data current_config(int32_t node) const { return read(node, yfoc_current_config); }
    std::optional<yfoc_feedback_data> wait_feedback(int32_t node, uint32_t timeout_ms) const {
        yfoc_feedback_data result{};
        auto rc = yfoc_wait_feedback(handle_, node, timeout_ms, &result);
        if (rc == 1) return std::nullopt;
        check(rc);
        return result;
    }
    void set_mode(int32_t node, Mode mode) { check(yfoc_set_mode(handle_, node, static_cast<int32_t>(mode))); }
    void set_inner(int32_t node, InnerLoop inner) { check(yfoc_set_inner(handle_, node, static_cast<int32_t>(inner))); }
    void set_position(int32_t node, double rad, double max_rad_s) { check(yfoc_set_position(handle_, node, rad, max_rad_s)); }
    void set_velocity(int32_t node, double rad_s) { check(yfoc_set_velocity(handle_, node, rad_s)); }
    void motion_setup(int32_t node, double kp, double kd) { check(yfoc_motion_setup(handle_, node, kp, kd)); }
    void set_motion(int32_t node, double rad, double rad_s, double torque_nm) { check(yfoc_set_motion(handle_, node, rad, rad_s, torque_nm)); }
    void disable(int32_t node) { check(yfoc_disable(handle_, node)); }
    void cancel_group() { check(yfoc_cancel_group(handle_)); }

    // Callback runs on the calling thread. Return false to stop the group.
    void run_group(const std::vector<Axis>& axes, double seconds, CycleCallback on_cycle = {}) {
        std::vector<yfoc_target> targets;
        targets.reserve(axes.size());
        for (const auto& a : axes)
            targets.push_back({a.node, a.second.has_value(), a.target, a.second.value_or(0.0)});
        struct Context { CycleCallback& fn; std::exception_ptr error; } ctx{on_cycle, {}};
        yfoc_cycle_callback callback = [](void *opaque, const yfoc_sample *samples, size_t n) noexcept -> int32_t {
            auto& state = *static_cast<Context *>(opaque);
            try { return state.fn(samples, n) ? 0 : 1; }
            catch (...) { state.error = std::current_exception(); return 1; }
        };
        auto rc = yfoc_run_group(handle_, targets.data(), targets.size(), seconds,
                                 on_cycle ? callback : nullptr, &ctx);
        if (ctx.error) std::rethrow_exception(ctx.error);
        check(rc);
    }
private:
    yfoc_handle handle_{};
    static void check(int32_t rc) { if (rc < 0) throw std::runtime_error(yfoc_last_error()); }
    template<class T>
    T read(int32_t node, int32_t (*fn)(yfoc_handle, int32_t, T *)) const {
        T result{};
        check(fn(handle_, node, &result));
        return result;
    }
};
}
