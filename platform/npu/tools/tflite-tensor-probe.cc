// Compare a model's intermediate tensors between the CPU and Teflon paths.
//
// The C API only exposes the model's declared outputs. A partitioned graph
// fails somewhere between them, so this uses the C++ interpreter, which can
// read any tensor in the primary subgraph -- including the tensors that the
// CPU and NPU partitions hand to each other.
//
// g++ -O2 -std=c++17 tflite-tensor-probe.cc -ltensorflow-lite -o tflite-tensor-probe
// tflite-tensor-probe model.tflite frame.rgb cpu|npu [tensor_index ...]
//
// With no tensor indices it lists every readable tensor instead of dumping.
//
// Append "+poison" to the backend ("npu+poison") to fill the requested
// declared output tensors with 0xAA before copying the input and invoking.
// Changed bytes prove a host-buffer write, not successful hardware execution.
// Intermediate tensors are preserved in the TFLite arena; this does not expose
// tensors internal to a delegate. Always check kernel logs for NPU faults.
// DUMP_DIR=/existing/directory writes requested and declared tensors as N.bin.
//
// EXTRA_OUTPUTS="12,34" declares those tensors as additional model outputs
// before the delegate partitions the graph. That turns a single-output
// delegated subgraph into a multi-output one without touching the model, which
// is how to test whether output count is what a delegate mishandles.
#include <tensorflow/lite/interpreter.h>
#include <tensorflow/lite/interpreter_options.h>
#include <tensorflow/lite/interpreter_builder.h>
#include <tensorflow/lite/kernels/register.h>
#include <tensorflow/lite/model_builder.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

const char* type_name(TfLiteType t) { return TfLiteTypeGetName(t); }

std::string shape_of(const TfLiteTensor* t) {
    std::string s;
    for (int i = 0; i < t->dims->size; ++i) {
        if (i) s += "x";
        s += std::to_string(t->dims->data[i]);
    }
    return s.empty() ? "-" : s;
}

// Summarise a tensor numerically. Quantized tensors are reported in real
// units as well, because a u8 range means nothing without scale and zero
// point, and the whole question here is whether the values are plausible.
void dump(tflite::Interpreter* interp, int index) {
    const TfLiteTensor* t = interp->tensor(index);
    if (!t) { printf("tensor %d: absent\n", index); return; }
    printf("tensor %-4d %-22s %-8s %-18s bytes=%-9zu ",
           index, t->name ? t->name : "(unnamed)", type_name(t->type),
           shape_of(t).c_str(), t->bytes);
    if (!t->data.raw) { printf("NO DATA (consumed inside a partition)\n"); return; }

    const float scale = t->params.scale;
    const int zp = t->params.zero_point;
    double lo = 0, hi = 0, sum = 0;
    size_t n = 0;
    auto note = [&](double v) {
        if (n == 0) { lo = hi = v; } else { lo = std::min(lo, v); hi = std::max(hi, v); }
        sum += v; ++n;
    };
    if (t->type == kTfLiteFloat32) {
        const float* p = reinterpret_cast<const float*>(t->data.raw);
        for (size_t i = 0; i < t->bytes / sizeof(float); ++i) note(p[i]);
        printf("f32 min=%.6g max=%.6g mean=%.6g", lo, hi, n ? sum / n : 0.0);
    } else if (t->type == kTfLiteUInt8) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(t->data.raw);
        for (size_t i = 0; i < t->bytes; ++i) note(p[i]);
        printf("u8 min=%d max=%d mean=%.3f  scale=%.6g zp=%d  real[%.6g..%.6g]",
               (int)lo, (int)hi, n ? sum / n : 0.0, scale, zp,
               (lo - zp) * scale, (hi - zp) * scale);
    } else if (t->type == kTfLiteInt32) {
        const int32_t* p = reinterpret_cast<const int32_t*>(t->data.raw);
        for (size_t i = 0; i < t->bytes / sizeof(int32_t); ++i) note(p[i]);
        printf("i32 min=%lld max=%lld", (long long)lo, (long long)hi);
    } else {
        printf("(type not summarised)");
    }
    printf("\n         first:");
    for (size_t i = 0; i < 8; ++i) {
        if (t->type == kTfLiteFloat32 && (i + 1) * sizeof(float) <= t->bytes)
            printf(" %.5g", reinterpret_cast<const float*>(t->data.raw)[i]);
        else if (t->type == kTfLiteUInt8 && i < t->bytes)
            printf(" %d", reinterpret_cast<const uint8_t*>(t->data.raw)[i]);
    }
    printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s model.tflite frame.rgb cpu|npu [tensor_index ...]\n", argv[0]);
        return 2;
    }
    std::string mode = argv[3];
    bool poison = false;
    const std::string suffix = "+poison";
    if (mode.size() > suffix.size() &&
        mode.compare(mode.size() - suffix.size(), suffix.size(), suffix) == 0) {
        poison = true;
        mode.erase(mode.size() - suffix.size());
    }
    if (mode != "cpu" && mode != "npu") return 2;

    std::ifstream frame(argv[2], std::ios::binary);
    std::vector<uint8_t> pixels((std::istreambuf_iterator<char>(frame)),
                                std::istreambuf_iterator<char>());
    if (pixels.empty()) { fprintf(stderr, "cannot read RGB24 frame\n"); return 2; }

    auto model = tflite::FlatBufferModel::BuildFromFile(argv[1]);
    if (!model) { fprintf(stderr, "cannot load model\n"); return 1; }
    tflite::ops::builtin::BuiltinOpResolver resolver;
    std::unique_ptr<tflite::Interpreter> interp;
    if (tflite::InterpreterBuilder(*model, resolver)(&interp) != kTfLiteOk || !interp) {
        fprintf(stderr, "cannot build interpreter\n"); return 1;
    }
    interp->SetNumThreads(1);
    tflite::InterpreterOptions audit_options;
    audit_options.SetPreserveAllTensors();
    if (interp->ApplyOptions(&audit_options) != kTfLiteOk) {
        fprintf(stderr, "ApplyOptions failed\n"); return 1;
    }
    printf("preserve_all_tensors=true\n");
    for (int i = 4; i < argc; ++i) {
        char* end = nullptr;
        long index = strtol(argv[i], &end, 10);
        if (!*argv[i] || *end || index < 0 ||
            static_cast<size_t>(index) >= interp->tensors_size()) {
            fprintf(stderr, "invalid tensor index: %s\n", argv[i]); return 2;
        }
    }

    if (const char* extra = getenv("EXTRA_OUTPUTS")) {
        std::vector<int> outputs = interp->outputs();
        std::string spec(extra);
        size_t at = 0;
        while (at < spec.size()) {
            size_t comma = spec.find(',', at);
            if (comma == std::string::npos) comma = spec.size();
            std::string token = spec.substr(at, comma - at);
            char* end = nullptr;
            long index = strtol(token.c_str(), &end, 10);
            if (token.empty() || *end || index < 0 ||
                static_cast<size_t>(index) >= interp->tensors_size()) {
                fprintf(stderr, "invalid EXTRA_OUTPUTS index: %s\n", token.c_str());
                return 2;
            }
            if (std::find(outputs.begin(), outputs.end(), index) == outputs.end())
                outputs.push_back(static_cast<int>(index));
            at = comma + 1;
        }
        if (interp->SetOutputs(outputs) != kTfLiteOk) {
            fprintf(stderr, "SetOutputs failed\n"); return 1;
        }
        printf("declared %zu outputs:", outputs.size());
        for (int o : outputs) printf(" %d", o);
        printf("\n");
    }

    TfLiteDelegate* delegate = nullptr;
    if (mode == "npu") {
        // TEFLON_LIB points at a locally built delegate, so a patched Mesa can
        // be A/B tested against the packaged one without installing anything.
        const char* lib = getenv("TEFLON_LIB");
        if (!lib) lib = "/usr/lib/teflon/libteflon.so";
        printf("delegate: %s\n", lib);
        auto opts = TfLiteExternalDelegateOptionsDefault(lib);
        delegate = TfLiteExternalDelegateCreate(&opts);
        if (!delegate) { fprintf(stderr, "Teflon delegate could not be loaded\n"); return 1; }
        if (interp->ModifyGraphWithDelegate(delegate) != kTfLiteOk) {
            fprintf(stderr, "delegate rejected the graph\n"); return 1;
        }
    }
    if (interp->AllocateTensors() != kTfLiteOk) {
        fprintf(stderr, "AllocateTensors failed\n"); return 1;
    }

    const int in = interp->inputs()[0];
    TfLiteTensor* input = interp->tensor(in);
    if (input->bytes != pixels.size()) {
        fprintf(stderr, "frame is %zu bytes, model wants %zu\n", pixels.size(), input->bytes);
        return 1;
    }

    if (poison) {
        for (int i = 4; i < argc; ++i) {
            TfLiteTensor* t = interp->tensor(atoi(argv[i]));
            if (std::find(interp->outputs().begin(), interp->outputs().end(),
                          atoi(argv[i])) == interp->outputs().end()) {
                fprintf(stderr, "poison requires a declared output: %s\n", argv[i]);
                return 2;
            }
            if (t && t->data.raw && t->allocation_type != kTfLiteMmapRo) {
                memset(t->data.raw, 0xAA, t->bytes);
                printf("poisoned tensor %s with 0xAA\n", argv[i]);
            }
        }
    }

    memcpy(input->data.raw, pixels.data(), pixels.size());

    if (interp->Invoke() != kTfLiteOk) { fprintf(stderr, "Invoke failed\n"); return 1; }

    printf("== backend=%s tensors=%zu nodes=%zu ==\n", mode.c_str(),
           interp->tensors_size(), interp->nodes_size());

    if (argc == 4) {
        for (size_t i = 0; i < interp->tensors_size(); ++i) {
            const TfLiteTensor* t = interp->tensor((int)i);
            if (!t || !t->name) continue;
            printf("%4zu %-10s %-16s %-9zu %s %s\n", i, type_name(t->type),
                   shape_of(t).c_str(), t->bytes,
                   t->allocation_type == kTfLiteMmapRo ? "const" : "live ",
                   t->name);
        }
    }
    for (int i = 4; i < argc; ++i) dump(interp.get(), atoi(argv[i]));

    printf("-- declared outputs --\n");
    for (int o : interp->outputs()) dump(interp.get(), o);
    if (const char* dir = getenv("DUMP_DIR")) {
        std::vector<int> indices = interp->outputs();
        for (int i = 4; i < argc; ++i) indices.push_back(atoi(argv[i]));
        for (int index : indices) {
            const TfLiteTensor* t = interp->tensor(index);
            if (!t || !t->data.raw) continue;
            std::ofstream out(std::string(dir) + "/" + std::to_string(index) + ".bin",
                              std::ios::binary);
            out.write(t->data.raw, t->bytes);
            if (!out) { fprintf(stderr, "tensor dump failed\n"); return 1; }
        }
    }
    interp.reset();
    if (delegate) TfLiteExternalDelegateDelete(delegate);
    return 0;
}
