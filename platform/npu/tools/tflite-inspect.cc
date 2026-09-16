// Print TFLite input/output tensor contracts before wiring a new model.
// g++ -O2 -std=c++17 tflite-inspect.cc -ltensorflow-lite -o tflite-inspect
#include <tensorflow/lite/c/c_api.h>

#include <iostream>
#include <memory>

static void show(const char* role, int index, const TfLiteTensor* tensor) {
    std::cout << role << '[' << index << "] " << TfLiteTensorName(tensor)
              << " type=" << TfLiteTensorType(tensor) << " shape=[";
    for (int i = 0; i < TfLiteTensorNumDims(tensor); ++i) {
        if (i) std::cout << ',';
        std::cout << TfLiteTensorDim(tensor, i);
    }
    auto q = TfLiteTensorQuantizationParams(tensor);
    std::cout << "] bytes=" << TfLiteTensorByteSize(tensor)
              << " scale=" << q.scale << " zero_point=" << q.zero_point << '\n';
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " model.tflite\n";
        return 2;
    }
    std::unique_ptr<TfLiteModel, decltype(&TfLiteModelDelete)> model(
        TfLiteModelCreateFromFile(argv[1]), TfLiteModelDelete);
    if (!model) return 1;
    std::unique_ptr<TfLiteInterpreterOptions, decltype(&TfLiteInterpreterOptionsDelete)>
        options(TfLiteInterpreterOptionsCreate(), TfLiteInterpreterOptionsDelete);
    std::unique_ptr<TfLiteInterpreter, decltype(&TfLiteInterpreterDelete)> interpreter(
        TfLiteInterpreterCreate(model.get(), options.get()), TfLiteInterpreterDelete);
    if (!interpreter || TfLiteInterpreterAllocateTensors(interpreter.get()) != kTfLiteOk)
        return 1;
    for (int i = 0; i < TfLiteInterpreterGetInputTensorCount(interpreter.get()); ++i)
        show("input", i, TfLiteInterpreterGetInputTensor(interpreter.get(), i));
    for (int i = 0; i < TfLiteInterpreterGetOutputTensorCount(interpreter.get()); ++i)
        show("output", i, TfLiteInterpreterGetOutputTensor(interpreter.get(), i));
}
