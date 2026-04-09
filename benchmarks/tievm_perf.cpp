#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

#include "tie/vm/api.hpp"

namespace {

enum class PerfTask {
    kSum,
    kFib,
};

bool ParseInt64(std::string_view text, int64_t* out) {
    int64_t parsed = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    *out = parsed;
    return true;
}

bool ParseInt(std::string_view text, int* out) {
    int parsed = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    *out = parsed;
    return true;
}

bool ParseTask(std::string_view text, PerfTask* out) {
    if (text == "sum") {
        *out = PerfTask::kSum;
        return true;
    }
    if (text == "fib") {
        *out = PerfTask::kFib;
        return true;
    }
    return false;
}

int Usage() {
    std::cerr << "usage: tievm_perf "
                 "[--task <sum|fib>] "
                 "[--n <int64>] "
                 "[--fib-n <int>] "
                 "[--repeat <int64>] "
                 "[--warmup <int>] "
                 "[--rounds <int>]\n";
    return 1;
}

int64_t FibValue(int n) {
    if (n <= 0) {
        return 0;
    }
    if (n == 1) {
        return 1;
    }
    int64_t a = 0;
    int64_t b = 1;
    for (int i = 2; i <= n; ++i) {
        const int64_t c = a + b;
        a = b;
        b = c;
    }
    return b;
}

tie::vm::Module BuildSumModule(int64_t n) {
    tie::vm::Module module("bench.sum");
    const auto c_zero = module.AddConstant(tie::vm::Constant::Int64(0));
    const auto c_n = module.AddConstant(tie::vm::Constant::Int64(n));

    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    tie::vm::InstructionBuilder(bb)
        .LoadK(0, c_zero)
        .LoadK(1, c_n)
        .AddDecJnz(0, 1, 0)
        .Ret(0);
    module.set_entry_function(0);
    return module;
}

tie::vm::Module BuildFibModule(int fib_n) {
    tie::vm::Module module("bench.fib");
    const auto c_zero = module.AddConstant(tie::vm::Constant::Int64(0));
    const auto c_one = module.AddConstant(tie::vm::Constant::Int64(1));
    const auto c_fib_n = module.AddConstant(tie::vm::Constant::Int64(fib_n));

    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    tie::vm::InstructionBuilder(bb)
        .LoadK(1, c_zero)      // a = 0
        .LoadK(2, c_one)       // b = 1
        .LoadK(3, c_fib_n)     // counter = n
        .JmpIfZero(3, 10)      // n == 0 => return a
        .LoadK(4, c_one)       // const 1
        .CmpEq(5, 3, 4)        // n == 1 ?
        .JmpIf(5, 6)           // yes => return b
        .SubImm(3, 3, 1)       // counter = n - 1
        .Add(4, 1, 2)          // t = a + b
        .Mov(1, 2)             // a = b
        .Mov(2, 4)             // b = t
        .DecJnz(3, -3)         // loop
        .Ret(2)                // fib(n)
        .Ret(1);               // fib(0)
    module.set_entry_function(0);
    return module;
}

}  // namespace

int main(int argc, char** argv) {
    PerfTask task = PerfTask::kSum;
    int64_t n = 2000000;
    int fib_n = 40;
    int64_t repeat = 1;
    int warmup = 2;
    int rounds = 8;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            return Usage();
        }
        if (arg == "--task") {
            if (i + 1 >= argc || !ParseTask(argv[++i], &task)) {
                std::cerr << "invalid --task value\n";
                return Usage();
            }
            continue;
        }
        if (arg == "--n") {
            if (i + 1 >= argc || !ParseInt64(argv[++i], &n)) {
                std::cerr << "invalid --n value\n";
                return Usage();
            }
            continue;
        }
        if (arg == "--fib-n") {
            if (i + 1 >= argc || !ParseInt(argv[++i], &fib_n)) {
                std::cerr << "invalid --fib-n value\n";
                return Usage();
            }
            continue;
        }
        if (arg == "--repeat") {
            if (i + 1 >= argc || !ParseInt64(argv[++i], &repeat)) {
                std::cerr << "invalid --repeat value\n";
                return Usage();
            }
            continue;
        }
        if (arg == "--warmup") {
            if (i + 1 >= argc || !ParseInt(argv[++i], &warmup)) {
                std::cerr << "invalid --warmup value\n";
                return Usage();
            }
            continue;
        }
        if (arg == "--rounds") {
            if (i + 1 >= argc || !ParseInt(argv[++i], &rounds)) {
                std::cerr << "invalid --rounds value\n";
                return Usage();
            }
            continue;
        }
        std::cerr << "unknown argument: " << arg << "\n";
        return Usage();
    }

    if (warmup < 0 || rounds <= 0 || repeat <= 0) {
        std::cerr << "invalid benchmark settings\n";
        return Usage();
    }

    tie::vm::Module module("bench");
    int64_t expect = 0;
    const char* task_name = "";
    if (task == PerfTask::kSum) {
        if (n <= 0) {
            std::cerr << "sum task requires --n > 0\n";
            return Usage();
        }
        const int64_t kMaxSafeN = 3037000499LL;
        if (n > kMaxSafeN) {
            std::cerr << "sum expectation would overflow int64, reduce --n\n";
            return Usage();
        }
        module = BuildSumModule(n);
        expect = n * (n + 1) / 2;
        task_name = "sum";
    } else {
        if (fib_n < 0 || fib_n > 92) {
            std::cerr << "fib task requires --fib-n in [0,92]\n";
            return Usage();
        }
        module = BuildFibModule(fib_n);
        expect = FibValue(fib_n);
        task_name = "fib";
    }

    tie::vm::VmInstance vm;

    for (int i = 0; i < warmup; ++i) {
        auto result_or = vm.ExecuteModule(module);
        if (!result_or.ok()) {
            std::cerr << "warmup failed: " << result_or.status().message() << "\n";
            return 2;
        }
    }

    std::vector<double> ms;
    ms.reserve(rounds);
    int64_t result = 0;
    for (int i = 0; i < rounds; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto result_or = vm.ExecuteModule(module);
        for (int64_t rep = 1; rep < repeat && result_or.ok(); ++rep) {
            result_or = vm.ExecuteModule(module);
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (!result_or.ok()) {
            std::cerr << "run failed: " << result_or.status().message() << "\n";
            return 3;
        }
        result = result_or.value().AsInt64();
        const double elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms.push_back(elapsed);
    }

    const double total = std::accumulate(ms.begin(), ms.end(), 0.0);
    const double avg = total / static_cast<double>(ms.size());
    const auto [min_it, max_it] = std::minmax_element(ms.begin(), ms.end());

    std::cout << "bench=TieVM task=" << task_name
              << " warmup=" << warmup
              << " rounds=" << rounds
              << " repeat=" << repeat
              << " result=" << result
              << " expect=" << expect
              << " avg_ms=" << avg
              << " min_ms=" << *min_it
              << " max_ms=" << *max_it;
    if (task == PerfTask::kSum) {
        std::cout << " n=" << n;
    } else {
        std::cout << " fib_n=" << fib_n;
    }
    std::cout << "\n";

    return result == expect ? 0 : 4;
}
