---
name: design-embedded-ui-architecture
description: Design, review, or refactor modular C/C++ application architecture for embedded Linux products using Qt, QML, LVGL, vendor media SDKs, or replaceable hardware backends. Use for UI framework boundaries, action-camera software planning, module dependency and circular-dependency problems, ports and adapters, event buses, coordinators, threading, media pipelines, CMake target structure, portability, testability, and multi-team ownership.
---

# Design Embedded UI Architecture

Design product architecture with explicit dependency direction, stable module contracts, and replaceable UI/platform implementations. Optimize for realistic product constraints rather than maximum abstraction.

## Workflow

1. Establish product and platform constraints:
   - OS or RTOS, CPU/MCU, memory, display, input, startup time, and real-time needs.
   - UI backend: Qt Widgets, QML, LVGL, or multiple product variants.
   - Hardware and middleware: camera sensor, ISP, codec, V4L2, GStreamer, vendor SDK, storage, network.
   - Team boundaries, product variants, test environment, and expected ports.

2. Identify business capabilities before technical layers. Typical action-camera modules include:
   - camera, recording, photo, preview, gallery
   - storage, battery, thermal, power
   - connectivity, update, settings, diagnostics

3. Define the dependency rule:

   ```text
   UI -> Presentation -> Application -> Domain
                              |
                              v
                            Ports
                              ^
                              |
                 Infrastructure / Platform
   ```

   Keep Qt, QML, LVGL, POSIX, and vendor SDK types outside Domain and Application public contracts.

4. Classify every cross-module interaction:
   - Synchronous query or command with an immediate result: use a narrow Port/interface.
   - Asynchronous fact that already occurred: publish a typed event.
   - Policy spanning multiple modules: place it in Application or a Coordinator.
   - High-rate media or sensor data: use a dedicated bounded pipeline, buffer pool, or zero-copy channel.
   - Shared status for presentation: expose immutable snapshots; avoid writable global state.

5. Break cycles by moving policy upward, not by hiding dependencies in `common/`:

   ```text
   Recording -> StoragePort
   Storage implementation -> StoragePort
   Storage -> StorageRemoved event
   SafetyCoordinator -> Recording stop command
   ```

6. Define public contracts before implementations. Keep interfaces narrow and state ownership explicit. For C, prefer structs plus function tables or explicit dependency structs; do not introduce a reflection-style IoC container unless the project already uses one.

7. Define lifecycle and threading:
   - Centralize composition in `app_context` or the composition root.
   - Use phased `init/start/stop/deinit`; destroy in reverse order.
   - Restrict all Qt/LVGL calls to the UI thread.
   - Separate control events from video/audio frame transport.
   - Specify queue capacity, overflow policy, object ownership, cancellation, and shutdown behavior.

8. Encode architecture in the build:
   - Build modules as separate CMake targets.
   - Export only public include directories.
   - Select Qt/LVGL, hardware, and mock backends at composition/build time.
   - Make forbidden cycles visible as target dependency errors.

9. Plan verification:
   - Unit-test Domain, Application, presenters, and state machines with mocks.
   - Integration-test storage/media/platform adapters.
   - Test card removal, full disk, low battery, overheating, codec failure, and shutdown during recording.
   - Compile all supported backend variants in CI.

## Design Rules

- Treat a framework as reusable runtime mechanisms, not a container for product business logic.
- Share ViewModels, actions, use cases, and state conversions across Qt/LVGL; implement concrete widgets separately.
- Do not build a universal widget abstraction over all Qt and LVGL features.
- Let each module own its state and vocabulary.
- Distinguish commands from events: commands request work; events report facts.
- Avoid a global service locator in business code. Inject dependencies at construction/init time.
- Avoid using an event bus for request/response calls or high-frequency frame data.
- Keep vendor-specific capabilities available through explicit capability models or extension interfaces.
- Prefer a practical boundary with one known implementation over speculative abstraction with no replacement case.

## Deliverables

When designing a system, provide:

1. Constraints and assumptions.
2. Module map and responsibility table.
3. Compile-time dependency diagram.
4. Runtime command, event, and data flows.
5. Key C/C++ public interfaces with ownership semantics.
6. Thread and lifecycle model.
7. Directory and CMake target layout.
8. Failure handling and test strategy.
9. Risks, deliberate non-abstractions, and migration stages.

When reviewing an existing repository, inspect actual includes, target links, globals, callbacks, threads, and ownership before proposing changes. Lead with concrete dependency cycles and unsafe boundaries.

## References

- Read [architecture-patterns.md](references/architecture-patterns.md) for directory templates, module interaction patterns, action-camera flows, and C interface examples.
- Read [review-checklist.md](references/review-checklist.md) when reviewing a design or repository for coupling, portability, concurrency, build, and testing risks.
