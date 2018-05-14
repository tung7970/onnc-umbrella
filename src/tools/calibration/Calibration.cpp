//===- Calibration.cpp ----------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <onnc/ADT/Color.h>
#include <onnc/Core/ModulePass.h>
#include <onnc/IR/ONNXUtils.h>
#include <onnc/Support/IOStream.h>

#include "Calibration.h"
#include "Kld.h"

#include <fstream>
#include <streambuf>
#include <string>

#include <caffe2/onnx/backend.h>

using namespace onnc;
using namespace caffe2;

namespace {

void copyTensor(::onnx::Tensor &pDstTensor, const ::onnx::Tensor &pSrcTensor,
                const std::string &pValueName,
                const ::onnx::TensorProto_DataType pDataType)
{
  // copy sizes
  auto &newTensorSize = pDstTensor.sizes();
  newTensorSize = pSrcTensor.sizes();
  // copy dataType
  auto &newTensorType = pDstTensor.elem_type();
  newTensorType = pDataType;
  // copy segment info
  if (pSrcTensor.is_segment()) {
    pDstTensor.set_segment_begin_and_end(pSrcTensor.segment_begin(),
                                         pSrcTensor.segment_end());
  }
  // copy name
  if (pSrcTensor.hasName()) {
    pDstTensor.setName(pSrcTensor.name());
  }
}

template <class T>
void copyData2Tensor(::onnx::Tensor &pTensor, const std::vector<T> &pDataVector)
{
  // onnx does not support int8 tensor
  auto &tensorData = pTensor.int32s();
  tensorData.reserve(pDataVector.size());
  for (auto &data : pDataVector) {
    tensorData.push_back(data);
  }
}

} // anonymous namespace

namespace onnc {
static int getRightShift(Blob *pBlob, float pScale)
{
  int m = 0;
  auto tensor = pBlob->Get<TensorCPU>();
  if (tensor.IsType<float>()) {
    const auto &probs = tensor.data<float>();
    auto size = tensor.size();
    // Find max abs in the tensor.
    float max = *std::max_element(probs, probs + size);
    float min = *std::max_element(probs, probs + size);
    float abs_max = (std::abs(min) > max) ? std::abs(min) : max;
    float data_max = abs_max * pScale;
    if (data_max <= 0) {
      std::cout << "data_max = " << data_max << std::endl;
      throw std::runtime_error("data_max <= 0");
    }
    while (data_max < 64) {
      m += 1;
      data_max *= 2;
    }
    while (data_max >= 128) {
      std::cout << "data_max = " << data_max << std::endl;
      std::cout << "Error in quantize_right_shift: rshift will be negative..."
                << data_max << std::endl;
      exit(-1);
    }
    return m;
  } else {
    // Assert.
    throw std::runtime_error("Blob format is not float!");
  }
  return m;
}

template <class T> static inline T saturate(int pValue)
{
  const int32_t max = std::numeric_limits<T>::max();
  const int32_t min = std::numeric_limits<T>::min();

  T sValue = std::min(max, pValue);
  sValue = std::max(min, pValue);

  return sValue;
}

static bool isBlobUsed(const string &pBlobName, caffe2::NetDef &pDef,
                       const int pOpIdx)
{
  bool start = false;
  int opIndex = 0;
  for (const OperatorDef &op : pDef.op()) {
    for (const string &in : op.input()) {
      if (start && (in == pBlobName)) {
        return true;
      }
      if ((in == pBlobName) && (opIndex == pOpIdx)) {
        start = true;
      }
    }
    opIndex++;
  }
  return false;
}

void Calibration::quantizeWeight(Blob *pBlob, float pThresX, float pThresY,
                                 int pRightShift, string pName)
{
  auto tensor = pBlob->Get<TensorCPU>();
  int shiftScale = 1 << pRightShift;
  if (tensor.IsType<float>()) {
    const auto &probs = tensor.data<float>();
    auto size = tensor.size();
    m_QWeights[pName].reserve(size);

    for (int i = 0; i < tensor.size(); i++) {
      float fWeight =
          floor(probs[i] * ((pThresX / pThresY) * shiftScale) + 0.5);
      int8_t qWeight = saturate<int8_t>((int)fWeight);
      m_QWeights[pName].emplace_back(qWeight);
    }
  } else {
    throw std::runtime_error("Blob format is not float!");
  }
}

void Calibration::quantizeBias(Blob *pBlob, float pThresX, float pThresY,
                               int pRightShift, string pName)
{
  auto tensor = pBlob->Get<TensorCPU>();
  int shiftScale = 1 << pRightShift;
  if (tensor.IsType<float>()) {
    const auto &probs = tensor.data<float>();
    auto size = tensor.size();
    m_QBias[pName].reserve(size);

    for (int i = 0; i < tensor.size(); i++) {
      float fWeight =
          floor(probs[i] * ((pThresX / pThresY) * shiftScale) + 0.5);
      int16_t qWeight = saturate<int16_t>((int)fWeight);
      m_QBias[pName].emplace_back(qWeight);
    }
  } else {
    throw std::runtime_error("Blob format is not float!");
  }
}

bool Calibration::readDataset(TensorCPU *pInputTensor, const string &pDataLayer,
                              int pIteration)
{
  // FIXME: Can read from onnx?
  constexpr static TIndex batch = 1;
  constexpr static TIndex channel = 3;
  constexpr static TIndex height = 224;
  constexpr static TIndex width = 224;
  constexpr auto nums = batch * channel * height * width;
  std::vector<TIndex> inputDims({ batch, channel, height, width });

  // FIXME: Should read from lmdb. If fail then return false.
  for (int run = 0; run < pIteration; run++) {
    std::vector<float> data;
    for (int i = 0; i < nums; i++) {
      float value = run * -100;
      data.emplace_back(value);
    }
    TensorCPU tensor(inputDims, data, nullptr);
    pInputTensor->ResizeLike(tensor);
    pInputTensor->ShareData(tensor);
    m_BlobData[pDataLayer].emplace_back(tensor);
  }

  return true;
}

void Calibration::updateQuantizeWeight(::onnx::Graph *pGraph)
{
  // update elemType
  for (auto input : pGraph->inputs()) {
    auto elemType = input->elemType();
    if (elemType == ::onnx::TensorProto_DataType_FLOAT) {
      auto name = input->uniqueName();
      if (m_QWeights.count(name)) {
        input->setElemType(::onnx::TensorProto_DataType_INT8);
      } else if (m_QBias.count(name)) {
        input->setElemType(::onnx::TensorProto_DataType_INT16);
      } else {
        // input data default is INT8
        input->setElemType(::onnx::TensorProto_DataType_INT8);
      }
    } else {
      std::cout << "unsupported quantize type:"
                << TensorProto_DataType_Name(elemType) << std::endl;
      assert(0);
    }
  }

  // update Tensor
  std::unordered_map<std::string, ::onnx::Tensor> valueTensorMap;
  const std::vector< ::onnx::Tensor> initTensors = pGraph->initializers();
  const std::vector<std::string> tensorNames = pGraph->initializer_names();
  for (size_t i = 0; i < initTensors.size(); ++i) {
    auto valueName = tensorNames[i];
    auto oldTensor = initTensors[i];
    if (1 == m_QWeights.count(valueName)) {
      ::onnx::Tensor newTensor;
      copyTensor(newTensor, oldTensor, valueName,
                 ::onnx::TensorProto_DataType_INT8);
      assert(m_QWeights[valueName].size() == getTotalCount(oldTensor.sizes()));
      copyData2Tensor(newTensor, m_QWeights[valueName]);
      valueTensorMap.emplace(valueName, newTensor);
      continue;
    } else if (1 == m_QBias.count(valueName)) {
      ::onnx::Tensor newTensor;
      copyTensor(newTensor, oldTensor, valueName,
                 ::onnx::TensorProto_DataType_INT16);
      assert(m_QBias[valueName].size() == getTotalCount(oldTensor.sizes()));
      copyData2Tensor(newTensor, m_QBias[valueName]);
      valueTensorMap.emplace(valueName, newTensor);
      continue;
    }
    assert(0);
  }

  pGraph->clearInitializers();
  for (auto &kv : valueTensorMap) {
    pGraph->addInitializer(kv.second, kv.first);
  }
}

float Calibration::calculateKLD(const string &pBlobName)
{
  int iteration = m_BlobData[pBlobName].size();
  int dataSize = 0;
  vector<float> dataCollect;
  // Count memory size to reserve.
  for (int i = 0; i < iteration; i++) {
    dataSize += m_BlobData[pBlobName][i].size();
  }
  dataCollect.reserve(dataSize);
  // Concat all batches' tensor.
  for (int i = 0; i < iteration; i++) {
    auto probs = m_BlobData[pBlobName][i].data<float>();
    dataCollect.insert(dataCollect.end(), probs,
                       probs + m_BlobData[pBlobName][i].size());
  }
  return KLDiversity(dataCollect.data(), dataCollect.size());
}

void Calibration::profileModel(int pIteration, caffe2::NetDef &pDef,
                               const string &pDataLayer)
{
  // Calculate "data layer" KLD.
  m_ThresholdY[pDataLayer] = calculateKLD(pDataLayer);

  int opIdx = 0;
  for (const OperatorDef &op : pDef.op()) {
    for (int run = 0; run < pIteration; run++) {
      // Prepare input blobs.
      for (const string &in : op.input()) {
        // Only feed data to input. (no needs to feed weights)
        if (m_BlobData.find(in) != m_BlobData.end()) {
          // Feed data from previous layer.
          auto input = m_Workspace->GetBlob(in)->GetMutable<TensorCPU>();
          input->ResizeLike(m_BlobData[in][run]);
          input->ShareData(m_BlobData[in][run]);
        }
      }
      CAFFE_ENFORCE(m_Workspace->RunOperatorOnce(op));
      // Save output blobs.
      for (const string &in : op.output()) {
        auto blob = m_Workspace->GetBlob(in);
        // Special case: Skip Dropout layer.
        // FIXME: Remove. Dropout layer should be deleted in layer-opt. phase.
        if (string(blob->TypeName()) != "nullptr (uninitialized)") {
          CPUContext cpuContext;
          TensorCPU output(m_Workspace->GetBlob(in)->Get<TensorCPU>(),
                           &cpuContext);
          m_BlobData[in].emplace_back(output);
        }
      }
    }
    // Calculate KLD. Save results in threshold_y.
    for (const string &in : op.output()) {
      auto blob = m_Workspace->GetBlob(in);
      // FIXME: Remove. Dropout layer should be deleted in layer-opt. phase.
      if (string(blob->TypeName()) != "nullptr (uninitialized)" &&
          blob->Get<TensorCPU>().IsType<float>()) {
        m_ThresholdY[in] = calculateKLD(in);
      }
    }
    // FIXME: Needs to check this input is unused anymore...
    // Free unused input blobs for saving memory.
    for (const string &in : op.input()) {
      if (m_BlobData.find(in) != m_BlobData.end()) {
        if (!isBlobUsed(in, pDef, opIdx)) {
          m_BlobData.erase(in);
        }
      }
    }
    opIdx++;
  }
}

#include "LayerImpl.h"
void Calibration::getRightShiftQuantize(caffe2::NetDef &pDef)
{
  for (const OperatorDef &op : pDef.op()) {
    // FIXME: caffe2 seems no layer name... maybe set "op.type()" + "idx".
    // layerCalibrationParam.set_name =
    if (op.type() == "Conv" || op.type() == "FC" || op.type() == "Scale") {
      Conv(op, pDef);
    }
    // TODO: Add other layers.
  }
}

Pass::ReturnType Calibration::runOnModule(Module &pModule)
{
  // TODO: Check If Ctable exist, then skip this pass.
  // FIXME: Sould be specified by user.
  constexpr int iteration = 5;

  std::string onnxStr;
  ::onnc::SerializeToString(onnxStr, pModule);

  // Create caffe2 backend.
  auto *backend = new caffe2::onnx::Caffe2Backend(nullptr);
  std::vector<caffe2::onnx::Caffe2Ops> extras;
  auto rep = backend->Prepare(onnxStr, "CPU", extras);
  auto def = rep->pred_net();

  // Init net weights.
  m_Workspace = new Workspace("layer_calibration");
  CAFFE_ENFORCE(m_Workspace->RunNetOnce(rep->init_net()));

  // Find data layer's name.
  const OperatorDef &op = def.op(0);
  const string &dataLayer = op.input(0);
  auto inputTensor =
      m_Workspace->CreateBlob(dataLayer)->GetMutable<TensorCPU>();
  if (!readDataset(inputTensor, dataLayer, iteration)) {
    errs() << Color::RED << "Error" << Color::RESET << ": Read data set fail..."
           << std::endl;
    return kModuleNoChanged;
  }

  // Run inference and calculate KLD.
  profileModel(iteration, def, dataLayer);

  // TODO: Optimize threshold.

  // Caliculate right-shift each layer and Quantize weights.
  getRightShiftQuantize(def);

  std::cout << m_NetCtableParam.DebugString() << std::endl;
  // write ctable
  pModule.getMetaData().insert(
      { "bm1880_ctable", m_NetCtableParam.DebugString() });
  // write qWeights
  updateQuantizeWeight(pModule.getGraphIR().get());
  return kModuleChanged;
}
} // namespace onnc

char Calibration::ID = 0;
ModulePass *onnc::createCalibrationPass() { return new Calibration(); }