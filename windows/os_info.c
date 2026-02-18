/*------------------------------------------------------------------------
 * os_info.c
 *              Operating system information
 *
 * Copyright (c) 2020, EnterpriseDB Corporation. All Rights Reserved.
 *
 *------------------------------------------------------------------------
 */

#include "postgres.h"
#include "system_stats.h"

#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ntdll.lib")

/* Function pointer type for RtlGetVersion */
typedef NTSTATUS (WINAPI *RtlGetVersionFunc)(PRTL_OSVERSIONINFOW);

/* Helper function to get OS friendly name from registry */
static bool get_os_friendly_name(char *buffer, size_t bufsize);

/* Helper function to convert FILETIME to readable string */
static void format_boot_time(ULONGLONG uptimeMs, char *buffer, size_t bufsize);

/* Static cache for OS information that doesn't change during runtime */
typedef struct {
	bool initialized;
	char os_name[256];
	bool os_name_null;
	char os_version[128];
	bool os_version_null;
	char hostname[256];
	bool hostname_null;
	char architecture[64];
	bool architecture_null;
	char boot_time[128];
	bool boot_time_null;
} OSInfoCache;

static OSInfoCache os_cache = {false};

/* Get OS friendly name from Windows registry */
static bool get_os_friendly_name(char *buffer, size_t bufsize)
{
	HKEY hKey;
	DWORD dwType = REG_SZ;
	DWORD dwSize = (DWORD)bufsize;
	LONG result;

	memset(buffer, 0, bufsize);

	/* Open registry key for Windows version info */
	result = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
						   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
						   0,
						   KEY_READ,
						   &hKey);

	if (result != ERROR_SUCCESS)
	{
		ereport(DEBUG1, (errmsg("Failed to open registry key for OS name")));
		return false;
	}

	/* Try to read ProductName first (most descriptive) */
	result = RegQueryValueExA(hKey, "ProductName", NULL, &dwType,
							  (LPBYTE)buffer, &dwSize);

	if (result != ERROR_SUCCESS || dwSize == 0)
	{
		/* Fallback to DisplayVersion or ReleaseId */
		dwSize = (DWORD)bufsize;
		result = RegQueryValueExA(hKey, "DisplayVersion", NULL, &dwType,
								  (LPBYTE)buffer, &dwSize);
	}

	RegCloseKey(hKey);

	return (result == ERROR_SUCCESS && strlen(buffer) > 0);
}

/* Format boot time as a readable string (WMI format for compatibility) */
static void format_boot_time(ULONGLONG uptimeMs, char *buffer, size_t bufsize)
{
	FILETIME currentFt;
	ULARGE_INTEGER currentTime, bootTime;
	SYSTEMTIME bootSt, localSt;
	TIME_ZONE_INFORMATION tzi;

	/* Get current time as FILETIME */
	GetSystemTimeAsFileTime(&currentFt);

	currentTime.LowPart = currentFt.dwLowDateTime;
	currentTime.HighPart = currentFt.dwHighDateTime;

	/* Subtract uptime to get boot time (uptime is in ms, FILETIME is in 100ns intervals) */
	bootTime.QuadPart = currentTime.QuadPart - (uptimeMs * 10000ULL);

	/* Convert to SYSTEMTIME */
	currentFt.dwLowDateTime = bootTime.LowPart;
	currentFt.dwHighDateTime = bootTime.HighPart;

	if (FileTimeToSystemTime(&currentFt, &bootSt))
	{
		/* Convert to local time for display */
		if (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID &&
			SystemTimeToTzSpecificLocalTime(&tzi, &bootSt, &localSt))
		{
			/* Format as YYYYMMDDHHmmss.mmmmmm+offset (WMI CIM_DATETIME format) */
			snprintf(buffer, bufsize, "%04d%02d%02d%02d%02d%02d.000000%+03d0",
					 localSt.wYear, localSt.wMonth, localSt.wDay,
					 localSt.wHour, localSt.wMinute, localSt.wSecond,
					 -(tzi.Bias / 60));
		}
		else
		{
			/* Fallback to UTC if timezone conversion fails */
			snprintf(buffer, bufsize, "%04d%02d%02d%02d%02d%02d.000000+000",
					 bootSt.wYear, bootSt.wMonth, bootSt.wDay,
					 bootSt.wHour, bootSt.wMinute, bootSt.wSecond);
		}
	}
}

void ReadOSInformations(Tuplestorestate *tupstore, TupleDesc tupdesc)
{
	Datum            values[Natts_os_info];
	bool             nulls[Natts_os_info];
	int              handle_count = 0;
	int              process_count = 0;
	int              thread_count = 0;

	memset(nulls, 0, sizeof(nulls));

	/* Always query dynamic performance data */
	PERFORMANCE_INFORMATION per_statex;
	per_statex.cb = sizeof(per_statex);
	if (GetPerformanceInfo(&per_statex, per_statex.cb) == 0)
	{
		LPVOID lpMsgBuf;
		DWORD dw = GetLastError();
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpMsgBuf, 0, NULL);
		ereport(DEBUG1, (errmsg("Error while getting memory peformance information: %s", (char *)lpMsgBuf)));
		LocalFree(lpMsgBuf);
	}
	else
	{
		handle_count = (int)(per_statex.HandleCount);
		process_count = (int)(per_statex.ProcessCount);
		thread_count = (int)(per_statex.ThreadCount);
	}

	/* Check if static OS information is cached */
	if (!os_cache.initialized)
	{
		/* First call - use native Windows APIs to populate cache (fast, no WMI overhead) */
		RTL_OSVERSIONINFOEXW osvi;
		SYSTEM_INFO si;
		DWORD computerNameSize;
		HMODULE hNtdll;
		RtlGetVersionFunc pRtlGetVersion;

		/* Initialize cache fields as NULL by default */
		os_cache.os_name_null = true;
		os_cache.os_version_null = true;
		os_cache.hostname_null = true;
		os_cache.architecture_null = true;
		os_cache.boot_time_null = true;

		memset(os_cache.os_name, 0, sizeof(os_cache.os_name));
		memset(os_cache.os_version, 0, sizeof(os_cache.os_version));
		memset(os_cache.hostname, 0, sizeof(os_cache.hostname));
		memset(os_cache.architecture, 0, sizeof(os_cache.architecture));
		memset(os_cache.boot_time, 0, sizeof(os_cache.boot_time));

		/* Get OS friendly name from registry */
		if (get_os_friendly_name(os_cache.os_name, sizeof(os_cache.os_name)))
		{
			os_cache.os_name_null = false;
		}
		else
		{
			ereport(DEBUG1, (errmsg("[ReadOSInformations]: Failed to get OS friendly name")));
		}

		/* Get OS version using RtlGetVersion (more reliable than GetVersionEx) */
		hNtdll = GetModuleHandleA("ntdll.dll");
		if (hNtdll != NULL)
		{
			pRtlGetVersion = (RtlGetVersionFunc)GetProcAddress(hNtdll, "RtlGetVersion");
			if (pRtlGetVersion != NULL)
			{
				memset(&osvi, 0, sizeof(osvi));
				osvi.dwOSVersionInfoSize = sizeof(osvi);

				if (pRtlGetVersion((PRTL_OSVERSIONINFOW)&osvi) == 0)
				{
					/* Format as Major.Minor.Build (e.g., "10.0.19045") */
					snprintf(os_cache.os_version, sizeof(os_cache.os_version),
							 "%lu.%lu.%lu",
							 osvi.dwMajorVersion,
							 osvi.dwMinorVersion,
							 osvi.dwBuildNumber);
					os_cache.os_version_null = false;
				}
				else
				{
					ereport(DEBUG1, (errmsg("[ReadOSInformations]: RtlGetVersion failed")));
				}
			}
		}

		/* Get computer name (hostname) */
		computerNameSize = sizeof(os_cache.hostname);
		if (GetComputerNameExA(ComputerNameDnsHostname, os_cache.hostname, &computerNameSize))
		{
			os_cache.hostname_null = false;
		}
		else
		{
			/* Fallback to NetBIOS name if DNS hostname fails */
			computerNameSize = sizeof(os_cache.hostname);
			if (GetComputerNameA(os_cache.hostname, &computerNameSize))
			{
				os_cache.hostname_null = false;
			}
			else
			{
				ereport(DEBUG1, (errmsg("[ReadOSInformations]: Failed to get computer name")));
			}
		}

		/* Get system architecture */
		GetNativeSystemInfo(&si);
		switch (si.wProcessorArchitecture)
		{
			case PROCESSOR_ARCHITECTURE_AMD64:
				snprintf(os_cache.architecture, sizeof(os_cache.architecture), "64-bit");
				os_cache.architecture_null = false;
				break;
			case PROCESSOR_ARCHITECTURE_INTEL:
				snprintf(os_cache.architecture, sizeof(os_cache.architecture), "32-bit");
				os_cache.architecture_null = false;
				break;
			case PROCESSOR_ARCHITECTURE_ARM:
				snprintf(os_cache.architecture, sizeof(os_cache.architecture), "ARM");
				os_cache.architecture_null = false;
				break;
			case PROCESSOR_ARCHITECTURE_ARM64:
				snprintf(os_cache.architecture, sizeof(os_cache.architecture), "ARM64");
				os_cache.architecture_null = false;
				break;
			default:
				snprintf(os_cache.architecture, sizeof(os_cache.architecture), "Unknown");
				os_cache.architecture_null = false;
				break;
		}

		/* Get boot time using GetTickCount64 (system uptime in milliseconds) */
		format_boot_time(GetTickCount64(), os_cache.boot_time, sizeof(os_cache.boot_time));
		if (strlen(os_cache.boot_time) > 0)
		{
			os_cache.boot_time_null = false;
		}

		/* Mark cache as initialized */
		os_cache.initialized = true;
		ereport(DEBUG1, (errmsg("[ReadOSInformations]: OS information cached using native APIs")));
	}

	/* Use cached static data to populate values */
	if (os_cache.os_name_null)
		nulls[Anum_os_name] = true;
	else
		values[Anum_os_name] = CStringGetTextDatum(os_cache.os_name);

	if (os_cache.os_version_null)
		nulls[Anum_os_version] = true;
	else
		values[Anum_os_version] = CStringGetTextDatum(os_cache.os_version);

	if (os_cache.hostname_null)
		nulls[Anum_host_name] = true;
	else
		values[Anum_host_name] = CStringGetTextDatum(os_cache.hostname);

	if (os_cache.architecture_null)
		nulls[Anum_os_architecture] = true;
	else
		values[Anum_os_architecture] = CStringGetTextDatum(os_cache.architecture);

	if (os_cache.boot_time_null)
		nulls[Anum_os_boot_time] = true;
	else
		values[Anum_os_boot_time] = CStringGetTextDatum(os_cache.boot_time);

	/* Set dynamic performance data */
	values[Anum_os_handle_count] = handle_count;
	values[Anum_os_process_count] = process_count;
	values[Anum_os_thread_count] = thread_count;

	/* Set NULL values for columns not applicable to this platform */
	nulls[Anum_domain_name] = true;
	nulls[Anum_os_up_since_seconds] = true;

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}
