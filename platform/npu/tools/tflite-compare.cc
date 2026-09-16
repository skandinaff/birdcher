// Compare a quantized TFLite model on CPU and Mesa Teflon/etnaviv.
// Build on the VIM3 with:
//   g++ -O2 -std=c++17 tflite-compare.cc -ltensorflow-lite -o tflite-compare
// Run with:
//   ./tflite-compare model.tflite [iterations] [compare|npu] [input.rgb] [labels.txt]
// Set TEFLON_DEBUG=verbose once to verify delegated operators and NN jobs.
#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using Model = std::unique_ptr<TfLiteModel, decltype(&TfLiteModelDelete)>;
using Options = std::unique_ptr<TfLiteInterpreterOptions,
                                decltype(&TfLiteInterpreterOptionsDelete)>;
using Interpreter = std::unique_ptr<TfLiteInterpreter,
                                    decltype(&TfLiteInterpreterDelete)>;
using Delegate = std::unique_ptr<TfLiteDelegate,
                                 decltype(&TfLiteExternalDelegateDelete)>;

struct Result {
    std::vector<uint8_t> output;
    double mean_ms = 0;
    int top_index = -1;
    int top_value = -1;
};

static void show_top(const Result& result, const char* labels_path, const char* name) {
    if (!labels_path) return;
    std::ifstream file(labels_path);
    std::vector<std::string> labels;
    for (std::string line; std::getline(file, line);) labels.push_back(line);
    std::vector<size_t> order(result.output.size());
    std::iota(order.begin(), order.end(), 0);
    size_t n = std::min<size_t>(5, order.size());
    std::partial_sort(order.begin(), order.begin() + n, order.end(),
                      [&](size_t a, size_t b) { return result.output[a] > result.output[b]; });
    for (size_t i = 0; i < n; ++i) {
        size_t index = order[i];
        std::cout << name << " top" << i + 1 << " index=" << index
                  << " value=" << static_cast<int>(result.output[index])
                  << " label=\"" << (index < labels.size() ? labels[index] : "?")
                  << "\"\n";
    }
}

static bool run(const TfLiteModel* model, TfLiteDelegate* delegate,
                const std::vector<uint8_t>& input, int iterations,
                const char* name, Result& result) {
    Options options(TfLiteInterpreterOptionsCreate(), TfLiteInterpreterOptionsDelete);
    if (!options) return false;
    TfLiteInterpreterOptionsSetNumThreads(options.get(), 1);
    if (delegate) TfLiteInterpreterOptionsAddDelegate(options.get(), delegate);
    Interpreter interpreter(TfLiteInterpreterCreate(model, options.get()),
                            TfLiteInterpreterDelete);
    if (!interpreter || TfLiteInterpreterAllocateTensors(interpreter.get()) != kTfLiteOk) {
        std::cerr << name << ": interpreter setup failed\n";
        return false;
    }
    if (TfLiteInterpreterGetInputTensorCount(interpreter.get()) != 1 ||
        TfLiteInterpreterGetOutputTensorCount(interpreter.get()) != 1) {
        std::cerr << name << ": expected one input and one output\n";
        return false;
    }
    TfLiteTensor* in = TfLiteInterpreterGetInputTensor(interpreter.get(), 0);
    const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(interpreter.get(), 0);
    if (TfLiteTensorType(in) != kTfLiteUInt8 || TfLiteTensorType(out) != kTfLiteUInt8 ||
        TfLiteTensorByteSize(in) != input.size()) {
        std::cerr << name << ": expected UINT8 model and matching input shape\n";
        return false;
    }
    std::cout << name << " input bytes=" << input.size() << " output bytes="
              << TfLiteTensorByteSize(out) << '\n';
    if (TfLiteTensorCopyFromBuffer(in, input.data(), input.size()) != kTfLiteOk) return false;
    // Separate setup and first-invoke costs from steady-state inference.
    if (TfLiteInterpreterInvoke(interpreter.get()) != kTfLiteOk) {
        std::cerr << name << ": warmup failed\n";
        return false;
    }
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (TfLiteInterpreterInvoke(interpreter.get()) != kTfLiteOk) {
            std::cerr << name << ": invoke " << i << " failed\n";
            return false;
        }
    }
    auto end = std::chrono::steady_clock::now();
    result.mean_ms = std::chrono::duration<double, std::milli>(end - start).count() /
                     iterations;
    out = TfLiteInterpreterGetOutputTensor(interpreter.get(), 0);
    result.output.resize(TfLiteTensorByteSize(out));
    if (TfLiteTensorCopyToBuffer(out, result.output.data(), result.output.size()) != kTfLiteOk)
        return false;
    auto best = std::max_element(result.output.begin(), result.output.end());
    result.top_index = static_cast<int>(best - result.output.begin());
    result.top_value = *best;
    std::cout << name << " mean=" << result.mean_ms << " ms (" << iterations
              << " invokes), top index=" << result.top_index
              << " value=" << result.top_value << '\n';
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 6) {
        std::cerr << "usage: " << argv[0]
                  << " model.tflite [iterations] [compare|npu] [input.rgb] [labels.txt]\n";
        return 2;
    }
    int iterations = argc >= 3 ? std::atoi(argv[2]) : 50;
    const std::string mode = argc >= 4 ? argv[3] : "compare";
    if (iterations < 1 || iterations > 1000000 ||
        (mode != "compare" && mode != "npu")) {
        std::cerr << "iterations must be 1..1000000; mode compare or npu\n";
        return 2;
    }
    Model model(TfLiteModelCreateFromFile(argv[1]), TfLiteModelDelete);
    if (!model) {
        std::cerr << "cannot open model: " << argv[1] << '\n';
        return 1;
    }
    // The validated MobileNet V1 UINT8 model has one 224x224x3 input.
    // A deterministic gradient makes the CPU/NPU comparison repeatable.
    std::vector<uint8_t> input(224 * 224 * 3);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>((i / 3 + i / (224 * 3)) % 256);
    if (argc >= 5) {
        std::ifstream image(argv[4], std::ios::binary);
        std::vector<uint8_t> actual((std::istreambuf_iterator<char>(image)),
                                    std::istreambuf_iterator<char>());
        if (actual.size() != input.size()) {
            std::cerr << "input.rgb must contain exactly " << input.size()
                      << " RGB24 bytes; got " << actual.size() << '\n';
            return 2;
        }
        input = std::move(actual);
    }

    Result cpu, npu;
    if (mode == "compare") {
        if (!run(model.get(), nullptr, input, iterations, "CPU", cpu)) return 1;
        show_top(cpu, argc >= 6 ? argv[5] : nullptr, "CPU");
    }
    auto delegate_options = TfLiteExternalDelegateOptionsDefault("/usr/lib/teflon/libteflon.so");
    Delegate delegate(TfLiteExternalDelegateCreate(&delegate_options),
                      TfLiteExternalDelegateDelete);
    if (!delegate) {
        std::cerr << "Teflon delegate could not be loaded\n";
        return 1;
    }
    if (!run(model.get(), delegate.get(), input, iterations, "NPU", npu)) return 1;
    show_top(npu, argc >= 6 ? argv[5] : nullptr, "NPU");
    if (mode == "npu") return 0;
    if (cpu.output.size() != npu.output.size()) {
        std::cerr << "output sizes differ\n";
        return 1;
    }
    int max_diff = 0, differing = 0;
    double mean_diff = 0;
    for (size_t i = 0; i < cpu.output.size(); ++i) {
        int diff = std::abs(static_cast<int>(cpu.output[i]) - npu.output[i]);
        max_diff = std::max(max_diff, diff);
        mean_diff += diff;
        if (diff) ++differing;
    }
    mean_diff /= cpu.output.size();
    std::cout << "compare: " << differing << "/" << cpu.output.size()
              << " outputs differ, max_abs=" << max_diff
              << " mean_abs=" << mean_diff
              << " speedup=" << cpu.mean_ms / npu.mean_ms << "x\n";
    // This is a smoke-test tolerance, not a numerical-equivalence proof.
    // Real camera crops have shown differences up to nine output levels.
    return cpu.top_index == npu.top_index && max_diff <= 16 ? 0 : 1;
}
