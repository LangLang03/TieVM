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

int Usage() {
    std::cerr << "usage: tievm_perf [--n <int64>] [--warmup <int>] [--rounds <int>]\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    int64_t n = 2000000;
    int warmup = 2;
    int rounds = 8;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            return Usage();
        }
        if (arg == "--n") {
            if (i + 1 >= argc || !ParseInt64(argv[++i], &n)) {
                std::cerr << "invalid --n value\n";
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

    if (n <= 0 || warmup < 0 || rounds <= 0) {
        std::cerr << "invalid benchmark settings\n";
        return Usage();
    }

    tie::vm::Module module("bench.sum");
    const auto c_zero = module.AddConstant(tie::vm::Constant::Int64(0));
    const auto c_n = module.AddConstant(tie::vm::Constant::Int64(n));

    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    tie::vm::InstructionBuilder(bb)
        .LoadK(0, c_zero)
        .LoadK(1, c_n)
        .Add(0, 0, 1)
        .DecJnz(1, -1)
        .Ret(0);
    module.set_entry_function(0);

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
    const int64_t expect = n * (n + 1) / 2;

    std::cout << "bench=TieVM task=sum_desc N=" << n
              << " warmup=" << warmup
              << " rounds=" << rounds
              << " result=" << result
              << " expect=" << expect
              << " avg_ms=" << avg
              << " min_ms=" << *min_it
              << " max_ms=" << *max_it
              << "\n";

    return result == expect ? 0 : 4;
}
