// FileLocksmith standalone - common includes / NT shims
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// NT status helpers.
// winternl.h on MSVC does not provide these; MinGW provides them only when its
// own ntstatus-ish section is active, so define them defensively.
// ---------------------------------------------------------------------------
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#ifndef NT_ERROR
#define NT_ERROR(Status) ((((ULONG)(Status)) >> 30) == 3)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

// SystemExtendedHandleInformation is not exposed by any public header.
static constexpr ULONG SystemExtendedHandleInformationClass = 64;
// OBJECT_INFORMATION_CLASS
static constexpr ULONG ObjectNameInformationClass = 1;
static constexpr ULONG ObjectTypeInformationClass = 2;
