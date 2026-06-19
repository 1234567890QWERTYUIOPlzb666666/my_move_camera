# Architecture Patterns

## Contents

- Framework boundary
- Recommended repository layout
- Module interaction selection
- C ports and dependency injection
- Coordinator pattern
- UI backend boundary
- Action-camera media architecture
- Threading and shutdown
- CMake boundaries
- Adoption stages

## Framework Boundary

Put reusable mechanisms in the framework:

- module lifecycle
- event and task dispatch
- clocks and timers
- logging and diagnostics
- settings and filesystem primitives
- thread, mutex, and queue wrappers when portability requires them
- UI-thread dispatcher
- error and result conventions

Keep product decisions in modules or Application:

- when recording may start
- low-battery and thermal policies
- recording segmentation and naming
- gallery behavior
- camera-mode transitions

Use this test: if a second unrelated Linux product would not need the behavior, it probably does not belong in the framework.

## Recommended Repository Layout

```text
product/
|-- app/                         # composition root and executable
|-- framework/                   # product-independent mechanisms
|-- modules/
|   |-- camera/
|   |-- recording/
|   |-- gallery/
|   |-- storage/
|   |-- battery/
|   |-- thermal/
|   `-- update/
|-- application/                 # cross-module use cases/coordinators
|-- presentation/                # backend-neutral screen models
|-- ports/                       # contracts required by inner layers
|-- infrastructure/
|   |-- linux/
|   |-- vendor_sdk/
|   |-- v4l2/
|   `-- mocks/
|-- ui/
|   |-- qt/
|   `-- lvgl/
|-- tests/
`-- CMakeLists.txt
```

For a large organization, colocate a module's Domain, Application, Presentation, and tests under the feature when that reduces ownership conflicts. Preserve the same inward dependency rule.

## Module Interaction Selection

| Need | Mechanism | Example |
|---|---|---|
| Immediate result | Direct narrow interface | Query free storage |
| Request accepted/rejected | Use case or command API | Start recording |
| Fact notification | Typed event | SD card removed |
| Cross-module policy | Coordinator | Stop then shut down on critical heat |
| UI-readable aggregate | Immutable snapshot | Header status model |
| Video/audio frames | Dedicated pipeline | Capture to encoder |

An event bus reduces source-level references but creates behavioral coupling. Keep events typed, documented, observable, and limited to facts.

## C Ports and Dependency Injection

Define an interface owned by the consumer or neutral contract layer:

```c
typedef struct {
    void *context;
    bool (*is_ready)(void *context);
    int (*free_bytes)(void *context, uint64_t *out);
    int (*make_recording_path)(void *context, char *out, size_t size);
} StoragePort;

typedef struct {
    void *context;
    int (*prepare)(void *context, const RecorderConfig *config);
    int (*start)(void *context);
    int (*stop)(void *context);
} RecorderPort;

typedef struct {
    StoragePort storage;
    RecorderPort recorder;
    RecordState state;
} RecordingService;
```

Inject implementations at the composition root:

```c
int app_compose(AppContext *app)
{
    StoragePort storage = linux_storage_port(&app->storage);
    RecorderPort recorder = vendor_recorder_port(&app->recorder);

    return recording_service_init(
        &app->recording,
        &storage,
        &recorder);
}
```

Document for every pointer:

- borrowing or ownership transfer
- valid lifetime
- thread affinity
- whether callbacks may be reentrant
- who frees asynchronous payloads

## Coordinator Pattern

Keep storage concerned with storage facts. Put product policy in a coordinator:

```c
void safety_on_event(SafetyCoordinator *self, const AppEvent *event)
{
    switch (event->type) {
    case APP_EVENT_STORAGE_REMOVED:
        recording_request_stop(self->recording, STOP_STORAGE_REMOVED);
        break;
    case APP_EVENT_BATTERY_CRITICAL:
        recording_request_stop(self->recording, STOP_LOW_BATTERY);
        break;
    case APP_EVENT_THERMAL_HIGH:
        camera_request_lower_profile(self->camera);
        break;
    case APP_EVENT_THERMAL_CRITICAL:
        recording_request_stop(self->recording, STOP_OVERHEAT);
        power_request_shutdown_after_recording(self->power);
        break;
    default:
        break;
    }
}
```

Avoid coordinators that become universal managers. Split by workflow or policy, such as `SafetyCoordinator`, `CaptureCoordinator`, and `ShutdownCoordinator`.

## UI Backend Boundary

Share backend-neutral presentation data:

```c
typedef struct {
    char elapsed[16];
    char remaining[16];
    uint8_t battery_percent;
    RecordState recording;
    bool record_enabled;
    bool show_thermal_warning;
} CameraScreenModel;
```

Qt and LVGL render the same model using native controls. Keep `QObject`, `QString`, `lv_obj_t`, and UI callbacks inside their backend directories.

Use an application-level action vocabulary:

```c
typedef enum {
    APP_ACTION_RECORD_TOGGLE,
    APP_ACTION_TAKE_PHOTO,
    APP_ACTION_OPEN_GALLERY,
    APP_ACTION_OPEN_SETTINGS
} AppAction;
```

Physical buttons, Qt actions, LVGL callbacks, and remote control can map to the same use cases.

## Action-Camera Media Architecture

Keep control and frame paths separate:

```text
Control:
UI -> Application -> RecorderPort -> media implementation

Status:
media implementation -> typed event -> Application -> Presentation -> UI

Frame data:
Sensor -> ISP -> Capture -> Buffer Pool -> Encoder -> Muxer -> Storage
                         `-> Preview -> Display
Microphone -> Audio Encoder ---------> Muxer
```

Do not send frames through a generic event bus. Prefer vendor zero-copy handles, DMA buffers, bounded queues, and explicit backpressure.

Model recording as a state machine:

```text
Idle -> Preparing -> Recording -> Stopping -> Idle
           |             |           |
           `-----------> Error <-----'
```

Specify legal transitions, idempotency, timeout behavior, and file finalization. A stop request may need asynchronous completion before shutdown proceeds.

Expose capabilities instead of assuming all backends match:

```c
typedef struct {
    bool hdr;
    bool eis;
    bool video_4k60;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_fps;
} CameraCapabilities;
```

## Threading and Shutdown

Typical threads:

- UI/event-loop thread
- media control thread
- capture/encoder threads, often SDK-owned
- storage writer
- device monitor
- network worker

Rules:

- Call Qt/LVGL only on the UI thread.
- Keep SDK callbacks short; enqueue owned/copied state.
- Define queue bounds and overflow policy.
- Avoid blocking the UI thread on media or filesystem completion.
- Make shutdown a coordinated state machine.

Safe shutdown while recording:

```text
Reject new commands
-> request recording stop
-> drain encoder/muxer
-> finalize container
-> fsync/synchronize required files
-> stop preview/media pipeline
-> unmount storage if required
-> stop infrastructure
-> power off
```

## CMake Boundaries

```cmake
add_library(camera_domain STATIC ...)
add_library(camera_ports INTERFACE)
add_library(recording STATIC ...)
add_library(storage_linux STATIC ...)
add_library(camera_application STATIC ...)

target_link_libraries(recording
    PUBLIC camera_domain camera_ports)

target_link_libraries(storage_linux
    PUBLIC camera_ports
    PRIVATE platform_linux)

target_link_libraries(camera_application
    PRIVATE recording)
```

Select implementations at the composition root:

```cmake
set(UI_BACKEND "LVGL" CACHE STRING "QT or LVGL")
set(MEDIA_BACKEND "VENDOR" CACHE STRING "VENDOR, GSTREAMER or MOCK")
```

Do not let module targets link to both their contract and unrelated concrete implementations.

## Adoption Stages

1. Establish one vertical slice: preview, start recording, stop, finalize file.
2. Extract hardware/media Ports only at actual substitution or test boundaries.
3. Add Recording state machine and failure tests.
4. Add storage, battery, thermal, and shutdown coordinators.
5. Extract shared Presentation models when a second UI needs them.
6. Compile all variants in CI.
7. Split reusable framework code into another repository only after a second product proves reuse.
