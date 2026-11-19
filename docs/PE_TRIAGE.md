# PE triage ÔÇö LoK.exe

| Property | Value |
|---|---|
| Size | 6764032 bytes |
| SHA-256 | `497eedaf07af54241ae2357fb9c57908e5947c2e547de4e1363e116ce9f4f683` |
| Machine | 0x14c |
| Subsystem | 2 |
| Image base | 0x400000 |
| Entry point | 0x77c3b7 |
| Timestamp | 1409794731 |
| DllCharacteristics | 0x8140 |
| Imports | 194 functions in 12 DLLs |
| TLS directory | False |
| Base relocations | RVA 0xac4000, 0x544c4 bytes |
| RTTI names (`.?AV`) | 1367 |

## Sections

| Name | VA | Virtual | Raw | Entropy | Characteristics |
|---|---|---|---|---|---|
| `.text` | 0x1000 | 0x4fb9ba | 0x4fba00 | 6.565 | 0x60000020 |
| `.rdata` | 0x4fd000 | 0xb3288 | 0xb3400 | 6.055 | 0x40000040 |
| `.data` | 0x5b1000 | 0x50e688 | 0x43400 | 3.827 | 0xc0000040 |
| `.rsrc` | 0xac0000 | 0x3590 | 0x3600 | 6.193 | 0x40000040 |
| `.reloc` | 0xac4000 | 0x7d952 | 0x7da00 | 5.304 | 0x42000040 |

## Imports

### DINPUT8.dll (1)

`DirectInput8Create`

### GDI32.dll (2)

`GetDeviceCaps`, `GetObjectA`

### KERNEL32.dll (121)

`CloseHandle`, `CompareStringW`, `CreateDirectoryA`, `CreateEventA`, `CreateEventW`, `CreateFileA`, `CreateFileW`, `CreateMutexA`, `CreateSemaphoreA`, `CreateThread`, `DecodePointer`, `DeleteCriticalSection`, `DeleteFileA`, `EncodePointer`, `EnterCriticalSection`, `ExitProcess`, `ExitThread`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileA`, `FindFirstFileExA`, `FindNextFileA`, `FlushFileBuffers`, `FreeEnvironmentStringsW`, `FreeLibrary`, `GetACP`, `GetCPInfo`, `GetCommandLineA`, `GetConsoleCP`, `GetConsoleMode`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCurrentThread`, `GetCurrentThreadId`, `GetEnvironmentStringsW`, `GetExitCodeThread`, `GetFileAttributesA`, `GetFileSize`, `GetFileType`, `GetLastError`, `GetModuleFileNameA`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetModuleHandleExW`, `GetModuleHandleW`, `GetOEMCP`, `GetProcAddress`, `GetProcessAffinityMask`, `GetProcessHeap`, `GetStartupInfoW`, `GetStdHandle`, `GetStringTypeW`, `GetSystemInfo`, `GetSystemTimeAsFileTime`, `GetThreadPriority`, `GetTickCount`, `GetTimeZoneInformation`, `GlobalAlloc`, `GlobalFree`, `GlobalSize`, `HeapAlloc`, `HeapCreate`, `HeapDestroy`, `HeapFree`, `HeapReAlloc`, `HeapSize`, `InitializeCriticalSection`, `InitializeCriticalSectionAndSpinCount`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedExchange`, `InterlockedExchangeAdd`, `InterlockedIncrement`, `IsDebuggerPresent`, `IsProcessorFeaturePresent`, `IsValidCodePage`, `LCMapStringW`, `LeaveCriticalSection`, `LoadLibraryExW`, `LoadLibraryW`, `MoveFileA`, `MultiByteToWideChar`, `OpenThread`, `OutputDebugStringW`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `RaiseException`, `ReadConsoleW`, `ReadFile`, `ReleaseMutex`, `ReleaseSemaphore`, `ResetEvent`, `ResumeThread`, `RtlUnwind`, `SetCriticalSectionSpinCount`, `SetCurrentDirectoryA`, `SetEndOfFile`, `SetEnvironmentVariableA`, `SetEvent`, `SetFilePointer`, `SetFilePointerEx`, `SetLastError`, `SetStdHandle`, `SetThreadAffinityMask`, `SetThreadPriority`, `SetUnhandledExceptionFilter`, `Sleep`, `SleepEx`, `SystemTimeToTzSpecificLocalTime`, `TerminateProcess`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `UnhandledExceptionFilter`, `VirtualQuery`, `WaitForMultipleObjectsEx`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteConsoleW`, `WriteFile`

### OLEAUT32.dll (3)

`OleLoadPicture`, `SysAllocString`, `SysFreeString`

### SHELL32.dll (1)

`SHGetSpecialFolderPathA`

### USER32.dll (41)

`AdjustWindowRect`, `BeginPaint`, `CallWindowProcW`, `ClientToScreen`, `ClipCursor`, `CreateWindowExA`, `DefWindowProcA`, `DestroyWindow`, `DispatchMessageA`, `EndPaint`, `EnumDisplaySettingsA`, `GetClientRect`, `GetCursorPos`, `GetDC`, `GetDesktopWindow`, `GetForegroundWindow`, `GetSystemMetrics`, `GetWindowLongA`, `GetWindowLongW`, `GetWindowRect`, `LoadCursorA`, `LoadIconA`, `MessageBoxA`, `MessageBoxW`, `PeekMessageA`, `PostQuitMessage`, `RegisterClassExA`, `RegisterDeviceNotificationW`, `ScreenToClient`, `SendMessageA`, `SetCursorPos`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowPos`, `ShowCursor`, `ShowWindow`, `SwapMouseButton`, `TranslateMessage`, `UnregisterClassA`, `UnregisterDeviceNotification`, `UpdateWindow`

### WINMM.dll (3)

`timeBeginPeriod`, `timeEndPeriod`, `timeGetTime`

### XINPUT1_3.dll (2)

`#2`, `#3`

### d3d9.dll (1)

`Direct3DCreate9`

### d3dx9_43.dll (4)

`D3DXCreateCubeTextureFromFileInMemoryEx`, `D3DXCreateTexture`, `D3DXCreateTextureFromFileInMemoryEx`, `D3DXGetShaderConstantTable`

### ole32.dll (8)

`CoCreateInstance`, `CoInitialize`, `CoInitializeEx`, `CoSetProxyBlanket`, `CoUninitialize`, `CreateStreamOnHGlobal`, `OleInitialize`, `OleUninitialize`

### steam_api.dll (7)

`SteamAPI_Init`, `SteamAPI_RegisterCallback`, `SteamAPI_RunCallbacks`, `SteamAPI_Shutdown`, `SteamAPI_UnregisterCallback`, `SteamUser`, `SteamUserStats`

## Debug / PDB

- type 2 ÔÇö `D:\project\PRJ_024\p1\common\project\build\exe\Win32\Release\App.pdb` GUID 206f9f8ba73e4507 age 1
- type 12

## Strings of interest

| String | Occurrences |
|---|---|
| `CRI File System` | 2 |
| `CRILAYLA` | 1 |
| `USM` | 1 |
| `HCA-MX` | 6 |
| `NcVoice` | 2 |
| `Wwise` | 14 |
| `Init.bnk` | 1 |
| `WwiseInfo` | 1 |
| `Direct3DCreate9` | 1 |
| `D3DXCreateTexture` | 2 |
| `XAudio2` | 14 |
| `CoCreateInstance` | 1 |
| `ProgressFrameRateType` | 1 |
| `DrawFrameRateType` | 1 |
| `MotionFrameRateType` | 1 |
| `framerate_d` | 1 |
| `framerate_n` | 1 |
| `PRJ_024` | 4 |
| `steam_api` | 1 |
| `Present` | 2 |

## Data directories

- IMAGE_DIRECTORY_ENTRY_IMPORT: VA 0x5aefd8 size 0x104
- IMAGE_DIRECTORY_ENTRY_RESOURCE: VA 0xac0000 size 0x3590
- IMAGE_DIRECTORY_ENTRY_BASERELOC: VA 0xac4000 size 0x544c4
- IMAGE_DIRECTORY_ENTRY_DEBUG: VA 0x4fe0f0 size 0x38
- IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG: VA 0x5817b8 size 0x40
- IMAGE_DIRECTORY_ENTRY_IAT: VA 0x4fd000 size 0x338

