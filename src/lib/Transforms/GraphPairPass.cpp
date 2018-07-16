//===- GraphPairPass.cpp --------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <onnc/Transforms/GraphPairPass.h>

using namespace onnc;

//===----------------------------------------------------------------------===//
// GraphPairPass
//===----------------------------------------------------------------------===//
GraphPairPass::GraphPairPass(char& pPassID)
  : ModulePass(pPassID) {
}

Pass::ReturnType GraphPairPass::runOnModule(::onnc::Module &pModule)
{
  if (!pModule.hasRootTensorGraph())
    return Pass::kModuleNoChanged;

  Pass::ReturnType result = 0x0;
  Module::tg_iterator tg, tEnd = pModule.tgEnd();
  for (tg = pModule.tgBegin(); tg != tEnd; ++tg) {
    ComputeGraph* cg = pModule.getComputeGraph(tg->value()->name());
    if (nullptr == cg)
      return Pass::kPassFailure;
    Pass::ReturnType gres = this->runOnGraphs(*tg->value(), *cg); 
    if (Pass::IsFailed(gres) || Pass::IsRetry(gres))
      return gres;
    result |= gres;
  }
  return result;
}
