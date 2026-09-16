// Experimental two-stage ball overlay for 640x360 RGB24 frames on stdin/stdout.
// CPU SSDLite proposes boxes; Teflon/etnaviv MobileNet verifies tennis ball.
// Preview output stays independent of the slower sampled inference worker.
// g++ -O2 -std=c++17 -pthread tflite-ball-stream.cc -ltensorflow-lite -o tflite-ball-stream
// tflite-ball-stream detector.tflite classifier.tflite < frames.rgb > boxed.rgb
#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

static uint8_t glyph(char ch, int row) {
    static constexpr uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };
    if (ch >= '0' && ch <= '9') return digits[ch - '0'][row];
    switch (ch) {
        case 'A': { static constexpr uint8_t a[] = {14,17,17,31,17,17,17}; return a[row]; }
        case 'B': { static constexpr uint8_t b[] = {30,17,17,30,17,17,30}; return b[row]; }
        case 'L': { static constexpr uint8_t l[] = {16,16,16,16,16,16,31}; return l[row]; }
        case '%': { static constexpr uint8_t p[] = {17,18,2,4,8,9,17}; return p[row]; }
        case '?': { static constexpr uint8_t q[] = {14,17,1,2,4,0,4}; return q[row]; }
        default: return 0;
    }
}

static void draw_tag(std::vector<uint8_t>& frame, Box b, int pct, bool top) {
    const std::string label = std::string(top ? "BALL " : "BALL? ") +
                              std::to_string(pct) + "%";
    constexpr int scale = 2, advance = 6 * scale, height = 7 * scale;
    const int width = static_cast<int>(label.size()) * advance + 6;
    int x = std::clamp(b.x0, 0, W - width);
    int y = b.y0 >= height + 8 ? b.y0 - height - 8 : std::min(H - height - 6, b.y1 + 4);
    for (int yy = y; yy < y + height + 6; ++yy)
        for (int xx = x; xx < x + width; ++xx) {
            uint8_t* p = &frame[(yy * W + xx) * 3];
            p[0] = 15; p[1] = 15; p[2] = 15;
        }
    for (size_t i = 0; i < label.size(); ++i)
        for (int gy = 0; gy < 7; ++gy)
            for (int gx = 0; gx < 5; ++gx)
                if (glyph(label[i], gy) & (1 << (4 - gx)))
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx) {
                            uint8_t* p = &frame[((y + 3 + gy * scale + dy) * W +
                                                 x + 3 + static_cast<int>(i) * advance + gx * scale + dx) * 3];
                            p[0] = 255; p[1] = 255; p[2] = 255;
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

    std::vector<uint8_t> frame(W * H * 3), latest(W * H * 3);
    std::mutex input_mutex, box_mutex;
    std::condition_variable ready;
    bool pending = false, stopping = false, has_box = false, current_top = false;
    int current_pct = 0;
    Box current_box{};
    std::chrono::steady_clock::time_point box_time{};
    std::atomic<bool> failed{false};
    std::thread worker([&] {
        std::vector<uint8_t> work(W * H * 3), small(D * D * 3), crop(C * C * 3);
        unsigned long analyzed = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(input_mutex);
                ready.wait(lock, [&] { return pending || stopping; });
                if (stopping) break;
                work = latest;
                pending = false;
            }
            auto start = std::chrono::steady_clock::now();
            resize_rgb(work.data(), W, 0, 0, W, H, small.data(), D, D);
            if (TfLiteTensorCopyFromBuffer(din, small.data(), small.size()) != kTfLiteOk ||
                TfLiteInterpreterInvoke(detector.get()) != kTfLiteOk) {
                failed = true; break;
            }
            auto boxes = result(detector.get(), 0), classes = result(detector.get(), 1);
            auto scores = result(detector.get(), 2);
            if (boxes.size() != scores.size() * 4 || classes.size() != scores.size()) {
                failed = true; break;
            }
            int checked = 0;
            int best_pct = -1, best_raw = -1;
            bool best_top = false;
            Box best_box{};
            size_t best_index = 0;
            for (size_t i = 0; i < scores.size() && checked < 6; ++i) {
                int cls = static_cast<int>(std::lround(classes[i]));
                // This ball is proposed as an apple by the COCO model. Ignore
                // unrelated boxes before asking the ImageNet model to verify.
                if (scores[i] < 0.25f || (cls != 52 && cls != 36)) continue;
                Box b{std::clamp(static_cast<int>(std::lround(boxes[i * 4 + 1] * W)), 0, W - 1),
                      std::clamp(static_cast<int>(std::lround(boxes[i * 4] * H)), 0, H - 1),
                      std::clamp(static_cast<int>(std::lround(boxes[i * 4 + 3] * W)), 0, W - 1),
                      std::clamp(static_cast<int>(std::lround(boxes[i * 4 + 2] * H)), 0, H - 1)};
                if (b.x1 - b.x0 < 24 || b.y1 - b.y0 < 24) continue;
                ++checked;
                resize_rgb(work.data(), W, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0,
                           crop.data(), C, C);
                if (TfLiteTensorCopyFromBuffer(cin, crop.data(), crop.size()) != kTfLiteOk ||
                    TfLiteInterpreterInvoke(classifier.get()) != kTfLiteOk) {
                    failed = true; break;
                }
                const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(classifier.get(), 0);
                if (!out || TfLiteTensorType(out) != kTfLiteUInt8) { failed = true; break; }
                std::vector<uint8_t> values(TfLiteTensorByteSize(out));
                if (values.size() <= TENNIS_BALL ||
                    TfLiteTensorCopyToBuffer(out, values.data(), values.size()) != kTfLiteOk) {
                    failed = true; break;
                }
                int raw = values[TENNIS_BALL];
                if (raw <= best_raw) continue;
                auto q = TfLiteTensorQuantizationParams(out);
                int pct = std::clamp(static_cast<int>(std::lround(
                    100.0f * (raw - q.zero_point) * q.scale)), 0, 100);
                auto top = std::max_element(values.begin(), values.end());
                best_raw = raw;
                best_pct = pct;
                best_box = b;
                best_top = (top - values.begin()) == TENNIS_BALL;
                best_index = i;
            }
            if (failed) break;
            if (best_pct >= 10) {
                {
                    std::lock_guard<std::mutex> lock(box_mutex);
                    current_box = best_box;
                    current_pct = best_pct;
                    current_top = best_top;
                    box_time = std::chrono::steady_clock::now();
                    has_box = true;
                }
                std::fprintf(stderr, "analysis=%lu ball proposal=%zu box=%d,%d,%d,%d score=%d%% top=%d\n",
                             analyzed, best_index, best_box.x0, best_box.y0, best_box.x1,
                             best_box.y1, best_pct, static_cast<int>(best_top));
            } else {
                std::fprintf(stderr, "analysis=%lu no ball proposals=%d best_score=%d%%\n",
                             analyzed, checked, std::max(best_pct, 0));
            }
            ++analyzed;
            if (analyzed % 10 == 0) {
                auto end = std::chrono::steady_clock::now();
                std::fprintf(stderr, "analysis=%lu last_processing_ms=%.1f\n", analyzed,
                             std::chrono::duration<double, std::milli>(end - start).count());
            }
        }
    });
    unsigned long frames = 0;
    while (!failed && read_frame(frame)) {
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            latest = frame;
            pending = true;
        }
        ready.notify_one();
        {
            std::lock_guard<std::mutex> lock(box_mutex);
            if (has_box && std::chrono::steady_clock::now() - box_time <
                               std::chrono::milliseconds(1500)) {
                draw_box(frame, current_box);
                draw_tag(frame, current_box, current_pct, current_top);
            }
        }
        if (std::fwrite(frame.data(), 1, frame.size(), stdout) != frame.size()) break;
        std::fflush(stdout);
        ++frames;
    }
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        stopping = true;
    }
    ready.notify_one();
    worker.join();
    std::fprintf(stderr, "stream finished frames=%lu failed=%d\n", frames, static_cast<int>(failed.load()));
    return failed ? 1 : 0;
}
