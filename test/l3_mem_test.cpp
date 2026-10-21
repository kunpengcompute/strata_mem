#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "l3_mem_workload.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

namespace {
struct Options {
    unsigned threads = 14;
    unsigned rounds = 1;
    unsigned seconds = 90;
    unsigned warmup = 10;
    std::array<int, 14> cpus{};
    const char* cpu_list = nullptr;
};

void usage(const char* program) {
    std::printf(
        "Usage: %s [--threads 7|14] [--rounds N] [--seconds N] "
        "[--warmup N] [--cpus c0,c1,...]\n"
        "Fixed: 32 x 1GiB pages, 70 x 1MiB regions, seed35-fixed-v1 layout.\n"
        "Defaults: threads=14 rounds=1 seconds=90 warmup=10 CPUs=0..13.\n"
        "CPU list must have exactly one distinct CPU per thread (no ranges).\n"
        "Region selection is fixed; per-thread random access order is preserved.\n",
        program);
}

bool number(const char* text, unsigned min, unsigned max, unsigned& value) {
    if (!*text) return false;
    for (auto p = text; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    errno = 0;
    char* end;
    const auto n = std::strtoul(text, &end, 10);
    if (errno || *end || n < min || n > max) return false;
    value = static_cast<unsigned>(n);
    return true;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return false;
        const auto* key = argv[i];
        const auto* val = argv[i + 1];
        if (!std::strcmp(key, "--threads")) {
            if (!number(val, 7, 14, o.threads) ||
                (o.threads != 7 && o.threads != 14)) return false;
        } else if (!std::strcmp(key, "--rounds")) {
            if (!number(val, 1, 100000, o.rounds)) return false;
        } else if (!std::strcmp(key, "--seconds")) {
            if (!number(val, 1, 86400, o.seconds)) return false;
        } else if (!std::strcmp(key, "--warmup")) {
            if (!number(val, 0, 86400, o.warmup)) return false;
        } else if (!std::strcmp(key, "--cpus")) {
            o.cpu_list = val;
        } else return false;
    }
    for (unsigned i = 0; i < o.threads; ++i) o.cpus[i] = static_cast<int>(i);
    if (!o.cpu_list) return true;
    const char* p = o.cpu_list;
    for (unsigned i = 0; i < o.threads; ++i) {
        if (*p < '0' || *p > '9') return false;
        errno = 0;
        char* end;
        const auto n = std::strtoul(p, &end, 10);
        if (errno || n > 1048575) return false;
        o.cpus[i] = static_cast<int>(n);
        for (unsigned j = 0; j < i; ++j)
            if (o.cpus[j] == o.cpus[i]) return false;
        if (i + 1 == o.threads) return *end == '\0';
        if (*end != ',') return false;
        p = end + 1;
    }
    return false;
}

#ifdef __linux__
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

struct Gate {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    unsigned ready = 0;
    bool go = false;
    std::atomic<bool> stop{false};
};
struct ThreadArg {
    Gate* gate = nullptr;
    const l3mem::L3Buffer* buffer = nullptr;
    unsigned tid = 0;
    int cpu = 0;
    std::size_t begin = 0;
    std::size_t count = 0;
    int error = 0;
    std::uint64_t blocks = 0;
};

void* worker(void* arg) {
    auto& a = *static_cast<ThreadArg*>(arg);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(a.cpu, &cpuset);
    a.error = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    auto& gate = *a.gate;
    pthread_mutex_lock(&gate.mutex);
    ++gate.ready;
    pthread_cond_broadcast(&gate.condition);
    while (!gate.go) pthread_cond_wait(&gate.condition, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
    if (a.error) return nullptr;
    std::uint64_t state = UINT64_C(0x123456789abcdef0) + a.tid;
    std::uint64_t blocks = 0;
    while (!gate.stop.load(std::memory_order_relaxed)) {
        auto index = a.begin + workload::bounded_random(state, a.count);
        workload::read_region(a.buffer->regions[index]);
        ++blocks;
    }
    a.blocks = blocks;
    return nullptr;
}

bool valid_cpus(const Options& o) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
        std::perror("sched_getaffinity");
        return false;
    }
    for (unsigned i = 0; i < o.threads; ++i) {
        if (o.cpus[i] >= CPU_SETSIZE || !CPU_ISSET(o.cpus[i], &allowed)) {
            std::fprintf(stderr, "CPU %d is unavailable in current affinity; "
                         "set --cpus explicitly\n", o.cpus[i]);
            return false;
        }
    }
    return true;
}

bool run_stage(const l3mem::L3Buffer& buffer, const Options& o,
               unsigned round, const char* stage, unsigned seconds) {
    Gate gate;
    std::array<pthread_t, 14> threads{};
    std::array<ThreadArg, 14> args{};
    unsigned created = 0;
    bool ok = true;
    for (unsigned i = 0; i < o.threads; ++i) {
        const auto count = l3mem::kRegionCount / o.threads;
        args[i] = {&gate, &buffer, i, o.cpus[i], i * count, count, 0, 0};
        int rc = pthread_create(&threads[i], nullptr, worker, &args[i]);
        if (rc) {
            std::fprintf(stderr, "pthread_create: %s\n", std::strerror(rc));
            ok = false;
            break;
        }
        ++created;
    }
    pthread_mutex_lock(&gate.mutex);
    while (gate.ready < created)
        pthread_cond_wait(&gate.condition, &gate.mutex);
    for (unsigned i = 0; i < created; ++i) {
        if (args[i].error) {
            std::fprintf(stderr, "Thread %u bind CPU %d: %s\n",
                         i, args[i].cpu, std::strerror(args[i].error));
            ok = false;
        }
    }
    if (!ok || interrupted) gate.stop.store(true, std::memory_order_relaxed);
    std::printf("STAGE_BEGIN round=%u mode=fixed stage=%s seconds=%u unix_time=%lld\n",
                round, stage, seconds, static_cast<long long>(time(nullptr)));
    const auto start = std::chrono::steady_clock::now();
    gate.go = true;
    pthread_cond_broadcast(&gate.condition);
    pthread_mutex_unlock(&gate.mutex);
    const auto deadline = start + std::chrono::seconds(seconds);
    while (ok && !interrupted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gate.stop.store(true, std::memory_order_relaxed);
    std::uint64_t blocks = 0;
    for (unsigned i = 0; i < created; ++i) {
        const int rc = pthread_join(threads[i], nullptr);
        if (rc) {
            // Never unmap memory while a worker might still be accessing it.
            std::fprintf(stderr, "pthread_join: %s; terminating process safely\n",
                         std::strerror(rc));
            std::abort();
        }
        blocks += args[i].blocks;
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double covered = static_cast<double>(blocks) * l3mem::kRegionSize /
                           (1024.0 * 1024 * 1024);
    std::printf("STAGE_END round=%u mode=fixed stage=%s status=%s elapsed=%.6f "
                "blocks=%" PRIu64 " covered_GiB=%.6f covered_GiB_s=%.6f "
                "load_GiB=%.6f\n", round, stage,
                !ok ? "FAIL" : (interrupted ? "INTERRUPTED" : "OK"),
                elapsed, blocks, covered, elapsed > 0 ? covered / elapsed : 0,
                covered / workload::kStride);
    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);
    return ok && !interrupted;
}
#endif
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }
    Options options;
    if (!parse(argc, argv, options)) {
        usage(argv[0]);
        return 2;
    }
#ifndef __linux__
    std::fprintf(stderr, "This benchmark requires Linux HugeTLB and CPU affinity.\n");
    return 1;
#else
    if (!valid_cpus(options)) return 1; // Fail before reserving 32GiB.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) || sigaction(SIGTERM, &action, nullptr)) {
        std::perror("sigaction");
        return 1;
    }
    auto buffer = l3mem::malloc_l3();
    if (!buffer.valid()) {
        std::perror("malloc_l3");
        return 1;
    }
    std::printf("32 x 1GiB; scan=70MiB; threads=%u; block=1024KiB; stride=64B\n",
                options.threads);
    for (unsigned t = 0; t < options.threads; ++t) {
        const auto count = l3mem::kRegionCount / options.threads;
        for (std::size_t j = 0; j < count; ++j) {
            const auto i = t * count + j;
            std::printf("tid=%u cpu=%d region=%zu region_va=%p size=%zu\n",
                        t, options.cpus[t], i, buffer.regions[i].address,
                        buffer.regions[i].size);
        }
    }
    bool ok = true;
    for (unsigned r = 1; r <= options.rounds && ok && !interrupted; ++r) {
        std::printf("TRIAL_BEGIN round=%u mode=fixed layout=seed35-fixed-v1\n", r);
        if (options.warmup)
            ok = run_stage(buffer, options, r, "warmup", options.warmup);
        if (ok && !interrupted)
            ok = run_stage(buffer, options, r, "measure", options.seconds);
        std::printf("TRIAL_END round=%u mode=fixed\n", r);
    }
    if (!l3mem::free_l3(buffer.base_address)) {
        std::perror("free_l3");
        return 1;
    }
    buffer = {};
    return interrupted ? 130 : (ok ? 0 : 1);
#endif
}
