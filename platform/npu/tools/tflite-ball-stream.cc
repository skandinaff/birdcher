// Experimental two-stage ball overlay for 640x360 RGB24 frames on stdin/stdout.
// CPU SSDLite proposes boxes; Teflon/etnaviv MobileNet verifies tennis ball.
// g++ -O2 -std=c++17 tflite-ball-stream.cc -ltensorflow-lite -o tflite-ball-stream
// tflite-ball-stream detector.tflite classifier.tflite < frames.rgb > boxed.rgb
#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

constexpr int W = 640, H = 360, D = 320, C = 224;
constexpr int TENNIS_BALL = 853;
using Model = std::unique_ptr<TfLiteModel, decltype(&TfLiteModelDelete)>;
using Options = std::unique_ptr<TfLiteInterpreterOptions, decltype(&TfLiteInterpreterOptionsDelete)>;
using Interpreter = std::unique_ptr<TfLiteInterpreter, decltype(&TfLiteInterpreterDelete)>;
using Delegate = std::unique_ptr<TfLiteDelegate, decltype(&TfLiteExternalDelegateDelete)>;

static bool read_frame(std::vector<uint8_t>& frame) {
    size_t at = 0;
    while (at < frame.size()) {
        size_t n = std::fread(frame.data() + at, 1, frame.size() - at, stdin);
        if (!n) return false;
        at += n;
    }
    return true;
}

static void resize_rgb(const uint8_t* src, int sw, int sx, int sy,
                       int cw, int ch, uint8_t* dst, int dw, int dh) {
    // Bilinear resize keeps the preprocessing close to PIL's RGB resize.
    for (int y = 0; y < dh; ++y) {
        float fy = sy + (y + 0.5f) * ch / dh - 0.5f;
        int y0 = std::clamp(static_cast<int>(std::floor(fy)), sy, sy + ch - 1);
        int y1 = std::min(y0 + 1, sy + ch - 1);
        float wy = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int x = 0; x < dw; ++x) {
            float fx = sx + (x + 0.5f) * cw / dw - 0.5f;
            int x0 = std::clamp(static_cast<int>(std::floor(fx)), sx, sx + cw - 1);
            int x1 = std::min(x0 + 1, sx + cw - 1);
            float wx = std::clamp(fx - x0, 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                float a = src[(y0 * sw + x0) * 3 + c] * (1 - wx) +
                          src[(y0 * sw + x1) * 3 + c] * wx;
                float b = src[(y1 * sw + x0) * 3 + c] * (1 - wx) +
                          src[(y1 * sw + x1) * 3 + c] * wx;
                dst[(y * dw + x) * 3 + c] = static_cast<uint8_t>(std::lround(a * (1 - wy) + b * wy));
            }
        }
    }
}

static std::vector<float> result(TfLiteInterpreter* interpreter, int index) {
    const TfLiteTensor* tensor = TfLiteInterpreterGetOutputTensor(interpreter, index);
    if (!tensor || TfLiteTensorType(tensor) != kTfLiteFloat32) return {};
    std::vector<float> values(TfLiteTensorByteSize(tensor) / sizeof(float));
    if (TfLiteTensorCopyToBuffer(tensor, values.data(), TfLiteTensorByteSize(tensor)) != kTfLiteOk)
        return {};
    return values;
}

struct Box { int x0, y0, x1, y1; };
static void draw_box(std::vector<uint8_t>& frame, Box b) {
    for (int t = 0; t < 3; ++t) {
        int x0 = std::clamp(b.x0 - t, 0, W - 1), x1 = std::clamp(b.x1 + t, 0, W - 1);
        int y0 = std::clamp(b.y0 - t, 0, H - 1), y1 = std::clamp(b.y1 + t, 0, H - 1);
        for (int x = x0; x <= x1; ++x) {
            for (int y : {y0, y1}) {
                uint8_t* p = &frame[(y * W + x) * 3];
                p[0] = 255; p[1] = 20; p[2] = 20;
            }
        }
        for (int y = y0; y <= y1; ++y) {
            for (int x : {x0, x1}) {
                uint8_t* p = &frame[(y * W + x) * 3];
                p[0] = 255; p[1] = 20; p[2] = 20;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s detector.tflite classifier.tflite\n", argv[0]);
        return 2;
    }
    Model detector_model(TfLiteModelCreateFromFile(argv[1]), TfLiteModelDelete);
    Model classifier_model(TfLiteModelCreateFromFile(argv[2]), TfLiteModelDelete);
    Options cpu_options(TfLiteInterpreterOptionsCreate(), TfLiteInterpreterOptionsDelete);
    Options npu_options(TfLiteInterpreterOptionsCreate(), TfLiteInterpreterOptionsDelete);
    auto external = TfLiteExternalDelegateOptionsDefault("/usr/lib/teflon/libteflon.so");
    Delegate delegate(TfLiteExternalDelegateCreate(&external), TfLiteExternalDelegateDelete);
    if (!detector_model || !classifier_model || !cpu_options || !npu_options || !delegate)
        return 1;
    TfLiteInterpreterOptionsSetNumThreads(cpu_options.get(), 1);
    TfLiteInterpreterOptionsSetNumThreads(npu_options.get(), 1);
    TfLiteInterpreterOptionsAddDelegate(npu_options.get(), delegate.get());
    Interpreter detector(TfLiteInterpreterCreate(detector_model.get(), cpu_options.get()),
                         TfLiteInterpreterDelete);
    Interpreter classifier(TfLiteInterpreterCreate(classifier_model.get(), npu_options.get()),
                           TfLiteInterpreterDelete);
    if (!detector || !classifier ||
        TfLiteInterpreterAllocateTensors(detector.get()) != kTfLiteOk ||
        TfLiteInterpreterAllocateTensors(classifier.get()) != kTfLiteOk) return 1;
    TfLiteTensor* din = TfLiteInterpreterGetInputTensor(detector.get(), 0);
    TfLiteTensor* cin = TfLiteInterpreterGetInputTensor(classifier.get(), 0);
    if (!din || !cin || TfLiteTensorByteSize(din) != D * D * 3 ||
        TfLiteTensorByteSize(cin) != C * C * 3) return 1;

    std::vector<uint8_t> frame(W * H * 3), small(D * D * 3), crop(C * C * 3);
    unsigned long frames = 0;
    while (read_frame(frame)) {
        auto start = std::chrono::steady_clock::now();
        resize_rgb(frame.data(), W, 0, 0, W, H, small.data(), D, D);
        if (TfLiteTensorCopyFromBuffer(din, small.data(), small.size()) != kTfLiteOk ||
            TfLiteInterpreterInvoke(detector.get()) != kTfLiteOk) return 1;
        auto boxes = result(detector.get(), 0), scores = result(detector.get(), 2);
        if (boxes.size() != scores.size() * 4) return 1;
        int checked = 0;
        bool found = false;
        for (size_t i = 0; i < scores.size() && checked < 10; ++i) {
            if (scores[i] < 0.25f) continue;
            Box b{std::clamp(static_cast<int>(std::lround(boxes[i * 4 + 1] * W)), 0, W - 1),
                  std::clamp(static_cast<int>(std::lround(boxes[i * 4] * H)), 0, H - 1),
                  std::clamp(static_cast<int>(std::lround(boxes[i * 4 + 3] * W)), 0, W - 1),
                  std::clamp(static_cast<int>(std::lround(boxes[i * 4 + 2] * H)), 0, H - 1)};
            if (b.x1 - b.x0 < 24 || b.y1 - b.y0 < 24) continue;
            ++checked;
            resize_rgb(frame.data(), W, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0,
                       crop.data(), C, C);
            if (TfLiteTensorCopyFromBuffer(cin, crop.data(), crop.size()) != kTfLiteOk ||
                TfLiteInterpreterInvoke(classifier.get()) != kTfLiteOk) return 1;
            const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(classifier.get(), 0);
            if (!out || TfLiteTensorType(out) != kTfLiteUInt8) return 1;
            std::vector<uint8_t> values(TfLiteTensorByteSize(out));
            if (values.size() <= TENNIS_BALL ||
                TfLiteTensorCopyToBuffer(out, values.data(), values.size()) != kTfLiteOk)
                return 1;
            int second = 0;
            for (size_t j = 0; j < values.size(); ++j)
                if (j != TENNIS_BALL) second = std::max(second, static_cast<int>(values[j]));
            if (values[TENNIS_BALL] >= second + 30 && values[TENNIS_BALL] >= 50) {
                draw_box(frame, b);
                std::fprintf(stderr, "frame=%lu ball box=%d,%d,%d,%d class=%u next=%d\n",
                             frames, b.x0, b.y0, b.x1, b.y1, values[TENNIS_BALL], second);
                found = true;
                break;
            }
        }
        if (!found) std::fprintf(stderr, "frame=%lu no ball proposals=%d\n", frames, checked);
        if (std::fwrite(frame.data(), 1, frame.size(), stdout) != frame.size()) return 0;
        std::fflush(stdout);
        ++frames;
        if (frames % 10 == 0) {
            auto end = std::chrono::steady_clock::now();
            std::fprintf(stderr, "frame=%lu last_processing_ms=%.1f\n", frames,
                         std::chrono::duration<double, std::milli>(end - start).count());
        }
    }
    return 0;
}
