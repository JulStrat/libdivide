// Usage: benchmark [OPTIONS]
//
// You can pass the benchmark program one or more of the following
// options: u32, s32, u64, s64 to compare libdivide's speed against
// hardware division. If benchmark is run without any options u64
// is used as default option. benchmark tests a simple function that
// inputs an array of random numerators and a single divisor, and
// returns the sum of their quotients. It tests this using both
// hardware division, and the various division approaches supported
// by libdivide, including vector division.

// Silence MSVC sprintf unsafe warnings
#define _CRT_SECURE_NO_WARNINGS

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libdivide.h"

#if defined(__GNUC__)
#define NOINLINE __attribute__((__noinline__))
#elif defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE
#endif

#if defined(LIBDIVIDE_AVX512)
#define VECTOR_TYPE __m512i
#define SETZERO_SI _mm512_setzero_si512
#define LOAD_SI _mm512_load_si512
#define ADD_EPI64 _mm512_add_epi64
#define ADD_EPI32 _mm512_add_epi32
#elif defined(LIBDIVIDE_AVX2)
#define VECTOR_TYPE __m256i
#define SETZERO_SI _mm256_setzero_si256
#define LOAD_SI _mm256_load_si256
#define ADD_EPI64 _mm256_add_epi64
#define ADD_EPI32 _mm256_add_epi32
#elif defined(LIBDIVIDE_SSE2)
#define VECTOR_TYPE __m128i
#define SETZERO_SI _mm_setzero_si128
#define LOAD_SI _mm_load_si128
#define ADD_EPI64 _mm_add_epi64
#define ADD_EPI32 _mm_add_epi32
#endif

#define NANOSEC_PER_SEC 1000000000ULL
#define NANOSEC_PER_USEC 1000ULL
#define NANOSEC_PER_MILLISEC 1000000ULL
#define SEED \
    { 2147483563, 2147483563 ^ 0x49616E42 }

using namespace libdivide;

#if defined(_WIN32) || defined(WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <mmsystem.h>
#include <windows.h>
#define LIBDIVIDE_WINDOWS
#pragma comment(lib, "winmm")
#endif

#if !defined(LIBDIVIDE_WINDOWS)
#include <sys/time.h>  // for gettimeofday()
#endif

struct random_state {
    uint32_t hi;
    uint32_t lo;
};

volatile uint64_t sGlobalUInt64;
size_t iters = 1 << 19;
size_t genIters = 1 << 16;

static uint32_t my_random(struct random_state *state) {
    state->hi = (state->hi << 16) + (state->hi >> 16);
    state->hi += state->lo;
    state->lo += state->hi;
    return state->hi;
}

#if defined(LIBDIVIDE_WINDOWS)
static LARGE_INTEGER gPerfCounterFreq;
#endif

#if !defined(LIBDIVIDE_WINDOWS)
static uint64_t nanoseconds(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * NANOSEC_PER_SEC + now.tv_usec * NANOSEC_PER_USEC;
}
#endif

template <typename IntT, typename Divisor>
NOINLINE uint64_t sum_quotients(const IntT *vals, Divisor div) {
    IntT sum = 0;
    for (size_t iter = 0; iter < iters; iter += 1) {
        sum += vals[iter] / div;
    }
    return (uint64_t)sum;
}

#ifdef VECTOR_TYPE
template <typename IntT, typename Divisor>
NOINLINE uint64_t sum_quotients_vec(const IntT *vals, Divisor div) {
    size_t count = sizeof(VECTOR_TYPE) / sizeof(IntT);
    VECTOR_TYPE sumX4 = SETZERO_SI();
    for (size_t iter = 0; iter < iters; iter += count) {
        VECTOR_TYPE numers = LOAD_SI((const VECTOR_TYPE *)&vals[iter]);
        numers = numers / div;
        if (sizeof(IntT) == 4) {
            sumX4 = ADD_EPI32(sumX4, numers);
        } else if (sizeof(IntT) == 8) {
            sumX4 = ADD_EPI64(sumX4, numers);
        } else {
            abort();
        }
    }
    const IntT *comps = (const IntT *)&sumX4;
    IntT sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += comps[i];
    }
    return (uint64_t)sum;
}
#endif

// noinline to force compiler to emit this
template <typename IntT>
NOINLINE divider<IntT> generate_1_divisor(IntT d) {
    return divider<IntT>(d);
}

template <typename IntT>
NOINLINE void generate_divisor(IntT denom) {
    for (size_t iter = 0; iter < genIters; iter++) {
        (void)generate_1_divisor(denom);
    }
}

struct time_result {
    uint64_t time;  // in nanoseconds
    uint64_t result;
};

enum which_function_t {
    func_hardware,
    func_scalar_branchfull,
    func_scalar_branchfree,
    func_vec_branchfull,
    func_vec_branchfree,
    func_generate
};

template <which_function_t Which, typename IntT>
NOINLINE static struct time_result time_function(const IntT *vals, IntT denom) {
    struct time_result tresult;
    uint64_t result;
    uint64_t diff_nanos;
    divider<IntT, BRANCHFULL> div_bfull(denom);
    divider<IntT, BRANCHFREE> div_bfree(denom != 1 ? denom : 2);
#if defined(LIBDIVIDE_WINDOWS)
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
#else
    uint64_t start, end;
    start = nanoseconds();
#endif
    switch (Which) {
        case func_hardware:
            result = sum_quotients(vals, denom);
            break;
        case func_scalar_branchfull:
            result = sum_quotients(vals, div_bfull);
            break;
        case func_scalar_branchfree:
            result = sum_quotients(vals, div_bfree);
            break;
#ifdef VECTOR_TYPE
        case func_vec_branchfull:
            result = sum_quotients_vec(vals, div_bfull);
            break;
        case func_vec_branchfree:
            result = sum_quotients_vec(vals, div_bfree);
            break;
#endif
        case func_generate:
            generate_divisor(denom);
            result = 0;
            break;
        default:
            abort();
    }
#if defined(LIBDIVIDE_WINDOWS)
    QueryPerformanceCounter(&end);
    diff_nanos = ((end.QuadPart - start.QuadPart) * 1000000000ULL) / gPerfCounterFreq.QuadPart;
#else
    end = nanoseconds();
    diff_nanos = end - start;
#endif
    tresult.time = diff_nanos;
    sGlobalUInt64 += result;
    tresult.result = result;
    return tresult;
}

struct TestResult {
    double hardware_time;
    double base_time;
    double branchfree_time;
    double vector_time;
    double vector_branchfree_time;
    double gen_time;
    int algo;
};

static uint64_t find_min(const uint64_t *vals, size_t cnt) {
    uint64_t result = vals[0];
    size_t i;
    for (i = 1; i < cnt; i++) {
        if (vals[i] < result) result = vals[i];
    }
    return result;
}

template <typename IntT>
NOINLINE struct TestResult test_one(const IntT *vals, IntT denom) {
#define TEST_COUNT 30
    struct TestResult result;
    memset(&result, 0, sizeof result);
    const bool testBranchfree = (denom != 1);

#define CHECK(actual, expected)                                                               \
    do {                                                                                      \
        if ((actual) != (expected)) printf("Failure on line %lu\n", (unsigned long)__LINE__); \
    } while (0)

    uint64_t my_times[TEST_COUNT], my_times_branchfree[TEST_COUNT], my_times_vector[TEST_COUNT],
        my_times_vector_branchfree[TEST_COUNT], his_times[TEST_COUNT], gen_times[TEST_COUNT];
    struct time_result tresult;
    for (size_t iter = 0; iter < TEST_COUNT; iter++) {
        tresult = time_function<func_hardware>(vals, denom);
        his_times[iter] = tresult.time;
        const uint64_t expected = tresult.result;
        tresult = time_function<func_scalar_branchfull>(vals, denom);
        my_times[iter] = tresult.time;
        CHECK(tresult.result, expected);
        if (testBranchfree) {
            tresult = time_function<func_scalar_branchfree>(vals, denom);
            my_times_branchfree[iter] = tresult.time;
            CHECK(tresult.result, expected);
        }
#ifdef VECTOR_TYPE
        tresult = time_function<func_vec_branchfull>(vals, denom);
        my_times_vector[iter] = tresult.time;
        CHECK(tresult.result, expected);
        if (testBranchfree) {
            tresult = time_function<func_vec_branchfree>(vals, denom);
            my_times_vector_branchfree[iter] = tresult.time;
            CHECK(tresult.result, expected);
        }
#else
        my_times_vector[iter] = 0;
        my_times_vector_branchfree[iter] = 0;
#endif
        tresult = time_function<func_generate>(vals, denom);
        gen_times[iter] = tresult.time;
    }

    result.gen_time = find_min(gen_times, TEST_COUNT) / (double)genIters;
    result.base_time = find_min(my_times, TEST_COUNT) / (double)iters;
    result.branchfree_time =
        testBranchfree ? find_min(my_times_branchfree, TEST_COUNT) / (double)iters : -1;
    result.vector_time = find_min(my_times_vector, TEST_COUNT) / (double)iters;
    result.vector_branchfree_time =
        find_min(my_times_vector_branchfree, TEST_COUNT) / (double)iters;
    result.hardware_time = find_min(his_times, TEST_COUNT) / (double)iters;
    return result;
#undef TEST_COUNT
}

int libdivide_u32_get_algorithm(uint32_t d) {
    const struct libdivide_u32_t denom = libdivide_u32_gen(d);
    uint8_t more = denom.more;
    if (!denom.magic)
        return 0;
    else if (!(more & LIBDIVIDE_ADD_MARKER))
        return 1;
    else
        return 2;
}

NOINLINE struct TestResult test_one_u32(uint32_t d, const uint32_t *data) {
    struct TestResult result = test_one(data, d);
    result.algo = libdivide_u32_get_algorithm(d);
    return result;
}

int libdivide_s32_get_algorithm(int32_t d) {
    const struct libdivide_s32_t denom = libdivide_s32_gen(d);
    uint8_t more = denom.more;
    if (!denom.magic)
        return 0;
    else if (!(more & LIBDIVIDE_ADD_MARKER))
        return 1;
    else
        return 2;
}

NOINLINE struct TestResult test_one_s32(int32_t d, const int32_t *data) {
    struct TestResult result = test_one(data, d);
    result.algo = libdivide_s32_get_algorithm(d);
    return result;
}

int libdivide_u64_get_algorithm(uint64_t d) {
    const struct libdivide_u64_t denom = libdivide_u64_gen(d);
    uint8_t more = denom.more;
    if (!denom.magic)
        return 0;
    else if (!(more & LIBDIVIDE_ADD_MARKER))
        return 1;
    else
        return 2;
}

NOINLINE struct TestResult test_one_u64(uint64_t d, const uint64_t *data) {
    struct TestResult result = test_one(data, d);
    result.algo = libdivide_u64_get_algorithm(d);
    return result;
}

int libdivide_s64_get_algorithm(int64_t d) {
    const struct libdivide_s64_t denom = libdivide_s64_gen(d);
    uint8_t more = denom.more;
    if (!denom.magic)
        return 0;
    else if (!(more & LIBDIVIDE_ADD_MARKER))
        return 1;
    else
        return 2;
}

NOINLINE struct TestResult test_one_s64(int64_t d, const int64_t *data) {
    struct TestResult result = test_one(data, d);
    result.algo = libdivide_s64_get_algorithm(d);
    return result;
}

static void report_header(void) {
    printf("%6s%9s%8s%8s%8s%8s%8s%7s\n", "#", "system", "scalar", "scl_bf", "vector", "vec_bf",
        "gener", "algo");
}

static void report_result(const char *input, struct TestResult result) {
    printf("%6s%8.3f%8.3f%8.3f%8.3f%8.3f%9.3f%4d\n", input, result.hardware_time, result.base_time,
        result.branchfree_time, result.vector_time, result.vector_branchfree_time, result.gen_time,
        result.algo);
}

static void test_many_u32(const uint32_t *data) {
    printf("\n%50s", "=== libdivide u32 benchmark ===\n\n");
    report_header();
    uint32_t d;
    for (d = 1; d > 0; d++) {
        struct TestResult result = test_one_u32(d, data);
        char input_buff[32];
        sprintf(input_buff, "%u", d);
        report_result(input_buff, result);
    }
}

static void test_many_s32(const int32_t *data) {
    printf("\n%50s", "=== libdivide s32 benchmark ===\n\n");
    report_header();
    int32_t d;
    for (d = 1; d != 0;) {
        struct TestResult result = test_one_s32(d, data);
        char input_buff[32];
        sprintf(input_buff, "%d", d);
        report_result(input_buff, result);

        d = -d;
        if (d > 0) d++;
    }
}

static void test_many_u64(const uint64_t *data) {
    printf("\n%50s", "=== libdivide u64 benchmark ===\n\n");
    report_header();
    uint64_t d;
    for (d = 1; d > 0; d++) {
        struct TestResult result = test_one_u64(d, data);
        char input_buff[32];
        sprintf(input_buff, "%" PRIu64, d);
        report_result(input_buff, result);
    }
}

static void test_many_s64(const int64_t *data) {
    printf("\n%50s", "=== libdivide s64 benchmark ===\n\n");
    report_header();
    int64_t d;
    for (d = 1; d != 0;) {
        struct TestResult result = test_one_s64(d, data);
        char input_buff[32];
        sprintf(input_buff, "%" PRId64, d);
        report_result(input_buff, result);

        d = -d;
        if (d > 0) d++;
    }
}

static const uint32_t *random_data(unsigned sizeOfType) {
#if defined(LIBDIVIDE_WINDOWS)
    /* Align memory to 64 byte boundary for AVX512 */
    uint32_t *data = (uint32_t *)_aligned_malloc(iters * sizeOfType, 64);
#else
    /* Align memory to 64 byte boundary for AVX512 */
    void *ptr = NULL;
    int failed = posix_memalign(&ptr, 64, iters * sizeOfType);
    if (failed) {
        printf("Failed to align memory!\n");
        exit(1);
    }
    uint32_t *data = (uint32_t *)ptr;
#endif
    size_t size = (iters * sizeOfType) / sizeof(*data);
    struct random_state state = SEED;
    for (size_t i = 0; i < size; i++) {
        data[i] = my_random(&state);
    }
    return data;
}

int main(int argc, char *argv[]) {
    // Disable printf buffering.
    // This is mainly required for Windows.
    setbuf(stdout, NULL);

#if defined(LIBDIVIDE_WINDOWS)
    QueryPerformanceFrequency(&gPerfCounterFreq);
#endif
    int u32 = 0;
    int s32 = 0;
    int u64 = 0;
    int s64 = 0;

    if (argc == 1) {
        // By default test only u64
        u64 = 1;
    } else {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "u32"))
                u32 = 1;
            else if (!strcmp(argv[i], "u64"))
                u64 = 1;
            else if (!strcmp(argv[i], "s32"))
                s32 = 1;
            else if (!strcmp(argv[i], "s64"))
                s64 = 1;
            else {
                printf(
                    "Usage: benchmark [OPTIONS]\n"
                    "\n"
                    "You can pass the benchmark program one or more of the following\n"
                    "options: u32, s32, u64, s64 to compare libdivide's speed against\n"
                    "hardware division. If benchmark is run without any options u64\n"
                    "is used as default option. benchmark tests a simple function that\n"
                    "inputs an array of random numerators and a single divisor, and\n"
                    "returns the sum of their quotients. It tests this using both\n"
                    "hardware division, and the various division approaches supported\n"
                    "by libdivide, including vector division.\n");
                exit(1);
            }
        }
    }

    // Make sure that the number of iterations is not
    // known at compile time to prevent the compiler
    // from magically calculating results at compile
    // time and hence falsifying the benchmark.
    srand((unsigned)time(NULL));
    iters += (rand() % 3) * (1 << 10);
    genIters += (rand() % 3) * (1 << 10);

    const uint32_t *data = random_data(sizeof(uint32_t));
    if (u32) test_many_u32(data);
    if (s32) test_many_s32((const int32_t *)data);

#if defined(LIBDIVIDE_WINDOWS)
    _aligned_free((void *)data);
#else
    free((void *)data);
#endif

    data = random_data(sizeof(uint64_t));
    if (u64) test_many_u64((const uint64_t *)data);
    if (s64) test_many_s64((const int64_t *)data);

#if defined(LIBDIVIDE_WINDOWS)
    _aligned_free((void *)data);
#else
    free((void *)data);
#endif

    return 0;
}
