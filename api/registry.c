/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2021 WireGuard LLC. All Rights Reserved.
 */

#include "logger.h"
#include "registry.h"
#include <Windows.h>
#include <wchar.h>
#include <stdlib.h>
#include <strsafe.h>

_Must_inspect_result_
static _Return_type_success_(return != NULL)
_Post_maybenull_
HKEY
OpenKeyWait(_In_ HKEY Key, _Inout_z_ LPWSTR Path, _In_ DWORD Access, _In_ ULONGLONG Deadline)
{
    DWORD LastError;
    LPWSTR PathNext = wcschr(Path, L'\\');
    if (PathNext)
        *PathNext = 0;

    HANDLE Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Event)
    {
        LOG_LAST_ERROR(L"Failed to create event");
        return NULL;
    }
    for (;;)
    {
        LastError = RegNotifyChangeKeyValue(Key, FALSE, REG_NOTIFY_CHANGE_NAME, Event, TRUE);
        if (LastError != ERROR_SUCCESS)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG_ERROR(LastError, L"Failed to setup registry key %.*s notification", MAX_REG_PATH, RegPath);
            break;
        }

        HKEY Subkey;
        LastError = RegOpenKeyExW(Key, Path, 0, PathNext ? KEY_NOTIFY : Access, &Subkey);
        if (LastError == ERROR_SUCCESS)
        {
            if (PathNext)
            {
                HKEY KeyOut = OpenKeyWait(Subkey, PathNext + 1, Access, Deadline);
                if (KeyOut)
                {
                    RegCloseKey(Subkey);
                    CloseHandle(Event);
                    return KeyOut;
                }
                LastError = GetLastError();
                break;
            }
            else
            {
                CloseHandle(Event);
                return Subkey;
            }
        }
        if (LastError != ERROR_FILE_NOT_FOUND && LastError != ERROR_PATH_NOT_FOUND)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG_ERROR(LastError, L"Failed to open registry key %.*s\\%s", MAX_REG_PATH, RegPath, Path);
            break;
        }

        LONGLONG TimeLeft = Deadline - GetTickCount64();
        if (TimeLeft < 0)
            TimeLeft = 0;
        DWORD Result = WaitForSingleObject(Event, (DWORD)TimeLeft);
        if (Result != WAIT_OBJECT_0)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG(WIREGUARD_LOG_ERR,
                L"Timeout waiting for registry key %.*s\\%s (status: 0x%x)",
                MAX_REG_PATH,
                RegPath,
                Path,
                Result);
            break;
        }
    }
    CloseHandle(Event);
    SetLastError(LastError);
    return NULL;
}

_Use_decl_annotations_
HKEY
RegistryOpenKeyWait(HKEY Key, LPCWSTR Path, DWORD Access, DWORD Timeout)
{
    WCHAR Buf[MAX_REG_PATH];
    if (wcsncpy_s(Buf, _countof(Buf), Path, _TRUNCATE) == STRUNCATE)
    {
        LOG(WIREGUARD_LOG_ERR, L"Registry path too long: %s", Path);
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return OpenKeyWait(Key, Buf, Access, GetTickCount64() + Timeout);
}

_Use_decl_annotations_
BOOL
RegistryGetString(LPWSTR *Buf, DWORD Len, DWORD ValueType)
{
    if (wcsnlen(*Buf, Len) >= Len)
    {
        /* String is missing zero-terminator. */
        LPWSTR BufZ = ReZallocArray(*Buf, (SIZE_T)Len + 1, sizeof(*BufZ));
        if (!BufZ)
            return FALSE;
        _Analysis_assume_((wmemset(BufZ, L'A', (SIZE_T)Len + 1), TRUE));
        *Buf = BufZ;
    }

    if (ValueType != REG_EXPAND_SZ)
        return TRUE;

    /* ExpandEnvironmentStringsW() returns strlen on success or 0 on error. Bail out on empty input strings to
     * disambiguate. */
    if (!(*Buf)[0])
        return TRUE;

    for (;;)
    {
        LPWSTR Expanded = AllocArray(Len, sizeof(*Expanded));
        if (!Expanded)
            return FALSE;
        DWORD Result = ExpandEnvironmentStringsW(*Buf, Expanded, Len);
        if (!Result)
        {
            LOG_LAST_ERROR(L"Failed to expand environment variables: %s", *Buf);
            Free(Expanded);
            return FALSE;
        }
        if (Result > Len)
        {
            Free(Expanded);
            Len = Result;
            continue;
        }
        Free(*Buf);
        *Buf = Expanded;
        return TRUE;
    }
}

_Use_decl_annotations_
BOOL
RegistryGetMultiString(PZZWSTR *Buf, DWORD Len, DWORD ValueType)
{
    if (ValueType == REG_MULTI_SZ)
    {
        for (size_t i = 0;; i += wcsnlen(*Buf + i, Len - i) + 1)
        {
            if (i > Len)
            {
                /* Missing string and list terminators. */
                PZZWSTR BufZ = ReZallocArray(*Buf, (SIZE_T)Len + 2, sizeof(*BufZ));
                if (!BufZ)
                    return FALSE;
                *Buf = BufZ;
                return TRUE;
            }
            if (i == Len)
            {
                /* Missing list terminator. */
                PZZWSTR BufZ = ReZallocArray(*Buf, (SIZE_T)Len + 1, sizeof(*BufZ));
                if (!BufZ)
                    return FALSE;
                *Buf = BufZ;
                return TRUE;
            }
            if (!(*Buf)[i])
                return TRUE;
        }
    }

    /* Sanitize REG_SZ/REG_EXPAND_SZ and append a list terminator to make a multi-string. */
    if (!RegistryGetString(Buf, Len, ValueType))
        return FALSE;
    Len = (DWORD)wcslen(*Buf) + 1;
    PZZWSTR BufZ = ReZallocArray(*Buf, (SIZE_T)Len + 1, sizeof(WCHAR));
    if (!BufZ)
        return FALSE;
    *Buf = BufZ;
    return TRUE;
}

_Must_inspect_result_
static _Return_type_success_(return != NULL)
_Post_maybenull_
_Post_writable_byte_size_(*BufLen)
VOID *
RegistryQuery(_In_ HKEY Key, _In_opt_z_ LPCWSTR Name, _Out_opt_ DWORD *ValueType, _Inout_ DWORD *BufLen, _In_ BOOL Log)
{
    for (;;)
    {
        BYTE *p = Alloc(*BufLen);
        if (!p)
            return NULL;
        LSTATUS LastError = RegQueryValueExW(Key, Name, NULL, ValueType, p, BufLen);
        if (LastError == ERROR_SUCCESS)
            return p;
        Free(p);
        if (LastError != ERROR_MORE_DATA)
        {
            if (Log)
            {
                WCHAR RegPath[MAX_REG_PATH];
                LoggerGetRegistryKeyPath(Key, RegPath);
                LOG_ERROR(LastError, L"Failed to query registry value %.*s\\%s", MAX_REG_PATH, RegPath, Name);
            }
            SetLastError(LastError);
            return NULL;
        }
    }
}

_Use_decl_annotations_
LPWSTR
RegistryQueryString(HKEY Key, LPCWSTR Name, BOOL Log)
{
    DWORD LastError, ValueType, Size = 256 * sizeof(WCHAR);
    LPWSTR Value = RegistryQuery(Key, Name, &ValueType, &Size, Log);
    if (!Value)
        return NULL;
    switch (ValueType)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ:
        if (RegistryGetString(&Value, Size / sizeof(*Value), ValueType))
            return Value;
        LastError = GetLastError();
        break;
    default: {
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LOG(WIREGUARD_LOG_ERR,
            L"Registry value %.*s\\%s is not a string (type: %u)",
            MAX_REG_PATH,
            RegPath,
            Name,
            ValueType);
        LastError = ERROR_INVALID_DATATYPE;
    }
    }
    Free(Value);
    SetLastError(LastError);
    return NULL;
}

_Use_decl_annotations_
LPWSTR
RegistryQueryStringWait(HKEY Key, LPCWSTR Name, DWORD Timeout)
{
    DWORD LastError;
    ULONGLONG Deadline = GetTickCount64() + Timeout;
    HANDLE Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Event)
    {
        LOG_LAST_ERROR(L"Failed to create event");
        return NULL;
    }
    for (;;)
    {
        LastError = RegNotifyChangeKeyValue(Key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, Event, TRUE);
        if (LastError != ERROR_SUCCESS)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG_ERROR(LastError, L"Failed to setup registry key %.*s notification", MAX_REG_PATH, RegPath);
            break;
        }
        LPWSTR Value = RegistryQueryString(Key, Name, FALSE);
        if (Value)
        {
            CloseHandle(Event);
            return Value;
        }
        LastError = GetLastError();
        if (LastError != ERROR_FILE_NOT_FOUND && LastError != ERROR_PATH_NOT_FOUND)
            break;
        LONGLONG TimeLeft = Deadline - GetTickCount64();
        if (TimeLeft < 0)
            TimeLeft = 0;
        DWORD Result = WaitForSingleObject(Event, (DWORD)TimeLeft);
        if (Result != WAIT_OBJECT_0)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG(WIREGUARD_LOG_ERR,
                L"Timeout waiting for registry value %.*s\\%s (status: 0x%x)",
                MAX_REG_PATH,
                RegPath,
                Name,
                Result);
            break;
        }
    }
    CloseHandle(Event);
    SetLastError(LastError);
    return NULL;
}

_Use_decl_annotations_
BOOL
RegistryQueryDWORD(HKEY Key, LPCWSTR Name, DWORD *Value, BOOL Log)
{
    DWORD ValueType, Size = sizeof(DWORD);
    DWORD LastError = RegQueryValueExW(Key, Name, NULL, &ValueType, (BYTE *)Value, &Size);
    if (LastError != ERROR_SUCCESS)
    {
        if (Log)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG_ERROR(LastError, L"Failed to query registry value %.*s\\%s", MAX_REG_PATH, RegPath, Name);
        }
        SetLastError(LastError);
        return FALSE;
    }
    if (ValueType != REG_DWORD)
    {
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LOG(WIREGUARD_LOG_ERR, L"Value %.*s\\%s is not a DWORD (type: %u)", MAX_REG_PATH, RegPath, Name, ValueType);
        SetLastError(ERROR_INVALID_DATATYPE);
        return FALSE;
    }
    if (Size != sizeof(DWORD))
    {
        WCHAR RegPath[MAX_REG_PATH];
        LoggerGetRegistryKeyPath(Key, RegPath);
        LOG(WIREGUARD_LOG_ERR, L"Value %.*s\\%s size is not 4 bytes (size: %u)", MAX_REG_PATH, RegPath, Name, Size);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    return TRUE;
}

_Use_decl_annotations_
BOOL
RegistryQueryDWORDWait(HKEY Key, LPCWSTR Name, DWORD Timeout, DWORD *Value)
{
    DWORD LastError;
    ULONGLONG Deadline = GetTickCount64() + Timeout;
    HANDLE Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Event)
    {
        LOG_LAST_ERROR(L"Failed to create event");
        return FALSE;
    }
    for (;;)
    {
        LastError = RegNotifyChangeKeyValue(Key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, Event, TRUE);
        if (LastError != ERROR_SUCCESS)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG_ERROR(LastError, L"Failed to setup registry key %.*s notification", MAX_REG_PATH, RegPath);
            break;
        }
        if (RegistryQueryDWORD(Key, Name, Value, FALSE))
        {
            CloseHandle(Event);
            return TRUE;
        }
        LastError = GetLastError();
        if (LastError != ERROR_FILE_NOT_FOUND && LastError != ERROR_PATH_NOT_FOUND)
            break;
        LONGLONG TimeLeft = Deadline - GetTickCount64();
        if (TimeLeft < 0)
            TimeLeft = 0;
        DWORD Result = WaitForSingleObject(Event, (DWORD)TimeLeft);
        if (Result != WAIT_OBJECT_0)
        {
            WCHAR RegPath[MAX_REG_PATH];
            LoggerGetRegistryKeyPath(Key, RegPath);
            LOG(WIREGUARD_LOG_ERR,
                L"Timeout waiting registry value %.*s\\%s (status: 0x%x)",
                MAX_REG_PATH,
                RegPath,
                Name,
                Result);
            break;
        }
    }
    CloseHandle(Event);
    SetLastError(LastError);
    return FALSE;
}
