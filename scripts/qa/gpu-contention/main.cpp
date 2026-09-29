#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
  if (seconds < 1 || seconds > 600) return 2;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level{};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  if (FAILED(hr) || level < D3D_FEATURE_LEVEL_11_0) return 3;

  constexpr char shaderSource[] = R"(
    RWStructuredBuffer<float> output : register(u0);
    [numthreads(8, 8, 1)]
    void main(uint3 id : SV_DispatchThreadID) {
      uint index = id.y * 2048 + id.x;
      float x = (index + 1) * 0.00001;
      [loop] for (uint i = 0; i < 128; ++i) {
        x = sin(x * 1.01 + i * 0.001) * cos(x + i * 0.002) + 0.7;
      }
      output[index] = x;
    }
  )";
  ComPtr<ID3DBlob> bytecode;
  ComPtr<ID3DBlob> errors;
  hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
                  "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                  &bytecode, &errors);
  if (FAILED(hr)) {
    if (errors) std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
                             static_cast<const char*>(errors->GetBufferPointer()));
    return 4;
  }
  ComPtr<ID3D11ComputeShader> shader;
  hr = device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                   nullptr, &shader);
  if (FAILED(hr)) return 5;

  D3D11_BUFFER_DESC bufferDesc{};
  bufferDesc.ByteWidth = 2048 * 2048 * sizeof(float);
  bufferDesc.Usage = D3D11_USAGE_DEFAULT;
  bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bufferDesc.StructureByteStride = sizeof(float);
  ComPtr<ID3D11Buffer> buffer;
  hr = device->CreateBuffer(&bufferDesc, nullptr, &buffer);
  if (FAILED(hr)) return 6;

  D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
  viewDesc.Format = DXGI_FORMAT_UNKNOWN;
  viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  viewDesc.Buffer.NumElements = 2048 * 2048;
  ComPtr<ID3D11UnorderedAccessView> view;
  hr = device->CreateUnorderedAccessView(buffer.Get(), &viewDesc, &view);
  if (FAILED(hr)) return 7;

  D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
  ComPtr<ID3D11Query> query;
  hr = device->CreateQuery(&queryDesc, &query);
  if (FAILED(hr)) return 8;

  ID3D11UnorderedAccessView* rawView = view.Get();
  context->CSSetUnorderedAccessViews(0, 1, &rawView, nullptr);
  context->CSSetShader(shader.Get(), nullptr, 0);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  uint64_t dispatches = 0;
  while (std::chrono::steady_clock::now() < end) {
    context->Dispatch(256, 256, 1);
    context->End(query.Get());
    context->Flush();
    while (context->GetData(query.Get(), nullptr, 0, 0) == S_FALSE) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++dispatches;
  }
  std::printf("D3D11 contention: %llu dispatches in %d seconds\n",
              static_cast<unsigned long long>(dispatches), seconds);
  return 0;
}
