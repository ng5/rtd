#pragma once
#include "RtdTickLib_i.h"
#include <atlbase.h>
#include <atlcom.h>

class CRtdTickModule : public CAtlDllModuleT<CRtdTickModule> {
  public:
    DECLARE_LIBID(LIBID_RtdTickLib)
};
extern CRtdTickModule _AtlModule;
