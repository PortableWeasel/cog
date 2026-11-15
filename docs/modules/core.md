# cog-core {#page_cog_core}

## What it does

`cog-core` is the heart of every Cog process. It owns four things:

- the **agent loop** (`cog_core_run`) that pulls events and advances state,
- the **table-driven state machine** (`(state, event) -> handler`),
- the **session tree** that stores the conversation and supports forking,
- the **event queue** that carries input, tool results, IPC traffic, and
  signals into the loop.

It deliberately contains no networking, no provider implementation, and no
tool implementation. Those live in sibling libraries and enter the core either
through function pointers in the configuration (service/idle hooks) or through
events.

Headers: `core.h`, `event.h`, `state.h`, `handler.h`, `message.h`,
`result.h`, `agent.h`, `tool.h`, `session.h`, `session_store.h`.

## Key ideas

**Every API returns `cog_result_t`.** `COG_OK` is zero; failures are negative
and stable. See `result.h`.

**The dispatch table is `static const`.** Rows pair a state and an event with
a handler and the list of target states that transition may enter. Nothing is
registered at run time, so the table is indexer-friendly and lives in
read-only memory.

**Handlers return a `{transition, status}` pair.** A non-`COG_OK` status
prevents the transition from being applied and is reported by
`cog_core_run`.

**State is caller-owned.** `cog_core_new` does not take ownership of the
session, provider, observation context, or store. The IPC endpoint is the one
documented exception: ownership transfers on a successful `cog_core_new`.

**Single-threaded first.** One core is driven by one thread calling
`cog_core_run`. `cog_core_stop` is the only call intended to be safe from
another thread or a signal handler.

## Getting started: a minimal agent loop

The smallest useful core has one state, one handler, and one dispatch row.
The handler appends the user message to the session and stops the loop so the
program can exit.

```c
#include <cog/core.h>

#include <stdio.h>
#include <string.h>

enum { ST_IDLE = 0 };

/* ST_IDLE + user input: append the prompt, then leave the loop. */
static cog_handler_result_t on_user_input(cog_core_t *core,
                                          const cog_event_t *event)
{
    cog_message_t message = {0};

    message.role = COG_MESSAGE_ROLE_USER;
    message.content = (const char *)event->payload;
    message.content_len = event->payload_len;

    if (cog_session_append(cog_core_session(core), &message, NULL) != COG_OK) {
        return (cog_handler_result_t){COG_TRANSITION_SELF, COG_ERROR_IO};
    }

    cog_core_stop(core); /* one-shot demo: return from run */
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}

static const cog_state_t stay_put[] = {
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, COG_EVENT_USER_INPUT, on_user_input, stay_put),
};

int main(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t config = {0};
    cog_event_t event = {0};
    int status = 1;

    if (cog_session_new(&session, NULL) != COG_OK) {
        return 1;
    }

    config.session = session;
    config.dispatch_table = dispatch;
    config.dispatch_table_size = sizeof dispatch / sizeof dispatch[0];

    if (cog_core_new(&core, &config) != COG_OK) {
        cog_session_free(session);
        return 1;
    }

    event.id = (cog_event_id_t)COG_EVENT_USER_INPUT;
    event.kind = COG_EVENT_USER_INPUT;
    event.payload = "hello";
    event.payload_len = 5;

    if (cog_core_enqueue(core, &event) == COG_OK &&
        cog_core_run(core) == COG_OK) {
        status = 0;
    }

    cog_core_free(core);
    cog_session_free(session);
    return status;
}
```

In a real harness the handler would call a provider, enqueue
`COG_EVENT_LLM_COMPLETE`, and let the loop keep running. `cog_core_stop` is
for shutdown, not for making a single turn finish.

## Sessions and forking

A session is a tree of messages, not a flat list. `cog_session_append`
continues the active branch; `cog_session_fork` marks a node as an immutable
branch point so a second answer can be produced without rewriting history.

```c
#include <cog/session.h>

#include <stdio.h>

void build_and_fork(cog_session_t *session);

void build_and_fork(cog_session_t *session)
{
    cog_message_t user = {0};
    cog_message_t first_reply = {0};
    cog_message_t second_reply = {0};
    cog_session_node_t *user_node = NULL;
    cog_session_node_t *fork_point = NULL;
    const cog_message_t *history = NULL;
    size_t count = 0;

    user.role = COG_MESSAGE_ROLE_USER;
    user.content = "Summarise DESIGN.md";
    user.content_len = 19;
    (void)cog_session_append(session, &user, &user_node);

    first_reply.role = COG_MESSAGE_ROLE_ASSISTANT;
    first_reply.content = "It is the system blueprint.";
    first_reply.content_len = 28;
    (void)cog_session_append(session, &first_reply, NULL);

    /* Branch from the user turn; the first reply stays untouched. */
    if (cog_session_fork(session, user_node, &fork_point) == COG_OK) {
        second_reply.role = COG_MESSAGE_ROLE_ASSISTANT;
        second_reply.content = "Alternative answer.";
        second_reply.content_len = 18;
        (void)cog_session_add_message(session, fork_point, &second_reply, NULL);
    }

    if (cog_session_get_history(session, cog_session_active(session),
                                &history, &count) == COG_OK) {
        printf("%zu messages on the active branch\n", count);
    }
}
```

Forking is refused while any identified tool call on the session is
unresolved, because branching away could otherwise strand recovery. See
`session.h` and the `session` contract for the ledger rules.

## Persistence

`cog_session_store` persists sessions through a pluggable backend. The default
backend writes one directory per session under `base_dir`. The core can
auto-save after each completed exchange when `auto_save` is set.

```c
#include <cog/session.h>
#include <cog/session_store.h>

#include <stdio.h>

int save_and_load(cog_session_t *session);

int save_and_load(cog_session_t *session)
{
    cog_store_config_t config = {0};
    cog_store_t *store = NULL;
    cog_session_t *loaded = NULL;
    int status = 1;

    config.base_dir = "/tmp/cog-demo-sessions";
    config.auto_save = true;

    if (cog_store_new(&store, &config) != COG_OK) {
        return 1;
    }

    if (cog_store_save(store, session) == COG_OK &&
        cog_store_load(store, cog_session_id(session), &loaded) == COG_OK) {
        printf("restored %s\n", cog_session_id(loaded));
        status = 0;
    }

    cog_session_free(loaded); /* NULL-safe */
    cog_store_free(store);
    return status;
}
```

A restore that finds an in-flight `STARTED` tool call turns it into
`INDETERMINATE` and refuses ordinary continuation. That is intentional: Cog
never re-runs an effect whose outcome is unknown.

## Beyond the basics

- **Service and idle hooks.** `cog_core_config_t.service_fn` and `idle_fn`
  let the application drive provider, network, and tool work at a bounded
  cadence without a second thread. This is how the CLI multiplexes everything
  in one loop.
- **Subagents.** `agent.h` adds queued spawn/cancel intents, bounded agent
  records, and update messages between parent and child.
- **Pending messages.** `cog_core_queue_message` stores input for the next
  turn and re-injects it after the current one completes.
- **IPC ingress.** Passing a bound `cog_ipc_endpoint_t` makes the core drain
  peers and turn their frames into `COG_EVENT_IPC_*` events.

## See also

- @ref page_cog_provider — where events come from
- @ref page_cog_toolkit — executing the tools a handler selects
- @ref page_cog_ipc — multi-process cores
- @ref page_cog_obs — lifecycle logging from the core
