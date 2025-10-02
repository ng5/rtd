#pragma once
#include <atlbase.h>
#include <atlcom.h>
#include "RtdTickLib_i.h"

class CRtdTickModule : public ATL::CAtlDllModuleT< CRtdTickModule >
{
public:
  DECLARE_LIBID(LIBID_RtdTickLib)
};
extern class CRtdTickModule _AtlModule;
