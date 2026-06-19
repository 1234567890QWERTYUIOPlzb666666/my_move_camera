# Architecture Review Checklist

## Module Boundaries

- Does each module have one clear business capability?
- Are public headers separate from internal implementation?
- Do names such as `manager`, `common`, or `utils` hide mixed responsibilities?
- Does each state value have one authoritative owner?
- Are cross-module policies located in Application/Coordinator rather than low-level modules?

## Dependency Direction

- Do Domain and Application headers avoid Qt, LVGL, POSIX, and vendor SDK types?
- Does a module depend on a narrow Port instead of a concrete backend?
- Are any A-to-B and B-to-A include/link cycles present?
- Was a cycle merely moved into a broad `common` module or global service locator?
- Are CMake target links consistent with the intended dependency diagram?

## Communication

- Are synchronous operations modeled as direct calls with explicit results?
- Are events facts in past tense rather than hidden commands?
- Is the event producer, payload owner, consumer thread, and delivery guarantee documented?
- Are request/response and high-rate frames kept off the generic event bus?
- Are queue capacity and overflow/backpressure policies defined?

## UI

- Can business rules run without creating Qt/LVGL objects?
- Does the UI call use cases rather than storage, devices, or vendor SDKs directly?
- Are backend-neutral screen models free of `QString`, `QObject`, and `lv_obj_t`?
- Are Qt and LVGL allowed to use their native widget/layout capabilities?
- Are physical buttons and remote commands mapped to application actions?

## C Ownership and Lifecycle

- Does each pointer contract state borrowed, retained, copied, or transferred ownership?
- Are callback contexts valid for the entire subscription?
- Can callbacks be reentrant?
- Are init failure rollback and partial construction handled?
- Is destruction performed in reverse dependency order?
- Can asynchronous operations be cancelled or awaited during shutdown?

## Concurrency

- Is thread ownership of mutable state explicit?
- Are all Qt/LVGL calls dispatched onto the UI thread?
- Can SDK callbacks block or acquire locks in unsafe order?
- Are snapshots copied consistently?
- Are deadlock, starvation, queue saturation, and shutdown races tested?

## Media and Storage

- Are media frames transported through dedicated bounded or zero-copy paths?
- Is recording controlled by a state machine?
- Is container finalization guaranteed before power-off or card unmount?
- Are card removal, full storage, I/O error, and corrupted-file recovery defined?
- Can thermal or battery policies degrade quality before forced shutdown?

## Portability

- Are platform types absent from stable public APIs?
- Are platform abstractions driven by a real port or test need?
- Are vendor-specific capabilities represented without reducing every backend to the lowest common denominator?
- Can a mock backend run core use cases on a development PC?

## Testing and Delivery

- Are Domain, state machines, presenters, and coordinators unit-tested?
- Are adapters integration-tested against hardware or simulators?
- Does CI compile every supported UI/media/backend combination?
- Are fault injection and long-duration recording tests included?
- Can logs correlate commands, events, state transitions, and module sources?

## Review Output

Report findings in this order:

1. Dependency cycles and ownership/concurrency defects.
2. Product failure risks such as unfinished recording files.
3. Portability and testability blockers.
4. Over-abstraction and unnecessary framework code.
5. A staged remediation plan that preserves working vertical slices.
