# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Real-Time Data (RTD) server for Excel that streams data from WebSocket servers. Written in C++ using ATL (Active Template Library) and built with CMake, it allows Excel to receive live data updates from WebSocket endpoints via COM automation. The implementation supports per-user COM registration (no admin rights required) and embeds a Type Library for IDispatch support.

Key features:
- WebSocket client using WinHTTP API
- Multi-topic support (multiple concurrent WebSocket subscriptions)
- JSON message parsing using nlohmann/json
- Thread-safe operation with proper STA marshaling

## Build System

This project uses CMake with Visual Studio 2022 as the generator. There are two CMake presets defined:
- `x64-debug`: Debug build for x64 architecture
- `x64-release`: Release build for x64 architecture

### Build Commands

**CRITICAL: The DLL bitness MUST match Excel's bitness (x64 Excel requires x64 DLL; 32-bit Excel requires 32-bit DLL).**

Configure and build (Release):
```bat
cmake --preset x64-release
cmake --build --preset build-x64-release
```

Configure and build (Debug):
```bat
cmake --preset x64-debug
cmake --build --preset build-x64-debug
```

Register the DLL (per-user, no admin):
```bat
cmake --build build --config Release --target register
```

Unregister the DLL:
```bat
cmake --build build --config Release --target unregister
```

Alternative registration (manual):
```bat
regsvr32 /n /i:user build\Release\MyRtd.dll
```

### Verify Registration

Check that the COM class is registered:
```bat
reg query HKCU\Software\Classes\MyCompany.RtdTickCPP /s
reg query HKCU\Software\Classes\CLSID\{C5D2C3F2-FA6B-4B3A-9B6E-7B8E07C54111}\InprocServer32
```

## Architecture

### COM Registration & Type Library

- **ProgID**: `MyCompany.RtdTickCPP`
- **CLSID**: `{C5D2C3F2-FA6B-4B3A-9B6E-7B8E07C54111}`
- **Threading Model**: Apartment (STA)
- **Registration**: Per-user via `DllInstall` (implemented in dllmain.cpp:21-26)

The build system uses MIDL (Microsoft Interface Definition Language) compiler to:
1. Generate C++ headers from `idl/TypeLibrary.idl`
2. Generate IID definitions (`RtdTickLib_i.c`)
3. Create a Type Library binary (`TypeLibrary.tlb`) that gets embedded in the DLL resource

### Key Components

**src/RtdTick.cpp**: The main RTD server implementation
- `RtdTick` class: Implements `IRtdServer` interface using ATL's `CComObjectRootEx` and `IDispatchImpl`
- `NotifyWindow` class: ATL window that receives WM_WEBSOCKET_DATA messages from worker threads
- Multi-topic support using `std::set<long>` to track active topic IDs
- Integrates with `WebSocketManager` for WebSocket connections

**src/WebSocketManager.h**: WebSocket client implementation
- `WebSocketManager` class: Manages WebSocket connections with connection pooling by URL
- `ConnectionData` struct: Per-URL connection state including handles, worker thread, and topic map
- `TopicSubscription` struct: Per-topic subscription with filter and cached value
- **Connection pooling**: Multiple Excel cells connecting to the same URL share a single WebSocket connection
- **Message routing**: Incoming messages are routed to all topics matching the filter
- Uses WinHTTP API for WebSocket protocol (upgrade from HTTP/HTTPS)
- Each unique URL runs on a dedicated worker thread
- Parses JSON messages using nlohmann/json library
- Thread-safe with mutex-protected connection and topic maps

**src/third_party/json.hpp**: Single-header JSON library (nlohmann/json v3.12.0)
- Used for parsing WebSocket messages in JSON format
- Supports flexible JSON structures: `{"topic": "...", "value": ...}` or just numeric values

**src/dllmain.cpp/dllmain.h**: ATL module and DLL entry points
- Defines `CRtdTickModule` ATL module
- Exports required COM functions: `DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`, `DllUnregisterServer`, `DllInstall`
- `DllInstall` handles per-user registration when called with `/i:user` flag

**idl/TypeLibrary.idl**: COM interface definitions
- `IRTDUpdateEvent`: Excel's callback interface (UUID: A43788C1-D91B-11D3-8F39-00C04F3651B8)
- `IRtdServer`: Standard RTD server interface (UUID: EC0E6191-DB51-11D3-8F3E-00C04F3651B8)
- `RtdTickLib`: Type library (UUID: 8E2A1E0A-2E4E-4B69-9C2B-3B9B0F2F1234)

**MyRtd.def**: Module definition file defining DLL exports

**res/RtdTick.rgs**: ATL registry script for COM registration

**res/RtdTick.rc.in**: Resource template configured by CMake to embed the TLB and RGS files

### Build Process Flow

1. CMake runs MIDL compiler on `idl/TypeLibrary.idl` → generates headers, IID file, and .tlb in `build/midl/`
2. CMake configures `res/RtdTick.rc.in` → generates `build/RtdTick_gen.rc` with absolute paths to .tlb and .rgs
3. C++ compilation includes MIDL-generated headers
4. Resource compiler embeds the Type Library into the DLL
5. Linker creates `MyRtd.dll` with proper exports defined in `MyRtd.def`

### RTD Server Lifecycle

1. **ServerStart**: Excel calls this, provides `IRTDUpdateEvent` callback, creates notification window for thread marshaling
2. **ConnectData**: Excel requests a topic with parameters (WebSocket URL and optional topic string)
   - Parses parameters from SAFEARRAY: `[ws://url, topic]`
   - Calls `WebSocketManager::Subscribe()` which spawns a worker thread
   - Worker thread establishes WebSocket connection via WinHTTP
   - Returns `VT_EMPTY` with `getNewValues=TRUE` to wait for data
3. **WebSocket worker thread**:
   - Upgrades HTTP connection to WebSocket protocol
   - Sends optional subscription message (JSON format)
   - Enters receive loop, parsing JSON messages
   - Caches parsed values in `TopicData::cachedValue`
   - Posts `WM_WEBSOCKET_DATA` to notification window
4. **RefreshData**: Notification window receives message → calls `UpdateNotify()` → Excel calls `RefreshData()`
   - Calls `WebSocketManager::GetAllNewData()` to collect all topics with new data
   - Builds 2D SAFEARRAY (rows=2, cols=number of updates): `[[topicId1, topicId2, ...], [value1, value2, ...]]`
   - Returns array to Excel
5. **DisconnectData/ServerTerminate**: Cleanup
   - Sets `shouldStop` flag on worker threads
   - Closes WebSocket handles
   - Joins worker threads
   - Destroys notification window

### Threading Model

- **COM STA Thread**: Main thread where `RtdTick` COM object lives (single-threaded apartment)
- **Worker Threads**: One per unique WebSocket URL (not per topic)
- **Thread Marshaling**: Worker threads post window messages (`WM_WEBSOCKET_DATA`) to the STA notification window
- **Synchronization**: `std::mutex` protects both the connection map and per-connection topic maps

### Connection Pooling

To optimize resource usage, the RTD server uses connection pooling:
- **Same URL = Same Connection**: Multiple Excel cells connecting to the same WebSocket URL share a single TCP connection
- **Example**: If you have 10 cells all connecting to `ws://localhost:8080/stream` with different topics (BTC, EURUSD, etc.), they all share one WebSocket connection
- **Message Routing**: When a message arrives, it's distributed to all subscribed topics that match the filter
- **Automatic Cleanup**: When the last topic unsubscribes from a URL, the connection is automatically closed

## Excel Usage

Set throttle interval (optional):
```vba
Application.RTD.ThrottleInterval = 1000
```

Use in cell formula:
```
=RTD("MyCompany.RtdTickCPP",, "ws://localhost:8080/stream", "EURUSD")
=RTD("MyCompany.RtdTickCPP",, "wss://api.example.com/prices", "BTC")
```

Formula parameters:
1. ProgID: `"MyCompany.RtdTickCPP"` (required)
2. Server name: Empty string `""` (required, leave empty for local server)
3. WebSocket URL: `"ws://host:port/path"` or `"wss://host:port/path"` (required)
4. Topic: String identifying the data stream (optional, depends on server implementation)

### WebSocket Message Format

The server expects JSON messages with the following structure:

**With topic filtering**:
```json
{"topic": "EURUSD", "value": 1.0945}
```

**Without topic** (value only):
```json
{"value": 42.5}
```

**Direct numeric value**:
```json
123.45
```

The `value` field can be a number or a string that can be parsed as a number.

## Key Implementation Details

- The `RtdTick` class uses `CComSingleThreadModel` (STA threading)
- The notification window is created on-demand in `ServerStart` and destroyed in cleanup
- `m_stopping` atomic flag prevents callbacks during shutdown
- Each `TopicData` has its own `shouldStop` flag for graceful thread shutdown
- The `RefreshData` method returns a 2D SAFEARRAY: rows=2 (topic IDs and values), cols=number of updates
- WebSocket URL parsing supports both `ws://` and `wss://` schemes with optional port numbers
- JSON parsing is flexible: supports objects with `topic` and `value` fields, or direct numeric values
- WinHTTP WebSocket API handles protocol upgrade, framing, and message fragmentation
- Registry script (RtdTick.rgs) uses `%MODULE%` placeholder for DLL path
- Type Library version is 1.0 (defined in IDL and referenced in `IDispatchImpl`)
- Dependencies: `ole32.lib`, `oleaut32.lib`, `uuid.lib`, `user32.lib`, `winhttp.lib`
