# Upstream bug: an RC target crashes on the read after a failed accept

MXL `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, `lib/fabrics/ofi`, built with `MXL_ENABLE_FABRICS_OFI=ON`,
libfabric 2.3.1. Any provider that uses `RCTarget` (`FI_EP_MSG`: `verbs`, `tcp`).

## What happened

On the platform all three agents restarted together (agent 1.6.0, `verbs`). About 1 s after the start, the
host-01 agent died with SIGSEGV (exit 139) while it was accepting a peer's connection:

```
[RCTarget.cpp:96] Accept connection from: fi_sockaddr_in://192.168.250.2:35714
[libfabric:ep_ctrl:verbs:1055] rdma_create_qp: Invalid argument (22)
[libfabric:ep_ctrl:verbs:242] fi_control: Invalid argument (-22)
[error] [fabrics.cpp:79] Failed to try for new samples: Failed to accept connection: Invalid argument, code -22
[info] [RCTarget.cpp:167] Remote endpoint has shutdown the connection. Transitioning to listening to new connection.
-> exit 139
```

The `RCTarget.cpp:167` line comes from another target of the same agent. The crash is the next read of the
target whose accept failed.

## The sequence

`lib/fabrics/ofi/src/internal/RCTarget.cpp`, `RCTarget::readNext`:

1. Line 84 and line 189: `_state = std::visit(overloaded{...}, std::move(_state));`. Each lambda takes its state
   **by value** (line 86 `[&](WaitForConnectionRequest state)`, line 128, line 160). `std::visit` passes the
   alternative as an rvalue, so the lambda's `state` is move-constructed out of `_state`. From then on, the
   alternative in `_state` is moved-from.
2. Lines 98 to 108 open the completion queue, create the endpoint, bind both queues, open the event queue and
   call `endpoint.accept()`. Any of them can throw. On the platform `fi_accept` failed: the `verbs` provider
   creates the QP when the endpoint is enabled from `fi_accept`, and `rdma_create_qp` returned EINVAL.
3. Line 117 catches the `FabricException`. It is not `isInterrupted()`, so line 125 rethrows it. The
   assignment to `_state` never happens, and `_state` still holds the moved-from `WaitForConnectionRequest`.
   The lambda's local `state` is destroyed while the exception unwinds, which closes the listening passive
   endpoint.
4. `fabrics.cpp:79` (`try_run`) logs the error and returns the status. The target is still there.
5. The next `mxlFabricsTargetReadGrainNonBlocking` or `mxlFabricsTargetReadSamplesNonBlocking` reaches line 90:
   `readEventQueue<...>(*state.pep.eventQueue(), timeout)` on that moved-from `PassiveEndpoint`.
   - `PassiveEndpoint.cpp:55-62`, the move constructor: `_eq(std::move(other._eq))`. `_eq` is a
     `std::optional<std::shared_ptr<EventQueue>>`, and a moved-from optional is still **engaged**, holding a
     null `shared_ptr`.
   - `PassiveEndpoint.cpp:106-114`, `eventQueue()`: the `if (!_eq)` check passes and it returns the null
     pointer.
   - `*nullptr` gives `EventQueue::read()` with `this == nullptr`. At `EventQueue.cpp:61`,
     `fi_eq_read(_raw, ...)` reads `_raw` through the null pointer: SIGSEGV.

The `Connected` branch has the same problem. When `makeListener` (line 168) or `readEndpointQueues` (line 164)
throws something other than an interrupt, `_state` keeps a moved-from `Connected`. Its `Endpoint` has
`_raw == nullptr` and engaged optionals with null queues (`Endpoint.cpp:121-130`). `Endpoint::readQueues`
(`Endpoint.cpp:224-236`) then dereferences them. The `WaitForConnection` branch fails the same way when
`_proto->start()` (line 142) throws.

`RCInitiator.cpp` uses the same `_state = std::visit(..., std::move(_state))` pattern (lines 64-91, 105-134)
and should be checked too.

Destroying a target in that state is safe: the moved-from objects have `_raw == nullptr`, and their
destructors do not call `fi_close`.

## Reproduction (tcp, no RDMA hardware)

Any throw between the connection request and the new state is enough. Without root the easiest one is
running out of file descriptors. After the connection is accepted, the completion queue cannot get its
wait object (`EMFILE`):

1. Set up a target on `127.0.0.1:<port>`, `tcp`.
2. Fill the descriptor table and set `RLIMIT_NOFILE` so that 1 to 3 descriptors are left. The socket accept
   gets one, and `CompletionQueue::open` or `EventQueue::open` fails.
3. In another process, set up an initiator on a flow, add the target and make progress until it connects.
4. Call `mxlFabricsTargetReadGrainNonBlocking` in a loop.

Result (every time, with 1 to 8 descriptors left):

```
[RCTarget.cpp:96] Accept connection from: fi_sockaddr_in://127.0.0.1:54738
[fabrics.cpp:79] Failed to try for new grain: Failed to open completion queue: Too many open files, code -24
repro: read status 1 (error 1)
repro: SIGSEGV, backtrace:
libmxl-fabrics.so.1(mxl::lib::fabrics::ofi::EventQueue::read()+0x1a)
libmxl-fabrics.so.1(RCTarget::readNext<(QueueReadMode)1>(...)::{lambda(RCTarget::WaitForConnectionRequest)#2}::operator()(...) const+0x7c)
libmxl-fabrics.so.1(RCTarget::readNext<(QueueReadMode)1>(...)+0x151)
libmxl-fabrics.so.1(RCTarget::read()+0x23)
libmxl-fabrics.so.1(TargetWrapper::read()+0x2f)
libmxl-fabrics.so.1(mxlFabricsTargetReadGrainNonBlocking+0x60)
exit 139
```

The same thing happens in the agent. `tests/integration/accept_fail.sh` preloads `tests/tools/accept_fault.cpp`,
which makes the first queue descriptor after the target's first accept fail with `EMFILE`. Agent 1.7.0 dies
with SIGSEGV on the next pass. 1.7.1 closes the target and carries on.

## Suggested fix

Keep `_state` valid when a branch throws:

- take the state by reference and move out of it only on the path that returns a new state, or
- catch everything in each branch, put the state back (`return WaitForConnectionRequest{std::move(state.pep)}`
  on a failed accept, after rejecting or dropping that connection request), and report the error, or at least
- set `_state = std::monostate{}` before rethrowing. The existing `std::monostate` branch at line 85 then throws
  `invalidState` instead of crashing. `State` (`RCTarget.hpp:73`) needs `std::monostate` as an alternative for
  this.

A failed accept should leave the target listening (step 3 closes the listener, so even a valid state would not
accept again). Also, `PassiveEndpoint::eventQueue()` and `Endpoint::eventQueue()` should check that the pointer
is not null, as well as the optional.

## Why `rdma_create_qp` returned EINVAL (not proven)

This was not reproduced: the lab has no RDMA device without root, because soft-RoCE needs `rdma link add`.
The QP capabilities are the provider defaults from the connection request's `fi_info`: tx/rx size, iov limit
and inline size, which `verbs` takes from the device at `fi_getinfo` (MXL sets none). They are the same for
every accept on that device. The steady state and the restart right after accepted with the same values, so
a capability over a device limit would have failed every time.

The more likely cause is the connection request itself. `rdma_create_qp` → `ucma_init_conn_qp` →
`rdma_init_qp_attr` → kernel `ib_cm_init_qp_attr` returns -EINVAL when the passive side's CM id is no longer
in a connecting state. That happens when the peer withdrew the request (a REJ or a timeout) between the
request and the target's next poll, for example a peer agent that restarted at the same moment, or an
initiator that was set up again. librdmacm also returns EINVAL when the request arrived on a device other
than the one the target's PD belongs to (`id->verbs != pd->context`). That would fail every time, though,
like the capabilities.

Either way, a failed accept of one stale request must not take the process down.

## What the agent does (1.7.1)

`src/fabric/domain.cpp` `closeFailedTarget`: the first read that fails (not `MXL_ERR_NOT_READY` or
`MXL_ERR_INTERRUPTED`) is the last call into that target. The agent destroys it at once (log
`fabric_target_closed`). After 1 s without a grain the replication engine sets up a new target on the next
port and repeats the handshake (`replication_restarted`). In a loop the wait is 1, 2, 4 and then 8 s.
