#pragma once

#define PM_BEGIN_DYNAMIC_QUERY(type) struct type : DynamicQueryContainer { using DynamicQueryContainer::DynamicQueryContainer;
#define PM_BEGIN_FRAME_QUERY(type) struct type : FrameQueryContainer<type> { using FrameQueryContainer<type>::FrameQueryContainer;
#define PM_END_QUERY private: FinalizingElement finalizer{ this }; }

void WriteToCSV(std::ofstream& csvFile, const std::string& processName, const unsigned int& processId,
    PM_QUERY_ELEMENT(&queryElements)[15], pmapi::BlobContainer& blobs)
{

    try {

        for (auto pBlob : blobs) {
            //const auto appName = *reinterpret_cast<const std::string*>(&pBlob[queryElements[0].dataOffset]);
            const auto swapChain = *reinterpret_cast<const uint64_t*>(&pBlob[queryElements[0].dataOffset]);
            const auto graphicsRuntime = *reinterpret_cast<const PM_GRAPHICS_RUNTIME*>(&pBlob[queryElements[1].dataOffset]);
            const auto syncInterval = *reinterpret_cast<const uint32_t*>(&pBlob[queryElements[2].dataOffset]);
            const auto presentFlags = *reinterpret_cast<const uint32_t*>(&pBlob[queryElements[3].dataOffset]);
            const auto allowsTearing = *reinterpret_cast<const bool*>(&pBlob[queryElements[4].dataOffset]);
            const auto presentMode = *reinterpret_cast<const PM_PRESENT_MODE*>(&pBlob[queryElements[5].dataOffset]);
            const auto cpuFrameQpc = *reinterpret_cast<const uint64_t*>(&pBlob[queryElements[6].dataOffset]);
            const auto cpuDuration = *reinterpret_cast<const double*>(&pBlob[queryElements[7].dataOffset]);
            const auto cpuFramePacingStall = *reinterpret_cast<const double*>(&pBlob[queryElements[8].dataOffset]);
            const auto gpuLatency = *reinterpret_cast<const double*>(&pBlob[queryElements[9].dataOffset]);
            const auto gpuDuration = *reinterpret_cast<const double*>(&pBlob[queryElements[10].dataOffset]);
            const auto gpuBusyTime = *reinterpret_cast<const double*>(&pBlob[queryElements[11].dataOffset]);
            const auto gpuDisplayLatency = *reinterpret_cast<const double*>(&pBlob[queryElements[12].dataOffset]);
            const auto gpuDisplayDuration = *reinterpret_cast<const double*>(&pBlob[queryElements[13].dataOffset]);
            const auto inputLatency = *reinterpret_cast<const double*>(&pBlob[queryElements[14].dataOffset]);
            csvFile << processName << ",";
            csvFile << processId << ",";
            csvFile << std::hex << "0x" << std::dec << swapChain << ",";
            csvFile << TranslateGraphicsRuntime(graphicsRuntime) << ",";
            csvFile << syncInterval << ",";
            csvFile << presentFlags << ",";
            csvFile << allowsTearing << ",";
            csvFile << TranslatePresentMode(presentMode) << ",";
            csvFile << cpuFrameQpc << ",";
            csvFile << cpuDuration << ",";
            csvFile << cpuFramePacingStall << ",";
            csvFile << gpuLatency << ",";
            csvFile << gpuDuration << ",";
            csvFile << gpuBusyTime << ",";
            csvFile << gpuDisplayLatency << ",";
            csvFile << gpuDisplayDuration << ",";
            csvFile << inputLatency << "\n";
        }
    }
    catch (const std::exception& e) {
        std::cout
            << "a standard exception was caught, with message '"
            << e.what() << "'" << std::endl;
        return;
    }
    catch (...) {
        std::cout << "Unknown Error" << std::endl;
        return;
    }

}

std::optional<std::ofstream> CreateCsvFile(std::string& processName)
{
    // Setup csv file
    time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm local_time;
    localtime_s(&local_time, &now);
    std::ofstream csvFile;
    std::string csvFileName = processName;

    try {
        csvFileName += "_" + std::to_string(local_time.tm_year + 1900) +
            std::to_string(local_time.tm_mon + 1) +
            std::to_string(local_time.tm_mday + 1) +
            std::to_string(local_time.tm_hour) +
            std::to_string(local_time.tm_min) +
            std::to_string(local_time.tm_sec) + ".csv";
        csvFile.open(csvFileName);
        csvFile << "Application,ProcessID,SwapChainAddress,PresentRuntime,"
            "SyncInterval,PresentFlags,AllowsTearing,PresentMode,"
            "CPUFrameQPC,CPUDuration,CPUFramePacingStall,"
            "GPULatency,GPUDuration,GPUBusy,DisplayLatency,"
            "DisplayDuration,InputLatency";
        csvFile << std::endl;
        return csvFile;
    }
    catch (const std::exception& e) {
        std::cout
            << "a standard exception was caught, with message '"
            << e.what() << "'" << std::endl;
        return std::nullopt;
    }
    catch (...) {
        std::cout << "Unknown Error" << std::endl;
        return std::nullopt;
    }
}

int GenCsv(pmapi::Session& pSession, std::string processName, unsigned int processId)
{
    using namespace std::chrono_literals;

    try
    {
        pmapi::ProcessTracker processTracker;
        static constexpr uint32_t numberOfBlobs = 150u;

        // TODO: Had to comment out the application metric because when
        // calling RegisterFrameQuery we crash
        PM_QUERY_ELEMENT queryElements[]{
            //{ PM_METRIC_APPLICATION, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_SWAP_CHAIN_ADDRESS, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_PRESENT_RUNTIME, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_SYNC_INTERVAL, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_PRESENT_FLAGS, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_ALLOWS_TEARING, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_PRESENT_MODE, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_CPU_FRAME_QPC, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_CPU_DURATION, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_CPU_FRAME_PACING_STALL, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_GPU_LATENCY, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_GPU_DURATION, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_GPU_BUSY_TIME, PM_STAT_NONE, 0, 0},
            { PM_METRIC_DISPLAY_LATENCY, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_DISPLAY_DURATION, PM_STAT_NONE, 0, 0 },
            { PM_METRIC_INPUT_LATENCY, PM_STAT_NONE, 0, 0}
        };

        auto frameQuery = pSession.RegisterFrameQuery(queryElements);
        auto blobs = frameQuery.MakeBlobContainer(numberOfBlobs);

        processTracker = pSession.TrackProcess(processId);

        auto csvStream = CreateCsvFile(processName);
        if (!csvStream.has_value())
        {
            return -1;
        }

        while (!_kbhit()) {
            std::cout << "Checking for new frames...\n";
            uint32_t numFrames = numberOfBlobs;
            frameQuery.Consume(processTracker, blobs);
            if (blobs.GetNumBlobsPopulated() == 0) {
                std::this_thread::sleep_for(200ms);
            }
            else {
                std::cout << std::format("Dumping [{}] frames...\n", numFrames);
                WriteToCSV(csvStream.value(), processName, processId, queryElements, blobs);
            }
        }

        processTracker.Reset();
        //pSession->Reset();
        csvStream.value().close();
    }
    catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        return -1;
    }
    catch (...) {
        std::cout << "Unknown Error" << std::endl;
        return -1;
    }

    return 0;
}

int FrameQuerySample(std::unique_ptr<pmapi::Session>&& pSession)
{
    using namespace std::chrono_literals;
    using namespace pmapi;

    try {

        std::optional<unsigned int> processId;
        std::optional<std::string> processName;

        GetProcessInformation(processName, processId);
        if (!processId.has_value())
        {
            std::cout << "Must set valid process name or process id:\n";
            std::cout << "--dynamic-query-sample [--process-id id | --process-name name.exe] [--gen-csv]\n";
            return -1;
        }

        auto& opt = clio::Options::Get();
        if (opt.genCsv) {
            return GenCsv(*pSession, processName.value(), processId.value());
        }

        auto processTracker = pSession->TrackProcess(processId.value());

        PM_BEGIN_FRAME_QUERY(MyFrameQuery)
            QueryElement swapChain{ this, PM_METRIC_SWAP_CHAIN_ADDRESS, PM_STAT_NONE };
            QueryElement cpuFrameQpc{ this, PM_METRIC_CPU_FRAME_QPC, PM_STAT_NONE };
            QueryElement cpuDuration{ this, PM_METRIC_CPU_DURATION, PM_STAT_NONE };
            QueryElement cpuFpStall{ this, PM_METRIC_CPU_FRAME_PACING_STALL, PM_STAT_NONE };
            QueryElement gpuLatency{ this, PM_METRIC_GPU_LATENCY, PM_STAT_NONE };
            QueryElement gpuDuration{ this, PM_METRIC_GPU_DURATION, PM_STAT_NONE };
            QueryElement gpuBusyTime{ this, PM_METRIC_GPU_BUSY_TIME, PM_STAT_NONE };
            QueryElement gpuDisplayLatency{ this, PM_METRIC_DISPLAY_LATENCY, PM_STAT_NONE };
            QueryElement gpuDisplayDuration{ this, PM_METRIC_DISPLAY_DURATION, PM_STAT_NONE};
            QueryElement inputLatency{ this, PM_METRIC_DISPLAY_DURATION, PM_STAT_NONE };
            QueryElement gpuPower{ this, PM_METRIC_GPU_POWER, PM_STAT_NONE, 1 };
        PM_END_QUERY fq{ *pSession, 20, 1};

        while (!_kbhit()) {
            std::cout << "Consuming frames...";
            const auto nProcessed = fq.ForEachConsume(processTracker, [processName, processId](const MyFrameQuery& q) {
                std::cout << processName.value() << ",";
                std::cout << processId.value() << ",";
                std::cout << std::hex << "0x" << q.swapChain.As<uint64_t>() << std::dec << ",";
                std::cout << q.cpuFrameQpc.As<uint64_t>() << ",";
                std::cout << q.cpuDuration.As<double>() << ",";
                std::cout << q.cpuFpStall.As<double>() << ",";
                std::cout << q.gpuLatency.As<double>() << ",";
                std::cout << q.gpuDuration.As<double>() << ",";
                std::cout << q.gpuBusyTime.As<double>() << ",";
                std::cout << q.gpuDisplayLatency.As<double>() << ",";
                std::cout << q.gpuDisplayDuration.As<double>() << ",";
                std::cout << q.inputLatency.As<double>() << ",";
                std::cout << q.gpuPower.As<double>() << "\n";
                });
            std::cout << "Processed " << nProcessed << " frames, sleeping..." << std::endl;
            std::this_thread::sleep_for(150ms);
        }
        processTracker.Reset();
    }
    catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
        return -1;
    }
    catch (...) {
        std::cout << "Unknown Error" << std::endl;
        return -1;
    }

    return 0;
}