#include "CppUnitTest.h"
#include "../PresentMonAPI2/source/PresentMonAPI.h"
#include <cstring>
#include <crtdbg.h>
#include <vector>
#include <optional>
#include <boost/process.hpp>
#include "../Interprocess/source/ExperimentalInterprocess.h"

#include "../PresentMonAPIWrapper/source/PresentMonAPIWrapper.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

PRESENTMON_API_EXPORT void pmSetMiddlewareAsMock_(bool mocked, bool useCrtHeapDebug = false);
PRESENTMON_API_EXPORT _CrtMemState pmCreateHeapCheckpoint_();
PRESENTMON_API_EXPORT PM_STATUS pmMiddlewareSpeak_(char* buffer);
PRESENTMON_API_EXPORT PM_STATUS pmMiddlewareAdvanceTime_(uint32_t milliseconds);

namespace PresentMonAPI2
{
	bool CrtDiffHasMemoryLeaks(const _CrtMemState& before, const _CrtMemState& after)
	{
		_CrtMemState difference;
		if (_CrtMemDifference(&difference, &before, &after)) {
			if (difference.lCounts[_NORMAL_BLOCK] > 0) {
				return true;
			}
		}
		return false;
	}

	TEST_CLASS(CAPISessionTests)
	{
	public:
		TEST_METHOD_CLEANUP(AfterEachTestMethod)
		{
			pmCloseSession();
		}
		TEST_METHOD(OpenAndCloseMockSession)
		{
			char buffer[256]{};

			pmSetMiddlewareAsMock_(true);
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmOpenSession());
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmMiddlewareSpeak_(buffer));
			Assert::AreEqual("mock-middle", buffer);
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmCloseSession());
			Assert::AreEqual((int)PM_STATUS_SESSION_NOT_OPEN, (int)pmMiddlewareSpeak_(buffer));
		}
		TEST_METHOD(OpenAndCloseConcreteSession)
		{
			char buffer[256]{};

			pmSetMiddlewareAsMock_(false);
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmOpenSession());
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmMiddlewareSpeak_(buffer));
			Assert::AreEqual("concrete-middle", buffer);
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmCloseSession());
		}
		TEST_METHOD(FailUsingClosedSession)
		{
			char buffer[256]{};

			pmSetMiddlewareAsMock_(true);
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmOpenSession());
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmCloseSession());
			Assert::AreEqual((int)PM_STATUS_SESSION_NOT_OPEN, (int)pmMiddlewareSpeak_(buffer));
		}
		TEST_METHOD(OpenAndCloseWithoutLeak)
		{
			pmSetMiddlewareAsMock_(true, true);
			const auto heapBefore = pmCreateHeapCheckpoint_();

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmOpenSession());
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmCloseSession());

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsFalse(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
		TEST_METHOD(OpenWithoutCloseCausesLeak)
		{
			pmSetMiddlewareAsMock_(true, true);
			const auto heapBefore = pmCreateHeapCheckpoint_();

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmOpenSession());

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsTrue(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
		TEST_METHOD(LeaksArentDetectedWithoutMockSetting)
		{
			pmSetMiddlewareAsMock_(true, false);
			const auto heapBefore = pmCreateHeapCheckpoint_();

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmOpenSession());

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsFalse(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
	};

	TEST_CLASS(CAPIIntrospectionTests)
	{
	public:
		TEST_METHOD_INITIALIZE(BeforeEachTestMethod)
		{
			pmSetMiddlewareAsMock_(true, true);
			pmOpenSession();
		}
		TEST_METHOD_CLEANUP(AfterEachTestMethod)
		{
			pmCloseSession();
		}
		TEST_METHOD(FreeIntrospectionTree)
		{
			const auto heapBefore = pmCreateHeapCheckpoint_();

			const PM_INTROSPECTION_ROOT* pRoot{};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmEnumerateInterface(&pRoot));
			Assert::IsNotNull(pRoot);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeInterface(pRoot));

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsFalse(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
		TEST_METHOD(LeakIntrospectionTree)
		{
			const auto heapBefore = pmCreateHeapCheckpoint_();

			const PM_INTROSPECTION_ROOT* pRoot{};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmEnumerateInterface(&pRoot));
			Assert::IsNotNull(pRoot);

			// normally we would free the linked structure here via its root
			// Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeInterface(pRoot));

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsTrue(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
		TEST_METHOD(Introspect)
		{
			// introspection query
			const PM_INTROSPECTION_ROOT* pRoot{};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmEnumerateInterface(&pRoot));
			Assert::IsNotNull(pRoot);
			Assert::AreEqual(12ull, pRoot->pEnums->size);
			Assert::AreEqual(8ull, pRoot->pMetrics->size);
			Assert::AreEqual(3ull, pRoot->pDevices->size);

			// checking 7th enum (unit)
			{
				auto pEnum = static_cast<const PM_INTROSPECTION_ENUM*>(pRoot->pEnums->pData[6]);
				Assert::IsNotNull(pEnum);
				Assert::AreEqual((int)PM_ENUM_UNIT, (int)pEnum->id);
				Assert::AreEqual("PM_UNIT", pEnum->pSymbol->pData);
				Assert::AreEqual("List of all units of measure used for metrics", pEnum->pDescription->pData);
				Assert::AreEqual(13ull, pEnum->pKeys->size);
				// 1st key
				{
					auto pKey = static_cast<const PM_INTROSPECTION_ENUM_KEY*>(pEnum->pKeys->pData[0]);
					Assert::IsNotNull(pKey);
					Assert::IsNotNull(pKey->pSymbol);
					Assert::AreEqual("PM_UNIT_DIMENSIONLESS", pKey->pSymbol->pData);
					Assert::AreEqual("Dimensionless", pKey->pName->pData);
					Assert::AreEqual("", pKey->pShortName->pData);
					Assert::AreEqual((int)PM_ENUM_UNIT, (int)pKey->enumId);
					Assert::AreEqual((int)PM_UNIT_DIMENSIONLESS, pKey->value);
				}
				// 5th key
				{
					auto pKey = static_cast<const PM_INTROSPECTION_ENUM_KEY*>(pEnum->pKeys->pData[4]);
					Assert::IsNotNull(pKey);
					Assert::IsNotNull(pKey->pSymbol);
					Assert::AreEqual("PM_UNIT_PERCENT", pKey->pSymbol->pData);
					Assert::AreEqual("Percent", pKey->pName->pData);
					Assert::AreEqual("%", pKey->pShortName->pData);
					Assert::AreEqual((int)PM_ENUM_UNIT, (int)pKey->enumId);
					Assert::AreEqual((int)PM_UNIT_PERCENT, pKey->value);
				}
			}

			// check device
			{
				auto pDevice = static_cast<const PM_INTROSPECTION_DEVICE*>(pRoot->pDevices->pData[0]);
				Assert::IsNotNull(pDevice);
				Assert::AreEqual(0, (int)pDevice->id);
				Assert::AreEqual((int)PM_DEVICE_TYPE_INDEPENDENT, (int)pDevice->type);
				Assert::AreEqual((int)PM_DEVICE_VENDOR_UNKNOWN, (int)pDevice->vendor);
				Assert::AreEqual("Device-independent", pDevice->pName->pData);
			}

			// check metric 1st
			{
				auto pMetric = static_cast<const PM_INTROSPECTION_METRIC*>(pRoot->pMetrics->pData[0]);
				Assert::IsNotNull(pMetric);
				Assert::AreEqual((int)PM_METRIC_DISPLAYED_FPS, (int)pMetric->id);
				Assert::AreEqual((int)PM_UNIT_FPS, (int)pMetric->unit);
				Assert::AreEqual((int)PM_DATA_TYPE_DOUBLE, (int)pMetric->typeInfo.type);
				Assert::AreEqual(7ull, pMetric->pStats->size);
				// check 1st stat
				{
					auto pStat = static_cast<const PM_STAT*>(pMetric->pStats->pData[0]);
					Assert::AreEqual((int)PM_STAT_AVG, (int)*pStat);
				}
				// check device info
				Assert::AreEqual(1ull, pMetric->pDeviceMetricInfo->size);
				{
					auto pInfo = static_cast<const PM_INTROSPECTION_DEVICE_METRIC_INFO*>(pMetric->pDeviceMetricInfo->pData[0]);
					Assert::AreEqual(0u, pInfo->deviceId);
					Assert::AreEqual(1u, pInfo->arraySize);
					Assert::AreEqual((int)PM_METRIC_AVAILABILITY_AVAILABLE, (int)pInfo->availability);
				}
			}

			// check metric gpu array 2 device (fan)
			{
				auto pMetric = static_cast<const PM_INTROSPECTION_METRIC*>(pRoot->pMetrics->pData[6]);
				Assert::IsNotNull(pMetric);
				Assert::AreEqual((int)PM_METRIC_GPU_FAN_SPEED, (int)pMetric->id);
				Assert::AreEqual((int)PM_UNIT_RPM, (int)pMetric->unit);
				Assert::AreEqual((int)PM_DATA_TYPE_DOUBLE, (int)pMetric->typeInfo.type);
				Assert::AreEqual(7ull, pMetric->pStats->size);
				// check 7th stat
				{
					auto pStat = static_cast<const PM_STAT*>(pMetric->pStats->pData[6]);
					Assert::AreEqual((int)PM_STAT_RAW, (int)*pStat);
				}
				// check device infos
				Assert::AreEqual(2ull, pMetric->pDeviceMetricInfo->size);
				{
					auto pInfo = static_cast<const PM_INTROSPECTION_DEVICE_METRIC_INFO*>(pMetric->pDeviceMetricInfo->pData[0]);
					Assert::AreEqual(1u, pInfo->deviceId);
					Assert::AreEqual(1u, pInfo->arraySize);
					Assert::AreEqual((int)PM_METRIC_AVAILABILITY_AVAILABLE, (int)pInfo->availability);
				}
				{
					auto pInfo = static_cast<const PM_INTROSPECTION_DEVICE_METRIC_INFO*>(pMetric->pDeviceMetricInfo->pData[1]);
					Assert::AreEqual(2u, pInfo->deviceId);
					Assert::AreEqual(2u, pInfo->arraySize);
					Assert::AreEqual((int)PM_METRIC_AVAILABILITY_AVAILABLE, (int)pInfo->availability);
				}
			}

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeInterface(pRoot));
		}
	};

	TEST_CLASS(CAPIDynamicQueryTests)
	{
	public:
		TEST_METHOD_INITIALIZE(BeforeEachTestMethod)
		{
			pmSetMiddlewareAsMock_(true, true);
			pmOpenSession();
		}
		TEST_METHOD_CLEANUP(AfterEachTestMethod)
		{
			pmCloseSession();
		}
		TEST_METHOD(CreateAndFreeQuery)
		{
			const auto heapBefore = pmCreateHeapCheckpoint_();

			PM_DYNAMIC_QUERY_HANDLE q = nullptr;
			PM_QUERY_ELEMENT elements[2]{
				PM_QUERY_ELEMENT{.metric = PM_METRIC_CPU_UTILIZATION, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_GPU_POWER, .deviceId = 1, .arrayIndex = 0},
			};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmRegisterDynamicQuery(&q, elements, std::size(elements)));
			Assert::IsNotNull(q);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeDynamicQuery(q));

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsFalse(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
		TEST_METHOD(VerifyOffsetsAndSizes)
		{
			PM_DYNAMIC_QUERY_HANDLE q = nullptr;
			PM_QUERY_ELEMENT elements[]{
				PM_QUERY_ELEMENT{.metric = PM_METRIC_CPU_UTILIZATION, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_PRESENT_MODE, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_GPU_POWER, .deviceId = 1, .arrayIndex = 0},
			};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmRegisterDynamicQuery(&q, elements, std::size(elements)));
			Assert::IsNotNull(q);

			Assert::AreEqual(0ull, elements[0].dataOffset);
			Assert::AreEqual(8ull, elements[0].dataSize);
			Assert::AreEqual(8ull, elements[1].dataOffset);
			Assert::AreEqual(4ull, elements[1].dataSize);
			Assert::AreEqual(12ull, elements[2].dataOffset);
			Assert::AreEqual(8ull, elements[2].dataSize);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeDynamicQuery(q));
		}
		TEST_METHOD(FailToRegisterStaticQuery)
		{
			PM_DYNAMIC_QUERY_HANDLE q = nullptr;
			PM_QUERY_ELEMENT elements[]{
				PM_QUERY_ELEMENT{.metric = PM_METRIC_PROCESS_NAME, .deviceId = 0, .arrayIndex = 0},
			};
			Assert::AreEqual((int)PM_STATUS_FAILURE, (int)pmRegisterDynamicQuery(&q, elements, std::size(elements)));
			Assert::IsNull(q);
		}
		TEST_METHOD(PollValuesTimeZero)
		{
			PM_DYNAMIC_QUERY_HANDLE q = nullptr;
			PM_QUERY_ELEMENT elements[]{
				PM_QUERY_ELEMENT{.metric = PM_METRIC_CPU_UTILIZATION, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_PRESENT_MODE, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_GPU_POWER, .deviceId = 1, .arrayIndex = 0},
			};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmRegisterDynamicQuery(&q, elements, std::size(elements)));
			Assert::IsNotNull(q);

			auto pBlob = std::make_unique<uint8_t[]>(elements[2].dataOffset + elements[2].dataSize);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmPollDynamicQuery(q, pBlob.get()));
			Assert::AreEqual((double)PM_METRIC_CPU_UTILIZATION, reinterpret_cast<double&>(pBlob[elements[0].dataOffset]));
			Assert::AreEqual((int)PM_PRESENT_MODE_HARDWARE_LEGACY_FLIP, reinterpret_cast<int&>(pBlob[elements[1].dataOffset]));
			Assert::AreEqual((double)PM_METRIC_GPU_POWER, reinterpret_cast<double&>(pBlob[elements[2].dataOffset]));

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeDynamicQuery(q));
		}
		TEST_METHOD(PollValuesOverTime)
		{
			PM_DYNAMIC_QUERY_HANDLE q = nullptr;
			PM_QUERY_ELEMENT elements[]{
				PM_QUERY_ELEMENT{.metric = PM_METRIC_CPU_UTILIZATION, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_PRESENT_MODE, .deviceId = 0, .arrayIndex = 0},
				PM_QUERY_ELEMENT{.metric = PM_METRIC_GPU_POWER, .deviceId = 1, .arrayIndex = 0},
			};
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmRegisterDynamicQuery(&q, elements, std::size(elements)));
			Assert::IsNotNull(q);

			auto pBlob = std::make_unique<uint8_t[]>(elements[2].dataOffset + elements[2].dataSize);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmPollDynamicQuery(q, pBlob.get()));
			Assert::AreEqual((double)PM_METRIC_CPU_UTILIZATION, reinterpret_cast<double&>(pBlob[elements[0].dataOffset]));
			Assert::AreEqual((int)PM_PRESENT_MODE_HARDWARE_LEGACY_FLIP, reinterpret_cast<int&>(pBlob[elements[1].dataOffset]));
			Assert::AreEqual((double)PM_METRIC_GPU_POWER, reinterpret_cast<double&>(pBlob[elements[2].dataOffset]));

			pmMiddlewareAdvanceTime_(1);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmPollDynamicQuery(q, pBlob.get()));
			Assert::AreEqual(0., reinterpret_cast<double&>(pBlob[elements[0].dataOffset]));
			Assert::AreEqual((int)PM_PRESENT_MODE_HARDWARE_INDEPENDENT_FLIP, reinterpret_cast<int&>(pBlob[elements[1].dataOffset]));
			Assert::AreEqual(0., reinterpret_cast<double&>(pBlob[elements[2].dataOffset]));

			pmMiddlewareAdvanceTime_(1);

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmPollDynamicQuery(q, pBlob.get()));
			Assert::AreEqual((double)PM_METRIC_CPU_UTILIZATION, reinterpret_cast<double&>(pBlob[elements[0].dataOffset]));
			Assert::AreEqual((int)PM_PRESENT_MODE_HARDWARE_LEGACY_FLIP, reinterpret_cast<int&>(pBlob[elements[1].dataOffset]));
			Assert::AreEqual((double)PM_METRIC_GPU_POWER, reinterpret_cast<double&>(pBlob[elements[2].dataOffset]));

			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmFreeDynamicQuery(q));
		}
	};

	TEST_CLASS(CAPIStaticQueryTests)
	{
	public:
		TEST_METHOD_INITIALIZE(BeforeEachTestMethod)
		{
			pmSetMiddlewareAsMock_(true, true);
			pmOpenSession();
		}
		TEST_METHOD_CLEANUP(AfterEachTestMethod)
		{
			pmCloseSession();
		}
		TEST_METHOD(PollStaticMetricString)
		{
			PM_QUERY_ELEMENT element{ .metric = PM_METRIC_PROCESS_NAME, .deviceId = 0, .arrayIndex = 0 };
			auto pBlob = std::make_unique<uint8_t[]>(260);
			Assert::AreEqual((int)PM_STATUS_SUCCESS, (int)pmPollStaticQuery(&element, pBlob.get()));
			Assert::AreEqual("dota2.exe", (const char*)pBlob.get());
		}
		TEST_METHOD(FailToPollDynamicMetricAsStatic)
		{
			PM_QUERY_ELEMENT element{ .metric = PM_METRIC_CPU_UTILIZATION, .deviceId = 0, .arrayIndex = 0 };
			auto pBlob = std::make_unique<uint8_t>(8);
			Assert::AreEqual((int)PM_STATUS_FAILURE, (int)pmPollStaticQuery(&element, pBlob.get()));
		}
	};

	TEST_CLASS(WrapperSessionTests)
	{
	public:
		TEST_METHOD_INITIALIZE(BeforeEachTestMethod)
		{
			pmSetMiddlewareAsMock_(true, true);
		}
		TEST_METHOD(SessionRountripWithDatasetNoLeaks)
		{
			const auto heapBefore = pmCreateHeapCheckpoint_();

			{
				pmapi::Session session;
				auto data = session.GetIntrospectionDataset();
			}			

			const auto heapAfter = pmCreateHeapCheckpoint_();
			Assert::IsFalse(CrtDiffHasMemoryLeaks(heapBefore, heapAfter));
		}
		TEST_METHOD(IntrospectSessionError)
		{
			using namespace std::string_literals;

			pmapi::Session session1;
			Assert::ExpectException<pmapi::SessionException>([] {
				pmapi::Session session2;
			});
		}
	};
	TEST_CLASS(WrapperDatasetTests)
	{
	public:
		TEST_METHOD_INITIALIZE(BeforeEachTestMethod)
		{
			pmSetMiddlewareAsMock_(true, true);
			session.emplace();
			data = session->GetIntrospectionDataset();
		}
		TEST_METHOD_CLEANUP(AfterEachTestMethod)
		{
			data.reset();
			session.reset();
		}
		TEST_METHOD(IntrospectRootRange)
		{
			using namespace std::string_literals;

			const std::vector expected{
				"PM_STATUS"s, "PM_METRIC"s, "PM_METRIC_TYPE"s, "PM_DEVICE_VENDOR"s, "PM_PRESENT_MODE"s, "PM_PSU_TYPE"s,
				"PM_UNIT"s, "PM_STAT"s, "PM_DATA_TYPE"s, "PM_GRAPHICS_RUNTIME"s, "PM_DEVICE_TYPE"s, "PM_METRIC_AVAILABILITY"s
			};
			auto e = expected.begin();
			for (auto ev : data->GetEnums()) {
				Assert::AreEqual(*e, ev.GetSymbol());
				e++;
			}
		}
		TEST_METHOD(IntrospectViewRange)
		{
			using namespace std::string_literals;

			const std::vector expected{
				"PM_STATUS_SUCCESS"s,
				"PM_STATUS_FAILURE"s,
				"PM_STATUS_SESSION_NOT_OPEN"s,
			};
			auto e = expected.begin();
			for (auto kv : data->GetEnums().begin()->GetKeys()) {
				Assert::AreEqual(*e, kv.GetSymbol());
				e++;
			}
		}
		TEST_METHOD(IntrospectMetricToEnumKey)
		{
			using namespace std::string_literals;
			Assert::AreEqual("Displayed FPS"s, data->GetMetrics().begin()->GetMetricKey().GetName());
		}
		TEST_METHOD(IntrospectMetricUnit)
		{
			using namespace std::string_literals;
			Assert::AreEqual("fps"s, data->GetMetrics().begin()->GetUnit().GetShortName());
		}
		TEST_METHOD(IntrospectMetricStats)
		{
			using namespace std::string_literals;
			Assert::AreEqual("Average"s, data->GetMetrics().begin()->GetStats().begin()->GetName());
			Assert::AreEqual(7ull, data->GetMetrics().begin()->GetStats().size());
		}
		TEST_METHOD(IntrospectMetricStatsWithLookup)
		{
			using namespace std::string_literals;
			Assert::AreEqual("avg"s, data->GetMetrics().begin()->GetStats().begin()->GetShortName());
		}
		TEST_METHOD(IntrospectMetricDataType)
		{
			using namespace std::string_literals;
			auto metric = data->FindMetric(PM_METRIC_PRESENT_MODE);
			auto type = metric.GetDataTypeInfo();
			Assert::AreEqual("Present Mode"s, metric.GetMetricKey().GetName());
			Assert::AreEqual("PM_PRESENT_MODE"s, type.GetEnum().GetSymbol());
		}
		TEST_METHOD(IntrospectMetricDeviceMetricInfo)
		{
			using namespace std::string_literals;
			auto metric = data->FindMetric(PM_METRIC_GPU_FAN_SPEED);
			auto deviceInfos = metric.GetDeviceMetricInfo();
			Assert::AreEqual(2ull, deviceInfos.size());
			{
				auto deviceInfo = deviceInfos.begin()[0];
				Assert::AreEqual((int)PM_METRIC_AVAILABILITY_AVAILABLE, deviceInfo.GetAvailablity().GetValue());
				Assert::AreEqual(1u, deviceInfo.GetArraySize());
			}
			{
				auto deviceInfo = deviceInfos.begin()[1];
				Assert::AreEqual((int)PM_METRIC_AVAILABILITY_AVAILABLE, deviceInfo.GetAvailablity().GetValue());
				Assert::AreEqual(2u, deviceInfo.GetArraySize());
			}
		}
		TEST_METHOD(IntrospectDevices)
		{
			using namespace std::string_literals;
			auto devices = data->GetDevices();
			Assert::AreEqual(3ull, devices.size());
			{
				auto device = devices.begin()[0];
				Assert::AreEqual("Unknown"s, device.GetVendor().GetName());
				Assert::AreEqual("Device-independent"s, device.GetName());
				Assert::AreEqual("Device Independent"s, device.GetType().GetName());
				Assert::AreEqual(0u, device.GetId());
			}
			{
				auto device = devices.begin()[1];
				Assert::AreEqual("Intel"s, device.GetVendor().GetName());
				Assert::AreEqual("Arc 750"s, device.GetName());
				Assert::AreEqual("Graphics Adapter"s, device.GetType().GetName());
				Assert::AreEqual(1u, device.GetId());
			}
			{
				auto device = devices.begin()[2];
				Assert::AreEqual("NVIDIA"s, device.GetVendor().GetName());
				Assert::AreEqual("GeForce RTX 2080 ti"s, device.GetName());
				Assert::AreEqual("Graphics Adapter"s, device.GetType().GetName());
				Assert::AreEqual(2u, device.GetId());
			}
		}
		TEST_METHOD(IntrospectLookupDevice)
		{
			using namespace std::string_literals;
			{
				auto device = data->FindDevice(1);
				Assert::AreEqual("Intel"s, device.GetVendor().GetName());
				Assert::AreEqual("Arc 750"s, device.GetName());
				Assert::AreEqual("Graphics Adapter"s, device.GetType().GetName());
				Assert::AreEqual(1u, device.GetId());
			}
		}
		TEST_METHOD(IntrospectMetricDeviceMetricInfoLookupDevice)
		{
			using namespace std::string_literals;
			auto metric = data->FindMetric(PM_METRIC_GPU_FAN_SPEED);
			auto deviceInfos = metric.GetDeviceMetricInfo();
			Assert::AreEqual(2ull, deviceInfos.size());
			{
				auto deviceInfo = deviceInfos.begin()[1];
				Assert::AreEqual((int)PM_METRIC_AVAILABILITY_AVAILABLE, deviceInfo.GetAvailablity().GetValue());
				Assert::IsTrue(deviceInfo.IsAvailable());
				Assert::AreEqual(2u, deviceInfo.GetArraySize());
				Assert::AreEqual("NVIDIA"s, deviceInfo.GetDevice().GetVendor().GetName());
			}
		}
		TEST_METHOD(IntrospectMetricLookupError)
		{
			using namespace std::string_literals;
			Assert::ExpectException<pmapi::LookupException>([this] {
				data->FindMetric((PM_METRIC)420);
			});
		}
		TEST_METHOD(IntrospectDatatypeError)
		{
			using namespace std::string_literals;
			Assert::ExpectException<pmapi::DatatypeException>([this] {
				data->FindMetric(PM_METRIC_FRAME_TIME).GetDataTypeInfo().GetEnum();
			});
		}
		TEST_METHOD(IntrospectMetricType)
		{
			using namespace std::string_literals;

			Assert::AreEqual("Dynamic Metric"s, data->FindMetric(PM_METRIC_CPU_UTILIZATION).GetType().GetName());
			Assert::AreEqual("Static Metric"s, data->FindMetric(PM_METRIC_PROCESS_NAME).GetType().GetName());
		}
	private:
		std::optional<pmapi::Session> session;
		std::optional<pmapi::intro::Dataset> data;
	};

	TEST_CLASS(ProcessTests)
	{
	public:
		TEST_METHOD(ReadStdout)
		{
			namespace bp = boost::process;
			using namespace std::string_literals;

			bp::ipstream out; // Stream for reading the process's output
			bp::opstream in;  // Stream for writing to the process's input

			bp::child process("InterprocessMock.exe"s, bp::std_out > out, bp::std_in < in);

			std::string output;
			out >> output;

			process.wait();

			Assert::AreEqual("default-output"s, output);
		}
		TEST_METHOD(ReadStdoutWithCLI)
		{
			namespace bp = boost::process;
			using namespace std::string_literals;

			bp::ipstream out; // Stream for reading the process's output
			bp::opstream in;  // Stream for writing to the process's input

			bp::child process("InterprocessMock.exe"s, "--test-f"s, bp::std_out > out, bp::std_in < in);

			std::string output;
			out >> output;

			process.wait();

			Assert::AreEqual("inter-process-stub"s, output);
		}
		TEST_METHOD(ReadIpcStringMessage)
		{
			namespace bp = boost::process;
			using namespace std::string_literals;

			bp::ipstream out; // Stream for reading the process's output
			bp::opstream in;  // Stream for writing to the process's input

			bp::child process("InterprocessMock.exe"s, "--basic-message"s, bp::std_out > out, bp::std_in < in);

			// write the code string to server via stdio
			in << "scooby-dooby" << std::endl;

			// wait for goahead from server via stdio
			std::string go;
			out >> go;

			// connect client
			auto pClient = pmon::ipc::experimental::IClient::Make();

			// read string via shared memory
			Assert::AreEqual("scooby-dooby-served"s, pClient->Read());

			// ack to server that read is complete via stdio
			in << "ack" << std::endl;

			// wait for mock process to exit
			process.wait();
		}
		TEST_METHOD(ReadIpcPointerMessage)
		{
			namespace bp = boost::process;
			using namespace std::string_literals;

			bp::ipstream out; // Stream for reading the process's output
			bp::opstream in;  // Stream for writing to the process's input

			bp::child process("InterprocessMock.exe"s, "--basic-message"s, bp::std_out > out, bp::std_in < in);

			// write the code string to server via stdio
			in << "scooby-dooby" << std::endl;

			// wait for goahead from server via stdio
			std::string go;
			out >> go;

			// connect client
			auto pClient = pmon::ipc::experimental::IClient::Make();

			// read string via shared memory
			Assert::AreEqual("scooby-dooby-served"s, pClient->ReadWithPointer());

			// ack to server that read is complete via stdio
			in << "ack" << std::endl;

			// wait for mock process to exit
			process.wait();
		}
		TEST_METHOD(ReadIpcUptrMessage)
		{
			namespace bp = boost::process;
			using namespace std::string_literals;

			bp::ipstream out; // Stream for reading the process's output
			bp::opstream in;  // Stream for writing to the process's input

			bp::child process("InterprocessMock.exe"s, "--basic-message"s, bp::std_out > out, bp::std_in < in);

			// write the code string to server via stdio
			in << "scooby-dooby" << std::endl;

			// wait for goahead from server via stdio
			std::string go;
			out >> go;

			// connect client
			auto pClient = pmon::ipc::experimental::IClient::Make();

			// read string via shared memory
			Assert::AreEqual("scooby-dooby-u-served"s, pClient->ReadWithUptr());

			// ack to server that read is complete via stdio
			in << "ack" << std::endl;

			// wait for mock process to exit
			process.wait();
		}
	};
}
