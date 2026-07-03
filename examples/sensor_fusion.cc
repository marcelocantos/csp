// sensor_fusion.cc — Multi-sensor fusion dashboard with anomaly detection
//
// How to run:
//   make examples && ./build/examples/sensor_fusion
//
// Expected output: ~50 dashboard lines over ~10 s, with TEMP ANOMALY alerts
// appearing around t=5-7 s when the temperature spike fires.
//
// Features demonstrated:
//   - combine_latest: fuses temperature (10 Hz), pressure (1 Hz), and
//     vibration (100 Hz) streams into a unified SensorReading on every update.
//   - quantize: accumulates raw vibration amplitude from the 100 Hz sensor
//     and emits a batch every time 5.0 units of absolute amplitude accumulate,
//     converting the high-rate stream into a lower-rate energy-quantum stream
//     before it enters the fusion stage.
//   - sample + tick: throttles the fused stream to 5 Hz for dashboard display.
//   - window: maintains a sliding window of 10 readings to compute rolling
//     mean and stddev for per-field anomaly detection (threshold: 3 sigma).
//
// Architecture:
//
//   temp_imp  (100ms) ──────────────────────────────────────────────────────┐
//   pres_imp  (1000ms) ─────────────────────────────────────────────────────┼──► combine_latest ──► sample(tick(200ms)) ──► window(10) ──► dashboard
//   vib_imp   (10ms)  ──► quantize(5.0) ─────────────────────────────────────┘
//
// Sensor specs:
//   Temperature : 10 Hz, base=22°C, amplitude=0.5°C, spike=+12°C at t=5-7s
//   Pressure    :  1 Hz, base=1013 hPa, amplitude=0.3 hPa, no spike
//   Vibration   : 100 Hz, base=0.1, amplitude=0.05, spike=+2.0 at t=5-7s;
//                 quantize emits every 5.0 units of cumulative |amplitude|

#include "csp.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

// --- Types ---

struct SensorReading {
    double temperature;   // °C
    double pressure;      // hPa
    double vibration;     // energy quanta (quantize output)
};

struct FieldStats {
    double mean   = 0.0;
    double stddev = 0.0;
};

// --- Helpers ---

static FieldStats field_stats(const std::vector<SensorReading>& win,
                              double SensorReading::* field) {
    if (win.empty()) return {};
    double sum = 0.0;
    for (const auto& r : win) sum += r.*field;
    double mean = sum / static_cast<double>(win.size());
    double var  = 0.0;
    for (const auto& r : win) {
        double d = r.*field - mean;
        var += d * d;
    }
    double stddev = win.size() > 1
        ? std::sqrt(var / static_cast<double>(win.size() - 1))
        : 0.0;
    return {mean, stddev};
}

static bool is_anomaly(double val, const FieldStats& s, size_t n) {
    return n >= 3 && s.stddev > 1e-6 && std::abs(val - s.mean) > 3.0 * s.stddev;
}

// --- Sensor producers ---

// Temperature: 10 Hz. Sinusoidal drift; spike at steps [spike_lo, spike_hi).
static reader<double> make_temp_sensor(int count) {
    return spawn_producer<double>([count](writer<double> w) {
        for (int i = 0; i < count; ++i) {
            double v = 22.0 + 0.5 * std::sin(0.3 * i);
            if (i >= 50 && i < 70) v += 12.0;   // spike at ~5-7 s
            if (!(w << v)) break;
            sleep(100ms);
        }
    });
}

// Pressure: 1 Hz. Slow sinusoidal drift.
static reader<double> make_pres_sensor(int count) {
    return spawn_producer<double>([count](writer<double> w) {
        for (int i = 0; i < count; ++i) {
            double v = 1013.0 + 0.3 * std::cos(0.15 * i);
            if (!(w << v)) break;
            sleep(1000ms);
        }
    });
}

// Vibration: 100 Hz. High-rate absolute amplitudes.
// Feeds quantize, which emits every 5.0 accumulated units.
static reader<double> make_vib_sensor(int count) {
    return spawn_producer<double>([count](writer<double> w) {
        for (int i = 0; i < count; ++i) {
            double v = std::abs(0.1 + 0.05 * std::sin(0.8 * i));
            if (i >= 500 && i < 700) v += 2.0;  // spike at ~5-7 s
            if (!(w << v)) break;
            sleep(10ms);
        }
    });
}

// --- Main ---

int main() {
    spawn([] {
        printf("Sensor Fusion Dashboard\n");
        printf("=======================\n");
        printf("Sensors: temp(10 Hz)  pressure(1 Hz)  vibration(100 Hz → quantize)\n");
        printf("Display: 5 Hz via sample(tick(200ms))  Anomaly: 3σ over window(10)\n\n");

        // Vibration raw stream — quantize accumulates |amplitude| units and
        // emits every time 5.0 units have arrived.
        auto vib_raw  = make_vib_sensor(1000);
        auto vib_q    = spawn_quantize(std::move(vib_raw), 5.0);

        // Fuse all three streams.
        auto fused = combine_latest<double, double, double>(
            make_temp_sensor(100),
            make_pres_sensor(11),
            std::move(vib_q),
            [](double& t, double& p, double& v) {
                return SensorReading{t, p, v};
            });

        // Throttle to 5 Hz for display.
        auto sampled = sample(std::move(fused).spawn(), tick(200ms));

        // Sliding window of 10 readings for anomaly detection.
        reader<std::vector<SensorReading>> windowed =
            (std::move(sampled) | window<SensorReading>(10)).spawn();

        int n = 0;
        std::vector<SensorReading> win;

        while (windowed >> win) {
            const SensorReading& cur = win.back();

            // Baseline: all entries except the most-recent one.
            std::vector<SensorReading> baseline(win.begin(),
                                                win.begin() + static_cast<long>(win.size()) - 1);
            auto ts = field_stats(baseline, &SensorReading::temperature);
            auto ps = field_stats(baseline, &SensorReading::pressure);
            auto vs = field_stats(baseline, &SensorReading::vibration);

            printf("[%3d] T=%6.2f°C  P=%8.3f hPa  V=%5.2f qu",
                   ++n, cur.temperature, cur.pressure, cur.vibration);

            bool alert = false;
            if (is_anomaly(cur.temperature, ts, baseline.size())) {
                printf("  *** TEMP ANOMALY (μ=%.2f σ=%.2f)", ts.mean, ts.stddev);
                alert = true;
            }
            if (is_anomaly(cur.pressure, ps, baseline.size())) {
                printf("  *** PRES ANOMALY (μ=%.3f σ=%.3f)", ps.mean, ps.stddev);
                alert = true;
            }
            if (is_anomaly(cur.vibration, vs, baseline.size())) {
                printf("  *** VIB ANOMALY (μ=%.2f σ=%.2f)", vs.mean, vs.stddev);
                alert = true;
            }
            if (!alert && baseline.size() >= 3) printf("  [ok]");
            printf("\n");

            if (n >= 50) break;
        }

        printf("\nDone (%d dashboard updates).\n", n);
    });

    schedule();
}
