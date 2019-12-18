#include <iostream>
#include <intrin.h>
#include <windows.h>
#include <malloc.h>    
#include <stdio.h>
#include <tchar.h>
#include <thread> 

#include "CPUInfo.h"

#pragma intrinsic(__rdtsc)

using namespace std;

typedef BOOL(WINAPI* LPFN_GLPI)(
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION,
    PDWORD);

DWORD CountSetBits(ULONG_PTR bitMask)
{
    DWORD LSHIFT = sizeof(ULONG_PTR) * 8 - 1;
    DWORD bitSetCount = 0;
    ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;
    DWORD i;

    for (i = 0; i <= LSHIFT; ++i)
    {
        bitSetCount += ((bitMask & bitTest) ? 1 : 0);
        bitTest /= 2;
    }

    return bitSetCount;
}

char* getCpuidVendor(char* vendor) {
    int data[4];
    __cpuid(data, 0);
    *reinterpret_cast<int*>(vendor) = data[1];
    *reinterpret_cast<int*>(vendor + 4) = data[3];
    *reinterpret_cast<int*>(vendor + 8) = data[2];
    vendor[12] = 0;
    return vendor;
}

int getCpuidFamily() {
    int data[4];
    __cpuid(data, 1);
    int family = ((data[0] >> 8) & 0x0F);
    int extendedFamily = (data[0] >> 20) & 0xFF;
    int displayFamily = (family != 0x0F) ? family : (extendedFamily + family);
    return displayFamily;
}

CPUInfo getCPUInfo()
{
    LPFN_GLPI glpi;
    BOOL done = FALSE;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
    DWORD returnLength = 0;
    DWORD byteOffset = 0;
    PCACHE_DESCRIPTOR Cache;
    CPUInfo info;

    info.cpuidFamily = getCpuidFamily();
    getCpuidVendor(info.vendor);

    glpi = (LPFN_GLPI)GetProcAddress(GetModuleHandle(TEXT("kernel32")),"GetLogicalProcessorInformation");
    if (NULL == glpi)
    {
        cout << "GetLogicalProcessorInformation is not supported";
        return info;
    }

    while (!done)
    {
        DWORD rc = glpi(buffer, &returnLength);
        if (FALSE == rc)
        {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                if (buffer) 
                {
                    free(buffer);
                }

                buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(returnLength);

                if (NULL == buffer)
                {
                    cout << "Error: Allocation failure";
                    return info;
                }
            }
            else
            {
                cout << "Error: " << GetLastError();
                return info;
            }
        }
        else
        {
            done = TRUE;
        }
    }

    ptr = buffer;

    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength)
    {
        switch (ptr->Relationship)
        {
        case RelationNumaNode:
            // Non-NUMA systems report a single record of this type.
            info.numaNodeCount++;
            break;

        case RelationProcessorCore:
            info.physicalCoreCount++;
            info.logicalCoreCount += CountSetBits(ptr->ProcessorMask);
            break;

        case RelationCache:
            Cache = &ptr->Cache;
            if (Cache->Level == 1)
            {
                if (Cache->Type == CacheData) {
                    info.L1CacheCount++;
                }
            }
            else if (Cache->Level == 2)
            {
                info.L2CacheCount++;
            }
            else if (Cache->Level == 3)
            {
                info.L3CacheCount++;
            }
            break;

        case RelationProcessorPackage:
            info.packageCount++;
            break;

        default:
            break;
        }
        byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ptr++;
    }
    free(buffer);
    return info;
}

double getTSCTicksPerNanosecond() {
    //Calculate TSC frequency
    LARGE_INTEGER Frequency;
    QueryPerformanceFrequency(&Frequency);

    LARGE_INTEGER tStart;
    LARGE_INTEGER tEnd;

    QueryPerformanceCounter(&tStart);
    unsigned long long start = __rdtsc();

    //Sleep for a bit
    Sleep(2000);

    QueryPerformanceCounter(&tEnd);
    unsigned long long end = __rdtsc();

    LONGLONG deltaQPC = tEnd.QuadPart - tStart.QuadPart;
    unsigned long long deltaTSC = end - start;

    //Duration in nanoseconds
    double qpcDuration = (double)deltaQPC * 1000000000.0 / (double)Frequency.QuadPart;

    //Calculate TSC ticks per nanosecond
    return (double)deltaTSC / qpcDuration;
}

const int iterations = 100000000;
int counter = -1;

void workThread(int core) {
    //Lock thread to specified core
    SetThreadAffinityMask(GetCurrentThread(), (static_cast<DWORD_PTR>(1) << core));

    //Make sure core affinity gets set before continuing
    Sleep(10);

    //Loop bouncing data back and forth between cores
    while (counter != 0) {
        if (counter > 0) {
            counter = -counter + 1;
        }
    }
}

long long testSingleCore() {
    unsigned long long start;
    unsigned long long end;

    counter = -1;

    //Record start time
    start = __rdtsc();

    counter = counter - iterations;
    while (counter != 0) {
        if (counter < 0) {
            counter = -counter - 1;
        }
        else if (counter > 0) {
            counter = -counter + 1;
        }
    }

    //Record end time
    end = __rdtsc();

    return end - start;
}

long long measureLatency(int core) {
    unsigned long long start;
    unsigned long long end;

    //Enable counter
    counter = -1;

    //Start the far thread
    thread coreWorker(workThread, core);

    //Wait for it to start and lock affinity
    Sleep(250);

    //Record start time
    start = __rdtsc();

    //Loop bouncing data back and forth between cores
    counter = counter - iterations;
    while (counter != 0) {
        if (counter < 0) {
            counter = -counter - 1;
        }
    }

    //Record end time
    end = __rdtsc();

    //Make sure the thread exits before continuing
    coreWorker.join();

    //Return total time taken
    return end - start;
}

int main()
{
    //Lock main thread to Core 0
    SetThreadAffinityMask(GetCurrentThread(), (static_cast<DWORD_PTR>(1) << 0));

    //Set priority to high
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    //Fetch CPU information
    CPUInfo cpuInfo = getCPUInfo();

    //Check for Zen 
    if (strcmp(cpuInfo.vendor, "AuthenticAMD") != 0 || cpuInfo.cpuidFamily != 0x17) {
        cout << "Warning: This program has only been tested to work on AMD Zen." << endl;
    }

    cout << "Package Count: " << cpuInfo.packageCount << endl;
    cout << "NUMA Node Count: " << cpuInfo.packageCount << endl;
    cout << "Cores: " << cpuInfo.physicalCoreCount << "C / " << cpuInfo.logicalCoreCount << "T" << endl;
    cout << "CCX Count: " << cpuInfo.L3CacheCount << endl;
    cout << "Cores per CCX: " << cpuInfo.getCoresPerL3() << endl;
    cout << "Cores per package: " << cpuInfo.getCoresPerPackage() << endl;
    cout << "Cores per NUMA node: " << cpuInfo.getCoresPerNode() << endl << endl;

    cout << "Calculating TSC Frequency..." << endl;
    cpuInfo.ticksPerNanosecond = getTSCTicksPerNanosecond();

    cout << "Ticks per ns: " << cpuInfo.ticksPerNanosecond << endl << endl;

    cout << "Running latency tests..." << endl;
    unsigned long long time;
    
    //Same coref
    time = testSingleCore();
    cout << "Same Thread : " << (time / iterations) / cpuInfo.ticksPerNanosecond << " ns" << endl;

    //Same core
    if (cpuInfo.getThreadsPerCore() > 1) {
        //Measure latency to same core (other thread)
        time = measureLatency(1);
        cout << "Same Core   : " << (time / iterations) / cpuInfo.ticksPerNanosecond << " ns" << endl;
    }

    //Same CCX
    if (cpuInfo.getCoresPerL3() > 1) {
        //Measure latency to another core (same CCX)
        time = measureLatency(cpuInfo.getThreadsPerCore());
        cout << "Same CCX    : " << (time / iterations) / cpuInfo.ticksPerNanosecond << " ns" << endl;
    }

    //Different CCX (same NUMA, same Package)
    if (cpuInfo.L3CacheCount > 1 && cpuInfo.getL3PerPackage() > 1 && cpuInfo.getL3PerNUMANode() > 1) {
        //Measure latency to another core (different CCX)
        time = measureLatency(16);
        cout << "Other CCX   : " << (time / iterations) / cpuInfo.ticksPerNanosecond << " ns" << endl;
    }

    cout << endl;

    system("pause");
    return 0;
}
