# Resume Hooks
Resume hooks allow you to run arbitrary code immediately before a worker is
resumed. The daemon runs the hooks installed by default; the RPC that added or
removed a global hook at run time (`ConfigureResumeHook`) left with the legacy
service (D-140), so a hook that is not compiled in needs a control_v2 route
first.

The API is simple, similar to that of `GateHook`. All you need to do is:

- Define `YourHook::kName`. This must be unique across all other resume hooks.
- Define `YourHook::kPriority` (lower values get higher priority). Ties are broken by hook name in increasing lexographical order.
- Include `ResumeHook(kName, kPriority)` in `YourHook`'s initializer list.
- Define `void YourHook::Run()`.
- Define `void YourHook::Init(const bess::pb::YourHookArg &)`.
- Include `ADD_RESUME_HOOK(YourHook)` at the bottom of `resume_hooks/your_hook.cc`.
