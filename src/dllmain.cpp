#include "dllmain.h"
CRtdTickModule _AtlModule;

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved) {
  return _AtlModule.DllMain(reason, reserved);
}

STDAPI DllCanUnloadNow(void) {
  return _AtlModule.DllCanUnloadNow();
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}

// Default ATL registration (registers CLSID + embedded TypeLib)
STDAPI DllRegisterServer(void)   { return _AtlModule.DllRegisterServer(); }
STDAPI DllUnregisterServer(void) { return _AtlModule.DllUnregisterServer(); }

// Per-user installer entry point (regsvr32 /n /i:user MyRtd.dll)
STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine) {
  if (pszCmdLine && _wcsnicmp(pszCmdLine, L"user", 4) == 0) {
    AtlSetPerUserRegistration(true); // HKCU\Software\Classes
  }
  return bInstall ? DllRegisterServer() : DllUnregisterServer();
}
