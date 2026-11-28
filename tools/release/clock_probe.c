/* Guest check for clock selection and bounded sleep arguments. */
#include "libchiffon.h"

#if defined(__x86_64__)
#define CLOCK_GETTIME 228
#define CLOCK_NANOSLEEP 230
#define GETTIMEOFDAY 96
#define TIME 201
#elif defined(__aarch64__)
#define CLOCK_GETTIME 113
#define CLOCK_NANOSLEEP 115
#define GETTIMEOFDAY 169
#endif

#define EINVAL 22
#define EFAULT 14
#define BILLION 1000000000L

static long gettime(long id, long ts[2]) {
    return ksys(CLOCK_GETTIME, id, (long)ts, 0, 0, 0, 0);
}

static int fail(const char *stage) {
    chiffon_puts("FAIL clock-probe ");
    chiffon_puts(stage);
    chiffon_puts("\n");
    return 1;
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    long mono[2], real[2], after[2];
    if (gettime(1, mono) || gettime(0, real) || gettime(1, after)) return fail("clock read");
    if (mono[0] < 0 || mono[1] < 0 || mono[1] >= BILLION ||
        real[1] < 0 || real[1] >= BILLION ||
        real[0] - after[0] < 946684800L)
        return fail("clock epochs");
    if (gettime(-1, after) != -EINVAL) return fail("clock id");

    long coarse[2];
    if (gettime(5, coarse) || coarse[0] < real[0] || coarse[0] > real[0] + 1 ||
        gettime(6, coarse) || coarse[0] < after[0] || coarse[0] > after[0] + 1)
        return fail("coarse clocks");

    long before_real[2], after_real[2], tv[2] = { -1, -1 };
    int tz[2] = { -1, -1 };
    if (gettime(0, before_real) ||
        ksys(GETTIMEOFDAY, (long)tv, (long)tz, 0, 0, 0, 0) ||
        gettime(0, after_real)) return fail("gettimeofday read");
    if (tv[0] < before_real[0] || tv[0] > after_real[0] ||
        tv[1] < 0 || tv[1] >= 1000000 || tz[0] || tz[1])
        return fail("gettimeofday values");
    if (ksys(GETTIMEOFDAY, 0, (long)tz, 0, 0, 0, 0) ||
        ksys(GETTIMEOFDAY, (long)tv, 0, 0, 0, 0, 0) ||
        ksys(GETTIMEOFDAY, 1, 0, 0, 0, 0, 0) != -EFAULT ||
        ksys(GETTIMEOFDAY, 0, 1, 0, 0, 0, 0) != -EFAULT)
        return fail("gettimeofday pointers");
#if defined(__x86_64__)
    long seconds = -1;
    if (gettime(0, before_real)) return fail("time read");
    long result = ksys(TIME, (long)&seconds, 0, 0, 0, 0, 0);
    if (gettime(0, after_real) || result != seconds ||
        seconds < before_real[0] || seconds > after_real[0] ||
        ksys(TIME, 1, 0, 0, 0, 0, 0) != -EFAULT)
        return fail("time values");
    if (ksys(TIME, 0, 0, 0, 0, 0, 0) < seconds)
        return fail("time null pointer");
#endif

    if (gettime(1, after)) return fail("deadline clock read");
    long deadline[2] = { after[0], after[1] + 20000000L };
    if (deadline[1] >= BILLION) { deadline[0]++; deadline[1] -= BILLION; }
    if (ksys(CLOCK_NANOSLEEP, 1, 1, (long)deadline, 0, 0, 0))
        return fail("absolute sleep");
    if (gettime(1, after) || after[0] < deadline[0] ||
        (after[0] == deadline[0] && after[1] < deadline[1]))
        return fail("absolute deadline");

    long invalid[2] = { 0, BILLION };
    if (ksys(CF_nanosleep, (long)invalid, 0, 0, 0, 0, 0) != -EINVAL ||
        ksys(CLOCK_NANOSLEEP, 1, 0, (long)invalid, 0, 0, 0) != -EINVAL)
        return fail("invalid timespec");
    chiffon_puts("PASS clock-probe\n");
    return 0;
}
