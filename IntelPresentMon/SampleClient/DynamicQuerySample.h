#pragma once

#define PM_BEGIN_DYNAMIC_QUERY(type) struct type : DynamicQueryContainer { using DynamicQueryContainer::DynamicQueryContainer;
#define PM_BEGIN_FRAME_QUERY(type) struct type : FrameQueryContainer<type> { using FrameQueryContainer<type>::FrameQueryContainer;
#define PM_END_QUERY private: FinalizingElement finalizer{ this }; }

int DynamicQuerySample(std::unique_ptr<pmapi::Session>&& pSession, unsigned int processId, double windowSize, double metricOffset)
{
    using namespace std::chrono_literals;
    using namespace pmapi;

    try {
        auto proc = pSession->TrackProcess(processId);

        PM_BEGIN_DYNAMIC_QUERY(MyDynamicQuery)
            QueryElement appName{ this, PM_METRIC_APPLICATION, PM_STAT_MID_POINT };
            QueryElement fpsAvg{ this, PM_METRIC_DISPLAYED_FPS, PM_STAT_AVG };
            QueryElement fps90{ this, PM_METRIC_DISPLAYED_FPS, PM_STAT_PERCENTILE_90 };
            QueryElement fps95{ this, PM_METRIC_DISPLAYED_FPS, PM_STAT_PERCENTILE_95 };
            QueryElement fps99{ this, PM_METRIC_DISPLAYED_FPS, PM_STAT_PERCENTILE_99 };
            QueryElement fpsMin{ this, PM_METRIC_DISPLAYED_FPS, PM_STAT_MAX };
            QueryElement fpsMax{ this, PM_METRIC_DISPLAYED_FPS, PM_STAT_MIN };
            QueryElement frameDurationAvg{ this, PM_METRIC_FRAME_DURATION, PM_STAT_AVG };
            QueryElement fpStallAvg{ this, PM_METRIC_CPU_FRAME_PACING_STALL, PM_STAT_AVG };
            QueryElement gpuDurationAvg{ this, PM_METRIC_GPU_DURATION, PM_STAT_AVG };
            QueryElement gpuBusyTimeAvg{ this, PM_METRIC_GPU_BUSY_TIME, PM_STAT_AVG };
            QueryElement gpuDisplayLatencyAvg{ this, PM_METRIC_DISPLAY_LATENCY, PM_STAT_AVG };
            QueryElement gpuDisplayDurationAvg{ this, PM_METRIC_DISPLAY_DURATION, PM_STAT_AVG };
            QueryElement gpuInputLatencyAvg{ this, PM_METRIC_INPUT_LATENCY, PM_STAT_AVG };
            QueryElement gpuPower{ this, PM_METRIC_GPU_POWER, PM_STAT_AVG, 1 };
        PM_END_QUERY dq{ *pSession, windowSize, metricOffset, 1, 1 };

        if (InitializeConsole() == false) {
            std::cout << "\nFailed to initialize console.\n";
            return -1;
        }

        while (!_kbhit()) {
            dq.Poll(proc);
            ConsolePrintLn("Presented FPS Average = %f", dq.fpsAvg.As<double>());
            ConsolePrintLn("Presented FPS 90% = %f", dq.fps90.As<double>());
            ConsolePrintLn("Presented FPS 95% = %f", dq.fps95.As<double>());
            ConsolePrintLn("Presented FPS 99% = %f", dq.fps99.As<double>());
            ConsolePrintLn("Presented FPS Max = %f", dq.fpsMax.As<double>());
            ConsolePrintLn("Presented FPS Min = %f", dq.fpsMin.As<double>());
            ConsolePrintLn("Frame Duration Average = %f", dq.frameDurationAvg.As<double>());
            ConsolePrintLn("Frame Pacing Stall Average = %f", dq.fpStallAvg.As<double>());
            ConsolePrintLn("GPU Duration Average = %f", dq.gpuDisplayDurationAvg.As<double>());
            ConsolePrintLn("GPU Busy Time Average = %f", dq.gpuBusyTimeAvg.As<double>());
            ConsolePrintLn("Display Latency Average = %f", dq.gpuDisplayLatencyAvg.As<double>());
            ConsolePrintLn("Display Duration Average = %f", dq.gpuDisplayDurationAvg.As<double>());
            ConsolePrintLn("Input Latency Average = %f", dq.gpuInputLatencyAvg.As<double>());
            ConsolePrintLn("GPU Power Average = %f", dq.gpuPower.As<double>());
            CommitConsole();
            std::this_thread::sleep_for(20ms);
        }
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

int PollMetrics(std::unique_ptr<pmapi::Session>&& pSession, unsigned int processId, double windowSize, double metricOffset)
{
    pmapi::ProcessTracker processTracker;

    try {
        std::vector<PM_QUERY_ELEMENT> elements;
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_APPLICATION, .stat = PM_STAT_MID_POINT, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_PRESENTED_FPS, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_PRESENTED_FPS, .stat = PM_STAT_PERCENTILE_90, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_PRESENTED_FPS, .stat = PM_STAT_PERCENTILE_95, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_PRESENTED_FPS, .stat = PM_STAT_PERCENTILE_90, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_PRESENTED_FPS, .stat = PM_STAT_MAX, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_PRESENTED_FPS, .stat = PM_STAT_MIN, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_FRAME_DURATION, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_CPU_FRAME_PACING_STALL, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_GPU_DURATION, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_GPU_BUSY_TIME, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_DISPLAY_LATENCY, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_DISPLAY_DURATION, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });
        elements.push_back(PM_QUERY_ELEMENT{ .metric = PM_METRIC_INPUT_LATENCY, .stat = PM_STAT_AVG, .deviceId = 0, .arrayIndex = 0 });

        auto dynamicQuery = pSession->RegisterDyanamicQuery(elements, windowSize, metricOffset);
        auto blobs = dynamicQuery.MakeBlobContainer(1u);

        if (InitializeConsole() == false) {
            OutputString("\nFailed to initialize console.\n");
            return -1;
        }

        while (!_kbhit()) {
            dynamicQuery.Poll(processTracker, blobs);

            for (auto pBlob : blobs) {
                ConsolePrintLn("Process Name = %s", *reinterpret_cast<const std::string*>(&pBlob[elements[0].dataOffset]));
                ConsolePrintLn("Presented FPS Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[1].dataOffset]));
                ConsolePrintLn("Presented FPS 90% = %f", *reinterpret_cast<const double*>(&pBlob[elements[2].dataOffset]));
                ConsolePrintLn("Presented FPS 95% = %f", *reinterpret_cast<const double*>(&pBlob[elements[3].dataOffset]));
                ConsolePrintLn("Presented FPS 99% = %f", *reinterpret_cast<const double*>(&pBlob[elements[4].dataOffset]));
                ConsolePrintLn("Presented FPS Max = %f", *reinterpret_cast<const double*>(&pBlob[elements[5].dataOffset]));
                ConsolePrintLn("Presented FPS Min = %f", *reinterpret_cast<const double*>(&pBlob[elements[6].dataOffset]));
                ConsolePrintLn("Frame Duration Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[7].dataOffset]));
                ConsolePrintLn("Frame Pacing Stall Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[8].dataOffset]));
                ConsolePrintLn("GPU Duration Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[9].dataOffset]));
                ConsolePrintLn("GPU Busy Time Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[10].dataOffset]));
                ConsolePrintLn("Display Latency Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[11].dataOffset]));
                ConsolePrintLn("Display Duration Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[12].dataOffset]));
                ConsolePrintLn("Input Latency Average = %f", *reinterpret_cast<const double*>(&pBlob[elements[13].dataOffset]));
            }
            CommitConsole();
            Sleep(10);
        }
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
