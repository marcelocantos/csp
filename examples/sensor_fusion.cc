// sensor_fusion.cc — Multi-sensor fusion dashboard with anomaly detection
//
// Demonstrates: combine_latest (multi-stream fusion), sample (rate throttling),
// window (sliding window over fused readings), operator| composition, and
// concurrent sensor simulation imps.
//
// Architecture:
//   - Three sensor imps producing at different rates (temp 200ms, pressure
//     300ms, humidity 500ms).  Temperature has sinusoidal drift and a
//     deliberate spike on steps 15-20 (~3-4s) to trigger the anomaly detector.
//   - combine_latest fuses all three streams into SensorReading structs,
//     emitting on every update after all three have produced at least once.
//   - sample throttles the fused stream to 1 Hz for dashboard display.
//   - window(5) computes a sliding mean + stddev over the sampled readings.
//   - Anomaly alerts fire when any field exceeds 3 standard deviations from
//     its window mean.

#include "csp.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace csp;
using namespace csp::part;

// --- Types ---

struct SensorReading {
    double temperature;   // °C
    double pressure;      // hPa
    double humidity;      // %RH
};

struct WindowStats {
    double mean;
    double stddev;
};

// --- Helpers ---

static WindowStats stats(const std::vector<double>& vals) {
    double sum = 0;
    for (double v : vals) sum += v;
    double mean = sum / static_cast<double>(vals.size());
    double var = 0;
    for (double v : vals) var += (v - mean) * (v - mean);
    double stddev = vals.size() > 1 ? std::sqrt(var / static_cast<double>(vals.size() - 1)) : 0.0;
    return {mean, stddev};
}

// Returns true if val is more than 3 standard deviations from the baseline.
// Requires at least 3 baseline readings for a meaningful estimate.
static bool is_anomaly(double val, const WindowStats& s, size_t baseline_count) {
    return baseline_count >= 3 && s.stddev > 0.01 &&
           std::abs(val - s.mean) > 3.0 * s.stddev;
}

// Simulate a sensor: emits `count` readings at `interval`, sinusoidal around
// `base` with `amp` amplitude.  Injects a spike of `spike` on steps
// [spike_from, spike_to) to guarantee at least one sample catches it.
static reader<double> make_sensor(
    double base, double amp, double spike,
    int spike_from, int spike_to, int count,
    std::chrono::milliseconds interval)
{
    chan<double> ch;
    spawn([w = std::move(ch.w), base, amp, spike,
           spike_from, spike_to, count, interval] {
        for (int i = 0; i < count; ++i) {
            double v = base + amp * std::sin(0.5 * i);
            if (i >= spike_from && i < spike_to) v += spike;
            w << v;
            sleep(interval);
        }
    });
    return std::move(ch.r);
}

int main() {
    spawn([] {
        using namespace std::chrono_literals;

        printf("Sensor Fusion Dashboard\n");
        printf("=======================\n\n");

        // Three sensors at different rates.  Each has gentle sinusoidal
        // drift.  Temperature spikes +15°C on steps 25-30 (~5.0-6.0s) to
        // trigger the anomaly detector after the window fills with baselines.
        auto temp_r  = make_sensor(22.0, 0.8,  15.0, 25, 31, 120, 200ms);
        auto pres_r  = make_sensor(1013.0, 1.5, 0.0,  0,  0, 80,  300ms);
        auto hum_r   = make_sensor(55.0,   1.0, 0.0,  0,  0, 50,  500ms);

        // Fuse all three streams into SensorReading.
        auto fused = combine_latest<double, double, double>(
            std::move(temp_r), std::move(pres_r), std::move(hum_r),
            [](double& t, double& p, double& h) {
                return SensorReading{t, p, h};
            });

        // Throttle to 1 Hz for dashboard display.
        auto sampled = sample(std::move(fused).spawn(), tick(1s));

        // Sliding window (5 readings) for anomaly detection; spawn to reader.
        reader<std::vector<SensorReading>> windowed =
            (std::move(sampled) | window<SensorReading>(5)).spawn();

        // Dashboard: print each windowed batch with anomaly check.
        // Stats are computed over all-but-the-last window entries so the
        // current reading is compared against the established baseline.
        int n = 0;
        std::vector<SensorReading> win;
        while (windowed >> win) {
            const auto& r = win.back();

            // Compute baseline stats from prior entries (excluding latest).
            std::vector<double> temps, precs, hums;
            for (size_t i = 0; i + 1 < win.size(); ++i) {
                temps.push_back(win[i].temperature);
                precs.push_back(win[i].pressure);
                hums.push_back(win[i].humidity);
            }
            auto ts = stats(temps);
            auto ps = stats(precs);
            auto hs = stats(hums);

            printf("[%2d] T=%6.2f°C  P=%7.2f hPa  H=%5.2f%%RH",
                   ++n, r.temperature, r.pressure, r.humidity);

            bool alert = false;
            if (is_anomaly(r.temperature, ts, temps.size())) {
                printf("  *** TEMP ANOMALY (mean=%.2f, σ=%.2f)", ts.mean, ts.stddev);
                alert = true;
            }
            if (is_anomaly(r.pressure, ps, precs.size())) {
                printf("  *** PRES ANOMALY (mean=%.2f, σ=%.2f)", ps.mean, ps.stddev);
                alert = true;
            }
            if (is_anomaly(r.humidity, hs, hums.size())) {
                printf("  *** HUMID ANOMALY (mean=%.2f, σ=%.2f)", hs.mean, hs.stddev);
                alert = true;
            }
            if (!alert && temps.size() >= 3) {
                printf("  [ok]");
            }
            printf("\n");

            if (n >= 20) break;
        }

        printf("\nDone (%d dashboard updates).\n", n);
    });

    schedule();
}
