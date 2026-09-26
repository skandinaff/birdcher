// Extract the first N operators of a single-subgraph TFLite model.
// Usage: tflite-prefix MODEL N OUTPUT.tflite
// Build: g++ -O2 -std=c++17 tflite-prefix.cc -o tflite-prefix
// Requires TFLite schema and FlatBuffers headers. No runtime library needed.
// Retains original tensors/weights and declares the last operator's outputs.
// Changing graph boundaries can change delegate lowering; corroborate with dumps.
#include <tensorflow/lite/schema/schema_generated.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <cstdlib>
#include <cstdio>
int main(int argc, char** argv) {
 if (argc != 4) return 2;
 std::ifstream in(argv[1], std::ios::binary);
 std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
 flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
 if (!tflite::VerifyModelBuffer(verifier)) return 1;
 auto model = tflite::UnPackModel(bytes.data());
 if (model->subgraphs.size()!=1) return 1;
 auto& graph=*model->subgraphs[0];
 char* end; long count=strtol(argv[2],&end,10);
 if (*end || count<1 || size_t(count)>graph.operators.size()) return 2;
 graph.outputs=graph.operators[count-1]->outputs;
 graph.operators.resize(count);
 model->signature_defs.clear();
 flatbuffers::FlatBufferBuilder builder;
 tflite::FinishModelBuffer(builder,tflite::Model::Pack(builder,model.get()));
 std::ofstream out(argv[3],std::ios::binary);
 out.write(reinterpret_cast<const char*>(builder.GetBufferPointer()),builder.GetSize());
 printf("prefix=%ld output=%d\n",count,graph.outputs[0]);
 return out ? 0 : 1;
}
