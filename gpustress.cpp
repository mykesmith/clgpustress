/*
 *  GPUStress
 *  Copyright (C) 2014 Mateusz Szpakowski
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define __CL_ENABLE_EXCEPTIONS 1

#include <iostream>
#include <cstdio>
#include <fstream>
#include <string>
#include <cstring>
#include <sstream>
#include <vector>
#include <utility>
#include <thread>
#include <mutex>
#include <random>
#include <chrono>
#include <popt.h>
#include <CL/cl.hpp>

typedef unsigned short cxushort;
typedef signed short cxshort;
typedef unsigned int cxuint;
typedef signed int cxint;
typedef unsigned long cxulong;
typedef signed long cxlong;

class MyException: public std::exception
{
private:
    std::string message;
public:
    MyException() { }
    MyException(const std::string& msg)
    {
        this->message = msg;
    }
    virtual ~MyException() throw() { }
    
    const char* what() const throw()
    {
        return message.c_str();
    }
};

static int useCPUs = 0;
static int useGPUs = 0;
static int workFactor = 256;
static int passItersNum = 10;
static int choosenKitersNum = 0;

static const char* programName = nullptr;
static bool usePolyWalker = false;
static bool threeTimes = false;

static std::mutex stdOutputMutex;

static size_t clKernelSourceSize = 0;
static char* clKernelSource = nullptr;

static const poptOption optionsTable[] =
{
    { "useCPUs", 'C', POPT_ARG_VAL, &useCPUs, 'C', "use CPUs", NULL },
    { "useGPUs", 'G', POPT_ARG_VAL, &useGPUs, 'G', "use GPUs", NULL },
    { "program", 'P', POPT_ARG_STRING, &programName, 'P', "CL program name", "NAME" },
    { "workFactor", 'W', POPT_ARG_INT, &workFactor, 'W',
        "set workSize=factor*compUnits*grpSize", "FACTOR" },
    { "passIters", 'S', POPT_ARG_INT, &passItersNum, 'S', "pass iterations num",
        "ITERATION" },
    { "kiters", 'j', POPT_ARG_INT, &choosenKitersNum, 'j', "kitersNum",
        "ITERATION" },
    POPT_AUTOHELP
    { NULL, 0, 0, NULL, 0 }
};

static char* loadFromFile(const char* filename, size_t& size)
{
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs)
        throw MyException("Cant open file");
    ifs.exceptions(std::ifstream::badbit | std::ifstream::failbit);
    ifs.seekg(0, std::ios::end); // to end of file
    size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    char* buf = nullptr;
    try
    {
        buf = new char[size];
        ifs.read(buf, size);
        if (ifs.gcount() != std::streamsize(size))
            throw MyException("Cant read whole file");
    }
    catch(...)
    {
        delete[] buf;
        throw;
    }
    return buf;
}

std::vector<std::pair<cl::Platform, cl::Device> > getChoosenCLDevices()
{
    std::vector<std::pair<cl::Platform, cl::Device> > outDevices;
    std::vector<cl::Platform> clPlatforms;
    cl::Platform::get(&clPlatforms);
    
    for (const cl::Platform& clPlatform: clPlatforms)
    {
        std::string platformName;
        clPlatform.getInfo(CL_PLATFORM_NAME, &platformName);
        if (platformName.find("Intel") != std::string::npos)
            continue;
        std::vector<cl::Device> clDevices;
        cl_device_type deviceType = 0;
        if (useGPUs)
            deviceType |= CL_DEVICE_TYPE_GPU;
        if (useCPUs)
            deviceType |= CL_DEVICE_TYPE_CPU;
            
        clPlatform.getDevices(deviceType, &clDevices);
        for (const cl::Device& clDevice: clDevices)
            outDevices.push_back(std::make_pair(clPlatform, clDevice));
    }
    return outDevices;
}

static const float  examplePoly[5] = 
{ 4.43859953e+05,   1.13454169e+00,  -4.50175916e-06, -1.43865531e-12,   4.42133541e-18 };

class GPUStressTester
{
private:
    cxuint id;
    cl::Device clDevice;
    cl::Context clContext;
    
    std::string platformName;
    std::string deviceName;
    
    typedef std::chrono::time_point<std::chrono::high_resolution_clock> time_point;
    
    time_point lastTime;
    
    cl::CommandQueue clCmdQueue1, clCmdQueue2;
    
    cl::Buffer clBuffer1, clBuffer2;
    cl::Buffer clBuffer3, clBuffer4;
    
    cxuint passItersNum;
    cxuint kitersNum;
    
    float* initialValues;
    float* toCompare;
    float* results;
    
    cl::Program clProgram;
    cl::Kernel clKernel;
    
    size_t groupSize;
    size_t workSize;
    
    bool failed;
    std::string failMessage;
    
    void printBuildLog();
    void printStatus(cxuint passNum);
    
    void calibrateKernel();
public:
    GPUStressTester(cxuint id, cl::Platform& clPlatform, cl::Device& clDevice, size_t workFactor,
            cxuint passItersNum);
    ~GPUStressTester();
    
    void runTest();
    
    bool isFailed() const
    { return failed; }
    const std::string& getFailMessage() const
    { return failMessage; }
};

GPUStressTester::GPUStressTester(cxuint _id, cl::Platform& clPlatform, cl::Device& _clDevice,
        size_t workFactor, cxuint _passItersNum) : id(_id), clDevice(_clDevice),
        passItersNum(_passItersNum), initialValues(nullptr), toCompare(nullptr)
{
    failed = false;
    
    clPlatform.getInfo(CL_PLATFORM_NAME, &platformName);
    clDevice.getInfo(CL_DEVICE_NAME, &deviceName);
    
    cl_uint maxComputeUnits;
    clDevice.getInfo(CL_DEVICE_MAX_WORK_GROUP_SIZE, &groupSize);
    clDevice.getInfo(CL_DEVICE_MAX_COMPUTE_UNITS, &maxComputeUnits);
    
    workSize = size_t(maxComputeUnits)*groupSize*workFactor;
    
    {
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cout << "Preparing StressTester for\n  " <<
                "#" << id << " " << platformName << ":" << deviceName <<
                "\n    SetUp: workSize=" << workSize <<
                ", workFactor=" << workFactor <<
                ", computeUnits=" << maxComputeUnits <<
                ", groupSize=" << groupSize << std::endl;
    }
    
    cl_context_properties clContextProps[5];
    clContextProps[0] = CL_CONTEXT_PLATFORM;
    clContextProps[1] = reinterpret_cast<cl_context_properties>(clPlatform());
    clContextProps[2] = 0;
    clContext = cl::Context(clDevice, clContextProps);
    
    clCmdQueue1 = cl::CommandQueue(clContext, clDevice);
    clCmdQueue2 = cl::CommandQueue(clContext, clDevice);
    
    clBuffer1 = cl::Buffer(clContext, CL_MEM_READ_WRITE, workSize<<6);
    clBuffer2 = cl::Buffer(clContext, CL_MEM_READ_WRITE, workSize<<6);
    clBuffer3 = cl::Buffer(clContext, CL_MEM_READ_WRITE, workSize<<6);
    clBuffer4 = cl::Buffer(clContext, CL_MEM_READ_WRITE, workSize<<6);
    
    initialValues = new float[workSize<<4];
    toCompare = new float[workSize<<4];
    results = new float[workSize<<4];
    
    std::mt19937_64 random;
    if (!usePolyWalker)
    {
        for (size_t i = 0; i < workSize<<4; i++)
            initialValues[i] = (float(random())/float(
                        std::mt19937_64::max()-std::mt19937_64::min())-0.5f)*0.04f;
    }
    else
    {   /* data for polywalker */
        for (size_t i = 0; i < workSize<<4; i++)
            initialValues[i] = (float(random())/float(
                        std::mt19937_64::max()-std::mt19937_64::min()))*2e6 - 1e6;
    }
    
    clCmdQueue1.enqueueWriteBuffer(clBuffer1, CL_TRUE, size_t(0), workSize<<6,
            initialValues);
    
    calibrateKernel();
    
    clKernel.setArg(0, cl_uint(workSize));
    if (usePolyWalker)
    {
        clKernel.setArg(3, examplePoly[0]);
        clKernel.setArg(4, examplePoly[1]);
        clKernel.setArg(5, examplePoly[2]);
        clKernel.setArg(6, examplePoly[3]);
        clKernel.setArg(7, examplePoly[4]);
    }
    /* generate values to compare */
    for (cxuint i = 0; i < passItersNum; i++)
    {
        if ((i&1) == 0)
        {
            clKernel.setArg(1, clBuffer1);
            clKernel.setArg(2, clBuffer2);
        }
        else
        {
            clKernel.setArg(1, clBuffer2);
            clKernel.setArg(2, clBuffer1);
        }
        cl::Event clEvent;
        clCmdQueue1.enqueueNDRangeKernel(clKernel, cl::NDRange(0),
                cl::NDRange(workSize), cl::NDRange(groupSize), NULL, &clEvent);
        clEvent.wait();
        int eventStatus;
        clEvent.getInfo(CL_EVENT_COMMAND_EXECUTION_STATUS, &eventStatus);
        if (eventStatus < 0)
        {
            std::ostringstream oss;
            oss << "Failed NDRangeKernel with code: " << eventStatus << std::endl;
            throw MyException(oss.str());
        }
    }
    
    // get results
    if ((passItersNum&1) == 0)
        clCmdQueue1.enqueueReadBuffer(clBuffer1, CL_TRUE, size_t(0), workSize<<6,
                    toCompare);
    else //
        clCmdQueue1.enqueueReadBuffer(clBuffer2, CL_TRUE, size_t(0), workSize<<6,
                    toCompare);
    
    // print results
    /*for (size_t i = 0; i < workSize<<4; i++)
        std::cout << "out=" << i << ":" << toCompare[i] << '\n';
    std::cout.flush();*/
}

GPUStressTester::~GPUStressTester()
{
    delete[] toCompare;
    delete[] initialValues;
    delete[] results;
}

void GPUStressTester::calibrateKernel()
{
    cxuint bestKitersNum = 1;
    double bestBandwidth = 0.0;
    double bestPerf = 0.0;
    
    if (choosenKitersNum == 0)
    {
        {
            std::lock_guard<std::mutex> l(stdOutputMutex);
            std::cout << "Calibrating Kernel for\n  " <<
                    "#" << id << " " << platformName << ":" << deviceName << "..." << std::endl;
        }
        
        cl::CommandQueue profCmdQueue(clContext, clDevice, CL_QUEUE_PROFILING_ENABLE);
        
        for (cxuint kitersNum = 1; kitersNum <= 25; kitersNum++)
        {
            cl::Program::Sources clSources;
            clSources.push_back(std::make_pair(clKernelSource, clKernelSourceSize));
            clProgram = cl::Program(clContext, clSources);
            
            try
            {
                char buildOptions[128];
                snprintf(buildOptions, 128, "-O5 -DGROUPSIZE=%zu -DKITERSNUM=%u",
                        groupSize, kitersNum);
                clProgram.build(buildOptions);
            }
            catch(const cl::Error& error)
            {
                printBuildLog();
                throw;
            }
            //printBuildLog();
            
            clKernel = cl::Kernel(clProgram, "gpuStress");
            
            clKernel.setArg(0, cl_uint(workSize));
            clKernel.setArg(1, clBuffer1);
            clKernel.setArg(2, clBuffer2);
            if (usePolyWalker)
            {
                clKernel.setArg(3, examplePoly[0]);
                clKernel.setArg(4, examplePoly[1]);
                clKernel.setArg(5, examplePoly[2]);
                clKernel.setArg(6, examplePoly[3]);
                clKernel.setArg(7, examplePoly[4]);
            }
            
            cl::Event profEvent;
            profCmdQueue.enqueueNDRangeKernel(clKernel, cl::NDRange(0),
                    cl::NDRange(workSize), cl::NDRange(groupSize), NULL, &profEvent);
            profEvent.wait();
            int eventStatus;
            profEvent.getInfo(CL_EVENT_COMMAND_EXECUTION_STATUS, &eventStatus);
            if (eventStatus < 0)
            {
                std::ostringstream oss;
                oss << "Failed NDRangeKernel with code: " << eventStatus << std::endl;
                throw MyException(oss.str());
            }
            
            cl_ulong eventStartTime, eventEndTime;
            profEvent.getProfilingInfo(CL_PROFILING_COMMAND_START, &eventStartTime);
            profEvent.getProfilingInfo(CL_PROFILING_COMMAND_END, &eventEndTime);
            const cl_ulong currentTime = eventEndTime-eventStartTime;
            
            double currentBandwidth;
            double currentPerf;
            if (!usePolyWalker)
            {
                currentBandwidth = 6.0*64.0*double(workSize) /
                        double(currentTime);
                currentPerf = 3.0*96.0*double(kitersNum)*double(workSize) /
                        double(currentTime);
            }
            else
            {
                currentBandwidth = 2.0*64.0*double(workSize) /
                        double(currentTime);
                currentPerf = 128.0*double(kitersNum)*double(workSize) /
                        double(currentTime);
                if (threeTimes)
                {
                    currentBandwidth *= 3.0;
                    currentPerf *= 3.0;
                }
            }
            
            if (currentBandwidth*currentPerf > bestBandwidth*bestPerf)
            {
                bestKitersNum = kitersNum;
                bestPerf = currentPerf;
                bestBandwidth = currentBandwidth;
            }
            /*{
                std::lock_guard<std::mutex> l(stdOutputMutex);
                std::cout << "Choose for kernel \n  " <<
                        "#" << id << " " << platformName << ":" << deviceName << "\n"
                        "  BestKitersNum: " << kitersNum << ", Bandwidth: " << currentBandwidth <<
                        " GB/s, Performance: " << currentPerf << " GFLOPS" << std::endl;
            }*/
        }
        /* if choosen we compile real code */
        {
            std::lock_guard<std::mutex> l(stdOutputMutex);
            std::cout << "Kernel Calibrated for\n  " <<
                    "#" << id << " " << platformName << ":" << deviceName << "\n"
                    "  BestKitersNum: " << bestKitersNum << ", Bandwidth: " << bestBandwidth <<
                    " GB/s, Performance: " << bestPerf << " GFLOPS" << std::endl;
        }
    }
    else
    {
        bestKitersNum = choosenKitersNum;
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cout << "Kernel KitersNum: " << bestKitersNum << std::endl;
    }
    
    kitersNum = bestKitersNum;
    
    cl::Program::Sources clSources;
    clSources.push_back(std::make_pair(clKernelSource, clKernelSourceSize));
    clProgram = cl::Program(clContext, clSources);
    
    try
    {
        char buildOptions[128];
        snprintf(buildOptions, 128, "-O5 -DGROUPSIZE=%zu -DKITERSNUM=%u",
                    groupSize, bestKitersNum);
        clProgram.build(buildOptions);
    }
    catch(const cl::Error& error)
    {
        printBuildLog();
        throw;
    }
    printBuildLog();
    clKernel = cl::Kernel(clProgram, "gpuStress");
}

void GPUStressTester::printBuildLog()
{
    std::string buildLog;
    clProgram.getBuildInfo(clDevice, CL_PROGRAM_BUILD_LOG, &buildLog);
    std::lock_guard<std::mutex> l(stdOutputMutex);
    std::cout << "Program build log \n  " <<
            platformName << ":" << deviceName << "\n:--------------------\n" <<
            buildLog << std::endl;
}

void GPUStressTester::printStatus(cxuint passNum)
{
    if ((passNum%10) != 0)
        return;
    time_point currentTime = std::chrono::high_resolution_clock::now();
    const int64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                currentTime-lastTime).count();
    lastTime = currentTime;
    
    double bandwidth, perf;
    if (!usePolyWalker)
    {
        bandwidth = 6.0*10.0*64.0*double(passItersNum)*double(workSize) / double(nanos);
        perf = 3.0*10.0*96.0*double(kitersNum)*double(passItersNum)*double(workSize)
                / double(nanos);
    }
    else
    {
        bandwidth = 2.0*10.0*64.0*double(passItersNum)*double(workSize) / double(nanos);
        perf = 10.0*128.0*double(kitersNum)*double(passItersNum)*double(workSize)
                / double(nanos);
        if (threeTimes)
        {
            bandwidth *= 3.0;
            perf *= 3.0;
        }
    }
    std::lock_guard<std::mutex> l(stdOutputMutex);
    std::cout << "#" << id << " " << platformName << ":" << deviceName <<
            " was passed PASS #" << passNum << "\n"
            "Approx. bandwidth: " << bandwidth << " GB/s, "
            "Approx. perf: " << perf << " GFLOPS" << std::endl;
}

void GPUStressTester::runTest()
try
{
    bool run1Exec = false;
    bool run2Exec = false;
    std::vector<cl::Event> exec1Events(passItersNum);
    std::vector<cl::Event> exec2Events(passItersNum);
    clKernel.setArg(0, cl_uint(workSize));
    if (usePolyWalker)
    {
        clKernel.setArg(3, examplePoly[0]);
        clKernel.setArg(4, examplePoly[1]);
        clKernel.setArg(5, examplePoly[2]);
        clKernel.setArg(6, examplePoly[3]);
        clKernel.setArg(7, examplePoly[4]);
    }
    
    try
    {
    cxuint pass1Num = 1;
    cxuint pass2Num = 2;
    
    lastTime = std::chrono::high_resolution_clock::now();
    
    do {
        clCmdQueue2.enqueueWriteBuffer(clBuffer1, CL_TRUE, size_t(0), workSize<<6,
                initialValues);
        /* run execution 1 */
        for (cxuint i = 0; i < passItersNum; i++)
        {
            if ((i&1) == 0)
            {
                clKernel.setArg(1, clBuffer1);
                clKernel.setArg(2, clBuffer2);
            }
            else
            {
                clKernel.setArg(1, clBuffer2);
                clKernel.setArg(2, clBuffer1);
            }
            clCmdQueue1.enqueueNDRangeKernel(clKernel, cl::NDRange(0),
                    cl::NDRange(workSize), cl::NDRange(groupSize), NULL, &exec1Events[i]);
        }
        run1Exec = true;
        
        if (run2Exec)
        {   /* after exec2 */
            exec2Events[passItersNum-1].wait();
            run2Exec = false;
            for (cxuint i = 0; i < passItersNum; i++)
            {   // check kernel event status
                int eventStatus;
                exec2Events[i].getInfo(CL_EVENT_COMMAND_EXECUTION_STATUS, &eventStatus);
                if (eventStatus < 0)
                {
                    std::ostringstream oss;
                    oss << "Failed NDRangeKernel with code: " << eventStatus << std::endl;
                    throw MyException(oss.str());
                }
                exec2Events[i] = cl::Event(); // release event
            }
            // get results
            if ((passItersNum&1) == 0)
                clCmdQueue2.enqueueReadBuffer(clBuffer3, CL_TRUE, size_t(0), workSize<<6,
                            results);
            else //
                clCmdQueue2.enqueueReadBuffer(clBuffer4, CL_TRUE, size_t(0), workSize<<6,
                            results);
            if (::memcmp(toCompare, results, workSize<<6))
                throw MyException("FAILED COMPUTATIONS!!!!");
            
            printStatus(pass2Num);
            pass2Num += 2;
        }
        
        clCmdQueue2.enqueueWriteBuffer(clBuffer3, CL_TRUE, size_t(0), workSize<<6,
                initialValues);
        /* run execution 2 */
        for (cxuint i = 0; i < passItersNum; i++)
        {
            if ((i&1) == 0)
            {
                clKernel.setArg(1, clBuffer3);
                clKernel.setArg(2, clBuffer4);
            }
            else
            {
                clKernel.setArg(1, clBuffer4);
                clKernel.setArg(2, clBuffer3);
            }
            clCmdQueue1.enqueueNDRangeKernel(clKernel, cl::NDRange(0),
                    cl::NDRange(workSize), cl::NDRange(groupSize), NULL, &exec2Events[i]);
        }
        run2Exec = true;
        
        if (run1Exec)
        {   /* after exec1 */
            exec1Events[passItersNum-1].wait();
            run1Exec = false;
            for (cxuint i = 0; i < passItersNum; i++)
            {   // check kernel event status
                int eventStatus;
                exec1Events[i].getInfo(CL_EVENT_COMMAND_EXECUTION_STATUS, &eventStatus);
                if (eventStatus < 0)
                {
                    std::ostringstream oss;
                    oss << "Failed NDRangeKernel with code: " << eventStatus << std::endl;
                    throw MyException(oss.str());
                }
                exec1Events[i] = cl::Event(); // release event
            }
            // get results
            if ((passItersNum&1) == 0)
                clCmdQueue2.enqueueReadBuffer(clBuffer1, CL_TRUE, size_t(0), workSize<<6,
                            results);
            else //
                clCmdQueue2.enqueueReadBuffer(clBuffer2, CL_TRUE, size_t(0), workSize<<6,
                            results);
            if (::memcmp(toCompare, results, workSize<<6))
                throw MyException("FAILED COMPUTATIONS!!!!");
            printStatus(pass1Num);
            pass1Num += 2;
        }
    } while (run1Exec || run2Exec);
    }
    catch(...)
    {   /* wait for finish kernels */
        if (exec1Events[passItersNum-1]() != nullptr)
            exec1Events[passItersNum-1].wait();
        if (exec2Events[passItersNum-1]() != nullptr)
            exec2Events[passItersNum-1].wait();
        throw;
    }
    if (exec1Events[passItersNum-1]() != nullptr)
        exec1Events[passItersNum-1].wait();
    if (exec2Events[passItersNum-1]() != nullptr)
        exec2Events[passItersNum-1].wait();
}
catch(const cl::Error& error)
{
    failed = true;
    try
    {
        std::ostringstream oss;
        oss << "OpenCL error happened: " << error.what() << ", Code: " << error.err();;
        oss.flush();
        failMessage = oss.str();
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cerr << "Failed StressTester for" <<
                "#" << id  << " " << platformName << ":" << deviceName << ": " <<
                failMessage << std::endl;
    }
    catch(...)
    { } // fatal exception!!!
}
catch(const std::exception& ex)
{
    failed = true;
    try
    {
        std::ostringstream oss;
        oss << "Standard exception happened: " << ex.what();
        oss.flush();
        failMessage = oss.str();
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cerr << "Failed StressTester for\n  " <<
                "#" << id << " " << platformName << ":" << deviceName << ":\n    " <<
                failMessage << std::endl;
    }
    catch(...)
    { } // fatal exception!!!
}
catch(...)
{
    failed = true;
    try
    {
        failMessage = "Standard exception happened";
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cerr << "Failed StressTester for\n  " <<
                "#" << id << " " << platformName << ":" << deviceName << ":\n    " <<
                failMessage << std::endl;
    }
    catch(...)
    { } // fatal exception!!!
}

int main(int argc, const char** argv)
{
    int cmd;
    poptContext optsContext;
    
    optsContext = poptGetContext("gpustress", argc, argv, optionsTable, 0);
    
    /* parse options */
    while((cmd = poptGetNextOpt(optsContext)) >= 0);
    
    if (cmd < -1)
    {
        std::cerr << poptBadOption(optsContext, POPT_BADOPTION_NOALIAS) << ": " <<
            poptStrerror(cmd) << std::endl;
        return 1;
    }
    
    if (workFactor < 0)
    {
        std::cerr << "WorkFactor is negative!" << std::endl;
        return 1;
    }
    if (passItersNum <= 0)
    {
        std::cerr << "PassIters is not positive!" << std::endl;
        return 1;
    }
    if (choosenKitersNum < 0 || choosenKitersNum > 25)
    {
        std::cerr << "KitersNum out of range" << std::endl;
        return 1;
    }
    
    std::cout << "WARNING: THIS PROGRAM CAN OVERHEAT YOUR GRAPHIC CARD FASTER THAN "
                 "ANY FURMARK STRESS.\nPLEASE USE CAREFULLY!!!" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    
    if (!useGPUs && !useCPUs)
        useGPUs = 1;
    
    if (programName == nullptr)
        programName = "gpustressKernel2.cl";
    if (::strcmp(programName, "gpustressPW.cl") == 0)
        usePolyWalker = true;
    else if (::strcmp(programName, "gpustressPW2.cl") == 0 ||
            ::strcmp(programName, "gpustressPW3.cl") == 0)
    {
        usePolyWalker = true;
        threeTimes = true;
    }
    
    int retVal = 0;
    
    std::vector<GPUStressTester*> gpuStressTesters;
    std::vector<std::thread*> testerThreads;
    try
    {
        clKernelSource = loadFromFile(programName, clKernelSourceSize);
        
        std::vector<std::pair<cl::Platform, cl::Device> > choosenCLDevices =
            getChoosenCLDevices();
        for (size_t i = 0; i < choosenCLDevices.size(); i++)
        {
            auto& p = choosenCLDevices[i];
            gpuStressTesters.push_back(new GPUStressTester(i, p.first, p.second, workFactor,
                            passItersNum));
        }
        
        delete[] clKernelSource;
        clKernelSource = nullptr;
        
        for (size_t i = 0; i < choosenCLDevices.size(); i++)
            testerThreads.push_back(new std::thread(
                    &GPUStressTester::runTest, gpuStressTesters[i]));
    }
    catch(const cl::Error& error)
    {
        delete[] clKernelSource;
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cerr << "OpenCL error happened: " << error.what() <<
                ", Code: " << error.err() << std::endl;
        retVal = 1;
    }
    catch(const std::exception& ex)
    {
        delete[] clKernelSource;
        std::lock_guard<std::mutex> l(stdOutputMutex);
        std::cerr << "Standard exception happened: " << ex.what() << std::endl;
        retVal = 1;
    }
    // clean up
    for (size_t i = 0; i < testerThreads.size(); i++)
        if (testerThreads[i] != nullptr)
        {
            try
            { testerThreads[i]->join(); }
            catch(const std::exception& ex)
            {
                std::lock_guard<std::mutex> l(stdOutputMutex);
                std::cerr << "Failed to join GPU stress #" << i << "!!!" << std::endl;
                retVal = 1;
            }
            delete testerThreads[i];
            testerThreads[i] = nullptr;
            std::lock_guard<std::mutex> l(stdOutputMutex);
            std::cout << "Finished #" << i << std::endl;
        }
    for (size_t i = 0; i < gpuStressTesters.size(); i++)
    {
        if (gpuStressTesters[i]->isFailed())
        {
            retVal = 1;
            std::lock_guard<std::mutex> l(stdOutputMutex);
            std::cerr << "Failed #" << i << std::endl;
        }
        delete gpuStressTesters[i];
    }
    
    poptFreeContext(optsContext);
    return retVal;
}
