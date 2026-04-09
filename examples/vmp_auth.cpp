#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "tie/vm/api.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using tie::vm::Constant;
using tie::vm::InstructionBuilder;
using tie::vm::Module;
using tie::vm::StatusOr;
using tie::vm::Value;
using tie::vm::VmInstance;

constexpr uint8_t kXorKey = 0x6D;

constexpr std::array<uint8_t, 16> kPromptEnc{
    133, 194, 218, 133, 211, 254, 136, 232, 200, 136, 224, 204, 136, 194, 235, 87};
constexpr std::array<uint8_t, 26> kPasswordEnc{
    7, 9, 3, 7, 6, 3, 6, 12, 7, 3, 30, 12, 6, 3, 30, 12, 6, 7, 30, 3, 12, 30, 3, 12, 3, 30};
constexpr std::array<uint8_t, 15> kSuccessEnc{
    139, 229, 252, 136, 227, 214, 138, 241, 242, 138, 228, 246, 132, 237, 209};
constexpr std::array<uint8_t, 12> kFailEnc{
    136, 224, 204, 136, 194, 235, 132, 249, 244, 133, 194, 194};
constexpr std::array<uint8_t, 16> kJunkEnc{
    3, 30, 7, 3, 7, 30, 3, 9, 7, 30, 3, 9, 7, 3, 30, 6};

template <size_t N>
std::string XorDecode(const std::array<uint8_t, N>& enc) {
    std::string out;
    out.resize(N);
    for (size_t i = 0; i < N; ++i) {
        out[i] = static_cast<char>(enc[i] ^ kXorKey);
    }
    return out;
}

uint64_t MixHost(uint64_t x) {
    x ^= (x << 13);
    x ^= (x >> 7);
    x ^= (x << 17);
    return x;
}

std::string RandomSymbol(std::mt19937_64& rng, size_t min_len, size_t max_len) {
    static constexpr char kAlpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<size_t> len_dist(min_len, max_len);
    std::uniform_int_distribution<size_t> ch_dist(0, sizeof(kAlpha) - 2);
    const size_t n = len_dist(rng);
    std::string s;
    s.reserve(n + 2);
    s.push_back('n');
    s.push_back('s');
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kAlpha[ch_dist(rng)]);
    }
    return s;
}

uint32_t AddNoiseFunction(
    Module& module,
    std::mt19937_64& rng,
    const std::vector<uint32_t>& int_consts,
    uint32_t c_one,
    uint32_t c_loop_a,
    uint32_t c_loop_b,
    uint32_t c_shift,
    uint32_t c_junk,
    const std::vector<uint32_t>& prev_noise) {
    std::uniform_int_distribution<size_t> pick_int(0, int_consts.size() - 1);
    const uint32_t c_a = int_consts[pick_int(rng)];
    const uint32_t c_b = int_consts[pick_int(rng)];
    const uint32_t c_c = int_consts[pick_int(rng)];

    const uint32_t idx = static_cast<uint32_t>(module.functions().size());
    auto& fn = module.AddFunction(RandomSymbol(rng, 18, 32), 96, 0);
    auto& block = fn.AddBlock(RandomSymbol(rng, 6, 10));
    InstructionBuilder ib(block);

    ib.Nop()
        .LoadK(1, c_one)
        .LoadK(2, c_a)
        .LoadK(3, c_b)
        .LoadK(4, c_c)
        .Add(5, 2, 3)
        .Sub(6, 5, 4)
        .Mul(7, 6, 1)
        .Div(8, 7, 1)
        .BitXor(9, 8, 3)
        .BitAnd(10, 9, 2)
        .BitOr(11, 10, 4)
        .LoadK(12, c_loop_a)
        .DecJnz(12, 0)
        .LoadK(13, c_loop_b)
        .AddImmJnz(13, -1, 0)
        .LoadK(14, c_junk)
        .StrLen(15, 14)
        .AddImm(16, 15, 7)
        .SubImm(16, 16, 7)
        .CmpEq(17, 16, 15)
        .JmpIfZero(17, 3)
        .Add(18, 11, 16)
        .Jmp(2)
        .Sub(18, 11, 16)
        .Nop();

    if (!prev_noise.empty()) {
        std::uniform_int_distribution<size_t> pick_fn(0, prev_noise.size() - 1);
        const uint32_t f1 = prev_noise[pick_fn(rng)];
        const uint32_t f2 = prev_noise[pick_fn(rng)];
        ib.Call(40, f1, 0)
            .Call(41, f2, 0)
            .Add(42, 40, 41)
            .BitXor(43, 42, 18);
    } else {
        ib.Mov(43, 18);
    }

    ib.StrConcat(44, 14, 14)
        .StrLen(45, 44)
        .LoadK(46, c_shift)
        .BitShl(47, 43, 46)
        .BitShr(48, 47, 46)
        .LoadK(49, c_loop_a)
        .SubImmJnz(49, 1, 0)
        .Add(50, 48, 45)
        .Add(0, 50, 10)
        .Ret(0);
    return idx;
}

uint32_t AddUpvalueFn(Module& module, std::mt19937_64& rng) {
    const uint32_t idx = static_cast<uint32_t>(module.functions().size());
    auto& fn = module.AddFunction(RandomSymbol(rng, 16, 28), 8, 1, 1, false);
    auto& block = fn.AddBlock(RandomSymbol(rng, 6, 10));
    InstructionBuilder ib(block);
    ib.GetUpval(1, 0).Add(0, 0, 1).Ret(0);
    return idx;
}

uint32_t AddVarArgFn(Module& module, std::mt19937_64& rng) {
    const uint32_t idx = static_cast<uint32_t>(module.functions().size());
    auto& fn = module.AddFunction(RandomSymbol(rng, 16, 28), 8, 1, 0, true);
    auto& block = fn.AddBlock(RandomSymbol(rng, 6, 10));
    InstructionBuilder ib(block);
    ib.VarArg(1, 0, 1).Add(0, 0, 1).Ret(0);
    return idx;
}

uint32_t AddTailTargetFn(Module& module, std::mt19937_64& rng) {
    const uint32_t idx = static_cast<uint32_t>(module.functions().size());
    auto& fn = module.AddFunction(RandomSymbol(rng, 16, 28), 8, 1);
    auto& block = fn.AddBlock(RandomSymbol(rng, 6, 10));
    InstructionBuilder ib(block);
    ib.AddImm(0, 0, 1).Ret(0);
    return idx;
}

uint32_t AddTailWrapFn(Module& module, std::mt19937_64& rng, uint32_t target_fn) {
    const uint32_t idx = static_cast<uint32_t>(module.functions().size());
    auto& fn = module.AddFunction(RandomSymbol(rng, 16, 28), 8, 1);
    auto& block = fn.AddBlock(RandomSymbol(rng, 6, 10));
    InstructionBuilder ib(block);
    ib.Mov(1, 0).TailCall(0, target_fn, 1);
    return idx;
}

struct BuildOut {
    Module module;
    uint32_t verify_fn = 0;
};

BuildOut BuildModule(
    const std::string& password,
    const std::string& junk,
    uint64_t seed) {
    std::mt19937_64 rng(seed);
    Module module(RandomSymbol(rng, 20, 36));

    const uint32_t c_zero = module.AddConstant(Constant::Int64(0));
    const uint32_t c_one = module.AddConstant(Constant::Int64(1));
    const uint32_t c_pw_len = module.AddConstant(Constant::Int64(static_cast<int64_t>(password.size())));
    const uint32_t c_password = module.AddConstant(Constant::Utf8(password));
    const uint32_t c_junk = module.AddConstant(Constant::Utf8(junk));
    const uint32_t c_loop_a = module.AddConstant(Constant::Int64(17));
    const uint32_t c_loop_b = module.AddConstant(Constant::Int64(29));
    const uint32_t c_shift = module.AddConstant(Constant::Int64(3));
    const uint32_t c_i0 = module.AddConstant(Constant::Int64(0x1337));
    const uint32_t c_i1 = module.AddConstant(Constant::Int64(0x55AA));
    const uint32_t c_i2 = module.AddConstant(Constant::Int64(0x1024));
    const uint32_t c_i3 = module.AddConstant(Constant::Int64(0x77));
    const uint32_t c_i4 = module.AddConstant(Constant::Int64(0x909));
    const uint32_t c_i5 = module.AddConstant(Constant::Int64(0xDE));
    const uint32_t c_i6 = module.AddConstant(Constant::Int64(0x2A2A));
    const uint32_t c_i7 = module.AddConstant(Constant::Int64(0x4040));
    const uint32_t c_i8 = module.AddConstant(Constant::Int64(0x88));
    const uint32_t c_i9 = module.AddConstant(Constant::Int64(0x6262));
    std::vector<uint32_t> int_consts{
        c_i0, c_i1, c_i2, c_i3, c_i4, c_i5, c_i6, c_i7, c_i8, c_i9};

    std::vector<uint32_t> noise_fns;
    noise_fns.reserve(48);
    for (size_t i = 0; i < 48; ++i) {
        noise_fns.push_back(AddNoiseFunction(
            module, rng, int_consts, c_one, c_loop_a, c_loop_b, c_shift, c_junk, noise_fns));
    }

    const uint32_t upvalue_fn = AddUpvalueFn(module, rng);
    const uint32_t vararg_fn = AddVarArgFn(module, rng);
    const uint32_t tail_target_fn = AddTailTargetFn(module, rng);
    const uint32_t tail_wrap_fn = AddTailWrapFn(module, rng, tail_target_fn);

    const uint32_t verify_idx = static_cast<uint32_t>(module.functions().size());
    auto& verify = module.AddFunction(RandomSymbol(rng, 20, 38), 192, 1);
    auto& vb = verify.AddBlock(RandomSymbol(rng, 6, 10));
    InstructionBuilder ib(vb);

    for (uint32_t i = 0; i < 24; ++i) {
        ib.Call(60 + i, noise_fns[i], 0);
    }

    ib.Add(10, 60, 61)
        .BitXor(11, 62, 63)
        .BitAnd(12, 64, 65)
        .BitOr(13, 66, 67)
        .Add(14, 10, 11)
        .Sub(15, 14, 12)
        .Add(16, 15, 13)
        .LoadK(17, c_loop_a)
        .DecJnz(17, 0)
        .LoadK(18, c_loop_b)
        .AddImmJnz(18, -1, 0)
        .LoadK(20, c_one)
        .Closure(21, upvalue_fn, 20, 1)
        .LoadK(23, c_one)
        .CallClosure(22, 21, 1)
        .LoadK(27, c_one)
        .LoadK(28, c_one)
        .Call(26, vararg_fn, 2)
        .LoadK(30, c_one)
        .Call(29, tail_wrap_fn, 1)
        .CmpEq(31, 22, 22)
        .JmpIfZero(31, 4)
        .Add(32, 26, 29)
        .Sub(33, 32, 16)
        .Jmp(2)
        .Add(33, 16, 22)
        .Nop()
        .Mov(2, 0)
        .StrLen(3, 2)
        .LoadK(4, c_pw_len)
        .CmpEq(5, 3, 4)
        .JmpIfZero(5, 6)
        .LoadK(6, c_password)
        .CmpEq(7, 2, 6)
        .JmpIfZero(7, 3)
        .LoadK(0, c_one)
        .Ret(0)
        .LoadK(0, c_zero)
        .Ret(0);

    module.set_entry_function(verify_idx);
    return BuildOut{std::move(module), verify_idx};
}

}  // namespace

int main() {
#if defined(_WIN32)
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    const std::string prompt = XorDecode(kPromptEnc);
    const std::string password = XorDecode(kPasswordEnc);
    const std::string ok_text = XorDecode(kSuccessEnc);
    const std::string fail_text = XorDecode(kFailEnc);
    const std::string junk = XorDecode(kJunkEnc);

    std::cout << prompt;
    std::string input;
    std::getline(std::cin, input);
    if (input.size() >= 2 && static_cast<uint8_t>(input[0]) == 0xFF &&
        static_cast<uint8_t>(input[1]) == 0xFE) {
        input.erase(0, 2);
    }
    if (input.size() >= 3 && static_cast<uint8_t>(input[0]) == 0xEF &&
        static_cast<uint8_t>(input[1]) == 0xBB &&
        static_cast<uint8_t>(input[2]) == 0xBF) {
        input.erase(0, 3);
    }
    input.erase(std::remove(input.begin(), input.end(), '\0'), input.end());
    while (!input.empty() && (input.back() == '\r' || input.back() == '\n')) {
        input.pop_back();
    }

    volatile uint64_t host_noise = 0x1337133713371337ULL;
    for (int i = 0; i < 512; ++i) {
        host_noise ^= MixHost(host_noise + static_cast<uint64_t>(i));
    }
    const uint64_t time_seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const uint64_t rd_seed = (static_cast<uint64_t>(std::random_device{}()) << 32u) ^
                             static_cast<uint64_t>(std::random_device{}());
    const uint64_t seed = host_noise ^ time_seed ^ rd_seed;

    VmInstance vm;
    auto built = BuildModule(password, junk, seed);
    auto input_or = vm.InternString(input);
    if (!input_or.ok()) {
        return 21;
    }

    auto th = vm.CreateThread();
    StatusOr<Value> result_or = th.Execute(built.module, built.verify_fn, {input_or.value()});
    if (!result_or.ok()) {
        return 22;
    }

    const bool pass = result_or.value().type() == Value::Type::kInt64 &&
                      result_or.value().AsInt64Fast() == 1;
    if (pass) {
        std::cout << ok_text << "\n";
    } else {
        std::cout << fail_text << "\n";
    }

    if (host_noise == 0) {
        std::cout << "";
    }
    return pass ? 0 : 1;
}
