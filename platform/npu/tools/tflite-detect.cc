// Run a COCO SSD detector on a model-sized square RGB24 frame.
// g++ -O2 -std=c++17 tflite-detect.cc -ltensorflow-lite -o tflite-detect
// tflite-detect model.tflite labels.txt frame.rgb [cpu|npu]
#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using Model = std::unique_ptr<TfLiteModel, decltype(&TfLiteModelDelete)>;
using Options = std::unique_ptr<TfLiteInterpreterOptions,
                                decltype(&TfLiteInterpreterOptionsDelete)>;
using Interpreter = std::unique_ptr<TfLiteInterpreter,
                                    decltype(&TfLiteInterpreterDelete)>;
using Delegate = std::unique_ptr<TfLiteDelegate,
                                 decltype(&TfLiteExternalDelegateDelete)>;

static std::vector<float> output(TfLiteInterpreter* interpreter, int index) {
    const TfLiteTensor* tensor = TfLiteInterpreterGetOutputTensor(interpreter, index);
    if (!tensor || TfLiteTensorType(tensor) != kTfLiteFloat32 ||
        TfLiteTensorByteSize(tensor) % sizeof(float) != 0) return {};
    std::vector<float> values(TfLiteTensorByteSize(tensor) / sizeof(float));
    if (TfLiteTensorCopyToBuffer(tensor, values.data(), TfLiteTensorByteSize(tensor)) != kTfLiteOk)
        return {};
    return values;
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: " << argv[0]
                  << " model.tflite coco_labels.txt frame.rgb [cpu|npu]\n";
        return 2;
    }
    const std::string mode = argc == 5 ? argv[4] : "npu";
    if (mode != "cpu" && mode != "npu") return 2;
    std::ifstream frame(argv[3], std::ios::binary);
    std::vector<uint8_t> pixels((std::istreambuf_iterator<char>(frame)),
                                std::istreambuf_iterator<char>());
    if (pixels.empty()) {
        std::cerr << "cannot read RGB24 frame\n";
        return 2;
    }
    std::ifstream label_file(argv[2]);
    std::vector<std::string> labels;
    for (std::string line; std::getline(label_file, line);) labels.push_back(line);
    if (labels.empty()) {
        std::cerr << "cannot read COCO labels\n";
        return 2;
    }
    Model model(TfLiteModelCreateFromFile(argv[1]), TfLiteModelDelete);
    Options options(TfLiteInterpreterOptionsCreate(), TfLiteInterpreterOptionsDelete);
    if (!model || !options) return 1;
    TfLiteInterpreterOptionsSetNumThreads(options.get(), 1);
    Delegate delegate(nullptr, TfLiteExternalDelegateDelete);
    if (mode == "npu") {
        auto delegate_options = TfLiteExternalDelegateOptionsDefault("/usr/lib/teflon/libteflon.so");
        delegate.reset(TfLiteExternalDelegateCreate(&delegate_options));
        if (!delegate) {
            std::cerr << "Teflon delegate could not be loaded\n";
            return 1;
        }
        TfLiteInterpreterOptionsAddDelegate(options.get(), delegate.get());
    }
    Interpreter interpreter(TfLiteInterpreterCreate(model.get(), options.get()),
                            TfLiteInterpreterDelete);
    if (!interpreter || TfLiteInterpreterAllocateTensors(interpreter.get()) != kTfLiteOk) {
        std::cerr << "model setup failed\n";
        return 1;
    }
    if (TfLiteInterpreterGetInputTensorCount(interpreter.get()) != 1 ||
        TfLiteInterpreterGetOutputTensorCount(interpreter.get()) != 4) {
        std::cerr << "expected one input and four detection outputs\n";
        return 1;
    }
    TfLiteTensor* input = TfLiteInterpreterGetInputTensor(interpreter.get(), 0);
    if (TfLiteTensorType(input) != kTfLiteUInt8 ||
        TfLiteTensorByteSize(input) != pixels.size() ||
        TfLiteTensorCopyFromBuffer(input, pixels.data(), pixels.size()) != kTfLiteOk) {
        std::cerr << "input format mismatch\n";
        return 1;
    }
    auto start = std::chrono::steady_clock::now();
    if (TfLiteInterpreterInvoke(interpreter.get()) != kTfLiteOk) {
        std::cerr << "inference failed\n";
        return 1;
    }
    auto end = std::chrono::steady_clock::now();
    auto boxes = output(interpreter.get(), 0);
    auto classes = output(interpreter.get(), 1);
    auto scores = output(interpreter.get(), 2);
    auto count = output(interpreter.get(), 3);
    if (classes.empty() || boxes.size() != 4 * classes.size() ||
        scores.size() != classes.size() || count.size() != 1) {
        std::cerr << "unexpected detection output shapes\n";
        return 1;
    }
    std::cout << "backend=" << mode << " inference_ms="
              << std::chrono::duration<double, std::milli>(end - start).count()
              << " count=" << count[0] << '\n';
    int shown = 0;
    for (int i = 0; i < std::min(static_cast<int>(classes.size()),
                                static_cast<int>(count[0])); ++i) {
        if (scores[i] < 0.25f) continue;
        int cls = static_cast<int>(std::lround(classes[i]));
        std::string label = cls >= 0 && cls < static_cast<int>(labels.size())
                                ? labels[cls] : "unknown";
        std::cout << "detection " << i << " class=" << cls << " label=\""
                  << label << "\" score=" << std::fixed << std::setprecision(3)
                  << scores[i] << " box=[" << boxes[4 * i] << ','
                  << boxes[4 * i + 1] << ',' << boxes[4 * i + 2] << ','
                  << boxes[4 * i + 3] << "]\n";
        if (++shown == 20) break;
    }
}
