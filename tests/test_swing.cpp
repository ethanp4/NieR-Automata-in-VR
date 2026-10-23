// Motion controls: a swing of a hand fires once (pure::SwingDetector).
#include <cstdio>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
    if (!c) {
        std::printf("FAIL %s\n", w);
        fails++;
    }
}
// Feeds speeds at 90 Hz (11 ms apart) starting at t, returns how many times it fired.
static int run(pure::SwingDetector& d, const float* speeds, int n, uint32_t& t, float fire = 2.0f) {
    int fires = 0;
    for (int i = 0; i < n; i++, t += 11) fires += d.update(speeds[i], t, fire);
    return fires;
}
int main() {
    {  // one swing: speed rises past 2 m/s and falls again -> one attack
        pure::SwingDetector d;
        uint32_t t = 1000;
        const float s[] = {0.1f, 0.5f, 1.2f, 2.1f, 3.0f, 3.4f, 2.8f, 1.9f, 1.1f, 0.4f, 0.1f};
        check(run(d, s, 11, t) == 1, "one swing fires once");
    }
    {  // slow movement never fires
        pure::SwingDetector d;
        uint32_t t = 1000;
        const float s[] = {0.3f, 0.8f, 1.5f, 1.9f, 1.7f, 1.0f, 0.5f};
        check(run(d, s, 7, t) == 0, "below the threshold: nothing");
    }
    {  // jitter around the threshold without slowing down: still one attack
        pure::SwingDetector d;
        uint32_t t = 1000;
        const float s[] = {2.1f, 1.9f, 2.05f, 1.95f, 2.2f, 1.8f, 2.1f, 1.6f, 2.0f};
        check(run(d, s, 9, t) == 1, "jitter at the threshold fires once");
    }
    {  // two separate swings -> two attacks
        pure::SwingDetector d;
        uint32_t t = 1000;
        const float s[] = {0.2f, 2.5f, 3.0f, 1.5f, 0.6f, 0.2f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                           0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 2.6f, 3.1f, 1.0f, 0.2f};
        check(run(d, s, 29, t) == 2, "two swings fire twice");
    }
    {  // a quick back-and-forth inside the cooldown counts once
        pure::SwingDetector d;
        uint32_t t = 1000;
        const float s[] = {2.5f, 0.5f, 2.5f, 0.5f, 2.5f};  // 44 ms: shorter than the 250 ms cooldown
        check(run(d, s, 5, t) == 1, "back-and-forth within the cooldown fires once");
    }
    {  // a different threshold
        pure::SwingDetector d;
        uint32_t t = 1000;
        const float s[] = {0.5f, 1.2f, 0.4f};
        check(run(d, s, 3, t, 1.0f) == 1, "threshold 1.0 m/s");
    }
    {  // direction: upward swings are recognised, also in a flipped (+Y down) space
        using pure::mostly_up;
        check(mostly_up({0, 3, 0}, 1), "straight up");
        check(mostly_up({1, 2, 0}, 1), "steeply up (63 deg)");
        check(!mostly_up({2, 1, 0}, 1), "shallow up (27 deg) still counts as a swing");
        check(!mostly_up({0, -3, 0}, 1), "straight down");
        check(!mostly_up({3, 0, 1}, 1), "sideways");
        check(!mostly_up({0, 0, 0}, 1), "not moving");
        check(mostly_up({0, -3, 0}, -1), "flipped space: -Y is up");
        check(!mostly_up({0, 3, 0}, -1), "flipped space: +Y is down");
    }
    {  // raise then swing down: the raise is ignored, the downswing attacks once
        pure::SwingDetector d;
        uint32_t t = 1000;
        const pure::V3 v[] = {{0, 1, 0}, {0, 2.5f, 0}, {0, 3, 0.2f}, {0, 1, 0}, {0, 0.2f, 0}, {0, -1, 0.5f},
                              {0, -2.5f, 1}, {0, -3.5f, 1}, {0, -2, 0.5f}, {0, -0.5f, 0}};
        int fires = 0;
        for (const pure::V3& s : v) {
            float speed = pure::fsqrt(s.x * s.x + s.y * s.y + s.z * s.z);
            fires += d.update(pure::mostly_up(s, 1) ? 0 : speed, t, 2.0f);
            t += 11;
        }
        check(fires == 1, "raise ignored, downswing fires once");
    }
    {  // headset zone: hand at the head, then lowered quickly -> no attack; a normal swing later -> attack
        pure::SwingDetector d;
        pure::HeadZone z;
        uint32_t t = 1000;
        struct F {
            float dist, speed;
        };
        const F f[] = {{0.5f, 0.5f}, {0.3f, 1.0f}, {0.2f, 0.8f},  {0.15f, 2.5f}, {0.15f, 2.2f}, {0.2f, 0.5f},  // adjusting
                       {0.3f, 2.4f}, {0.4f, 2.8f}, {0.5f, 2.0f},  {0.6f, 0.8f}};                                  // lowering
        int fires = 0;
        for (const F& s : f) {
            fires += d.update(z.update(s.dist, t) ? 0 : s.speed, t, 2.0f);
            t += 11;
        }
        check(fires == 0, "hand at the headset and lowered right after: no attack");
        t += 500;  // later, a normal swing away from the head
        const F g[] = {{0.5f, 1.0f}, {0.55f, 2.6f}, {0.6f, 3.2f}, {0.6f, 1.0f}};
        for (const F& s : g) {
            fires += d.update(z.update(s.dist, t) ? 0 : s.speed, t, 2.0f);
            t += 11;
        }
        check(fires == 1, "normal swing after the headset zone fires");
    }
    if (!fails) std::printf("swing tests passed\n");
    return fails ? 1 : 0;
}
