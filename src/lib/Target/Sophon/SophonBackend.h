//===- SophonBackend.h ----------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef TARGET_SOPHON_SOPHON_BACKEND_H
#define TARGET_SOPHON_SOPHON_BACKEND_H
#include <string>
#include <onnc/Target/DLATargetBackend.h>

namespace onnc {

class SophonBackend : public DLATargetBackend
{
public:
  SophonBackend(const CompilerConfig& pConfig);

  virtual ~SophonBackend();
};

class BM1680Backend : public SophonBackend
{
public:
  BM1680Backend(const CompilerConfig& pConfig);

  virtual ~BM1680Backend();
};

class BM1682Backend : public SophonBackend
{
public:
  BM1682Backend(const CompilerConfig& pConfig);

  virtual ~BM1682Backend();
};

}  // namespace onnc

#endif  // TARGET_SOPHON_SOPHON_BACKEND_H