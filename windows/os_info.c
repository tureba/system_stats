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
#include <wbemidl.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

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
		/* First call - query WMI and populate cache */
		HRESULT hres = 0;
		IEnumWbemClassObject *results = NULL;
		BSTR query = SysAllocString(L"SELECT * FROM Win32_Operatingsystem");

		/* Initialize cache fields as NULL by default */
		os_cache.os_name_null = true;
		os_cache.os_version_null = true;
		os_cache.hostname_null = true;
		os_cache.architecture_null = true;
		os_cache.boot_time_null = true;

		/* Issue WMI query */
		results = execute_query(query);

		if (results != NULL)
		{
			IWbemClassObject *result = NULL;
			ULONG returnedCount = 0;

			/* Enumerate the retrieved objects */
			while ((hres = results->lpVtbl->Next(results, WBEM_INFINITE, 1, &result, &returnedCount)) == S_OK)
			{
				VARIANT query_result;
				int     wstr_length = 0;
				size_t  charsConverted = 0;

				/* Get OS Caption (name) */
				hres = result->lpVtbl->Get(result, L"Caption", 0, &query_result, 0, 0);
				if (SUCCEEDED(hres))
				{
					wstr_length = SysStringLen(query_result.bstrVal);
					if (wstr_length > 0)
					{
						memset(os_cache.os_name, 0, sizeof(os_cache.os_name));
						wcstombs_s(&charsConverted, os_cache.os_name, sizeof(os_cache.os_name),
								   query_result.bstrVal, wstr_length);
						os_cache.os_name_null = false;
					}
					VariantClear(&query_result);
				}

				/* Get OS Version */
				hres = result->lpVtbl->Get(result, L"Version", 0, &query_result, 0, 0);
				if (SUCCEEDED(hres))
				{
					wstr_length = SysStringLen(query_result.bstrVal);
					if (wstr_length > 0)
					{
						memset(os_cache.os_version, 0, sizeof(os_cache.os_version));
						wcstombs_s(&charsConverted, os_cache.os_version, sizeof(os_cache.os_version),
								   query_result.bstrVal, wstr_length);
						os_cache.os_version_null = false;
					}
					VariantClear(&query_result);
				}

				/* Get Computer Name (hostname) */
				hres = result->lpVtbl->Get(result, L"CSName", 0, &query_result, 0, 0);
				if (SUCCEEDED(hres))
				{
					wstr_length = SysStringLen(query_result.bstrVal);
					if (wstr_length > 0)
					{
						memset(os_cache.hostname, 0, sizeof(os_cache.hostname));
						wcstombs_s(&charsConverted, os_cache.hostname, sizeof(os_cache.hostname),
								   query_result.bstrVal, wstr_length);
						os_cache.hostname_null = false;
					}
					VariantClear(&query_result);
				}

				/* Get OS Architecture */
				hres = result->lpVtbl->Get(result, L"OSArchitecture", 0, &query_result, 0, 0);
				if (SUCCEEDED(hres))
				{
					wstr_length = SysStringLen(query_result.bstrVal);
					if (wstr_length > 0)
					{
						memset(os_cache.architecture, 0, sizeof(os_cache.architecture));
						wcstombs_s(&charsConverted, os_cache.architecture, sizeof(os_cache.architecture),
								   query_result.bstrVal, wstr_length);
						os_cache.architecture_null = false;
					}
					VariantClear(&query_result);
				}

				/* Get Last Boot Up Time */
				hres = result->lpVtbl->Get(result, L"LastBootUpTime", 0, &query_result, 0, 0);
				if (SUCCEEDED(hres))
				{
					wstr_length = SysStringLen(query_result.bstrVal);
					if (wstr_length > 0)
					{
						memset(os_cache.boot_time, 0, sizeof(os_cache.boot_time));
						wcstombs_s(&charsConverted, os_cache.boot_time, sizeof(os_cache.boot_time),
								   query_result.bstrVal, wstr_length);
						os_cache.boot_time_null = false;
					}
					VariantClear(&query_result);
				}

				/* Release the current result object */
				result->lpVtbl->Release(result);
			}

			/* Release results set */
			results->lpVtbl->Release(results);

			/* Mark cache as initialized */
			os_cache.initialized = true;
			ereport(DEBUG1, (errmsg("[ReadOSInformations]: OS information cached for subsequent calls")));
		}
		else
		{
			ereport(DEBUG1, (errmsg("[ReadOSInformations]: Failed to get query result")));
		}

		SysFreeString(query);
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
