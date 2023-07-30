# PresentMon Capture Application

 ![Architecture](docs/images/app-cef-overlay-architecture.jpg)

## Overview

The PresentMon Capture Application is both an offline trace capture and realtime performance overlay for games and other graphics-intensive applications. It uses the PresentMon Service to source performance data, a custom Direct3D 11 renderer to display a realtime performance overlay, and a CEF-based UI to configure overlay and trace capture functionality. 

## License

Copyright (C) 2017-2023 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Building

### Prerequisites

Node.js / NPM are required to build the web assets for the control UI. A specific root signing authority certificate is required to be present on the system to build in release configuration.

### Chromium Embedded Framework (CEF)

#### Version

CEF version 114.2.11 is officially supported. Proximal versions will most likely be compatible, but are not officially supported.

#### Download Distribution

Download CEF distribution from spotify and extract to a local folder. The "Minimal Distribution" is sufficient. [Download Link](https://cef-builds.spotifycdn.com/index.html)

#### Build Wrapper

The "Binary Distribution" also contains source code which must be built. Build steps are as follow:

0. In the distribution root, follow the instructions in CMakeLists.txt to generate a .sln file for a Windows 64-bit CEF binary distribution
0. Open the generated .sln file
0. Update the libcef_dll_wrapper properties
    - Project Properties > C++ > Code Generation > Runtime Library: set to Multi-threaded DLL for Release
    - Project Properties > C++ > Code Generation > Runtime Library: set to Multi-threaded Debug DLL for Debug
    - Project Properties > C++ > Preprocessor > Preprocessor Definitions: set the following macros for Debug
       - _ITERATOR_DEBUG_LEVEL=2
       - _HAS_ITERATOR_DEBUGGING=1
0. Build the Release and Debug versions of the wrapper library

#### Pull CEF Distribution into AppCef Project

The files from the CEF distribution need to be rearranged before they can be consumed by this project. A batch file is provided that will copy necessary files from the CEF distribution folder to the required locations in the AppCef project folder.

Relative to the PresentMon solution root, the batch file is located at `AppCef\Batch\pull-cef.bat`. Run this batch command by passing it the full path to the CEF distribution.

### Web Assets

#### Grab Dependencies

Download dependencies via NPM (only needs to be run once on fresh clone, or after new packages are added)

```
npm ci
```

#### Build Vue.js SPA

You need to build the Vue.js web application (compile single file components and bundle into chunks) before it can be loaded by the Chromium engine embedded in the desktop application.

These build artifacts are automatically copied to the output directory as a post-build step in the MSVC project. Furthermore, in Release builds the post-build script automatically downloads dependencies and executes a production build.

You can either run a development build process, which builds in dev mode and starts a local server with hotloading support:

```
npm run serve
```

Or you can do a full production build, which places all necessary build artifacts in a directory named `dist/`:

```
npm run build
```

Note that the command line option `--p2c-url=http://localhost:8080/` is necessary to use the local development server.

### C++ Application

#### Debug Configuration

Debug configuration has no special requirements to build. It can be built and run from Visual Studio with debugger attached, because it does not create its overlay window in an elevated Z-band, and thus does not need uiAccess to be set.

#### Release Configuration

Release configuration requires a specific Trusted Root Certificate to be present on the system in order to successfully build (see next section). Release configuration creates its overlay window in an elevated Z-band, requiring uiAccess to be set, which in turn requires that the executable be cryptographically signed.

### Trusted Root Certificate

The build automation for Release configuration signs the executable as a post-build step. For this to work, your `PrivateCertStore` must contain a certificate named "Test Certificate - For Internal Use Only".

Such a certificate can be created with the following command:

```
makecert -r -pe -n "CN=Test Certificate - For Internal Use Only" -ss PrivateCertStore testcert.cer
```

## Running

### Debug Configuration

No special instructions. Can be run from IDE with debugger attached. 

(Note: Visual Studio stores the Debugger working directory setting in user-local config. When doing a fresh clone, you will need to change the Working Directory Debugging setting to `$(OutDir)`)

### Release Configuration

Since `uiAccess=true`, the application must be run from a secure location (e.g. "Program Files" or "System32"). It also cannot be started from Visual Studio (either with or without debugger attached, even if VS is running with admin privilege).

### Command Line Options (AppCef)

The app will load web content located at Web/index.html by default. The following can be used to load from an HTTP server (typically a local dev server) instead:
```
--p2c-url=http://localhost:8080/
```
Only severe errors are logged by default in Release configuration. To log all errors in release:
```
--p2c-verbose
```
Logs and cache files are written to %AppData%\PresentMon2Capture by default. You can change this to the working directory of the application (convenient when launching Debug build from IDE):
```
--p2c-files-working
```
Enable experimental support for tearing presents (required for Variable Refresh Rate):
```
--p2c-allow-tearing
```
In Debug configuration, the application will halt with a modal error dialog whenever a resource is requested from a non-local (network) URL. This flag disables that behavior:
```
--p2c-no-net-fail
```

## Projects

### AppCef

Main deliverable. Contains all logic for the UI control layer (mainly in the form of Vue.js components). Contains CEF API and binaries.

Depends on: Core, Shaders

Dependencies: None

### Core

Contains code for spawning Z-band overlay window, rendering realtime graphs in Direct 3D, and interfacing with PresentMon 2 service.

Depends on: None

Dependencies: AppCef, Unit Tests

### Unit Tests

Automated testing for select Core components.

Depends on: Core

Dependencies: None

### Shaders

Contains shaders needed for rendering the overlay.

Depends on: None

Dependencies: AppCef

## Technology

### Z-band

[Z-bands](https://blog.adeltax.com/window-z-order-in-windows-10/)

Windows has a concept of Z-bands. These add a hierarchical layer to the idea of Z-order, such that all windows in a higher Z-band will always appear on top of windows in any lower Z-bands. By default, user application windows are created in the lowest Z-band (ZBID_DESKTOP), and certain OS elements such as the Start Menu or Xbox Game Bar exist on higher Z-bands.

There exists an undocumented WinAPI function called `CreateWindowInBand` that allows an application to create a window in a Z-band above the default one. When this function is called, the OS will perform a check to make sure the application has the required privileges. We give the app these privileges by setting `uiAccess=true` in the app manifest.

#### Motivation

Our motivation to use `CreateWindowInBand` is to ensure that the performance monitoring overlay appears above the target game application, even when running in fullscreen exclusive mode.

### uiAccess

[MSDN:uiAccess](https://docs.microsoft.com/en-us/windows/security/threat-protection/security-policy-settings/user-account-control-only-elevate-uiaccess-applications-that-are-installed-in-secure-locations)

`uiAccess` is an option that is set in an executable's manifest. It enables bypassing UI restrictions and is meant mainly for accessibility applications such as IMEs that need to appear above the active application.

This ability to bypass UI restrictions means that certain precaution are taken with respect to uiAccess applications:

- The application must be cryptographically signed to protect against tampering
- The application must be run from a trusted location (such as "Program Files")

#### Issues

- There seems to be problems with spawning a uiAccess process from another (non-admin) process.
- There might be problems when a normal (non-admin) process tries to Send/PostMessage to a uiAccess process

#### uiAccess Application Special Abilities / Vulnerabilities

- Set the foreground window.
- Drive any application window by using the SendInput function.
- Use read input for all integrity levels by using low-level hooks, raw input, - GetKeyState, GetAsyncKeyState, and GetKeyboardInput.
- Set journal hooks.
- Use AttachThreadInput to attach a thread to a higher integrity input queue.

#### Observations

We have noted that an application can remain on top (even above fullscreen exclusive games) when uiAccess is set to true, even when `CreateWindowInBand` is not used. This also seems to be reported elsewhere (https://www.autohotkey.com/boards/viewtopic.php?t=75695).

#### Related Info

[MSDN:Integrity Levels](https://docs.microsoft.com/en-us/previous-versions/dotnet/articles/bb625963(v=msdn.10)?redirectedfrom=MSDN)

### CEF

(https://bitbucket.org/chromiumembedded/cef/wiki/Home)

PresentMon 2 Capture uses CEF to implement the the control UI. 

The Chromium Embedded Framework (CEF) is a C++ framework that streamlines development of custom applications with Chromium. With some minimal bootstrapping and configuring code, the framework will spin up and connect Chromium components, binding them to windows, inputs, sockets, etc. on the platform of choice.

Behavior of the framework can be customized by inheriting from base class interfaces and injecting them into the framework, thus hooking various callback functions to implement your desired behavior. In particular, custom objects can be implemented in C++ and then injected into the global (window) namespace in V8 to create an interop between JS and C++ code.

A major challenge when dealing with CEF is the multi-process nature of Chromium. One must be aware at all time on which process and which thread each piece of code is running on. Thread task queues and IPC message queues are used to make sure that operations are executed on the appropriate thread and process. V8 contexts must also be captured and managed when interacting with V8 state.

## Metric Definitions

| Metric                             |  Description                                  |  Stats Generated Using User Specified Settings                                                    |
| ---------------------------------- | ----------------------------------------------|:-------------------------------------------------------------------------------------------------:|
| Presented FPS                      | Rate of frames presented by the application.                                                                                                  | Y |
| Displayed FPS                      | Rate of frames scanned out to the display.                                                                                                    | Y |
| GPU Busy                           | Average time GPU was busy.                                                                                                                    | Y |
| Time Between Presents              | The time between Present() calls, in milliseconds.                                                                                            | N |
| Dropped Frames                     | Percentage of frames dropped because they were replaced by a more recent frame.                                                               | Y |
| Present Mode                       | The presentation mode used by the system. Only the most recent Present() call is returned.                                                    | N |
| Sync Interval                      | The sync interval provided by the application in the Present() call. Only the most recent Present() call is returned.                         | N |
| Allows Tearing                     | Indicates whether tearing is possible. Only the most recent frame is returned.                                                                | N |
| Gfx Latency Display                | Time between when GPU work for a frame has completed to when the frame is displayed.                                                          | N |
| Gfx Latency Render                 | Time between the Present() call  and when the frame is displayed.                                                                             | N |
| GPU Power                          | GPU power usage in W                                                                                                                          | Y |
| GPU Sustained Power Limit          | Sustained GPU power limit in W                                                                                                                | Y |
| GPU Voltage                        | Voltage feeding the GPU chip                                                                                                                  | Y |
| GPU Frequency                      | GPU chip frequency                                                                                                                            | Y |
| GPU Temp                           | GPU chip temperature in C                                                                                                                     | Y |
| GPU Utilization                    | Percentage utilization of the GPU                                                                                                             | Y |
| GPU Power Limited                  | Percentage of the time the GPU frequency is being throttled because the GPU chip is exceeding the maximum power limits                        | Y |
| GPU Temperature Limited            | Percentage of the time the GPU frequency is being throttled because the GPU chip is exceeding the maximum temperature limits                  | Y |
| GPU Current Limited                | Percentage of the time the GPU frequency is being throttled because the GPU chip is exceeding the power supply current limits                 | Y |
| GPU Voltage Limited                | Percentage of the time the GPU frequency is being throttled because the GPU chip is exceeding the voltage limits                              | Y |
| GPU Utilization Limited            | Percentage of time the GPU frequency has been lowered due to low GPU utilization                                                              | Y |
| VRAM Power Limited                 | Percentage of the time the memory frequency is being throttled because the memory modules are exceeding the maximum power limits              | Y |
| VRAM Temperature Limited           | Percentage of the time the memory frequency is being throttled because the memory modules are exceeding the maximum temperature limits        | Y |
| VRAM Current Limited               | Percentage of the time the memory frequency is being throttled because the memory modules are exceeding the power supply current limits       | Y |
| VRAM Voltage Limited               | Percentage of the time the memory frequency is being throttled because the memory modules are exceeding the voltage limits                    | Y |
| VRAM Utilization Limited           | Percentage of time the memory frequency has been lowered due to low memory traffic                                                            | Y |
| CPU Utilization                    | Percentage utilization of the CPU                                                                                                             | Y |
| CPU Power                          | CPU power usage in W                                                                                                                          | Y |
| CPU Power Limit                    | Sustained CPU power limit in W                                                                                                                | Y |
| CPU Temperature                    | CPU temperature in C                                                                                                                          | Y |
| CPU Frequency                      | CPU frequency in GHz                                                                                                                          | Y |
| Date Time	                         | The current date and time. Shown in YYYY-MM-DD HH:MM:SS:MS format	                                                                         | N |
| Elapsed Time                       | The amount of elapsed time since selecting the process to monitor. Shown in HH:MM:SS:MS format	                                             | N |
| GPU Name                           | GPU name as provided by the respective graphics adapter.	                                                                                     | N |                           
| GPU Fan Speed 0                    | GPU fan speed in RPMs.                                           	                                                                         | Y |
| GPU Fan Speed 1                    | GPU fan speed in RPMs.                                           	                                                                         | Y |
| GPU Fan Speed 2                    | GPU fan speed in RPMs.                                           	                                                                         | Y |
| GPU Fan Speed 3                    | GPU fan speed in RPMs.                                           	                                                                         | Y |
| GPU Fan Speed 4                    | GPU fan speed in RPMs.                                           	                                                                         | Y |
| GPU Render/Compute Utilization     | Percentage utilization of the 3D and Compute blocks in the GPU.                                                                               | Y |                                                                             
| GPU Media Utilization              | Percentage utilization of the media blocks in the GPU.                                                                                        | Y |
| VRAM Power                         | Memory module power usage in Watts.                                                                                                           | Y |
| VRAM Voltage                       | Voltage feeding the memory modules.                                                                                                           | Y |
| VRAM Frequency                     | Memory module frequency.                                                                                                                      | Y |
| VRAM Frequency Effective Bandwidth | Effective data transfer rate the memory modules can sustain based on the current clock frequency.                                             | Y |
| VRAM Read Bandwidth                | Current memory module read bandwidth in bytes per second.                                                                                     | Y |
| VRAM Write Bandwidth               | Current memory module write bandwidth in bytes per second.                                                                                    | Y |
| VRAM Temperature                   | Memory modules temperature in celsius.                                                                                                        | Y |
| GPU Memory Size                    | Total GPU memory size in bytes.	                                                                                                             | Y |
| GPU Memory Used                    | Total GPU memory used in bytes.	                                                                                                             | Y |
| GPU Memory Read Bandwidth          | Current memory module read bandwidth in bytes per second.                                                                                     | Y |
| GPU Memory Write Bandwidth         | Current memory module write bandwidth in bytes per second.                                                                                    | Y |
| GPU Memory Max Bandwidth           | Max memory module write bandwidth in bytes per second.                                                                                        | Y |
| GPU Memory Used %                  | Percent utilization of the of GPU memory.	                                                                                                 | Y |
| PSU Type 0                         | Specfies the Power Supply Type (PCIE, Pin6, Pin8) at index 0.	                                                                             | Y |
| PSU Type 1                         | Specfies the Power Supply Type (PCIE, Pin6, Pin8) at index 1.	                                                                             | Y |
| PSU Type 2                         | Specfies the Power Supply Type (PCIE, Pin6, Pin8) at index 2.	                                                                             | Y |
| PSU Type 3                         | Specfies the Power Supply Type (PCIE, Pin6, Pin8) at index 3.	                                                                             | Y |
| PSU Type 4                         | Specfies the Power Supply Type (PCIE, Pin6, Pin8) at index 4.	                                                                             | Y |
| PSU Power 0                        | Total energy consumed by the power source in Watts.	                                                                                         | Y |
| PSU Power 1                        | Total energy consumed by the power source in Watts.	                                                                                         | Y |
| PSU Power 2                        | Total energy consumed by the power source in Watts.	                                                                                         | Y |
| PSU Power 3                        | Total energy consumed by the power source in Watts.	                                                                                         | Y |
| PSU Power 4                        | Total energy consumed by the power source in Watts.	                                                                                         | Y |
| PSU Voltage 0                      | Voltage of the power source in Volts.	                                                                                                     | Y |
| PSU Voltage 1                      | Voltage of the power source in Volts.	                                                                                                     | Y |
| PSU Voltage 2                      | Voltage of the power source in Volts.	                                                                                                     | Y |
| PSU Voltage 3                      | Voltage of the power source in Volts.	                                                                                                     | Y |
| PSU Voltage 4                      | Voltage of the power source in Volts.	                                                                                                     | Y |

## Comma-separated value (CSV) file output

### CSV file names

The PresentMon capture application creates two CSV files per capture. The first records the raw frame data of the capture and is named using the following pattern: "pmcap-[executablename]-YYMMDD-HHMMSS.csv".
The second CSV file generated is a stats summary file for the capture. It includes the duration of the capture, the total number of frames captured, plus the average, minimum, maximum, 99th, 95th and
90th FPS percentiles. The stats file is named using the following pattern: "pmcap-[executablename]-YYMMDD-HHMMSS-stats.csv". All files are stored in the user's appdata local directory in the "Intel\PresentMon\Capture" folder. 

### CSV columns

The PresentMon capture application outputs all of the telemetry provided by the PresentMon Service. Please see the following link for all metric definitions:

[CSV Column Definitions](pmcli-readme.md#csv-columns)

Note: There is no method for filtering the number of telemetry items reported by the PresentMon capture application. If you wish to limit or filter the number of telemetry items,
consider using the PresentMonCli application.